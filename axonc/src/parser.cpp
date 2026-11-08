// parser.cpp
#include "parser.h"

Parser::Parser(std::vector<Token> tokens, Module* m) : t(std::move(tokens)), mod(m) {}

void parseModuleSource(Module* mod) {
    Parser ps(lexFile(mod->fileId), mod);
    ps.parseModule();
}

bool Parser::accept(T k) {
    if (at(k)) { p++; return true; }
    return false;
}

const Token& Parser::expect(T k, const char* what) {
    if (at(k)) return t[p++];
    std::string msg = std::string("expected ") + what + ", found " + tokDisplay(cur());
    Span sp = cur().span;
    // Point just after the previous token for missing `;` / `)` etc.
    if (p > 0 && (k == T::Semi || k == T::RParen || k == T::RBrace || k == T::RBracket) &&
        t[p - 1].span.line != cur().span.line) {
        Span prev = t[p - 1].span;
        sp = g_diag.makeSpan(prev.file, prev.offset + prev.len, 1);
        auto& d = g_diag.error(sp, std::string("expected ") + what);
        if (k == T::Semi) d.help("statements end with `;`");
        throw ParseError();
    }
    g_diag.error(sp, msg);
    throw ParseError();
}

void Parser::fail(const std::string& msg, const std::string& help) {
    auto& d = g_diag.error(cur().span, msg);
    if (!help.empty()) d.help(help);
    throw ParseError();
}

Span Parser::spanFrom(const Span& start) const {
    const Token& last = t[p > 0 ? p - 1 : 0];
    if (last.span.file != start.file || last.span.offset < start.offset) return start;
    return g_diag.makeSpan(start.file, start.offset, last.span.offset + last.span.len - start.offset);
}

Expr* Parser::mk(EK k, Span sp) {
    Expr* e = new Expr();
    e->kind = k;
    e->span = sp;
    return e;
}

void Parser::syncTop() {
    int depth = 0;
    while (!at(T::Eof)) {
        if (at(T::LBrace)) depth++;
        else if (at(T::RBrace)) {
            depth--;
            if (depth <= 0) { p++; if (depth == 0) return; depth = 0; continue; }
        } else if (at(T::Semi) && depth == 0) { p++; return; }
        else if (depth == 0 && (at(T::KwStruct) || at(T::KwEnum) || at(T::KwInterface) || at(T::KwUsing) ||
                                at(T::KwExtend) || at(T::KwExtern)))
            return;
        p++;
    }
}

void Parser::syncStmt() {
    int depth = 0;
    while (!at(T::Eof)) {
        if (at(T::LBrace)) depth++;
        else if (at(T::RBrace)) {
            if (depth == 0) return;
            depth--;
            if (depth == 0) { p++; return; }
        } else if (at(T::Semi) && depth == 0) { p++; return; }
        p++;
    }
}

// ---------------------------------------------------------------- declarations

void Parser::parseModule() {
    while (!at(T::Eof)) {
        size_t before = p;
        try {
            parseTopDecl();
        } catch (ParseError&) {
            if (p == before) p++;
            syncTop();
        }
    }
}

void Parser::parseTopDecl() {
    std::vector<std::string> attrs;
    while (at(T::At)) {
        p++;
        attrs.push_back(expect(T::Ident, "an attribute name after `@`").text);
    }
    bool isPrivate = accept(T::KwPrivate);
    if (at(T::KwUsing)) { parseUsing(); return; }
    if (at(T::KwStruct)) { mod->structs.push_back(parseStruct(isPrivate)); return; }
    if (at(T::KwInterface)) { mod->ifaces.push_back(parseInterface(isPrivate)); return; }
    if (at(T::KwEnum)) { mod->enums.push_back(parseEnum(isPrivate)); return; }
    if (at(T::KwExtend)) { mod->extends.push_back(parseExtend()); return; }
    if (at(T::KwExtern)) { parseExtern(); return; }
    if (at(T::KwConst)) {
        VarDecl* v = parseConst(true);
        v->isPrivate = isPrivate;
        mod->globals.push_back(v);
        return;
    }
    if (at(T::KwClass))
        fail("Axon has no classes", "use `struct` for data and `interface` for shared behavior");
    if (!isTypeStart(p)) fail("expected a declaration, found " + tokDisplay(cur()),
                              "top-level code must be inside a function, e.g. `int main() { ... }`");
    TypeExpr* ret = parseType();
    const Token& name = expect(T::Ident, "a function name");
    if (at(T::Assign) || at(T::Semi))
        fail("global variables aren't supported", "use `const` for a fixed value, or pass the value as a parameter");
    std::vector<TypeParam> tps = parseTypeParams();
    FuncDecl* f = parseFuncRest(ret, name, true);
    f->gen.typeParams = tps;
    f->isPrivate = isPrivate;
    f->attrs = attrs;
    for (auto& a : attrs) {
        if (a == "test") f->isTest = true;
        else g_diag.error(f->span, "unknown attribute `@" + a + "`").help("the only attribute right now is `@test`");
    }
    mod->funcs.push_back(f);
}

void Parser::parseUsing() {
    UsingDecl u;
    Span start = cur().span;
    p++;
    if (atIdent("c") && peekTok().type == T::StrLit) {
        p++;
        u.isC = true;
        u.header = cur().sval;
        p++;
    } else {
        u.path.push_back(expect(T::Ident, "a module name").text);
        while (accept(T::Dot)) u.path.push_back(expect(T::Ident, "a module name").text);
    }
    if (atIdent("as")) {
        p++;
        u.alias = expect(T::Ident, "a name after `as`").text;
    }
    u.span = spanFrom(start);
    expect(T::Semi, "`;`");
    mod->usings.push_back(u);
}

// `<T, U : Printable + Damageable>` after a struct, enum or function name
std::vector<TypeParam> Parser::parseTypeParams() {
    std::vector<TypeParam> tps;
    if (!at(T::Lt)) return tps;
    p++;
    do {
        TypeParam tp;
        const Token& n = expect(T::Ident, "a type parameter name, like `T`");
        tp.name = n.text;
        tp.span = n.span;
        if (accept(T::Colon)) {
            do tp.bounds.push_back(parseType());
            while (accept(T::Plus));
        }
        for (auto& o : tps)
            if (o.name == tp.name) g_diag.error(n.span, "type parameter `" + tp.name + "` is listed twice");
        tps.push_back(tp);
    } while (accept(T::Comma));
    closeAngle();
    return tps;
}

std::vector<Param> Parser::parseParams() {
    std::vector<Param> ps;
    expect(T::LParen, "`(`");
    if (!at(T::RParen)) {
        do {
            Param pr;
            Span start = cur().span;
            if (accept(T::KwMut)) {
                expect(T::KwRef, "`ref` after `mut` (write `mut ref Type name`)");
                pr.mode = 2;
            } else if (accept(T::KwRef)) pr.mode = 1;
            pr.type = parseType();
            pr.name = expect(T::Ident, "a parameter name").text;
            pr.span = spanFrom(start);
            ps.push_back(pr);
        } while (accept(T::Comma));
    }
    expect(T::RParen, "`)`");
    return ps;
}

FuncDecl* Parser::parseFuncRest(TypeExpr* ret, const Token& nameTok, bool needBody) {
    FuncDecl* f = new FuncDecl();
    f->name = nameTok.text;
    f->ret = ret;
    f->span = nameTok.span;
    f->mod = mod;
    f->params = parseParams();
    if (needBody) {
        if (at(T::Semi)) fail("this function needs a body", "add `{ ... }` after the parameter list");
        f->body = parseBlock();
    } else {
        if (at(T::LBrace)) fail("declarations here can't have a body", "end the declaration with `;`");
        expect(T::Semi, "`;`");
    }
    return f;
}

VarDecl* Parser::parseConst(bool global) {
    Span start = cur().span;
    p++;  // const
    VarDecl* v = new VarDecl();
    v->kind = global ? VarDecl::Global : VarDecl::Local;
    v->isConst = true;
    v->mod = mod;
    if (at(T::KwVar)) p++;
    else v->texpr = parseType();
    const Token& name = expect(T::Ident, "a constant name");
    v->name = name.text;
    v->span = name.span;
    if (!at(T::Assign)) {
        g_diag.error(name.span, "constant `" + v->name + "` needs a value").help("write `const ... " + v->name + " = value;`");
        throw ParseError();
    }
    p++;
    v->init = parseExpr();
    expect(T::Semi, "`;`");
    (void)start;
    return v;
}

StructDecl* Parser::parseStruct(bool isPrivate) {
    p++;
    StructDecl* s = new StructDecl();
    const Token& name = expect(T::Ident, "a struct name");
    s->name = name.text;
    s->span = name.span;
    s->mod = mod;
    s->isPrivate = isPrivate;
    s->gen.typeParams = parseTypeParams();
    if (accept(T::Colon)) {
        do s->ifaceExprs.push_back(parseType());
        while (accept(T::Comma));
    }
    expect(T::LBrace, "`{`");
    while (!at(T::RBrace) && !at(T::Eof)) {
        size_t before = p;
        try {
            bool priv = accept(T::KwPrivate);
            bool isMut = accept(T::KwMut);
            TypeExpr* ty = parseType();
            const Token& n = expect(T::Ident, "a field or method name");
            if (at(T::LParen)) {
                FuncDecl* f = parseFuncRest(ty, n, true);
                f->isMut = isMut;
                f->isPrivate = priv;
                f->ownerStruct = s;
                s->methods.push_back(f);
            } else {
                if (isMut) g_diag.error(n.span, "`mut` only goes on methods").help("fields can always be changed by `mut` methods");
                FieldDecl* fd = new FieldDecl();
                fd->name = n.text;
                fd->type = ty;
                fd->span = n.span;
                fd->isPrivate = priv;
                if (accept(T::Assign)) fd->def = parseExpr();
                expect(T::Semi, "`;` after the field");
                s->fields.push_back(fd);
            }
        } catch (ParseError&) {
            if (p == before) p++;
            syncStmt();
        }
    }
    expect(T::RBrace, "`}`");
    return s;
}

InterfaceDecl* Parser::parseInterface(bool isPrivate) {
    p++;
    InterfaceDecl* d = new InterfaceDecl();
    const Token& name = expect(T::Ident, "an interface name");
    d->name = name.text;
    d->span = name.span;
    d->mod = mod;
    d->isPrivate = isPrivate;
    expect(T::LBrace, "`{`");
    while (!at(T::RBrace) && !at(T::Eof)) {
        size_t before = p;
        try {
            bool isMut = accept(T::KwMut);
            TypeExpr* ty = parseType();
            const Token& n = expect(T::Ident, "a method name");
            FuncDecl* f = parseFuncRest(ty, n, false);
            f->isMut = isMut;
            f->ownerIface = d;
            d->methods.push_back(f);
        } catch (ParseError&) {
            if (p == before) p++;
            syncStmt();
        }
    }
    expect(T::RBrace, "`}`");
    return d;
}

EnumDecl* Parser::parseEnum(bool isPrivate) {
    p++;
    EnumDecl* e = new EnumDecl();
    const Token& name = expect(T::Ident, "an enum name");
    e->name = name.text;
    e->span = name.span;
    e->mod = mod;
    e->isPrivate = isPrivate;
    e->gen.typeParams = parseTypeParams();
    expect(T::LBrace, "`{`");
    while (!at(T::RBrace) && !at(T::Eof)) {
        Variant v;
        const Token& vn = expect(T::Ident, "a variant name");
        v.name = vn.text;
        v.span = vn.span;
        if (accept(T::LParen)) {
            if (!at(T::RParen)) {
                do {
                    FieldDecl* f = new FieldDecl();
                    f->type = parseType();
                    const Token& fn = expect(T::Ident, "a name for this value (e.g. `int damage`)");
                    f->name = fn.text;
                    f->span = fn.span;
                    v.fields.push_back(f);
                } while (accept(T::Comma));
            }
            expect(T::RParen, "`)`");
        }
        e->variants.push_back(v);
        if (!accept(T::Comma)) break;
    }
    expect(T::RBrace, "`}`");
    return e;
}

ExtendDecl* Parser::parseExtend() {
    Span start = cur().span;
    p++;
    ExtendDecl* x = new ExtendDecl();
    x->mod = mod;
    x->target = parseType();
    x->span = spanFrom(start);
    if (accept(T::Colon)) {
        do x->ifaceExprs.push_back(parseType());
        while (accept(T::Comma));
    }
    expect(T::LBrace, "`{`");
    while (!at(T::RBrace) && !at(T::Eof)) {
        size_t before = p;
        try {
            bool priv = accept(T::KwPrivate);
            bool isMut = accept(T::KwMut);
            TypeExpr* ty = parseType();
            const Token& n = expect(T::Ident, "a method name");
            if (!at(T::LParen)) fail("`extend` blocks can only add methods", "fields must be declared in the struct itself");
            FuncDecl* f = parseFuncRest(ty, n, true);
            f->isMut = isMut;
            f->isPrivate = priv;
            f->ownerExtend = x;
            x->methods.push_back(f);
        } catch (ParseError&) {
            if (p == before) p++;
            syncStmt();
        }
    }
    expect(T::RBrace, "`}`");
    return x;
}

void Parser::parseExtern() {
    p++;
    std::string header;
    if (at(T::StrLit)) { header = cur().sval; p++; }
    bool axonRt = header == "@axon";
    if (!header.empty() && !axonRt) {
        bool dup = false;
        for (auto& h : mod->cIncludes) dup |= h == header;
        if (!dup) mod->cIncludes.push_back(header);
    }
    expect(T::LBrace, "`{` to start the extern block");
    while (!at(T::RBrace) && !at(T::Eof)) {
        size_t before = p;
        try {
            if (at(T::KwStruct)) {
                p++;
                StructDecl* s = new StructDecl();
                const Token& n = expect(T::Ident, "a struct name");
                s->name = n.text;
                s->span = n.span;
                s->mod = mod;
                s->isExtern = true;
                s->header = header;
                expect(T::LBrace, "`{`");
                while (!at(T::RBrace) && !at(T::Eof)) {
                    FieldDecl* f = new FieldDecl();
                    f->type = parseType();
                    const Token& fn = expect(T::Ident, "a field name");
                    f->name = fn.text;
                    f->span = fn.span;
                    s->fields.push_back(f);
                    while (accept(T::Comma)) {  // `byte r, g, b;`
                        FieldDecl* f2 = new FieldDecl();
                        f2->type = f->type;
                        const Token& fn2 = expect(T::Ident, "a field name");
                        f2->name = fn2.text;
                        f2->span = fn2.span;
                        s->fields.push_back(f2);
                    }
                    expect(T::Semi, "`;`");
                }
                expect(T::RBrace, "`}`");
                accept(T::Semi);
                mod->structs.push_back(s);
                continue;
            }
            if (at(T::KwConst)) {
                p++;
                VarDecl* v = new VarDecl();
                v->kind = VarDecl::Global;
                v->isConst = true;
                v->isExtern = true;
                v->header = header;
                v->mod = mod;
                v->texpr = parseType();
                const Token& n = expect(T::Ident, "a constant name");
                v->name = n.text;
                v->span = n.span;
                expect(T::Semi, "`;`");
                mod->globals.push_back(v);
                continue;
            }
            TypeExpr* ret = parseType();
            const Token& n = expect(T::Ident, "a function name");
            FuncDecl* f = parseFuncRest(ret, n, false);
            f->isExtern = !axonRt;
            f->isAxonRt = axonRt;
            f->header = header;
            mod->funcs.push_back(f);
        } catch (ParseError&) {
            if (p == before) p++;
            syncStmt();
        }
    }
    expect(T::RBrace, "`}`");
}

// ---------------------------------------------------------------- types

static bool isPrimKw(T k) {
    switch (k) {
        case T::KwInt: case T::KwLong: case T::KwByte: case T::KwFloat: case T::KwDouble:
        case T::KwBool: case T::KwChar: case T::KwString: case T::KwVoid:
            return true;
        default:
            return false;
    }
}

bool Parser::isTypeStart(size_t i) const { return isPrimKw(t[i].type) || t[i].type == T::Ident; }

// Scans a type starting at token i without reporting errors. Used to tell declarations from expressions.
bool Parser::scanType(size_t& i) {
    if (isPrimKw(t[i].type)) i++;
    else if (t[i].type == T::Ident) {
        i++;
        if (t[i].type == T::Dot && t[i + 1].type == T::Ident) i += 2;
        if (t[i].type == T::Lt) {
            i++;
            while (true) {
                if (!scanType(i)) return false;
                if (pendingGt) { pendingGt = 0; break; }  // an inner `>>` closed this level too
                if (t[i].type == T::Comma) { i++; continue; }
                if (t[i].type == T::Gt) { i++; break; }
                if (t[i].type == T::Shr) {
                    // `>>` closes this level and the enclosing one
                    i++;
                    pendingGt = 1;
                    return true;
                }
                return false;
            }
        }
    } else return false;
    while (true) {
        if (t[i].type == T::LBracket && t[i + 1].type == T::RBracket) i += 2;
        else if (t[i].type == T::Question) i++;
        else break;
    }
    return true;
}

void Parser::closeAngle() {
    if (at(T::Gt)) { p++; return; }
    if (at(T::Shr) || at(T::Ge) || at(T::ShrEq)) {
        Token rest = cur();
        Token gt = rest;
        gt.type = T::Gt;
        gt.text = ">";
        gt.span.len = 1;
        rest.text = rest.text.substr(1);
        rest.type = rest.type == T::Shr ? T::Gt : rest.type == T::Ge ? T::Assign : T::Ge;
        rest.span = g_diag.makeSpan(rest.span.file, rest.span.offset + 1, rest.span.len - 1);
        t[p] = rest;
        return;  // consumed one `>`
    }
    expect(T::Gt, "`>`");
}

TypeExpr* Parser::parseBaseType() {
    TypeExpr* te = new TypeExpr();
    te->span = cur().span;
    if (isPrimKw(cur().type)) {
        te->name = cur().text;
        p++;
        return te;
    }
    if (at(T::KwVar)) fail("`var` can only be used for local variables", "write the type out here");
    const Token& n = expect(T::Ident, "a type");
    te->name = n.text;
    if (at(T::Dot) && peekTok().type == T::Ident) {
        p++;
        te->ns = te->name;
        te->name = cur().text;
        p++;
    }
    if (at(T::Lt)) {
        p++;
        te->kind = TypeExpr::Generic;
        do te->args.push_back(parseType());
        while (accept(T::Comma));
        closeAngle();
    }
    te->span = spanFrom(te->span);
    return te;
}

TypeExpr* Parser::parseType() {
    TypeExpr* te = parseBaseType();
    while (true) {
        if (at(T::LBracket) && peekTok().type == T::RBracket) {
            p += 2;
            TypeExpr* a = new TypeExpr();
            a->kind = TypeExpr::Array;
            a->args.push_back(te);
            a->span = spanFrom(te->span);
            te = a;
        } else if (at(T::Question)) {
            p++;
            TypeExpr* o = new TypeExpr();
            o->kind = TypeExpr::Optional;
            o->args.push_back(te);
            o->span = spanFrom(te->span);
            te = o;
        } else break;
    }
    return te;
}

// ---------------------------------------------------------------- statements

Stmt* Parser::parseBlock() {
    Stmt* b = new Stmt();
    b->kind = SK::Block;
    b->span = cur().span;
    expect(T::LBrace, "`{`");
    while (!at(T::RBrace) && !at(T::Eof)) {
        size_t before = p;
        try {
            b->stmts.push_back(parseStmt());
        } catch (ParseError&) {
            if (p == before) p++;
            syncStmt();
        }
    }
    expect(T::RBrace, "`}`");
    return b;
}

Stmt* Parser::asBlock(Stmt* s) {
    if (s->kind == SK::Block) return s;
    Stmt* b = new Stmt();
    b->kind = SK::Block;
    b->span = s->span;
    b->stmts.push_back(s);
    return b;
}

bool Parser::looksLikeDecl() const {
    if (at(T::KwVar)) return true;
    if (isPrimKw(cur().type)) return true;
    if (!at(T::Ident)) return false;
    size_t i = p;
    Parser* self = const_cast<Parser*>(this);
    int savedGt = self->pendingGt;
    bool ok = self->scanType(i);
    self->pendingGt = savedGt;
    if (!ok) return false;
    return t[i].type == T::Ident && (t[i + 1].type == T::Assign || t[i + 1].type == T::Semi);
}

Stmt* Parser::parseVarDecl(bool requireSemi) {
    Stmt* s = new Stmt();
    s->kind = SK::Var;
    s->span = cur().span;
    VarDecl* v = new VarDecl();
    v->mod = mod;
    if (accept(T::KwVar)) v->texpr = nullptr;
    else v->texpr = parseType();
    const Token& name = expect(T::Ident, "a variable name");
    v->name = name.text;
    v->span = name.span;
    if (!at(T::Assign)) {
        std::string ty = v->texpr ? g_diag.file(v->texpr->span.file).text.substr(v->texpr->span.offset, v->texpr->span.len) : "var";
        g_diag.error(name.span, "variable `" + v->name + "` must be given a value")
            .lbl("declared without a value")
            .help("Axon has no uninitialized variables: write `" + ty + " " + v->name + " = ...;`");
        if (at(T::Semi) && requireSemi) p++;
        v->init = nullptr;
    } else {
        p++;
        v->init = parseExpr();
        if (requireSemi) expect(T::Semi, "`;`");
    }
    s->var = v;
    s->span = spanFrom(s->span);
    return s;
}

Stmt* Parser::parseSimpleStmt(bool requireSemi) {
    if (looksLikeDecl()) return parseVarDecl(requireSemi);
    Stmt* s = new Stmt();
    s->kind = SK::ExprStmt;
    s->span = cur().span;
    s->expr = parseExpr();
    s->span = spanFrom(s->span);
    if (requireSemi) expect(T::Semi, "`;`");
    return s;
}

Stmt* Parser::parseIf() {
    Stmt* s = new Stmt();
    s->kind = SK::If;
    s->span = cur().span;
    p++;
    expect(T::LParen, "`(` after `if`");
    s->expr = parseExpr();
    expect(T::RParen, "`)`");
    s->then = asBlock(parseStmt());
    if (accept(T::KwElse)) s->els = asBlock(parseStmt());
    return s;
}

std::vector<PatBind> Parser::parseBinds() {
    std::vector<PatBind> bs;
    expect(T::LParen, "`(`");
    if (!at(T::RParen)) {
        do {
            PatBind b;
            b.span = cur().span;
            if (accept(T::KwVar)) {
                b.name = expect(T::Ident, "a name").text;
            } else if (at(T::Ident) && cur().text == "_" ) {
                b.name = "_";
                p++;
            } else if (at(T::Ident) && (peekTok().type == T::Comma || peekTok().type == T::RParen)) {
                b.name = cur().text;  // `Sword(dmg)` shorthand for `Sword(var dmg)`
                p++;
            } else {
                b.type = parseType();
                b.name = expect(T::Ident, "a name for this value").text;
            }
            b.span = spanFrom(b.span);
            bs.push_back(b);
        } while (accept(T::Comma));
    }
    expect(T::RParen, "`)`");
    return bs;
}

Pattern* Parser::parseIsPattern() {
    Pattern* pt = new Pattern();
    pt->span = cur().span;
    if (at(T::KwNone)) {
        pt->kind = Pattern::ExprPat;
        pt->expr = mk(EK::None, cur().span);
        p++;
        return pt;
    }
    if (at(T::Ident) && peekTok().type == T::LParen) {
        pt->kind = Pattern::Variant;
        pt->name = cur().text;
        p++;
        pt->hasParens = true;
        pt->binds = parseBinds();
    } else if (at(T::Ident) && peekTok().type == T::Dot && peekTok(2).type == T::Ident && peekTok(3).type == T::LParen) {
        pt->kind = Pattern::Variant;
        pt->qual = cur().text;
        p += 2;
        pt->name = cur().text;
        p++;
        pt->hasParens = true;
        pt->binds = parseBinds();
    } else {
        size_t i = p;
        int savedGt = pendingGt;
        bool isType = scanType(i);
        pendingGt = savedGt;
        if (isType && t[i].type == T::Ident) {
            pt->kind = Pattern::TypeBind;
            pt->type = parseType();
            pt->bind.span = cur().span;
            pt->bind.name = expect(T::Ident, "a name").text;
        } else if (at(T::Ident)) {
            pt->kind = Pattern::Variant;
            pt->name = cur().text;
            p++;
            if (at(T::Dot) && peekTok().type == T::Ident) {
                p++;
                pt->qual = pt->name;
                pt->name = cur().text;
                p++;
            }
        } else if (isPrimKw(cur().type)) {
            pt->kind = Pattern::TypeBind;
            pt->type = parseType();
            pt->bind.name = "_";
        } else {
            fail("expected a pattern after `is`", "for example `x is Player p` or `item is Sword(int dmg)`");
        }
    }
    pt->span = spanFrom(pt->span);
    return pt;
}

Pattern* Parser::parseCasePattern() {
    Pattern* pt = new Pattern();
    pt->span = cur().span;
    if (at(T::Ident) && peekTok().type == T::LParen) {
        pt->kind = Pattern::Variant;
        pt->name = cur().text;
        p++;
        pt->hasParens = true;
        pt->binds = parseBinds();
    } else if (at(T::Ident) && peekTok().type == T::Dot && peekTok(2).type == T::Ident && peekTok(3).type == T::LParen) {
        pt->kind = Pattern::Variant;
        pt->qual = cur().text;
        p += 2;
        pt->name = cur().text;
        p++;
        pt->hasParens = true;
        pt->binds = parseBinds();
    } else {
        size_t i = p;
        int savedGt = pendingGt;
        bool isType = at(T::Ident) || isPrimKw(cur().type) ? scanType(i) : false;
        pendingGt = savedGt;
        if (isType && t[i].type == T::Ident && t[i + 1].type == T::Colon) {
            pt->kind = Pattern::TypeBind;
            pt->type = parseType();
            pt->bind.span = cur().span;
            pt->bind.name = expect(T::Ident, "a name").text;
        } else {
            pt->kind = Pattern::ExprPat;
            pt->expr = parseExpr(3);
        }
    }
    pt->span = spanFrom(pt->span);
    return pt;
}

Stmt* Parser::parseSwitch() {
    Stmt* s = new Stmt();
    s->kind = SK::Switch;
    s->span = cur().span;
    p++;
    expect(T::LParen, "`(` after `switch`");
    s->expr = parseExpr();
    expect(T::RParen, "`)`");
    expect(T::LBrace, "`{`");
    while (!at(T::RBrace) && !at(T::Eof)) {
        Case c;
        c.span = cur().span;
        while (at(T::KwCase) || at(T::KwDefault)) {
            if (accept(T::KwDefault)) {
                c.isDefault = true;
            } else {
                p++;
                c.pats.push_back(parseCasePattern());
                while (accept(T::Comma)) c.pats.push_back(parseCasePattern());
            }
            expect(T::Colon, "`:` after the case");
        }
        if (c.pats.empty() && !c.isDefault) fail("expected `case` or `default` inside the switch");
        while (!at(T::KwCase) && !at(T::KwDefault) && !at(T::RBrace) && !at(T::Eof)) {
            size_t before = p;
            try {
                c.body.push_back(parseStmt());
            } catch (ParseError&) {
                if (p == before) p++;
                syncStmt();
            }
        }
        s->cases.push_back(c);
    }
    expect(T::RBrace, "`}`");
    return s;
}

Stmt* Parser::parseStmt() {
    Span start = cur().span;
    switch (cur().type) {
        case T::LBrace: return parseBlock();
        case T::Semi: {
            p++;
            Stmt* b = new Stmt();
            b->kind = SK::Block;
            b->span = start;
            return b;
        }
        case T::KwIf: return parseIf();
        case T::KwWhile: {
            p++;
            Stmt* s = new Stmt();
            s->kind = SK::While;
            s->span = start;
            expect(T::LParen, "`(` after `while`");
            s->expr = parseExpr();
            expect(T::RParen, "`)`");
            s->body = asBlock(parseStmt());
            return s;
        }
        case T::KwDo: {
            p++;
            Stmt* s = new Stmt();
            s->kind = SK::DoWhile;
            s->span = start;
            s->body = asBlock(parseStmt());
            expect(T::KwWhile, "`while` after the `do` body");
            expect(T::LParen, "`(`");
            s->expr = parseExpr();
            expect(T::RParen, "`)`");
            expect(T::Semi, "`;`");
            return s;
        }
        case T::KwFor: {
            p++;
            Stmt* s = new Stmt();
            s->kind = SK::For;
            s->span = start;
            expect(T::LParen, "`(` after `for`");
            if (!at(T::Semi)) s->init = parseSimpleStmt(false);
            expect(T::Semi, "`;`");
            if (!at(T::Semi)) s->expr = parseExpr();
            expect(T::Semi, "`;`");
            if (!at(T::RParen)) s->step = parseExpr();
            expect(T::RParen, "`)`");
            s->body = asBlock(parseStmt());
            return s;
        }
        case T::KwForeach: {
            p++;
            Stmt* s = new Stmt();
            s->kind = SK::Foreach;
            s->span = start;
            expect(T::LParen, "`(` after `foreach`");
            if (accept(T::KwMut)) {
                expect(T::KwRef, "`ref` after `mut`");
                s->foreachMode = 2;
            } else if (accept(T::KwRef)) s->foreachMode = 1;
            VarDecl* v = new VarDecl();
            v->kind = VarDecl::ForeachBind;
            v->mod = mod;
            if (!accept(T::KwVar)) v->texpr = parseType();
            const Token& n = expect(T::Ident, "a loop variable name");
            v->name = n.text;
            v->span = n.span;
            s->var = v;
            if (!accept(T::KwIn)) fail("expected `in` after the loop variable", "write `foreach (var x in items)`");
            s->expr = parseExpr();
            expect(T::RParen, "`)`");
            s->body = asBlock(parseStmt());
            return s;
        }
        case T::KwSwitch: return parseSwitch();
        case T::KwBreak:
        case T::KwContinue: {
            Stmt* s = new Stmt();
            s->kind = at(T::KwBreak) ? SK::Break : SK::Continue;
            s->span = start;
            p++;
            expect(T::Semi, "`;`");
            return s;
        }
        case T::KwReturn: {
            Stmt* s = new Stmt();
            s->kind = SK::Return;
            s->span = start;
            p++;
            if (!at(T::Semi)) s->expr = parseExpr();
            s->span = spanFrom(start);
            expect(T::Semi, "`;`");
            return s;
        }
        case T::KwConst: {
            Stmt* s = new Stmt();
            s->kind = SK::Var;
            s->span = start;
            s->var = parseConst(false);
            return s;
        }
        case T::KwElse: fail("`else` without a matching `if`");
        case T::KwCase: fail("`case` outside of a `switch`");
        default: break;
    }
    return parseSimpleStmt(true);
}

// ---------------------------------------------------------------- expressions

static int infixPrec(T k) {
    switch (k) {
        case T::Assign: case T::PlusEq: case T::MinusEq: case T::StarEq: case T::SlashEq:
        case T::PercentEq: case T::AmpEq: case T::PipeEq: case T::CaretEq: case T::ShlEq: case T::ShrEq:
            return 1;
        case T::Question: return 2;
        case T::QQ: return 3;
        case T::OrOr: return 4;
        case T::AndAnd: return 5;
        case T::Pipe: return 6;
        case T::Caret: return 7;
        case T::Amp: return 8;
        case T::EqEq: case T::NotEq: return 9;
        case T::Lt: case T::Gt: case T::Le: case T::Ge: case T::KwIs: return 10;
        case T::Shl: case T::Shr: return 11;
        case T::Plus: case T::Minus: return 12;
        case T::Star: case T::Slash: case T::Percent: return 13;
        default: return -1;
    }
}

Expr* Parser::parseStandaloneExpr() {
    Expr* e = parseExpr();
    if (!at(T::Eof)) fail("unexpected " + tokDisplay(cur()) + " inside `{...}`");
    return e;
}

Expr* Parser::parseExpr(int minPrec) {
    Expr* left = parseUnary();
    while (true) {
        T op = cur().type;
        int prec = infixPrec(op);
        if (prec < 0 || prec < minPrec) break;
        Span opSpan = cur().span;
        p++;
        if (prec == 1) {
            Expr* e = mk(EK::Assign, left->span);
            e->op = op;
            e->a = left;
            e->b = parseExpr(1);
            e->span = spanFrom(left->span);
            left = e;
        } else if (op == T::Question) {
            Expr* e = mk(EK::Ternary, left->span);
            e->a = left;
            e->b = parseExpr(3);
            expect(T::Colon, "`:` in the `? :` expression");
            e->c = parseExpr(2);
            e->span = spanFrom(left->span);
            left = e;
        } else if (op == T::KwIs) {
            Expr* e = mk(EK::Is, left->span);
            e->a = left;
            e->pat = parseIsPattern();
            e->span = spanFrom(left->span);
            left = e;
        } else {
            Expr* e = mk(EK::Binary, opSpan);
            e->op = op;
            e->a = left;
            e->b = parseExpr(op == T::QQ ? prec : prec + 1);
            left = e;
        }
    }
    return left;
}

Expr* Parser::parseUnary() {
    Span start = cur().span;
    switch (cur().type) {
        case T::Minus: case T::Bang: case T::Tilde: case T::Plus: {
            T op = cur().type;
            p++;
            Expr* inner = parseUnary();
            if (op == T::Plus) return inner;
            if (op == T::Minus && (inner->kind == EK::IntLit || inner->kind == EK::FloatLit) && inner->span.offset == start.offset + 1) {
                inner->ival = -inner->ival;
                inner->fval = -inner->fval;
                inner->span = spanFrom(start);
                return inner;
            }
            Expr* e = mk(EK::Unary, start);
            e->op = op;
            e->a = inner;
            e->span = spanFrom(start);
            return e;
        }
        case T::PlusPlus: case T::MinusMinus: {
            T op = cur().type;
            p++;
            Expr* e = mk(EK::IncDec, start);
            e->op = op;
            e->prefix = true;
            e->a = parseUnary();
            e->span = spanFrom(start);
            return e;
        }
        case T::KwTry: {
            p++;
            Expr* e = mk(EK::Try, start);
            e->a = parseUnary();
            e->span = spanFrom(start);
            return e;
        }
        case T::KwRef: {
            p++;
            Expr* e = mk(EK::RefArg, start);
            e->a = parseUnary();
            e->span = spanFrom(start);
            return e;
        }
        case T::KwMut: {
            p++;
            expect(T::KwRef, "`ref` after `mut` (write `mut ref x`)");
            Expr* e = mk(EK::RefArg, start);
            e->isMut = true;
            e->a = parseUnary();
            e->span = spanFrom(start);
            return e;
        }
        case T::LParen: {
            // (int)x style cast: only with built-in type names, so it is never ambiguous
            if (isPrimKw(peekTok().type) && peekTok(2).type == T::RParen) {
                p++;
                Expr* e = mk(EK::Cast, start);
                e->texpr = parseType();
                expect(T::RParen, "`)`");
                e->a = parseUnary();
                e->span = spanFrom(start);
                return e;
            }
            break;
        }
        default: break;
    }
    return parsePostfix(parsePrimary());
}

Expr* Parser::parsePostfix(Expr* e) {
    while (true) {
        if (at(T::Dot)) {
            p++;
            Expr* m = mk(EK::Member, e->span);
            m->a = e;
            const Token& n = expect(T::Ident, "a field or method name after `.`");
            m->name = n.text;
            m->span = spanFrom(e->span);
            m->fieldSpans.push_back(n.span);
            e = m;
        } else if (at(T::LParen)) {
            p++;
            Expr* c = mk(EK::Call, e->span);
            c->a = e;
            if (!at(T::RParen)) {
                do c->args.push_back(parseExpr());
                while (accept(T::Comma));
            }
            expect(T::RParen, "`)` to close the call");
            c->span = spanFrom(e->span);
            e = c;
        } else if (at(T::LBracket)) {
            p++;
            Expr* ix = mk(EK::Index, e->span);
            ix->a = e;
            ix->b = parseExpr();
            expect(T::RBracket, "`]`");
            ix->span = spanFrom(e->span);
            e = ix;
        } else if (at(T::PlusPlus) || at(T::MinusMinus)) {
            Expr* x = mk(EK::IncDec, e->span);
            x->op = cur().type;
            x->a = e;
            p++;
            x->span = spanFrom(e->span);
            e = x;
        } else break;
    }
    return e;
}

Expr* Parser::parseInterp(const Token& tok) {
    Expr* e = mk(EK::Interp, tok.span);
    for (auto& part : tok.parts) {
        if (!part.isExpr) {
            if (part.text.empty()) continue;
            Expr* s = mk(EK::StrLit, tok.span);
            s->sval = part.text;
            e->args.push_back(s);
        } else {
            auto toks = lexFile(tok.span.file, part.start, part.end);
            Parser sub(std::move(toks), mod);
            try {
                if (sub.at(T::Eof)) {
                    g_diag.error(g_diag.makeSpan(tok.span.file, part.start - 1, 2), "empty `{}` in the string")
                        .help("put an expression inside, or write `{{` for a literal brace");
                    continue;
                }
                e->args.push_back(sub.parseStandaloneExpr());
            } catch (ParseError&) {
            }
        }
    }
    return e;
}

Expr* Parser::parseNew() {
    Span start = cur().span;
    p++;
    TypeExpr* base = parseBaseType();
    if (at(T::LBrace)) {
        p++;
        Expr* e = mk(EK::StructLit, start);
        e->texpr = base;
        while (!at(T::RBrace)) {
            const Token& n = expect(T::Ident, "a field name");
            e->fieldNames.push_back(n.text);
            e->fieldSpans.push_back(n.span);
            if (!at(T::Assign)) fail("expected `=` after the field name", "write `" + n.text + " = value`");
            p++;
            e->args.push_back(parseExpr());
            if (!accept(T::Comma)) break;
        }
        expect(T::RBrace, "`}` to close the struct value");
        e->span = spanFrom(start);
        return e;
    }
    if (at(T::LBracket)) {
        p++;
        Expr* e = mk(EK::NewArray, start);
        e->texpr = base;
        e->a = parseExpr();
        expect(T::RBracket, "`]`");
        e->span = spanFrom(start);
        return e;
    }
    if (at(T::LParen)) {
        p++;
        Expr* e = mk(EK::GenericNew, start);
        e->texpr = base;
        if (!at(T::RParen)) {
            do e->args.push_back(parseExpr());
            while (accept(T::Comma));
        }
        expect(T::RParen, "`)`");
        e->span = spanFrom(start);
        return e;
    }
    fail("expected `{`, `[` or `(` after `new " + base->name + "`",
         "build a struct with `new " + base->name + " { field = value }`");
}

Expr* Parser::parsePrimary() {
    const Token& tk = cur();
    Span sp = tk.span;
    switch (tk.type) {
        case T::IntLit: {
            Expr* e = mk(EK::IntLit, sp);
            e->ival = tk.ival;
            e->isLongLit = tk.isLong;
            p++;
            return e;
        }
        case T::FloatLit: {
            Expr* e = mk(EK::FloatLit, sp);
            e->fval = tk.fval;
            e->isF32 = tk.isF32;
            p++;
            return e;
        }
        case T::StrLit: {
            Expr* e = mk(EK::StrLit, sp);
            e->sval = tk.sval;
            p++;
            // "a" "b" adjacent literals are joined
            while (at(T::StrLit)) { e->sval += cur().sval; p++; }
            e->span = spanFrom(sp);
            return e;
        }
        case T::InterpStr: {
            Token copy = tk;
            p++;
            return parseInterp(copy);
        }
        case T::CharLit: {
            Expr* e = mk(EK::CharLit, sp);
            e->cval = tk.cval;
            p++;
            return e;
        }
        case T::KwTrue: case T::KwFalse: {
            Expr* e = mk(EK::BoolLit, sp);
            e->bval = tk.type == T::KwTrue;
            p++;
            return e;
        }
        case T::KwNone: p++; return mk(EK::None, sp);
        case T::KwNull:
            fail("Axon has no `null`", "use an optional type `T?` and the value `none`, e.g. `Player? p = none;`");
        case T::KwThis: p++; return mk(EK::This, sp);
        case T::KwNew: return parseNew();
        case T::LParen: {
            p++;
            Expr* e = parseExpr();
            expect(T::RParen, "`)`");
            return e;
        }
        case T::LBracket: {
            p++;
            Expr* e = mk(EK::ArrayLit, sp);
            if (!at(T::RBracket)) {
                do {
                    if (at(T::RBracket)) break;
                    e->args.push_back(parseExpr());
                } while (accept(T::Comma));
            }
            expect(T::RBracket, "`]` to close the array");
            e->span = spanFrom(sp);
            return e;
        }
        case T::Ident: {
            // Generic construction: List<int>()
            if (peekTok().type == T::Lt) {
                size_t i = p;
                int savedGt = pendingGt;
                bool isType = scanType(i);
                pendingGt = savedGt;
                if (isType && t[i].type == T::LParen) {
                    TypeExpr* te = parseBaseType();
                    p++;  // (
                    Expr* e = mk(EK::GenericNew, sp);
                    e->texpr = te;
                    if (!at(T::RParen)) {
                        do e->args.push_back(parseExpr());
                        while (accept(T::Comma));
                    }
                    expect(T::RParen, "`)`");
                    e->span = spanFrom(sp);
                    return e;
                }
            }
            Expr* e = mk(EK::Name, sp);
            e->name = tk.text;
            p++;
            return e;
        }
        case T::KwInt: case T::KwLong: case T::KwByte: case T::KwFloat: case T::KwDouble:
        case T::KwBool: case T::KwChar: case T::KwString:
            fail("expected a value, found the type " + tokDisplay(tk),
                 "to convert a value write a cast like `(int)x`");
        default:
            break;
    }
    fail("expected an expression, found " + tokDisplay(tk));
}
