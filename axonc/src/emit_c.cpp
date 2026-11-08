// emit_c.cpp - C code generation.
//
// Ownership model in the generated C:
//  * Owned values (string, arrays, lists, structs holding them) have ax_drop_X / ax_clone_X / ax_move_X helpers.
//  * Moving out of a local zeroes it (ax_move_X), so every owned local can be dropped unconditionally at scope end.
//  * Temporaries that are only borrowed are stored in `_tN` variables and dropped after the statement.
#include "emit_c.h"

#include <cstdio>
#include <functional>
#include <set>

#include "runtime.h"

namespace {

std::string cStrLit(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '\\': o += "\\\\"; break;
            case '"': o += "\\\""; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            case '?': o += "\\?"; break;
            default:
                if (c < 32 || c >= 127) {
                    char b[8];
                    snprintf(b, sizeof b, "\\%03o", c);
                    o += b;
                } else o += (char)c;
        }
    }
    return o + "\"";
}

std::string locStr(Span sp) {
    if (sp.file < 0) return "\"?\"";
    std::string path = g_diag.file(sp.file).display;
    for (auto& c : path) if (c == '\\') c = '/';
    return cStrLit(path + ":" + std::to_string(sp.line) + ":" + std::to_string(sp.col));
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string r;
    for (size_t i = 0; i < v.size(); i++) r += (i ? sep : "") + v[i];
    return r;
}

class Emitter {
public:
    Emitter(Program& p, const EmitOptions& o) : P(p), opt(o) {}
    std::string run();

private:
    Program& P;
    EmitOptions opt;
    std::string fwd, defs, ifaceDefs, protos, globals, vtables, helpers, bodies;
    std::set<Type*> defined, defining;
    std::string* out = nullptr;
    int ind = 0;
    int tmpN = 0;

    struct SCtx {
        std::vector<std::string> pre, post;
    };
    std::vector<SCtx*> ctx;
    struct EScope {
        int kind = 0;  // 0 block, 1 loop, 2 switch
        std::vector<std::string> drops;
        std::string label;  // continue label (loop) / end label (switch)
    };
    std::vector<EScope> scopes;
    FuncDecl* fn = nullptr;

    // ---- output helpers
    void line(const std::string& s) {
        if (!s.empty() && s[0] == '}') ind = ind > 0 ? ind - 1 : 0;
        out->append(std::string(ind * 4, ' ') + s + "\n");
        if (!s.empty() && s.back() == '{') ind++;
    }
    void pre(const std::string& s) {
        if (ctx.empty()) line(s);
        else ctx.back()->pre.push_back(s);
    }
    void post(const std::string& s) {
        if (ctx.empty()) line(s);
        else ctx.back()->post.push_back(s);
    }
    std::string newName(const char* base) { return std::string(base) + std::to_string(++tmpN); }
    void flushPre(SCtx& sc) { for (auto& l : sc.pre) line(l); }
    void flushPost(SCtx& sc) { for (auto it = sc.post.rbegin(); it != sc.post.rend(); ++it) line(*it); }

    // ---- types
    std::string ct(Type* t) {
        if (!t) return "void";
        if (t->k == TK::Struct && !t->sd->isExtern) return t->sd->cname;
        if (t->k == TK::Enum) return t->ed->cname;
        if (t->k == TK::Interface) return "ax_ref_" + t->id->cname;
        return t->cname;
    }
    std::string drop(Type* t, const std::string& ptr) { return "ax_drop_" + t->mangle + "(" + ptr + ");"; }
    std::string tagName(EnumDecl* ed, int v) { return ed->cname + "__" + ed->variants[v].name + "_TAG"; }
    std::string vfield(EnumDecl* ed, int v) { return "v_" + ed->variants[v].name; }
    bool isDisposable(Type* t) {
        if (t->k != TK::Struct || t->sd->isExtern) return false;
        for (auto* i : t->sd->ifaces) if (i == P.disposable) return true;
        return false;
    }
    std::string incKind(Type* t) {
        switch (t->k) {
            case TK::Int: return "i32";
            case TK::Long: return "i64";
            case TK::Byte: return "u8";
            case TK::Char: return "u32";
            case TK::Float: return "f32";
            default: return "f64";
        }
    }

    void emitTypeDef(Type* t);
    void emitHelpers(Type* t);
    std::set<Type*> valueHelpersDone;
    std::string valueHelpers(Type* e);
    void emitSeqHelpers(Type* t);
    void emitMapHelpers(Type* t);
    std::string writeAddr(Expr* e);
    std::string boxPtr(Expr* box);
    void emitIfaces();
    void emitFunc(FuncDecl* f);
    std::string sig(FuncDecl* f, const std::string& name);

    // ---- temporaries
    std::string tmpVar(Type* t, const std::string& init) {
        std::string n = newName("_t");
        pre(ct(t) + " " + n + " = " + init + ";");
        return n;
    }
    std::string tmpZero(Type* t) {
        std::string n = newName("_t");
        pre(ct(t) + " " + n + ";");
        pre("memset(&" + n + ", 0, sizeof " + n + ");");
        return n;
    }
    std::string ownTmp(Type* t, const std::string& init) {
        std::string n = tmpVar(t, init);
        post(drop(t, "&" + n));
        return n;
    }
    std::string ptrTmp(Type* t, const std::string& addrExpr) {
        std::string n = newName("_p");
        pre(ct(t) + "* " + n + " = " + addrExpr + ";");
        return n;
    }

    // ---- expressions
    bool needsDrop(Expr* e);
    bool isPtrVar(VarDecl* v) { return v && v->mode != 0 && v->type && v->type->k != TK::Interface; }
    std::string raw(Expr* e);
    std::string val(Expr* e);
    std::string view(Expr* e);
    std::string ownedCopy(Expr* e);
    std::string addr(Expr* e);
    std::string emitCall(Expr* e);
    std::string callArgs(Expr* e, FuncDecl* f, const std::string& first);
    std::string emitBinary(Expr* e);
    std::string arith(T op, Type* R, const std::string& a, const std::string& b, Span sp);
    std::string emitConcat(const std::vector<Expr*>& parts);
    void appendPart(const std::string& sb, Expr* p);
    std::string emitCoalesce(Expr* e);
    std::string emitTernary(Expr* e);
    std::string emitTry(Expr* e);
    std::string patternTest(Pattern* pt, Type* subj, const std::string& s, bool assign);
    std::vector<std::string> patternBindDecls(Pattern* pt, Type* subj, const std::string& s);
    std::string subCtx(SCtx& sc, const std::function<std::string()>& f) {
        ctx.push_back(&sc);
        std::string r = f();
        ctx.pop_back();
        return r;
    }
    void earlyReturnDrops(std::vector<std::string>& lines);

    // ---- statements
    void pushScope(int kind = 0, const std::string& label = "") {
        EScope s;
        s.kind = kind;
        s.label = label;
        scopes.push_back(s);
    }
    bool terminated = false;  // the last emitted statement was return/break/continue
    bool lastBlockTerminated = false;
    void popScope() {
        auto& s = scopes.back();
        if (!terminated)
            for (auto it = s.drops.rbegin(); it != s.drops.rend(); ++it) line(*it);
        scopes.pop_back();
    }
    void addOwned(VarDecl* v) {
        if (v->mode == 0 && v->type && isOwned(v->type)) scopes.back().drops.push_back(drop(v->type, "&" + v->cname));
    }
    void emitScopeDrops(size_t from) {
        for (size_t i = scopes.size(); i-- > from;)
            for (auto it = scopes[i].drops.rbegin(); it != scopes[i].drops.rend(); ++it) line(*it);
    }
    void emitBlockInner(Stmt* b);
    void emitStmt(Stmt* s);
    void emitStmtInner(Stmt* s);
    void emitAssign(Expr* e);
    void declPatVars(const std::vector<VarDecl*>& vs);
};

// ======================================================================== types

void Emitter::emitTypeDef(Type* t) {
    if (defined.count(t)) return;
    switch (t->k) {
        case TK::Struct:
            if (t->sd->isExtern) return;
            break;
        case TK::Enum: case TK::Optional: case TK::Result: case TK::Array: case TK::List: case TK::Map: break;
        default: return;  // Box is a pointer typedef emitted with the forward declarations
    }
    if (defining.count(t)) return;  // recursion is rejected by the checker
    defining.insert(t);
    std::string name = ct(t);
    std::string body;
    switch (t->k) {
        case TK::Struct: {
            for (auto* f : t->sd->fields) emitTypeDef(f->rtype);
            body = "struct " + name + " {\n";
            for (auto* f : t->sd->fields) body += "    " + ct(f->rtype) + " " + f->cname + ";\n";
            if (isDisposable(t)) body += "    uint8_t ax_live;\n";
            if (t->sd->fields.empty() && !isDisposable(t)) body += "    char ax_empty;\n";
            body += "};\n";
            break;
        }
        case TK::Enum: {
            EnumDecl* ed = t->ed;
            for (auto& v : ed->variants)
                for (auto* f : v.fields) emitTypeDef(f->rtype);
            for (size_t i = 0; i < ed->variants.size(); i++) body += "#define " + tagName(ed, (int)i) + " " + std::to_string(i) + "\n";
            body += "struct " + name + " {\n    int32_t tag;\n    union {\n";
            bool any = false;
            for (size_t i = 0; i < ed->variants.size(); i++) {
                auto& v = ed->variants[i];
                if (v.fields.empty()) continue;
                any = true;
                body += "        struct {";
                for (auto* f : v.fields) body += " " + ct(f->rtype) + " " + f->cname + ";";
                body += " } " + vfield(ed, (int)i) + ";\n";
            }
            if (!any) body += "        char ax_none;\n";
            body += "    } u;\n};\n";
            break;
        }
        case TK::Optional:
            emitTypeDef(t->a);
            body = "struct " + name + " { bool has; " + ct(t->a) + " val; };\n";
            break;
        case TK::Result:
            emitTypeDef(t->a);
            emitTypeDef(t->b);
            body = "struct " + name + " { int32_t tag; union { " + ct(t->a) + " ok; " + ct(t->b) + " err; } u; };\n";
            break;
        case TK::Array:
            body = "struct " + name + " { " + ct(t->a) + "* data; int64_t len; };\n";
            break;
        case TK::List:
            body = "struct " + name + " { " + ct(t->a) + "* data; int64_t len; int64_t cap; };\n";
            break;
        case TK::Map:
            // entries are kept in insertion order; `idx` is the open-addressing hash index into them
            body = "struct " + name + " { " + ct(t->a) + "* keys; " + ct(t->b) +
                   "* vals; int64_t len; int64_t ecap; int64_t* idx; int64_t cap; int64_t used; };\n";
            break;
        default: break;
    }
    defs += body;
    defining.erase(t);
    defined.insert(t);
}

void Emitter::emitHelpers(Type* t) {
    auto proto = [&](const std::string& s) { protos += s + ";\n"; };
    auto def = [&](const std::string& sigText, const std::string& bodyText) {
        proto(sigText);
        helpers += sigText + " {\n" + bodyText + "}\n";
    };
    std::string C = ct(t), M = t->mangle;
    bool owned = isOwned(t);
    switch (t->k) {
        case TK::Struct: {
            if (t->sd->isExtern || !owned) return;
            std::string d, c;
            if (isDisposable(t)) {
                FuncDecl* dispose = findMethod(P, t, "dispose");
                if (dispose) d += "    if (p->ax_live) { " + dispose->cname + "(p); }\n";
            }
            c += "    " + C + " r = *p;\n";
            for (auto* f : t->sd->fields)
                if (isOwned(f->rtype)) {
                    d += "    " + drop(f->rtype, "&p->" + f->cname) + "\n";
                    c += "    r." + f->cname + " = ax_clone_" + f->rtype->mangle + "(&p->" + f->cname + ");\n";
                }
            c += "    return r;\n";
            def("static void ax_drop_" + M + "(" + C + "* p)", d);
            def("static " + C + " ax_clone_" + M + "(" + C + "* p)", c);
            break;
        }
        case TK::Enum: {
            EnumDecl* ed = t->ed;
            for (size_t i = 0; i < ed->variants.size(); i++) {
                auto& v = ed->variants[i];
                std::vector<std::string> ps;
                // payload names come from the user, so the result variable uses a reserved name
                std::string b = "    " + C + " ax_r;\n    memset(&ax_r, 0, sizeof ax_r);\n    ax_r.tag = " + tagName(ed, (int)i) + ";\n";
                for (auto* f : v.fields) {
                    ps.push_back(ct(f->rtype) + " " + f->cname);
                    b += "    ax_r.u." + vfield(ed, (int)i) + "." + f->cname + " = " + f->cname + ";\n";
                }
                b += "    return ax_r;\n";
                def("static " + C + " " + ed->cname + "__" + v.name + "(" + (ps.empty() ? "void" : join(ps, ", ")) + ")", b);
            }
            if (!owned) return;
            std::string d = "    switch (p->tag) {\n", c = "    " + C + " r = *p;\n    switch (p->tag) {\n";
            for (size_t i = 0; i < ed->variants.size(); i++) {
                std::string dd, cc;
                for (auto* f : ed->variants[i].fields)
                    if (isOwned(f->rtype)) {
                        std::string fld = "u." + vfield(ed, (int)i) + "." + f->cname;
                        dd += " " + drop(f->rtype, "&p->" + fld);
                        cc += " r." + fld + " = ax_clone_" + f->rtype->mangle + "(&p->" + fld + ");";
                    }
                if (!dd.empty()) {
                    d += "        case " + tagName(ed, (int)i) + ":" + dd + " break;\n";
                    c += "        case " + tagName(ed, (int)i) + ":" + cc + " break;\n";
                }
            }
            d += "        default: break;\n    }\n";
            c += "        default: break;\n    }\n    return r;\n";
            def("static void ax_drop_" + M + "(" + C + "* p)", d);
            def("static " + C + " ax_clone_" + M + "(" + C + "* p)", c);
            break;
        }
        case TK::Optional: {
            std::string A = ct(t->a), AM = t->a->mangle;
            def("static " + C + " ax_some_" + AM + "(" + A + " v)", "    " + C + " r;\n    r.has = true;\n    r.val = v;\n    return r;\n");
            def("static " + C + " ax_none_" + AM + "(void)", "    " + C + " r;\n    memset(&r, 0, sizeof r);\n    return r;\n");
            if (!owned) return;
            def("static void ax_drop_" + M + "(" + C + "* p)", "    if (p->has) { " + drop(t->a, "&p->val") + " }\n    p->has = false;\n");
            def("static " + C + " ax_clone_" + M + "(" + C + "* p)",
                "    " + C + " r = *p;\n    if (p->has) r.val = ax_clone_" + AM + "(&p->val);\n    return r;\n");
            break;
        }
        case TK::Result: {
            def("static " + C + " ax_ok_" + M + "(" + ct(t->a) + " v)", "    " + C + " r;\n    memset(&r, 0, sizeof r);\n    r.tag = 0;\n    r.u.ok = v;\n    return r;\n");
            def("static " + C + " ax_err_" + M + "(" + ct(t->b) + " e)", "    " + C + " r;\n    memset(&r, 0, sizeof r);\n    r.tag = 1;\n    r.u.err = e;\n    return r;\n");
            if (!owned) return;
            std::string d, c = "    " + C + " r = *p;\n";
            if (isOwned(t->a)) { d += "    if (p->tag == 0) { " + drop(t->a, "&p->u.ok") + " }\n"; c += "    if (p->tag == 0) r.u.ok = ax_clone_" + t->a->mangle + "(&p->u.ok);\n"; }
            if (isOwned(t->b)) { d += "    if (p->tag == 1) { " + drop(t->b, "&p->u.err") + " }\n"; c += "    if (p->tag == 1) r.u.err = ax_clone_" + t->b->mangle + "(&p->u.err);\n"; }
            c += "    return r;\n";
            def("static void ax_drop_" + M + "(" + C + "* p)", d);
            def("static " + C + " ax_clone_" + M + "(" + C + "* p)", c);
            break;
        }
        case TK::Array: {
            std::string E = ct(t->a), EM = t->a->mangle;
            def("static " + C + " ax_arr_new_" + EM + "(int64_t n, const char* loc)",
                "    if (n < 0) ax_panicf(loc, \"array size can't be negative (%lld)\", (long long)n);\n"
                "    " + C + " r;\n    r.len = n;\n    r.data = n ? (" + E + "*)ax_calloc((size_t)n, sizeof(" + E + ")) : NULL;\n"
                "    if (n && !r.data) ax_panic_at(\"out of memory\", loc);\n    return r;\n");
            def("static " + E + "* ax_arr_at_" + EM + "(" + C + "* a, int64_t i, const char* loc)",
                "    ax_bounds(i, a->len, loc);\n    return &a->data[i];\n");
            std::string d, c;
            if (isOwned(t->a)) d += "    for (int64_t i = 0; i < p->len; i++) " + drop(t->a, "&p->data[i]") + "\n";
            d += "    ax_free(p->data);\n    p->data = NULL;\n    p->len = 0;\n";
            c += "    " + C + " r;\n    r.len = p->len;\n    r.data = p->len ? (" + E + "*)ax_alloc((size_t)p->len * sizeof(" + E + ")) : NULL;\n";
            if (isOwned(t->a)) c += "    for (int64_t i = 0; i < p->len; i++) r.data[i] = ax_clone_" + EM + "(&p->data[i]);\n";
            else c += "    if (p->len) memcpy(r.data, p->data, (size_t)p->len * sizeof(" + E + "));\n";
            c += "    return r;\n";
            def("static void ax_drop_" + M + "(" + C + "* p)", d);
            def("static " + C + " ax_clone_" + M + "(" + C + "* p)", c);
            break;
        }
        case TK::List: {
            std::string E = ct(t->a), EM = t->a->mangle;
            def("static " + C + " ax_list_new_" + EM + "(void)", "    " + C + " r;\n    r.data = NULL;\n    r.len = 0;\n    r.cap = 0;\n    return r;\n");
            def("static " + E + "* ax_list_at_" + EM + "(" + C + "* l, int64_t i, const char* loc)",
                "    ax_bounds(i, l->len, loc);\n    return &l->data[i];\n");
            def("static void ax_list_grow_" + EM + "(" + C + "* l)",
                "    if (l->len < l->cap) return;\n    l->cap = l->cap ? l->cap * 2 : 4;\n"
                "    l->data = (" + E + "*)ax_realloc(l->data, (size_t)l->cap * sizeof(" + E + "));\n");
            def("static void ax_list_add_" + EM + "(" + C + "* l, " + E + " v)",
                "    ax_list_grow_" + EM + "(l);\n    l->data[l->len++] = v;\n");
            def("static " + E + " ax_list_pop_" + EM + "(" + C + "* l, const char* loc)",
                "    if (l->len == 0) ax_panic_at(\"pop() on an empty list\", loc);\n    return l->data[--l->len];\n");
            def("static " + E + " ax_list_removeAt_" + EM + "(" + C + "* l, int32_t i, const char* loc)",
                "    if (i < 0 || i >= l->len) ax_panicf(loc, \"removeAt(%d) is out of range (length is %lld)\", i, (long long)l->len);\n"
                "    " + E + " v = l->data[i];\n    memmove(&l->data[i], &l->data[i + 1], (size_t)(l->len - i - 1) * sizeof(" + E + "));\n"
                "    l->len--;\n    return v;\n");
            def("static void ax_list_insert_" + EM + "(" + C + "* l, int32_t i, " + E + " v, const char* loc)",
                "    if (i < 0 || i > l->len) ax_panicf(loc, \"insert(%d) is out of range (length is %lld)\", i, (long long)l->len);\n"
                "    ax_list_grow_" + EM + "(l);\n    memmove(&l->data[i + 1], &l->data[i], (size_t)(l->len - i) * sizeof(" + E + "));\n"
                "    l->data[i] = v;\n    l->len++;\n");
            std::string clr;
            if (isOwned(t->a)) clr += "    for (int64_t i = 0; i < l->len; i++) " + drop(t->a, "&l->data[i]") + "\n";
            clr += "    l->len = 0;\n";
            def("static void ax_list_clear_" + EM + "(" + C + "* l)", clr);
            std::string d, c;
            d = "    ax_list_clear_" + EM + "(p);\n    ax_free(p->data);\n    p->data = NULL;\n    p->cap = 0;\n";
            c += "    " + C + " r;\n    r.len = p->len;\n    r.cap = p->len;\n    r.data = p->len ? (" + E + "*)ax_alloc((size_t)p->len * sizeof(" + E + ")) : NULL;\n";
            if (isOwned(t->a)) c += "    for (int64_t i = 0; i < p->len; i++) r.data[i] = ax_clone_" + EM + "(&p->data[i]);\n";
            else c += "    if (p->len) memcpy(r.data, p->data, (size_t)p->len * sizeof(" + E + "));\n";
            c += "    return r;\n";
            def("static void ax_drop_" + M + "(" + C + "* p)", d);
            def("static " + C + " ax_clone_" + M + "(" + C + "* p)", c);
            break;
        }
        case TK::Box: {
            Type* in = t->a;
            if (in->k == TK::Interface) {
                // Box<Iface>: an owned value of some type that implements the interface
                def("static void ax_drop_" + M + "(" + C + "* p)",
                    "    if (p->self) { p->vt->ax_drop(p->self); ax_free(p->self); }\n    p->self = NULL;\n    p->vt = NULL;\n");
                def("static " + C + " ax_clone_" + M + "(" + C + "* p)",
                    "    " + C + " r;\n    r.vt = p->vt;\n    r.self = p->self ? p->vt->ax_clone(p->self) : NULL;\n    return r;\n");
                def("static " + C + " ax_dyn_check_" + M + "(" + C + " b, const char* loc)",
                    "    if (!b.self) ax_panic_at(\"this box is empty (its value was moved out)\", loc);\n    return b;\n");
                break;
            }
            std::string I = ct(in), IM = in->mangle;
            def("static " + C + " ax_box_new_" + IM + "(" + I + " v)", "    " + I + "* p = (" + I + "*)ax_alloc(sizeof(" + I + "));\n    *p = v;\n    return p;\n");
            def("static " + I + "* ax_box_deref_" + IM + "(" + C + " b, const char* loc)",
                "    if (!b) ax_panic_at(\"this box is empty (its value was moved out)\", loc);\n    return b;\n");
            def("static void ax_drop_" + M + "(" + C + "* p)",
                "    if (*p) { " + (isOwned(in) ? drop(in, "*p") + " " : std::string()) + "ax_free(*p); }\n    *p = NULL;\n");
            def("static " + C + " ax_clone_" + M + "(" + C + "* p)",
                "    if (!*p) return NULL;\n    " + I + "* n = (" + I + "*)ax_alloc(sizeof(" + I + "));\n    *n = " +
                    (isOwned(in) ? "ax_clone_" + IM + "(*p)" : "**p") + ";\n    return n;\n");
            break;
        }
        case TK::Map: emitMapHelpers(t); break;
        default: return;
    }
    if (t->k == TK::Array || t->k == TK::List) emitSeqHelpers(t);
    // move: copy out and leave zeroes behind, so the later drop is a no-op
    def("static " + C + " ax_move_" + M + "(" + C + "* p)", "    " + C + " r = *p;\n    memset(p, 0, sizeof *p);\n    return r;\n");
}

// equality / ordering / hashing of a value type, used by Map keys and list search/sort
std::string Emitter::valueHelpers(Type* e) {
    std::string M = e->mangle, C = ct(e);
    if (valueHelpersDone.count(e)) return M;
    valueHelpersDone.insert(e);
    auto def = [&](const std::string& sigText, const std::string& bodyText) {
        protos += sigText + ";\n";
        helpers += sigText + " {\n" + bodyText + "}\n";
    };
    if (e->k == TK::String) {
        def("static bool ax_eq_string(ax_string a, ax_string b)", "    return ax_str_eq(a, b);\n");
        def("static int ax_cmp_string(const void* x, const void* y)",
            "    const ax_string* a = (const ax_string*)x;\n    const ax_string* b = (const ax_string*)y;\n"
            "    int64_t n = a->len < b->len ? a->len : b->len;\n    int c = n ? memcmp(a->ptr, b->ptr, (size_t)n) : 0;\n"
            "    if (c) return c;\n    return (a->len > b->len) - (a->len < b->len);\n");
        def("static uint64_t ax_hash_string(ax_string s)",
            "    uint64_t h = 1469598103934665603ull;\n    for (int64_t i = 0; i < s.len; i++) { h ^= (unsigned char)s.ptr[i]; h *= 1099511628211ull; }\n    return ax_hash_u64(h);\n");
        def("static ax_string ax_kclone_string(ax_string s)", "    return ax_str_new(s.ptr, s.len);\n");
    } else if (e->k == TK::Enum) {
        def("static bool ax_eq_" + M + "(" + C + " a, " + C + " b)", "    return a.tag == b.tag;\n");
    } else {
        def("static bool ax_eq_" + M + "(" + C + " a, " + C + " b)", "    return a == b;\n");
        def("static int ax_cmp_" + M + "(const void* x, const void* y)",
            "    " + C + " a = *(const " + C + "*)x;\n    " + C + " b = *(const " + C + "*)y;\n    return (a > b) - (a < b);\n");
        def("static uint64_t ax_hash_" + M + "(" + C + " k)", "    return ax_hash_u64((uint64_t)k);\n");
        def("static " + C + " ax_kclone_" + M + "(" + C + " k)", "    return k;\n");
    }
    return M;
}

void Emitter::emitSeqHelpers(Type* t) {
    Type* e = t->a;
    bool equatable = isNumeric(e) || e->k == TK::String || e->k == TK::Char || e->k == TK::Bool || e->k == TK::Enum;
    if (e->k == TK::Enum)
        for (auto& v : e->ed->variants) equatable &= v.fields.empty();
    if (!equatable) return;
    std::string C = ct(t), E = ct(e), EM = valueHelpers(e);
    std::string key = (t->k == TK::List ? "list_" : "arr_") + EM;
    protos += "static int32_t ax_" + key + "_indexOf(" + C + "* s, " + E + " x);\n";
    helpers += "static int32_t ax_" + key + "_indexOf(" + C + "* s, " + E + " x) {\n"
               "    for (int64_t i = 0; i < s->len; i++) if (ax_eq_" + EM + "(s->data[i], x)) return (int32_t)i;\n    return -1;\n}\n";
    if (e->k != TK::Bool && e->k != TK::Enum) {
        protos += "static void ax_" + key + "_sort(" + C + "* s);\n";
        helpers += "static void ax_" + key + "_sort(" + C + "* s) {\n    if (s->len > 1) qsort(s->data, (size_t)s->len, sizeof(" + E +
                   "), ax_cmp_" + EM + ");\n}\n";
    }
}

void Emitter::emitMapHelpers(Type* t) {
    auto def = [&](const std::string& sigText, const std::string& bodyText) {
        protos += sigText + ";\n";
        helpers += sigText + " {\n" + bodyText + "}\n";
    };
    Type* K = t->a;
    Type* V = t->b;
    std::string C = ct(t), M = t->mangle, KC = ct(K), VC = ct(V), KM = valueHelpers(K), VM = V->mangle;
    std::string OV = ct(g_types.optOf(V));
    bool ownK = isOwned(K), ownV = isOwned(V);
    std::string hash = "ax_hash_" + KM, eq = "ax_eq_" + KM;
    def("static " + C + " ax_map_new_" + M + "(void)", "    " + C + " m;\n    memset(&m, 0, sizeof m);\n    return m;\n");
    // entry index of a key, or -1
    def("static int64_t ax_map_find_" + M + "(" + C + "* m, " + KC + " k)",
        "    if (!m->cap) return -1;\n    uint64_t mask = (uint64_t)m->cap - 1;\n    uint64_t h = " + hash + "(k) & mask;\n"
        "    for (;;) {\n        int64_t e = m->idx[h];\n        if (e == -1) return -1;\n"
        "        if (e >= 0 && " + eq + "(m->keys[e], k)) return e;\n        h = (h + 1) & mask;\n    }\n");
    // hash slot that points at entry e
    def("static uint64_t ax_map_slotof_" + M + "(" + C + "* m, int64_t e)",
        "    uint64_t mask = (uint64_t)m->cap - 1;\n    uint64_t h = " + hash + "(m->keys[e]) & mask;\n"
        "    while (m->idx[h] != e) h = (h + 1) & mask;\n    return h;\n");
    def("static void ax_map_rehash_" + M + "(" + C + "* m, int64_t ncap)",
        "    ax_free(m->idx);\n    m->idx = (int64_t*)ax_alloc((size_t)ncap * sizeof(int64_t));\n"
        "    for (int64_t i = 0; i < ncap; i++) m->idx[i] = -1;\n    m->cap = ncap;\n    m->used = m->len;\n"
        "    uint64_t mask = (uint64_t)ncap - 1;\n"
        "    for (int64_t e = 0; e < m->len; e++) {\n        uint64_t h = " + hash + "(m->keys[e]) & mask;\n"
        "        while (m->idx[h] != -1) h = (h + 1) & mask;\n        m->idx[h] = e;\n    }\n");
    // pointer to the value for k, adding a zeroed entry if it is missing (used by `m[k] = v`)
    def("static " + VC + "* ax_map_slot_" + M + "(" + C + "* m, " + KC + " k)",
        "    int64_t e = ax_map_find_" + M + "(m, k);\n    if (e >= 0) return &m->vals[e];\n"
        "    if ((m->used + 1) * 10 >= m->cap * 7) {\n        int64_t ncap = m->cap ? m->cap : 8;\n"
        "        while ((m->len + 1) * 10 >= ncap * 7) ncap *= 2;\n        ax_map_rehash_" + M + "(m, ncap);\n    }\n"
        "    if (m->len == m->ecap) {\n        m->ecap = m->ecap ? m->ecap * 2 : 8;\n"
        "        m->keys = (" + KC + "*)ax_realloc(m->keys, (size_t)m->ecap * sizeof(" + KC + "));\n"
        "        m->vals = (" + VC + "*)ax_realloc(m->vals, (size_t)m->ecap * sizeof(" + VC + "));\n    }\n"
        "    e = m->len++;\n    m->keys[e] = ax_kclone_" + KM + "(k);\n    memset(&m->vals[e], 0, sizeof(" + VC + "));\n"
        "    uint64_t mask = (uint64_t)m->cap - 1;\n    uint64_t h = " + hash + "(k) & mask;\n"
        "    while (m->idx[h] >= 0) h = (h + 1) & mask;\n    if (m->idx[h] == -1) m->used++;\n    m->idx[h] = e;\n"
        "    return &m->vals[e];\n");
    def("static " + VC + "* ax_map_at_" + M + "(" + C + "* m, " + KC + " k, const char* loc)",
        "    int64_t e = ax_map_find_" + M + "(m, k);\n    if (e < 0) ax_panic_at(\"key not found in map (use .get(key) or .contains(key) to check first)\", loc);\n"
        "    return &m->vals[e];\n");
    def("static bool ax_map_contains_" + M + "(" + C + "* m, " + KC + " k)", "    return ax_map_find_" + M + "(m, k) >= 0;\n");
    def("static " + OV + " ax_map_get_" + M + "(" + C + "* m, " + KC + " k)",
        "    int64_t e = ax_map_find_" + M + "(m, k);\n    if (e < 0) return ax_none_" + VM + "();\n"
        "    return ax_some_" + VM + "(" + (ownV ? "ax_clone_" + VM + "(&m->vals[e])" : "m->vals[e]") + ");\n");
    def("static " + OV + " ax_map_remove_" + M + "(" + C + "* m, " + KC + " k)",
        "    int64_t e = ax_map_find_" + M + "(m, k);\n    if (e < 0) return ax_none_" + VM + "();\n"
        "    m->idx[ax_map_slotof_" + M + "(m, e)] = -2;\n" + (ownK ? "    " + drop(K, "&m->keys[e]") + "\n" : std::string()) +
        "    " + VC + " v = m->vals[e];\n    int64_t last = m->len - 1;\n"
        "    if (e != last) {\n        uint64_t s = ax_map_slotof_" + M + "(m, last);\n        m->keys[e] = m->keys[last];\n"
        "        m->vals[e] = m->vals[last];\n        m->idx[s] = e;\n    }\n    m->len--;\n    return ax_some_" + VM + "(v);\n");
    std::string AK = ct(g_types.arrayOf(K)), AV = ct(g_types.arrayOf(V));
    def("static " + AK + " ax_map_keys_" + M + "(" + C + "* m)",
        "    " + AK + " r;\n    r.len = m->len;\n    r.data = m->len ? (" + KC + "*)ax_alloc((size_t)m->len * sizeof(" + KC + ")) : NULL;\n"
        "    for (int64_t i = 0; i < m->len; i++) r.data[i] = " + (ownK ? "ax_clone_" + K->mangle + "(&m->keys[i])" : "m->keys[i]") + ";\n    return r;\n");
    def("static " + AV + " ax_map_values_" + M + "(" + C + "* m)",
        "    " + AV + " r;\n    r.len = m->len;\n    r.data = m->len ? (" + VC + "*)ax_alloc((size_t)m->len * sizeof(" + VC + ")) : NULL;\n"
        "    for (int64_t i = 0; i < m->len; i++) r.data[i] = " + (ownV ? "ax_clone_" + VM + "(&m->vals[i])" : "m->vals[i]") + ";\n    return r;\n");
    std::string clr;
    if (ownK || ownV) {
        clr += "    for (int64_t i = 0; i < m->len; i++) {";
        if (ownK) clr += " " + drop(K, "&m->keys[i]");
        if (ownV) clr += " " + drop(V, "&m->vals[i]");
        clr += " }\n";
    }
    clr += "    m->len = 0;\n    m->used = 0;\n    for (int64_t i = 0; i < m->cap; i++) m->idx[i] = -1;\n";
    def("static void ax_map_clear_" + M + "(" + C + "* m)", clr);
    def("static void ax_drop_" + M + "(" + C + "* p)",
        "    ax_map_clear_" + M + "(p);\n    ax_free(p->keys);\n    ax_free(p->vals);\n    ax_free(p->idx);\n    memset(p, 0, sizeof *p);\n");
    def("static " + C + " ax_clone_" + M + "(" + C + "* p)",
        "    " + C + " r = ax_map_new_" + M + "();\n    for (int64_t i = 0; i < p->len; i++) *ax_map_slot_" + M + "(&r, p->keys[i]) = " +
            (ownV ? "ax_clone_" + VM + "(&p->vals[i])" : "p->vals[i]") + ";\n    return r;\n");
}

std::string Emitter::sig(FuncDecl* f, const std::string& name) {
    std::vector<std::string> ps;
    if (f->selfType && !f->ownerIface) ps.push_back(ct(f->selfType) + "* self");
    for (auto& p : f->params) {
        Type* t = p.var->type;
        if (p.mode == 0 || t->k == TK::Interface) ps.push_back(ct(t) + " " + p.var->cname);
        else ps.push_back(ct(t) + "* " + p.var->cname);
    }
    return ct(f->retType) + " " + name + "(" + (ps.empty() ? "void" : join(ps, ", ")) + ")";
}

void Emitter::emitIfaces() {
    for (auto* i : P.ifaces) {
        std::string vt = i->cname + "_vt";
        fwd += "typedef struct " + vt + " " + vt + ";\n";
        fwd += "typedef struct ax_ref_" + i->cname + " { void* self; const " + vt + "* vt; } ax_ref_" + i->cname + ";\n";
        std::string b = "struct " + vt + " {\n";
        for (auto* m : i->methods) {
            std::vector<std::string> ps = {"void* self"};
            for (auto& p : m->params) {
                Type* t = p.var->type;
                ps.push_back(p.mode == 0 || t->k == TK::Interface ? ct(t) + " " + p.var->cname : ct(t) + "* " + p.var->cname);
            }
            b += "    " + ct(m->retType) + " (*" + m->name + ")(" + join(ps, ", ") + ");\n";
        }
        b += "    void (*ax_drop)(void* self);\n    void* (*ax_clone)(void* self);\n";  // used by Box<Iface>
        b += "};\n";
        ifaceDefs += b;
    }
    for (auto& im : P.impls) {
        std::string base = im.type->mangle + "__" + im.iface->cname;
        std::vector<std::string> fns;
        std::string T = ct(im.type);
        {
            std::string dropT = "static void " + base + "__ax_drop(void* self)";
            std::string cloneT = "static void* " + base + "__ax_clone(void* self)";
            protos += dropT + ";\n" + cloneT + ";\n";
            helpers += dropT + " { " + (isOwned(im.type) ? drop(im.type, "(" + T + "*)self") : std::string("(void)self;")) + " }\n";
            helpers += cloneT + " { " + T + "* n = (" + T + "*)ax_alloc(sizeof(" + T + ")); *n = " +
                       (isOwned(im.type) ? "ax_clone_" + im.type->mangle + "((" + T + "*)self)" : "*(" + T + "*)self") + "; return n; }\n";
        }
        for (size_t k = 0; k < im.methods.size(); k++) {
            FuncDecl* m = im.iface->methods[k];
            FuncDecl* f = im.methods[k];
            std::string thunk = base + "__" + m->name;
            std::vector<std::string> ps = {"void* self"}, as = {"(" + ct(im.type) + "*)self"};
            for (auto& p : m->params) {
                Type* t = p.var->type;
                ps.push_back(p.mode == 0 || t->k == TK::Interface ? ct(t) + " " + p.var->cname : ct(t) + "* " + p.var->cname);
                as.push_back(p.var->cname);
            }
            std::string s = "static " + ct(m->retType) + " " + thunk + "(" + join(ps, ", ") + ")";
            protos += s + ";\n";
            helpers += s + " { " + (m->retType->k == TK::Void ? "" : "return ") + f->cname + "(" + join(as, ", ") + "); }\n";
            fns.push_back(thunk);
        }
        fns.push_back(base + "__ax_drop");
        fns.push_back(base + "__ax_clone");
        vtables += "static const " + im.iface->cname + "_vt " + base + "_vt = { " + join(fns, ", ") + " };\n";
    }
}

// ======================================================================== expressions

bool Emitter::needsDrop(Expr* e) {
    if (!e->type || isCopy(e->type)) return false;
    switch (e->kind) {
        case EK::Name: case EK::This: case EK::Index: case EK::StrLit: case EK::BoxDeref: return false;
        case EK::Implicit: return e->implicit == ImplicitKind::BoxToDyn && needsDrop(e->a);
        case EK::Member: return e->field < 0;
        default: return true;
    }
}

// Address for writing: `m[key] = v` adds the key, while reading `m[key]` requires it.
std::string Emitter::writeAddr(Expr* e) {
    if (e->kind == EK::Index && e->a->type->k == TK::Map) {
        std::string m = addr(e->a);
        std::string k = e->b->type->k == TK::String ? view(e->b) : raw(e->b);
        return "ax_map_slot_" + e->a->type->mangle + "(" + m + ", " + k + ")";
    }
    return addr(e);
}

std::string Emitter::val(Expr* e) {
    if (e->kind == EK::Implicit) {
        if (e->implicit == ImplicitKind::WrapOptional) return "ax_some_" + e->a->type->mangle + "(" + val(e->a) + ")";
        return raw(e);
    }
    if (!e->type || isCopy(e->type)) return raw(e);
    switch (e->kind) {
        case EK::Name:
            if (e->var && e->var->kind == VarDecl::Global) return "ax_clone_" + e->type->mangle + "(&" + e->var->cname + ")";
            if (e->var && e->var->mode == 0 && e->var->kind != VarDecl::ForeachBind)
                return "ax_move_" + e->type->mangle + "(&" + e->var->cname + ")";
            return "ax_clone_" + e->type->mangle + "(" + addr(e) + ")";
        case EK::This:
        case EK::Index:
        case EK::BoxDeref:
            return "ax_clone_" + e->type->mangle + "(" + addr(e) + ")";
        case EK::Member:
            if (e->field >= 0) return "ax_clone_" + e->type->mangle + "(" + addr(e) + ")";
            return raw(e);
        default: return raw(e);
    }
}

// The C expression for a box (a pointer), without taking it from its owner.
std::string Emitter::boxPtr(Expr* box) { return needsDrop(box) ? view(box) : raw(box); }

std::string Emitter::view(Expr* e) {
    if (e->kind == EK::Implicit) {
        if (e->implicit == ImplicitKind::WrapOptional) return "ax_some_" + e->a->type->mangle + "(" + view(e->a) + ")";
        return raw(e);
    }
    if (!e->type || isCopy(e->type) || !needsDrop(e)) return raw(e);
    return ownTmp(e->type, raw(e));
}

std::string Emitter::ownedCopy(Expr* e) {
    if (e->kind == EK::Implicit) {
        if (e->implicit == ImplicitKind::WrapOptional) return "ax_some_" + e->a->type->mangle + "(" + ownedCopy(e->a) + ")";
        return raw(e);
    }
    if (!e->type || isCopy(e->type)) return raw(e);
    if (!needsDrop(e) && e->kind != EK::StrLit) return "ax_clone_" + e->type->mangle + "(" + addr(e) + ")";
    return raw(e);
}

std::string Emitter::addr(Expr* e) {
    switch (e->kind) {
        case EK::Name:
            if (e->var) {
                if (isPtrVar(e->var)) return e->var->cname;
                return "&" + e->var->cname;
            }
            break;
        case EK::This: return "self";
        case EK::Member:
            if (e->field >= 0) return "&(" + raw(e) + ")";
            break;
        case EK::Index: return "&(" + raw(e) + ")";
        case EK::BoxDeref: return "ax_box_deref_" + e->type->mangle + "(" + boxPtr(e->a) + ", " + locStr(e->span) + ")";
        default: break;
    }
    std::string n = needsDrop(e) ? ownTmp(e->type, raw(e)) : tmpVar(e->type, e->kind == EK::Implicit ? raw(e) : raw(e));
    return "&" + n;
}

static std::string fmtDouble(double v) {
    char b[64];
    snprintf(b, sizeof b, "%.17g", v);
    std::string s = b;
    if (s.find_first_of(".eEni") == std::string::npos) s += ".0";
    return s;
}

std::string Emitter::raw(Expr* e) {
    switch (e->kind) {
        case EK::IntLit: {
            int64_t v = e->ival;
            switch (e->type->k) {
                case TK::Long:
                    if (v == INT64_MIN) return "(-INT64_C(9223372036854775807) - 1)";
                    return "INT64_C(" + std::to_string(v) + ")";
                case TK::Float: return "((float)" + std::to_string(v) + ")";
                case TK::Double: return "((double)" + std::to_string(v) + ")";
                case TK::Byte: return "((uint8_t)" + std::to_string(v) + ")";
                default:
                    if (v == INT32_MIN) return "(-2147483647 - 1)";
                    return v < 0 ? "(" + std::to_string(v) + ")" : std::to_string(v);
            }
        }
        case EK::FloatLit: {
            std::string s = fmtDouble(e->fval);
            if (e->type->k == TK::Float) s += "f";
            return e->fval < 0 ? "(" + s + ")" : s;
        }
        case EK::StrLit: return "ax_str_lit(" + cStrLit(e->sval) + ", " + std::to_string(e->sval.size()) + ")";
        case EK::CharLit: return "((ax_char)" + std::to_string(e->cval) + ")";
        case EK::BoolLit: return e->bval ? "true" : "false";
        case EK::None: return "ax_none_" + e->type->a->mangle + "()";
        case EK::Interp: return emitConcat(e->args);
        case EK::Name:
            if (!e->var) return "0";
            if (isPtrVar(e->var)) return "(*" + e->var->cname + ")";
            return e->var->cname;
        case EK::This: return "(*self)";
        case EK::Member: {
            if (e->builtin == Builtin::PropLength) return "((int32_t)(" + view(e->a) + ").len)";
            if (e->builtin == Builtin::PropHasValue) return "((" + view(e->a) + ").has)";
            Type* bt = e->a->type;
            std::string fname = bt->sd->fields[e->field]->cname;
            if (e->a->kind == EK::This) return "self->" + fname;
            if (e->a->kind == EK::Name && isPtrVar(e->a->var)) return e->a->var->cname + "->" + fname;
            if (!needsDrop(e->a)) return raw(e->a) + "." + fname;
            return view(e->a) + "." + fname;
        }
        case EK::Call: return emitCall(e);
        case EK::BoxDeref: return "(*" + addr(e) + ")";
        case EK::Index: {
            Type* bt = e->a->type;
            if (bt->k == TK::Map) {
                std::string m = addr(e->a);
                std::string k = e->b->type->k == TK::String ? view(e->b) : raw(e->b);
                return "(*ax_map_at_" + bt->mangle + "(" + m + ", " + k + ", " + locStr(e->span) + "))";
            }
            std::string fnName = (bt->k == TK::List ? "ax_list_at_" : "ax_arr_at_") + bt->a->mangle;
            std::string base = addr(e->a);
            return "(*" + fnName + "(" + base + ", (int64_t)(" + raw(e->b) + "), " + locStr(e->span) + "))";
        }
        case EK::Unary: {
            std::string a = raw(e->a);
            if (e->op == T::Bang) return "(!" + a + ")";
            std::string R = ct(e->type);
            if (e->op == T::Tilde) return "((" + R + ")~(" + R + ")(" + a + "))";
            if (e->type->k == TK::Int) return "ax_neg_i32((int32_t)(" + a + "), " + locStr(e->span) + ")";
            if (e->type->k == TK::Long) return "ax_neg_i64((int64_t)(" + a + "), " + locStr(e->span) + ")";
            return "(-(" + R + ")(" + a + "))";
        }
        case EK::Binary: return emitBinary(e);
        case EK::Ternary: return emitTernary(e);
        case EK::Cast: return "((" + ct(e->type) + ")(" + raw(e->a) + "))";
        case EK::StructLit: {
            StructDecl* sd = e->sd;
            std::vector<std::string> inits;
            for (auto* f : sd->fields) {
                int idx = -1;
                for (size_t i = 0; i < e->fieldNames.size(); i++)
                    if (e->fieldNames[i] == f->name) idx = (int)i;
                std::string v = idx >= 0 ? val(e->args[idx]) : val(f->def);
                inits.push_back("." + f->cname + " = " + v);
            }
            if (isDisposable(e->type)) inits.push_back(".ax_live = 1");
            if (inits.empty()) inits.push_back("0");
            return "((" + ct(e->type) + "){ " + join(inits, ", ") + " })";
        }
        case EK::ArrayLit: {
            Type* t = e->type;
            std::string EM = t->a->mangle, E = ct(t->a);
            if (t->k == TK::List) {
                std::string l = tmpVar(t, "ax_list_new_" + EM + "()");
                for (auto* x : e->args) pre("ax_list_add_" + EM + "(&" + l + ", " + val(x) + ");");
                return l;
            }
            size_t n = e->args.size();
            if (n == 0) return "((" + ct(t) + "){ NULL, 0 })";
            std::string d = newName("_a");
            pre(E + "* " + d + " = (" + E + "*)ax_alloc(" + std::to_string(n) + " * sizeof(" + E + "));");
            for (size_t i = 0; i < n; i++) {
                std::string v = val(e->args[i]);
                pre(d + "[" + std::to_string(i) + "] = " + v + ";");
            }
            return "((" + ct(t) + "){ " + d + ", " + std::to_string(n) + " })";
        }
        case EK::NewArray:
            return "ax_arr_new_" + e->type->a->mangle + "((int64_t)(" + raw(e->a) + "), " + locStr(e->span) + ")";
        case EK::GenericNew:
            if (e->callKind == CallKind::MapNew) return "ax_map_new_" + e->type->mangle + "()";
            return "ax_list_new_" + e->type->a->mangle + "()";
        case EK::Is: {
            Type* S = e->a->type;
            std::string s;
            if (e->a->kind == EK::Name && e->a->var && e->a->var->kind != VarDecl::Global) s = addr(e->a);
            else s = ptrTmp(S, addr(e->a));
            if (s[0] == '&') s = "(" + s + ")";
            return patternTest(e->pat, S, s, true);
        }
        case EK::Try: return emitTry(e);
        case EK::IncDec: {
            std::string k = incKind(e->type);
            std::string which = std::string(e->prefix ? "pre" : "post") + (e->op == T::PlusPlus ? "inc" : "dec");
            return "ax_" + which + "_" + k + "(" + writeAddr(e->a) + ", " + locStr(e->span) + ")";
        }
        case EK::Implicit:
            if (e->implicit == ImplicitKind::Numeric) return "((" + ct(e->type) + ")(" + raw(e->a) + "))";
            if (e->implicit == ImplicitKind::BoxToDyn) {
                // Box<Zombie> -> Box<Damageable>: keep the allocation, attach the vtable
                std::string vt = e->a->type->a->mangle + "__" + e->type->a->id->cname + "_vt";
                return "((" + ct(e->type) + "){ (void*)(" + (e->consumed ? val(e->a) : raw(e->a)) + "), &" + vt + " })";
            }
            if (e->implicit == ImplicitKind::WrapOptional)
                return "ax_some_" + e->a->type->mangle + "(" + (e->consumed ? val(e->a) : ownedCopy(e->a)) + ")";
            return raw(e->a);
        case EK::RefArg: return addr(e->a);
        case EK::Assign: return "0";
    }
    return "0";
}

std::string Emitter::callArgs(Expr* e, FuncDecl* f, const std::string& first) {
    std::vector<std::string> as;
    if (!first.empty()) as.push_back(first);
    for (size_t i = 0; i < f->params.size() && i < e->args.size(); i++) {
        Param& p = f->params[i];
        Expr* a = e->args[i];
        Type* pt = p.var->type;
        if (p.mode == 0) {
            if (f->isExtern) {
                if (pt->k == TK::String) as.push_back("ax_cstr(" + view(a) + ")");
                else if (pt->k == TK::Optional) as.push_back("ax_opt_cstr(" + view(a) + ")");  // string? -> NULL or text
                else as.push_back(val(a));
            }
            else if (f->isAxonRt && pt->k == TK::String) as.push_back(view(a));
            else as.push_back(val(a));
            continue;
        }
        Expr* inner = a->kind == EK::RefArg ? a->a : a;
        if (pt->k == TK::Interface) {
            if (inner->type == pt) as.push_back(view(inner));
            else if (inner->type->k == TK::Box) {  // lend the value inside a Box<Iface>
                std::string b = tmpVar(inner->type, "ax_dyn_check_" + inner->type->mangle + "(" + view(inner) + ", " + locStr(inner->span) + ")");
                as.push_back("((" + ct(pt) + "){ " + b + ".self, " + b + ".vt })");
            }
            else {
                const Impl* im = findImpl(P, inner->type, pt->id);
                std::string vt = im ? im->type->mangle + "__" + pt->id->cname + "_vt" : "0";
                as.push_back("((" + ct(pt) + "){ (void*)" + addr(inner) + ", &" + vt + " })");
            }
        } else as.push_back(addr(inner));
    }
    return join(as, ", ");
}

std::string Emitter::emitCall(Expr* e) {
    std::string loc = locStr(e->span);
    switch (e->callKind) {
        case CallKind::Func: {
            FuncDecl* f = e->fn;
            std::string c = f->cname + "(" + callArgs(e, f, "") + ")";
            if (f->isExtern && f->retType->k == TK::String) return "ax_str_from_c(" + c + ")";
            if (f->isExtern && f->retType->k == TK::Optional) return "ax_opt_from_c(" + c + ")";
            return c;
        }
        case CallKind::Method: {
            Expr* recv = e->a->a;
            std::string self = addr(recv);
            return e->fn->cname + "(" + callArgs(e, e->fn, self) + ")";
        }
        case CallKind::IfaceMethod: {
            Expr* recv = e->a->a;
            std::string r = view(recv);
            return "(" + r + ").vt->" + e->fn->name + "(" + callArgs(e, e->fn, "(" + r + ").self") + ")";
        }
        case CallKind::Builtin: {
            switch (e->builtin) {
                case Builtin::Print:
                case Builtin::Write: {
                    bool nl = e->builtin == Builtin::Print;
                    if (e->args.empty()) return nl ? "ax_print_newline()" : "(void)0";
                    Expr* a = e->args[0];
                    std::string k = nl ? "ax_print_" : "ax_write_";
                    switch (a->type->k) {
                        case TK::String: return k + "str(" + view(a) + ")";
                        case TK::Float: return k + "f32(" + raw(a) + ")";
                        case TK::Double: return k + "f64(" + raw(a) + ")";
                        case TK::Bool: return k + "bool(" + raw(a) + ")";
                        case TK::Char: return k + "char(" + raw(a) + ")";
                        default: return k + "i64((int64_t)(" + raw(a) + "))";
                    }
                }
                case Builtin::Assert:
                    if (e->args.size() > 1) return "ax_assert_msg(" + raw(e->args[0]) + ", " + view(e->args[1]) + ", " + loc + ")";
                    return "ax_assert(" + raw(e->args[0]) + ", " + loc + ")";
                case Builtin::Panic: return "ax_panic_str(" + view(e->args[0]) + ", " + loc + ")";
                default: return "0";
            }
        }
        case CallKind::BuiltinMethod: {
            Expr* recv = e->a->a;
            Type* rt = recv->type;
            auto A = [&](int i) { return e->args[i]->type->k == TK::String ? view(e->args[i]) : raw(e->args[i]); };
            switch (e->builtin) {
                case Builtin::Clone:
                    if (isCopy(rt)) return raw(recv);
                    return "ax_clone_" + rt->mangle + "(" + addr(recv) + ")";
                case Builtin::StrSubstring: { std::string s = view(recv); return "ax_str_substring(" + s + ", " + A(0) + ", " + A(1) + ", " + loc + ")"; }
                case Builtin::StrContains: { std::string s = view(recv); return "ax_str_contains(" + s + ", " + A(0) + ")"; }
                case Builtin::StrStartsWith: { std::string s = view(recv); return "ax_str_startswith(" + s + ", " + A(0) + ")"; }
                case Builtin::StrEndsWith: { std::string s = view(recv); return "ax_str_endswith(" + s + ", " + A(0) + ")"; }
                case Builtin::StrIndexOf: { std::string s = view(recv); return "ax_str_indexof(" + s + ", " + A(0) + ")"; }
                case Builtin::StrTrim: return "ax_str_trim(" + view(recv) + ")";
                case Builtin::StrToUpper: return "ax_str_toupper(" + view(recv) + ")";
                case Builtin::StrToLower: return "ax_str_tolower(" + view(recv) + ")";
                case Builtin::StrSplit: { std::string s = view(recv); return "ax_str_split(" + s + ", " + A(0) + ")"; }
                case Builtin::StrToInt: return "ax_str_toint(" + view(recv) + ")";
                case Builtin::StrToDouble: return "ax_str_todouble(" + view(recv) + ")";
                case Builtin::StrByteAt: { std::string s = view(recv); return "ax_str_byteat(" + s + ", " + A(0) + ", " + loc + ")"; }
                case Builtin::StrReplace: { std::string s = view(recv); std::string a0 = A(0); return "ax_str_replace(" + s + ", " + a0 + ", " + A(1) + ")"; }
                case Builtin::ListAdd: { std::string l = addr(recv); return "ax_list_add_" + rt->a->mangle + "(" + l + ", " + val(e->args[0]) + ")"; }
                case Builtin::ListPop: return "ax_list_pop_" + rt->a->mangle + "(" + addr(recv) + ", " + loc + ")";
                case Builtin::ListRemoveAt: { std::string l = addr(recv); return "ax_list_removeAt_" + rt->a->mangle + "(" + l + ", " + raw(e->args[0]) + ", " + loc + ")"; }
                case Builtin::ListInsert: {
                    std::string l = addr(recv);
                    std::string i = raw(e->args[0]);
                    return "ax_list_insert_" + rt->a->mangle + "(" + l + ", " + i + ", " + val(e->args[1]) + ", " + loc + ")";
                }
                case Builtin::ListClear: return "ax_list_clear_" + rt->a->mangle + "(" + addr(recv) + ")";
                case Builtin::StrChars: return "ax_str_chars(" + view(recv) + ")";
                case Builtin::StrLastIndexOf: { std::string s = view(recv); return "ax_str_lastindexof(" + s + ", " + A(0) + ")"; }
                case Builtin::StrPadLeft: { std::string s = view(recv); return "ax_str_pad(" + s + ", " + A(0) + ", true)"; }
                case Builtin::StrPadRight: { std::string s = view(recv); return "ax_str_pad(" + s + ", " + A(0) + ", false)"; }
                case Builtin::StrRepeat: { std::string s = view(recv); return "ax_str_repeat(" + s + ", " + A(0) + ", " + loc + ")"; }
                case Builtin::StrIsEmpty: return "((" + view(recv) + ").len == 0)";
                case Builtin::SeqContains:
                case Builtin::SeqIndexOf: {
                    std::string p = addr(recv);
                    std::string fnName = std::string("ax_") + (rt->k == TK::List ? "list_" : "arr_") + rt->a->mangle + "_indexOf";
                    std::string c = fnName + "(" + p + ", " + A(0) + ")";
                    return e->builtin == Builtin::SeqContains ? "(" + c + " >= 0)" : c;
                }
                case Builtin::SeqSort:
                    return std::string("ax_") + (rt->k == TK::List ? "list_" : "arr_") + rt->a->mangle + "_sort(" + addr(recv) + ")";
                case Builtin::SeqReverse: {
                    std::string p = ptrTmp(rt, addr(recv));
                    return "ax_reverse(" + p + "->data, " + p + "->len, sizeof(" + ct(rt->a) + "))";
                }
                case Builtin::MapGet: { std::string m = addr(recv); return "ax_map_get_" + rt->mangle + "(" + m + ", " + A(0) + ")"; }
                case Builtin::MapContains: { std::string m = addr(recv); return "ax_map_contains_" + rt->mangle + "(" + m + ", " + A(0) + ")"; }
                case Builtin::MapRemove: { std::string m = addr(recv); return "ax_map_remove_" + rt->mangle + "(" + m + ", " + A(0) + ")"; }
                case Builtin::MapKeys: return "ax_map_keys_" + rt->mangle + "(" + addr(recv) + ")";
                case Builtin::MapValues: return "ax_map_values_" + rt->mangle + "(" + addr(recv) + ")";
                case Builtin::MapClear: return "ax_map_clear_" + rt->mangle + "(" + addr(recv) + ")";
                default: return "0";
            }
        }
        case CallKind::BoxNew: return "ax_box_new_" + e->type->a->mangle + "(" + val(e->args[0]) + ")";
        case CallKind::DynMethod: {
            Expr* recv = e->a->a;
            std::string b = tmpVar(recv->type, "ax_dyn_check_" + recv->type->mangle + "(" + view(recv) + ", " + loc + ")");
            return b + ".vt->" + e->fn->name + "(" + callArgs(e, e->fn, b + ".self") + ")";
        }
        case CallKind::EnumCtor: {
            std::vector<std::string> as;
            for (auto* a : e->args) as.push_back(val(a));
            return e->ed->cname + "__" + e->ed->variants[e->variant].name + "(" + join(as, ", ") + ")";
        }
        case CallKind::ResultOk: return "ax_ok_" + e->type->mangle + "(" + val(e->args[0]) + ")";
        case CallKind::ResultErr: return "ax_err_" + e->type->mangle + "(" + val(e->args[0]) + ")";
        default: return "0";
    }
}

std::string Emitter::arith(T op, Type* R, const std::string& a, const std::string& b, Span sp) {
    std::string C = ct(R);
    std::string ca = "(" + C + ")(" + a + ")", cb = "(" + C + ")(" + b + ")";
    bool isInt = R->k == TK::Int, isLong = R->k == TK::Long;
    std::string sfx = isInt ? "i32" : "i64";
    std::string loc = locStr(sp);
    switch (op) {
        case T::Plus: case T::PlusEq:
            if (isInt || isLong) return "ax_add_" + sfx + "(" + ca + ", " + cb + ", " + loc + ")";
            return "(" + ca + " + " + cb + ")";
        case T::Minus: case T::MinusEq:
            if (isInt || isLong) return "ax_sub_" + sfx + "(" + ca + ", " + cb + ", " + loc + ")";
            return "(" + ca + " - " + cb + ")";
        case T::Star: case T::StarEq:
            if (isInt || isLong) return "ax_mul_" + sfx + "(" + ca + ", " + cb + ", " + loc + ")";
            return "(" + ca + " * " + cb + ")";
        case T::Slash: case T::SlashEq:
            if (isInt || isLong) return "ax_div_" + sfx + "(" + ca + ", " + cb + ", " + loc + ")";
            return "(" + ca + " / " + cb + ")";
        case T::Percent: case T::PercentEq:
            if (isInt || isLong) return "ax_mod_" + sfx + "(" + ca + ", " + cb + ", " + loc + ")";
            return R->k == TK::Float ? "fmodf(" + ca + ", " + cb + ")" : "fmod(" + ca + ", " + cb + ")";
        case T::Amp: case T::AmpEq: return R->k == TK::Bool ? "(" + a + " & " + b + ")" : "((" + C + ")(" + ca + " & " + cb + "))";
        case T::Pipe: case T::PipeEq: return R->k == TK::Bool ? "(" + a + " | " + b + ")" : "((" + C + ")(" + ca + " | " + cb + "))";
        case T::Caret: case T::CaretEq: return R->k == TK::Bool ? "(" + a + " ^ " + b + ")" : "((" + C + ")(" + ca + " ^ " + cb + "))";
        case T::Shl: case T::ShlEq: return "((" + C + ")(" + ca + " << (" + b + ")))";
        case T::Shr: case T::ShrEq: return "((" + C + ")(" + ca + " >> (" + b + ")))";
        default: return "0";
    }
}

void Emitter::appendPart(const std::string& sb, Expr* p) {
    switch (p->type->k) {
        case TK::String: { std::string v = view(p); pre("ax_sb_str(&" + sb + ", " + v + ");"); break; }
        case TK::Float: { std::string v = raw(p); pre("ax_sb_f32(&" + sb + ", " + v + ");"); break; }
        case TK::Double: { std::string v = raw(p); pre("ax_sb_f64(&" + sb + ", " + v + ");"); break; }
        case TK::Bool: { std::string v = raw(p); pre("ax_sb_bool(&" + sb + ", " + v + ");"); break; }
        case TK::Char: { std::string v = raw(p); pre("ax_sb_char(&" + sb + ", " + v + ");"); break; }
        default: { std::string v = raw(p); pre("ax_sb_i64(&" + sb + ", (int64_t)(" + v + "));"); break; }
    }
}

std::string Emitter::emitConcat(const std::vector<Expr*>& parts) {
    std::string sb = newName("_sb");
    pre("ax_sb " + sb + " = ax_sb_new();");
    for (auto* p : parts) appendPart(sb, p);
    return "ax_sb_done(&" + sb + ")";
}

std::string Emitter::emitBinary(Expr* e) {
    T op = e->op;
    if (op == T::AndAnd || op == T::OrOr) {
        std::string l = raw(e->a);
        SCtx sc;
        std::string r = subCtx(sc, [&] { return raw(e->b); });
        if (sc.pre.empty() && sc.post.empty()) return "(" + l + (op == T::AndAnd ? " && " : " || ") + r + ")";
        std::string t = newName("_t");
        pre("bool " + t + " = " + l + ";");
        pre(std::string("if (") + (op == T::AndAnd ? "" : "!") + t + ") {");
        for (auto& x : sc.pre) pre(x);
        pre(t + " = " + r + ";");
        for (auto it = sc.post.rbegin(); it != sc.post.rend(); ++it) pre(*it);
        pre("}");
        return t;
    }
    if (op == T::QQ) return emitCoalesce(e);
    if (e->isNoneCompare) {
        Expr* other = e->a->kind == EK::None ? e->b : e->a;
        std::string v = view(other);
        return op == T::EqEq ? "(!(" + v + ").has)" : "((" + v + ").has)";
    }
    if (e->isConcat) {
        std::vector<Expr*> parts;
        std::function<void(Expr*)> collect = [&](Expr* x) {
            if (x->kind == EK::Binary && x->isConcat && x->op == T::Plus) { collect(x->a); collect(x->b); }
            else parts.push_back(x);
        };
        collect(e);
        return emitConcat(parts);
    }
    Type* ta = e->a->type;
    Type* tb = e->b->type;
    switch (op) {
        case T::EqEq: case T::NotEq: case T::Lt: case T::Gt: case T::Le: case T::Ge: {
            std::string cop = op == T::EqEq ? "==" : op == T::NotEq ? "!=" : op == T::Lt ? "<" : op == T::Gt ? ">" : op == T::Le ? "<=" : ">=";
            if (ta->k == TK::String) {
                std::string a = view(e->a);
                std::string b = view(e->b);
                return std::string(op == T::NotEq ? "(!" : "(") + "ax_str_eq(" + a + ", " + b + "))";
            }
            if (ta->k == TK::Enum) {
                std::string a = raw(e->a);
                return "((" + a + ").tag " + cop + " (" + raw(e->b) + ").tag)";
            }
            std::string a = raw(e->a);
            std::string b = raw(e->b);
            if (ta->k == TK::Bool) return "(" + a + " " + cop + " " + b + ")";
            bool fl = ta->k == TK::Float || ta->k == TK::Double || tb->k == TK::Float || tb->k == TK::Double;
            std::string c = fl ? "(double)" : "(int64_t)";
            return "(" + c + "(" + a + ") " + cop + " " + c + "(" + b + "))";
        }
        default: {
            std::string a = raw(e->a);
            std::string b = raw(e->b);
            return arith(op, e->type, a, b, e->span);
        }
    }
}

std::string Emitter::emitTernary(Expr* e) {
    std::string c = raw(e->a);
    bool owning = e->consumed || isCopy(e->type);
    SCtx sb, sc2;
    std::string b = subCtx(sb, [&] { return owning ? val(e->b) : ownedCopy(e->b); });
    std::string cc = subCtx(sc2, [&] { return owning ? val(e->c) : ownedCopy(e->c); });
    if (sb.pre.empty() && sb.post.empty() && sc2.pre.empty() && sc2.post.empty())
        return "(" + c + " ? " + b + " : " + cc + ")";
    std::string r = tmpZero(e->type);
    pre("if (" + c + ") {");
    for (auto& x : sb.pre) pre(x);
    pre(r + " = " + b + ";");
    for (auto it = sb.post.rbegin(); it != sb.post.rend(); ++it) pre(*it);
    pre("} else {");
    for (auto& x : sc2.pre) pre(x);
    pre(r + " = " + cc + ";");
    for (auto it = sc2.post.rbegin(); it != sc2.post.rend(); ++it) pre(*it);
    pre("}");
    return r;
}

std::string Emitter::emitCoalesce(Expr* e) {
    Type* T_ = e->type;
    Type* O = e->a->type;
    bool resultIsOpt = T_->k == TK::Optional;
    std::string r = tmpZero(T_);
    if (e->consumed || isCopy(T_)) {
        std::string o = tmpVar(O, val(e->a));
        SCtx sd;
        std::string d = subCtx(sd, [&] { return val(e->b); });
        pre("if (" + o + ".has) {");
        pre(r + " = " + (resultIsOpt ? o : o + ".val") + ";");
        pre("} else {");
        for (auto& x : sd.pre) pre(x);
        pre(r + " = " + d + ";");
        for (auto it = sd.post.rbegin(); it != sd.post.rend(); ++it) pre(*it);
        pre("}");
        return r;
    }
    // borrowed and owned: copy the value out without taking it from its owner
    std::string has, value;
    if (!needsDrop(e->a)) {
        std::string p = ptrTmp(O, addr(e->a));
        has = p + "->has";
        value = resultIsOpt ? "ax_clone_" + O->mangle + "(" + p + ")" : "ax_clone_" + O->a->mangle + "(&" + p + "->val)";
    } else {
        std::string o = tmpVar(O, val(e->a));
        has = o + ".has";
        value = resultIsOpt ? o : o + ".val";
    }
    SCtx sd;
    std::string d = subCtx(sd, [&] { return ownedCopy(e->b); });
    pre("if (" + has + ") {");
    pre(r + " = " + value + ";");
    pre("} else {");
    for (auto& x : sd.pre) pre(x);
    pre(r + " = " + d + ";");
    for (auto it = sd.post.rbegin(); it != sd.post.rend(); ++it) pre(*it);
    pre("}");
    return r;
}

void Emitter::earlyReturnDrops(std::vector<std::string>& lines) {
    for (size_t i = ctx.size(); i-- > 0;)
        for (auto it = ctx[i]->post.rbegin(); it != ctx[i]->post.rend(); ++it) lines.push_back(*it);
    for (size_t i = scopes.size(); i-- > 0;)
        for (auto it = scopes[i].drops.rbegin(); it != scopes[i].drops.rend(); ++it) lines.push_back(*it);
}

std::string Emitter::emitTry(Expr* e) {
    Type* R = e->a->type;
    Type* RetT = fn->retType;
    std::string r = tmpVar(R, val(e->a));
    std::vector<std::string> drops;
    earlyReturnDrops(drops);
    if (R->k == TK::Result) {
        pre("if (" + r + ".tag != 0) {");
        std::string er = newName("_e");
        pre(ct(RetT) + " " + er + " = ax_err_" + RetT->mangle + "(" + r + ".u.err);");
        for (auto& d : drops) pre(d);
        pre("return " + er + ";");
        pre("}");
        return r + ".u.ok";
    }
    pre("if (!" + r + ".has) {");
    for (auto& d : drops) pre(d);
    pre("return ax_none_" + RetT->a->mangle + "();");
    pre("}");
    return r + ".val";
}

std::string Emitter::patternTest(Pattern* pt, Type* subj, const std::string& s, bool assign) {
    std::string cond;
    std::vector<std::string> assigns;
    auto bindTo = [&](PatBind& b, const std::string& place) {
        if (!b.var || !assign) return;
        if (b.var->mode == 0) assigns.push_back(b.var->cname + " = " + place);
        else assigns.push_back(b.var->cname + " = &" + place);
    };
    switch (pt->kind) {
        case Pattern::Variant:
            if (subj->k == TK::Enum) {
                cond = s + "->tag == " + tagName(subj->ed, pt->variant);
                auto& v = subj->ed->variants[pt->variant];
                for (size_t i = 0; i < pt->binds.size(); i++)
                    bindTo(pt->binds[i], s + "->u." + vfield(subj->ed, pt->variant) + "." + v.fields[i]->cname);
            } else if (subj->k == TK::Result) {
                cond = s + "->tag == " + std::to_string(pt->variant);
                if (!pt->binds.empty()) bindTo(pt->binds[0], s + (pt->variant == 0 ? "->u.ok" : "->u.err"));
            } else if (subj->k == TK::Optional) {
                cond = s + "->has";
            }
            break;
        case Pattern::TypeBind:
            cond = s + "->has";
            bindTo(pt->bind, s + "->val");
            break;
        case Pattern::ExprPat:
            if (pt->expr->kind == EK::None) cond = "!" + s + "->has";
            else if (subj->k == TK::String) cond = "ax_str_eq(*" + s + ", " + view(pt->expr) + ")";
            else cond = "*" + s + " == " + raw(pt->expr);
            break;
    }
    if (assigns.empty()) return "(" + cond + ")";
    return "((" + cond + ") ? (" + join(assigns, ", ") + ", 1) : 0)";
}

std::vector<std::string> Emitter::patternBindDecls(Pattern* pt, Type* subj, const std::string& s) {
    std::vector<std::string> out;
    auto decl = [&](PatBind& b, const std::string& place) {
        if (!b.var) return;
        if (b.var->mode == 0) out.push_back(ct(b.var->type) + " " + b.var->cname + " = " + place + ";");
        else out.push_back(ct(b.var->type) + "* " + b.var->cname + " = &" + place + ";");
    };
    if (pt->kind == Pattern::Variant && subj->k == TK::Enum) {
        auto& v = subj->ed->variants[pt->variant];
        for (size_t i = 0; i < pt->binds.size(); i++)
            decl(pt->binds[i], s + "->u." + vfield(subj->ed, pt->variant) + "." + v.fields[i]->cname);
    } else if (pt->kind == Pattern::Variant && subj->k == TK::Result) {
        if (!pt->binds.empty()) decl(pt->binds[0], s + (pt->variant == 0 ? "->u.ok" : "->u.err"));
    } else if (pt->kind == Pattern::TypeBind) {
        decl(pt->bind, s + "->val");
    }
    return out;
}

// ======================================================================== statements

void Emitter::declPatVars(const std::vector<VarDecl*>& vs) {
    for (auto* v : vs) {
        if (v->mode == 0) {
            line(ct(v->type) + " " + v->cname + ";");
            line("memset(&" + v->cname + ", 0, sizeof " + v->cname + ");");
        } else line(ct(v->type) + "* " + v->cname + " = NULL;");
    }
}

void Emitter::emitBlockInner(Stmt* b) {
    pushScope(0);
    if (b->kind == SK::Block)
        for (auto* s : b->stmts) emitStmt(s);
    else emitStmt(b);
    popScope();
    lastBlockTerminated = terminated;
    terminated = false;  // code after a closed block is reachable again
}

void Emitter::emitAssign(Expr* e) {
    Expr* target = e->a;
    Type* T_ = target->type;
    SCtx sc;
    ctx.push_back(&sc);
    bool simpleVar = target->kind == EK::Name && target->var && !isPtrVar(target->var);
    std::string stmt;
    bool mapSlot = target->kind == EK::Index && target->a->type->k == TK::Map;
    if (e->op == T::Assign) {
        if (isCopy(T_)) {
            std::string v = val(e->b);
            stmt = (mapSlot ? "*" + writeAddr(target) : raw(target)) + " = " + v + ";";
        } else {
            std::string v = tmpVar(T_, val(e->b));
            if (simpleVar) {
                pre(drop(T_, "&" + target->var->cname));
                stmt = target->var->cname + " = " + v + ";";
            } else {
                std::string p = ptrTmp(T_, writeAddr(target));
                pre(drop(T_, p));
                stmt = "*" + p + " = " + v + ";";
            }
        }
    } else if (e->isConcat) {
        std::string v = tmpVar(T_, emitConcat({target, e->b}));
        if (simpleVar) {
            pre(drop(T_, "&" + target->var->cname));
            stmt = target->var->cname + " = " + v + ";";
        } else {
            std::string p = ptrTmp(T_, addr(target));
            pre(drop(T_, p));
            stmt = "*" + p + " = " + v + ";";
        }
    } else {
        std::string place;
        if (simpleVar) place = target->var->cname;
        else place = "*" + ptrTmp(T_, writeAddr(target));  // `counts[word] += 1` adds a missing key as 0
        std::string rhs = raw(e->b);
        stmt = place + " = " + arith(e->op, T_, place, rhs, e->span) + ";";
    }
    ctx.pop_back();
    flushPre(sc);
    line(stmt);
    flushPost(sc);
}

void Emitter::emitStmt(Stmt* s) {
    terminated = false;
    emitStmtInner(s);
    terminated = s->kind == SK::Return || s->kind == SK::Break || s->kind == SK::Continue;
}

void Emitter::emitStmtInner(Stmt* s) {
    switch (s->kind) {
        case SK::Block:
            line("{");
            emitBlockInner(s);
            line("}");
            break;
        case SK::Var: {
            VarDecl* v = s->var;
            SCtx sc;
            std::string init = subCtx(sc, [&] { return val(v->init); });
            flushPre(sc);
            line(ct(v->type) + " " + v->cname + " = " + init + ";");
            flushPost(sc);
            addOwned(v);
            break;
        }
        case SK::ExprStmt: {
            Expr* e = s->expr;
            if (e->kind == EK::Assign) { emitAssign(e); break; }
            SCtx sc;
            std::string r = subCtx(sc, [&] {
                if (!e->type || e->type->k == TK::Void || isCopy(e->type)) return raw(e);
                return view(e);
            });
            flushPre(sc);
            if (!r.empty()) {
                bool isTmp = r.size() > 2 && r[0] == '_' && r.find_first_of("(). ") == std::string::npos;
                if (!isTmp) line((e->type && e->type->k != TK::Void ? "(void)" : "") + r + ";");
            }
            flushPost(sc);
            break;
        }
        case SK::If: {
            SCtx sc;
            std::string c = subCtx(sc, [&] { return raw(s->expr); });
            if (!s->negPatVars.empty()) {
                // the bindings outlive the if, so everything stays in the enclosing scope
                declPatVars(s->negPatVars);
                declPatVars(s->patVars);
                flushPre(sc);
                for (auto& p : sc.post) scopes.back().drops.push_back(p);
                line("if (" + c + ") {");
                emitBlockInner(s->then);
                if (s->els) {
                    line("} else {");
                    emitBlockInner(s->els);
                }
                line("}");
                break;
            }
            bool wrap = !s->patVars.empty() || !sc.pre.empty() || !sc.post.empty();
            if (wrap) {
                line("{");
                pushScope(0);
                declPatVars(s->patVars);
                flushPre(sc);
                for (auto& p : sc.post) scopes.back().drops.push_back(p);
            }
            line("if (" + c + ") {");
            emitBlockInner(s->then);
            if (s->els) {
                line("} else {");
                emitBlockInner(s->els);
            }
            line("}");
            if (wrap) {
                popScope();
                line("}");
            }
            break;
        }
        case SK::While: {
            SCtx sc;
            std::string c = subCtx(sc, [&] { return raw(s->expr); });
            if (sc.pre.empty() && sc.post.empty() && s->patVars.empty()) {
                line("while (" + c + ") {");
                pushScope(1);
                emitBlockInner(s->body);
                popScope();
                line("}");
                break;
            }
            std::string cont = newName("_cont");
            line("{");
            declPatVars(s->patVars);
            line("for (;;) {");
            flushPre(sc);
            line("if (!(" + c + ")) {");
            flushPost(sc);
            line("break;");
            line("}");
            pushScope(1, cont);
            for (auto& p : sc.post) scopes.back().drops.push_back(p);
            emitBlockInner(s->body);
            line(cont + ": ;");
            popScope();
            line("}");
            line("}");
            break;
        }
        case SK::DoWhile: {
            SCtx sc;
            std::string c = subCtx(sc, [&] { return raw(s->expr); });
            if (sc.pre.empty() && sc.post.empty()) {
                line("do {");
                pushScope(1);
                emitBlockInner(s->body);
                popScope();
                line("} while (" + c + ");");
                break;
            }
            std::string cont = newName("_cont");
            line("for (;;) {");
            pushScope(1, cont);
            emitBlockInner(s->body);
            popScope();
            line(cont + ": ;");
            flushPre(sc);
            std::string cv = newName("_c");
            line("bool " + cv + " = " + c + ";");
            flushPost(sc);
            line("if (!" + cv + ") break;");
            line("}");
            break;
        }
        case SK::For: {
            line("{");
            pushScope(0);
            if (s->init) emitStmt(s->init);
            SCtx sc;
            std::string c = s->expr ? subCtx(sc, [&] { return raw(s->expr); }) : "";
            // render the step separately to see whether it fits in the `for (...)` header
            std::string stepBuf;
            if (s->step) {
                std::string* saved = out;
                int savedInd = ind;
                out = &stepBuf;
                ind = 0;
                Stmt tmp;
                tmp.kind = SK::ExprStmt;
                tmp.expr = s->step;
                emitStmt(&tmp);
                out = saved;
                ind = savedInd;
            }
            std::vector<std::string> stepLines;
            {
                size_t a = 0;
                while (a < stepBuf.size()) {
                    size_t b = stepBuf.find('\n', a);
                    if (b == std::string::npos) b = stepBuf.size();
                    if (b > a) stepLines.push_back(stepBuf.substr(a, b - a));
                    a = b + 1;
                }
            }
            bool simple = sc.pre.empty() && sc.post.empty() && stepLines.size() <= 1;
            if (simple) {
                std::string st = stepLines.empty() ? "" : stepLines[0].substr(0, stepLines[0].size() - 1);
                if (st.rfind("(void)", 0) == 0) st = st.substr(6);
                line("for (; " + c + "; " + st + ") {");
                pushScope(1);
                emitBlockInner(s->body);
                popScope();
                line("}");
            } else {
                std::string cont = newName("_cont");
                line("for (;;) {");
                if (s->expr) {
                    flushPre(sc);
                    line("if (!(" + c + ")) {");
                    flushPost(sc);
                    line("break;");
                    line("}");
                }
                pushScope(1, cont);
                for (auto& p : sc.post) scopes.back().drops.push_back(p);
                emitBlockInner(s->body);
                line(cont + ": ;");
                popScope();
                for (auto& l : stepLines) line(l);
                line("}");
            }
            popScope();
            line("}");
            break;
        }
        case SK::Foreach: {
            line("{");
            pushScope(0);
            Type* CT = s->expr->type;
            SCtx sc;
            std::string c = subCtx(sc, [&] { return addr(s->expr); });
            flushPre(sc);
            for (auto& p : sc.post) scopes.back().drops.push_back(p);
            std::string cv = newName("_c"), iv = newName("_i");
            line(ct(CT) + "* " + cv + " = " + c + ";");
            line("for (int64_t " + iv + " = 0; " + iv + " < " + cv + "->len; " + iv + "++) {");
            pushScope(1);
            VarDecl* v = s->var;
            if (v->mode == 0) line(ct(v->type) + " " + v->cname + " = " + cv + "->data[" + iv + "];");
            else line(ct(v->type) + "* " + v->cname + " = &" + cv + "->data[" + iv + "];");
            emitBlockInner(s->body);
            popScope();
            line("}");
            popScope();
            line("}");
            break;
        }
        case SK::Switch: {
            line("{");
            pushScope(0);
            Type* S = s->expr->type;
            SCtx sc;
            std::string a = subCtx(sc, [&] { return addr(s->expr); });
            flushPre(sc);
            for (auto& p : sc.post) scopes.back().drops.push_back(p);
            std::string sw = newName("_sw"), end = newName("_swend");
            line(ct(S) + "* " + sw + " = " + a + ";");
            pushScope(2, end);
            bool first = true;
            const Case* def = nullptr;
            for (auto& c : s->cases) {
                if (c.isDefault) { def = &c; continue; }
                SCtx tc;
                std::vector<std::string> tests;
                ctx.push_back(&tc);
                for (auto* pt : c.pats) tests.push_back(patternTest(pt, S, sw, false));
                ctx.pop_back();
                for (auto& l : tc.pre) line(l);  // only literals here, normally empty
                line((first ? "if (" : "} else if (") + join(tests, " || ") + ") {");
                first = false;
                pushScope(0);
                if (c.pats.size() == 1)
                    for (auto& d : patternBindDecls(c.pats[0], S, sw)) line(d);
                for (auto* st : c.body) emitStmt(st);
                popScope();
            }
            if (def) {
                line(first ? "{" : "} else {");
                pushScope(0);
                for (auto* st : def->body) emitStmt(st);
                popScope();
                line("}");
            } else if (!first) line("}");
            popScope();
            line(end + ": ;");
            popScope();
            line("}");
            break;
        }
        case SK::Break: {
            int k = (int)scopes.size() - 1;
            while (k >= 0 && scopes[k].kind == 0) k--;
            if (k < 0) break;
            if (scopes[k].kind == 2) {
                emitScopeDrops(k + 1);
                line("goto " + scopes[k].label + ";");
            } else {
                emitScopeDrops(k);
                line("break;");
            }
            break;
        }
        case SK::Continue: {
            int k = (int)scopes.size() - 1;
            while (k >= 0 && scopes[k].kind != 1) k--;
            if (k < 0) break;
            emitScopeDrops(k + 1);
            if (!scopes[k].label.empty()) line("goto " + scopes[k].label + ";");
            else line("continue;");
            break;
        }
        case SK::Return: {
            SCtx sc;
            std::string v = s->expr ? subCtx(sc, [&] { return val(s->expr); }) : "";
            flushPre(sc);
            bool anyDrops = !sc.post.empty();
            for (auto& sp : scopes) anyDrops |= !sp.drops.empty();
            if (s->expr && fn->retType->k != TK::Void) {
                if (!anyDrops) {
                    line("return " + v + ";");
                } else {
                    std::string rv = newName("_ret");
                    line(ct(fn->retType) + " " + rv + " = " + v + ";");
                    flushPost(sc);
                    emitScopeDrops(0);
                    line("return " + rv + ";");
                }
            } else {
                if (s->expr) line("(void)(" + v + ");");
                flushPost(sc);
                emitScopeDrops(0);
                line("return;");
            }
            break;
        }
    }
}

void Emitter::emitFunc(FuncDecl* f) {
    fn = f;
    out = &bodies;
    ind = 0;
    scopes.clear();
    std::string s = "static " + sig(f, f->cname);
    protos += s + ";\n";
    line(s + " {");
    pushScope(0);
    for (auto& p : f->params) addOwned(p.var);
    emitBlockInner(f->body);
    terminated = lastBlockTerminated;
    popScope();
    terminated = false;
    line("}");
    line("");
    fn = nullptr;
}

std::string Emitter::run() {
    // types the runtime itself needs
    Type* S = g_types.stringT();
    g_types.arrayOf(S);
    g_types.optOf(g_types.intT());
    g_types.optOf(g_types.doubleT());
    g_types.optOf(S);
    g_types.resultOf(S, S);
    g_types.arrayOf(g_types.prim(TK::Char));
    // types the generated helpers need (map.get returns V?, map.keys returns K[], ...)
    for (size_t i = 0; i < g_types.all().size(); i++) {
        Type* t = g_types.all()[i];
        if (t->k == TK::Map) {
            g_types.optOf(t->b);
            g_types.arrayOf(t->a);
            g_types.arrayOf(t->b);
        }
    }

    std::string head = "/* Generated by axonc - the Axon compiler. Do not edit. */\n";
    if (opt.release) head += "#define AXON_RELEASE 1\n";
    head += AXON_RT_CORE_1;
    head += AXON_RT_CORE_2;

    std::vector<std::string> includes;
    for (Module* m : P.modules)
        for (auto& h : m->cIncludes)
            if (std::find(includes.begin(), includes.end(), h) == includes.end()) includes.push_back(h);
    std::string inc;
    for (auto& h : includes) inc += h.find('<') == 0 ? "#include " + h + "\n" : "#include \"" + h + "\"\n";

    // forward declarations for every generated struct type
    std::vector<Type*> all = g_types.all();
    for (Type* t : all) {
        bool gen = (t->k == TK::Struct && !t->sd->isExtern) || t->k == TK::Enum || t->k == TK::Optional ||
                   t->k == TK::Result || t->k == TK::Array || t->k == TK::List || t->k == TK::Map;
        if (gen) fwd += "typedef struct " + ct(t) + " " + ct(t) + ";\n";
        if (t->k == TK::Box && t->a->k != TK::Interface) fwd += "typedef " + ct(t->a) + "* " + ct(t) + ";\n";
    }
    for (Type* t : all) emitTypeDef(t);
    emitIfaces();
    for (Type* t : all)
        if (t->k == TK::Box && t->a->k == TK::Interface)
            fwd += "typedef struct " + ct(t) + " { void* self; const " + t->a->id->cname + "_vt* vt; } " + ct(t) + ";\n";
    for (Type* t : all) emitHelpers(t);

    for (VarDecl* v : P.globalOrder) globals += "static " + ct(v->type) + " " + v->cname + ";\n";

    if (opt.support) {
        // user code comes from the native backend; declare it so thunks and main can call it
        for (FuncDecl* f : P.funcs) protos += sig(f, f->cname) + ";\n";
        protos += "void ax_init_globals(void);\n";
        // C functions and constants are reached through small wrappers: they may be macros or
        // `static inline`, and the wrappers do the string conversions
        std::set<std::string> done;
        for (Module* m : P.modules) {
            for (FuncDecl* f : m->funcs) {
                if (!f->isExtern || !done.insert(f->name).second) continue;
                std::vector<std::string> ps, as;
                for (size_t i = 0; i < f->params.size(); i++) {
                    Param& p = f->params[i];
                    Type* t = p.var->type;
                    std::string n = "p" + std::to_string(i);
                    if (p.mode != 0) { ps.push_back(ct(t) + "* " + n); as.push_back(n); }
                    else if (t->k == TK::String) { ps.push_back("ax_string " + n); as.push_back("ax_cstr(" + n + ")"); }
                    else if (t->k == TK::Optional) { ps.push_back(ct(t) + " " + n); as.push_back("ax_opt_cstr(" + n + ")"); }
                    else { ps.push_back(ct(t) + " " + n); as.push_back(n); }
                }
                std::string c = f->name + "(" + join(as, ", ") + ")";
                std::string body;
                if (f->retType->k == TK::Void) body = c + ";";
                else if (f->retType->k == TK::String) body = "return ax_str_from_c(" + c + ");";
                else if (f->retType->k == TK::Optional) body = "return ax_opt_from_c(" + c + ");";
                else body = "return " + c + ";";
                bodies += ct(f->retType) + " axw_" + f->name + "(" + (ps.empty() ? "void" : join(ps, ", ")) + ") { " + body + " }\n";
            }
            for (VarDecl* v : m->globals)
                if (v->isExtern && done.insert("g:" + v->name).second)
                    bodies += ct(v->type) + " axg_" + v->name + "(void) { return " + v->name + "; }\n";
        }
        out = &bodies;
        ind = 0;
    } else {
        for (FuncDecl* f : P.funcs) emitFunc(f);

        // global initialization
        out = &bodies;
        ind = 0;
        scopes.clear();
        line("static void ax_init_globals(void) {");
        for (VarDecl* v : P.globalOrder) {
            SCtx sc;
            std::string init = subCtx(sc, [&] { return val(v->init); });
            flushPre(sc);
            line(v->cname + " = " + init + ";");
            flushPost(sc);
        }
        line("}");
        line("");
    }
    line("static void ax_drop_globals(void) {");
    for (auto it = P.globalOrder.rbegin(); it != P.globalOrder.rend(); ++it)
        if (isOwned((*it)->type)) line(drop((*it)->type, "&" + (*it)->cname));
    line("}");
    line("");
    if (opt.testMode) {
        line("int main(int argc, char** argv) {");
        line("ax_rt_init(argc, argv);");
        line("ax_init_globals();");
        line("int passed = 0, failed = 0;");
        for (FuncDecl* t : P.tests) line("ax_run_test(" + cStrLit(t->name) + ", " + t->cname + ", &passed, &failed);");
        line("printf(\"\\n%d passed, %d failed\\n\", passed, failed);");
        line("ax_drop_globals();");
        line("ax_leak_report();");
        line("return failed ? 1 : 0;");
        line("}");
    } else if (P.mainFn) {
        line("int main(int argc, char** argv) {");
        line("ax_rt_init(argc, argv);");
        line("ax_init_globals();");
        if (P.mainFn->retType->k == TK::Int) line("int ax_exit = (int)" + P.mainFn->cname + "();");
        else {
            line(P.mainFn->cname + "();");
            line("int ax_exit = 0;");
        }
        line("ax_drop_globals();");
        line("ax_leak_report();");
        line("return ax_exit;");
        line("}");
    }

    std::string result = head;
    if (!inc.empty()) result += "\n/* ---- C libraries ---- */\n" + inc;
    result += "\n/* ---- types ---- */\n" + fwd + "\n" + defs + "\n" + ifaceDefs;
    result += AXON_RT_AFTER_TYPES;
    result += "\n/* ---- declarations ---- */\n" + protos + "\n" + globals + "\n" + vtables;
    result += "\n/* ---- helpers ---- */\n" + helpers;
    result += "\n/* ---- program ---- */\n" + bodies;
    if (opt.support) {
        // make every definition visible to the linker
        auto replaceAll = [&](const std::string& from, const std::string& to) {
            size_t pos = 0;
            while ((pos = result.find(from, pos)) != std::string::npos) {
                result.replace(pos, from.size(), to);
                pos += to.size();
            }
        };
        replaceAll("#define AX_INLINE static __inline", "#define AX_INLINE");
        replaceAll("#define AX_INLINE static inline", "#define AX_INLINE");
        replaceAll("static ", "");
        result = "/* support object for the native backend */\n" + result;
    }
    return result;
}

}  // namespace

std::string emitProgram(Program& prog, const EmitOptions& opt) {
    Emitter e(prog, opt);
    return e.run();
}
