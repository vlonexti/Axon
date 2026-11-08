// checker.cpp - resolves names, checks types, interfaces, exhaustiveness and ownership.
#include "checker.h"
#include "clone.h"

#include <algorithm>
#include <functional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace {

const std::unordered_set<std::string>& cReserved() {
    static const std::unordered_set<std::string> r = {
        "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum",
        "extern", "float", "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return",
        "short", "signed", "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void",
        "volatile", "while", "bool", "true", "false", "NULL", "main", "self", "errno", "stdin", "stdout",
        "stderr", "printf", "malloc", "free", "exit", "abs", "min", "max", "assert", "offsetof", "_Bool",
        "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "size_t",
        "asm", "fortran", "far", "near", "small", "interface", "string",
    };
    return r;
}

bool isBuiltinFuncName(const std::string& n) {
    return n == "print" || n == "write" || n == "assert" || n == "panic" || n == "Ok" || n == "Err";
}

struct Sym {
    enum K { Func, Struct, Enum, Iface, Global } k;
    void* d;
    bool isPrivate;
    Module* mod;
    Span span;
};

struct Flow {
    std::set<VarDecl*> moved;
    bool dead = false;
};

Flow mergeFlows(const Flow& a, const Flow& b) {
    if (a.dead) return b;
    if (b.dead) return a;
    Flow r = a;
    r.moved.insert(b.moved.begin(), b.moved.end());
    return r;
}

std::string srcText(Span sp) {
    if (sp.file < 0) return "";
    const auto& txt = g_diag.file(sp.file).text;
    if (sp.offset < 0 || sp.offset + sp.len > (int)txt.size()) return "";
    return txt.substr(sp.offset, sp.len);
}

class Checker {
public:
    explicit Checker(Program& p) : P(p) {}
    bool run();

private:
    Program& P;
    std::map<Module*, std::unordered_map<std::string, Sym>> syms;
    std::unordered_set<std::string> usedC;
    std::unordered_set<std::string> typeNamesC;
    Module* mod = nullptr;
    FuncDecl* fn = nullptr;
    std::vector<std::unordered_map<std::string, VarDecl*>> scopes;
    Flow flow;
    std::map<VarDecl*, Span> moveSite;
    std::unordered_map<VarDecl*, int> declLoop;
    struct Ctx {
        bool isSwitch;
        std::vector<Flow> breaks, conts;
    };
    std::vector<Ctx> ctxs;
    int loopDepth = 0;
    struct Frozen {
        VarDecl* var;
        Span span;
        std::string why;
    };
    std::vector<Frozen> frozen;
    std::vector<VarDecl*>* patSink = nullptr;
    std::vector<VarDecl*>* negSink = nullptr;
    bool patNegated = false;
    bool declareBinds = true;
    Expr* curIsSubject = nullptr;
    void freezeBorrows(const std::vector<VarDecl*>& vs) {
        for (VarDecl* v : vs)
            if (v->borrowOf) frozen.push_back({v->borrowOf, v->span, "while `" + v->name + "` borrows from it"});
    }
    std::set<VarDecl*> globalsBusy, globalsDone;

    // ---- helpers
    std::string uniqueC(const std::string& base) {
        std::string n = base;
        if (cReserved().count(n)) n += "_";
        int i = 2;
        while (usedC.count(n)) n = base + "_" + std::to_string(i++);
        usedC.insert(n);
        return n;
    }
    std::string localC(const std::string& name) {
        if (cReserved().count(name) || name[0] == '_' || name.rfind("ax_", 0) == 0 || typeNamesC.count(name) ||
            name.rfind("axrt_", 0) == 0)
            return name + "_";
        return name;
    }

    void addSym(Module* m, const std::string& name, Sym s) {
        auto& tbl = syms[m];
        auto it = tbl.find(name);
        if (it != tbl.end()) {
            g_diag.error(s.span, "`" + name + "` is declared twice")
                .note(it->second.span, "the first declaration is here")
                .help("rename one of them");
            return;
        }
        tbl[name] = s;
    }

    Sym* findIn(Module* m, const std::string& name) {
        auto& tbl = syms[m];
        auto it = tbl.find(name);
        return it == tbl.end() ? nullptr : &it->second;
    }

    Sym* findSym(const std::string& name, Module* m, Span sp, bool report = true) {
        if (Sym* s = findIn(m, name)) return s;
        Sym* found = nullptr;
        for (Module* im : m->imports) {
            Sym* s = findIn(im, name);
            if (!s || s->isPrivate) continue;
            if (found && found->d != s->d) {
                if (report)
                    g_diag.error(sp, "`" + name + "` is ambiguous: it exists in `" + found->mod->name + "` and `" + s->mod->name + "`")
                        .help("use `using ... as name;` to give one of the modules a prefix");
                return found;
            }
            found = s;
        }
        if (found) return found;
        if (P.prelude && m != P.prelude) return findIn(P.prelude, name);
        return nullptr;
    }

    Module* findNs(const std::string& name, Module* m) {
        auto it = m->namespaces.find(name);
        return it == m->namespaces.end() ? nullptr : it->second;
    }

    std::vector<std::string> visibleNames(Module* m) {
        std::vector<std::string> out;
        for (auto& kv : syms[m]) out.push_back(kv.first);
        for (Module* im : m->imports)
            for (auto& kv : syms[im])
                if (!kv.second.isPrivate) out.push_back(kv.first);
        for (auto& sc : scopes)
            for (auto& kv : sc) out.push_back(kv.first);
        return out;
    }

    // ---- generics
    std::vector<std::map<std::string, Type*>> substStack;
    std::vector<FuncDecl*> pendingFuncs;      // generic instances whose bodies still need checking
    std::vector<StructDecl*> pendingStructs;  // generic struct instances whose field defaults need checking
    struct BoundCheck {
        Type* type;
        TypeExpr* bound;
        Module* mod;
        Span span;
        std::string param;
    };
    std::vector<BoundCheck> pendingBounds;
    int instDepth = 0;
    std::string instKey(const std::vector<Type*>& args);
    std::string instName(const std::string& base, const std::vector<Type*>& args);
    std::string instCName(const std::string& base, const std::vector<Type*>& args);
    bool checkArity(const GenericInfo& g, const std::vector<Type*>& args, const std::string& name, Span sp);
    bool queueBounds(const GenericInfo& g, const std::vector<Type*>& args, Module* m, Span sp);
    bool checkBound(const BoundCheck& b);
    bool implsReady = false;
    template <class F>
    void inGenericScope(Module* m, const std::map<std::string, Type*>& subst, F f);
    Type* instantiateStruct(StructDecl* g, const std::vector<Type*>& args, Span sp);
    Type* instantiateEnum(EnumDecl* g, const std::vector<Type*>& args, Span sp);
    FuncDecl* instantiateFunc(FuncDecl* g, const std::vector<Type*>& args, Span sp);
    void unify(TypeExpr* te, Type* actual, const std::set<std::string>& params, std::map<std::string, Type*>& out);
    Type* checkGenericCall(Expr* e, FuncDecl* g, std::vector<TypeExpr*>* explicitArgs, Type* expected = nullptr);
    void drainGenerics();

    // ---- types
    Type* resolveType(TypeExpr* te, Module* m);
    Type* resolveTypeInner(TypeExpr* te, Module* m);
    bool sameType(Type* a, Type* b) { return a == b || isErr(a) || isErr(b); }

    // ---- declarations
    void registerModule(Module* m);
    void resolveSignatures(Module* m);
    void resolveStruct(StructDecl* s);
    void resolveEnum(EnumDecl* e);
    void resolveRest(Module* m);
    void setupFunc(FuncDecl* f, Type* self);
    void buildImpl(Type* t, InterfaceDecl* iface, Span sp);
    void checkContainment();
    void checkGlobal(VarDecl* v);
    void checkFunc(FuncDecl* f);
    void checkFuncBody(FuncDecl* f);
    void checkFieldDefaults(StructDecl* s);
    void checkExternSig(FuncDecl* f);

    // ---- scopes
    VarDecl* lookupLocal(const std::string& name) {
        for (int i = (int)scopes.size() - 1; i >= 0; i--) {
            auto it = scopes[i].find(name);
            if (it != scopes[i].end()) return it->second;
        }
        return nullptr;
    }
    void declare(VarDecl* v) {
        if (v->name == "_") return;
        if (VarDecl* prev = lookupLocal(v->name)) {
            g_diag.error(v->span, "`" + v->name + "` is already declared")
                .note(prev->span, "the earlier `" + v->name + "` is here")
                .help("pick a different name; Axon doesn't allow a variable to hide another one");
        }
        scopes.back()[v->name] = v;
        declLoop[v] = loopDepth;
        if (v->cname.empty()) v->cname = localC(v->name);
    }

    // ---- statements
    void checkBlock(Stmt* s, bool newScope = true);
    void checkStmt(Stmt* s);
    void checkLoopBody(Stmt* body, const Flow& entry, Flow* nextIter);
    void checkSwitch(Stmt* s);
    void checkExprStmt(Expr*& e, bool isStep);
    void checkAssign(Expr* e);

    // ---- expressions
    Type* check(Expr*& e, Type* expected, bool consume);
    Type* checkValue(Expr*& e, Type* expected, const char* what = nullptr);
    Type* checkCond(Expr*& e);
    bool coerce(Expr*& e, Type* to, const char* what = nullptr);
    bool canWiden(Type* from, Type* to);
    bool literalFits(Expr* e, Type* to);
    Type* checkName(Expr*& e, Type* expected, bool consume);
    Type* checkMember(Expr*& e, Type* expected, bool consume);
    Type* checkCall(Expr*& e, Type* expected, bool consume);
    Type* checkBinary(Expr*& e, Type* expected, bool consume);
    Type* checkStructLit(Expr* e);
    Type* checkArrayLit(Expr* e, Type* expected);
    bool checkArgs(Expr* call, FuncDecl* f, size_t firstArg, bool isExtern, bool pre = false);
    bool checkBuiltinMethod(Expr* e, Expr* recv, Type* rt, const std::string& name);
    bool checkPattern(Pattern* pt, Type* subj, bool allowBinds, std::vector<VarDecl*>* sink);
    void bindPatVar(PatBind& b, Type* t, std::vector<VarDecl*>* sink);
    bool checkMutablePlace(Expr* e, const std::string& action);
    bool isPlace(Expr* e);
    Expr* boxDeref(Expr* box);
    VarDecl* rootVar(Expr* e);
    void checkFrozen(Expr* e, const std::string& action);
    FuncDecl* toTextMethod(Type* t);
    Expr* wrapToText(Expr* e, FuncDecl* f);
    bool printable(Expr*& e);
    void useAfterMove(Expr* e, VarDecl* v);
    Type* promote(Type* a, Type* b);
};

// ======================================================================== types

Type* Checker::resolveType(TypeExpr* te, Module* m) {
    Type* t = resolveTypeInner(te, m);
    if (te) te->resolved = t;
    return t;
}

Type* Checker::resolveTypeInner(TypeExpr* te, Module* m) {
    if (!te) return g_types.error();
    switch (te->kind) {
        case TypeExpr::Array: {
            Type* e = resolveType(te->args[0], m);
            if (e->k == TK::Void) { g_diag.error(te->span, "can't make an array of `void`"); return g_types.error(); }
            if (e->k == TK::Interface) {
                g_diag.error(te->span, "can't store interface `" + typeName(e) + "` values in an array")
                    .help("store the concrete struct type instead");
                return g_types.error();
            }
            return isErr(e) ? e : g_types.arrayOf(e);
        }
        case TypeExpr::Optional: {
            Type* e = resolveType(te->args[0], m);
            if (e->k == TK::Void) { g_diag.error(te->span, "`void?` isn't a type"); return g_types.error(); }
            if (e->k == TK::Optional) { g_diag.error(te->span, "`" + typeName(e) + "?` is already optional"); return e; }
            if (e->k == TK::Interface) {
                g_diag.error(te->span, "interface types can't be optional").help("use the concrete struct type");
                return g_types.error();
            }
            return isErr(e) ? e : g_types.optOf(e);
        }
        case TypeExpr::Generic: {
            if (!te->ns.empty()) break;
            if (te->name == "List") {
                if (te->args.size() != 1) { g_diag.error(te->span, "`List` takes one type: `List<int>`"); return g_types.error(); }
                Type* e = resolveType(te->args[0], m);
                if (e->k == TK::Void || e->k == TK::Interface) {
                    g_diag.error(te->span, "can't make a `List` of `" + typeName(e) + "`");
                    return g_types.error();
                }
                return isErr(e) ? e : g_types.listOf(e);
            }
            if (te->name == "Result") {
                if (te->args.size() != 2) {
                    g_diag.error(te->span, "`Result` takes two types: `Result<int, string>`").help("the first is the success value, the second the error");
                    return g_types.error();
                }
                Type* a = resolveType(te->args[0], m);
                Type* b = resolveType(te->args[1], m);
                if (isErr(a) || isErr(b)) return g_types.error();
                if (a->k == TK::Void) {
                    g_diag.error(te->args[0]->span, "a Result needs a success value type").help("use `bool` if there is nothing useful to return");
                    return g_types.error();
                }
                return g_types.resultOf(a, b);
            }
            if (te->name == "Box") {
                if (te->args.size() != 1) { g_diag.error(te->span, "`Box` takes one type: `Box<Node>`"); return g_types.error(); }
                Type* e = resolveType(te->args[0], m);
                if (e->k == TK::Void) { g_diag.error(te->span, "can't box `void`"); return g_types.error(); }
                return isErr(e) ? e : g_types.boxOf(e);
            }
            if (te->name == "Map") {
                if (te->args.size() != 2) { g_diag.error(te->span, "`Map` takes two types: `Map<string, int>`"); return g_types.error(); }
                Type* k = resolveType(te->args[0], m);
                Type* v = resolveType(te->args[1], m);
                if (isErr(k) || isErr(v)) return g_types.error();
                bool hashable = isInteger(k) || k->k == TK::String || k->k == TK::Char || k->k == TK::Bool;
                if (!hashable) {
                    g_diag.error(te->args[0]->span, "`" + typeName(k) + "` can't be a map key")
                        .help("map keys can be int, long, byte, char, bool or string");
                    return g_types.error();
                }
                if (v->k == TK::Void || v->k == TK::Interface) {
                    g_diag.error(te->args[1]->span, "a map can't hold `" + typeName(v) + "` values");
                    return g_types.error();
                }
                return g_types.mapOf(k, v);
            }
            break;  // a user-defined generic: Pair<int, string>
        }
        case TypeExpr::Named: break;
    }
    const std::string& n = te->name;
    if (te->ns.empty() && te->kind == TypeExpr::Named) {
        for (size_t i = substStack.size(); i-- > 0;) {
            auto it = substStack[i].find(n);
            if (it != substStack[i].end()) return it->second;
        }
    }
    if (te->ns.empty() && te->kind == TypeExpr::Named) {
        if (n == "int") return g_types.prim(TK::Int);
        if (n == "long") return g_types.prim(TK::Long);
        if (n == "byte") return g_types.prim(TK::Byte);
        if (n == "float") return g_types.prim(TK::Float);
        if (n == "double") return g_types.prim(TK::Double);
        if (n == "bool") return g_types.prim(TK::Bool);
        if (n == "char") return g_types.prim(TK::Char);
        if (n == "string") return g_types.prim(TK::String);
        if (n == "void") return g_types.prim(TK::Void);
        if (n == "List" || n == "Result" || n == "Box" || n == "Map") {
            g_diag.error(te->span, "`" + n + "` needs type arguments")
                .help(n == "List" ? "write `List<int>`" : n == "Result" ? "write `Result<int, string>`" : n == "Box" ? "write `Box<Node>`" : "write `Map<string, int>`");
            return g_types.error();
        }
    }
    Sym* s = nullptr;
    if (!te->ns.empty()) {
        Module* ns = findNs(te->ns, m);
        if (!ns) {
            g_diag.error(te->span, "unknown module prefix `" + te->ns + "`").help("add `using ... as " + te->ns + ";` at the top of the file");
            return g_types.error();
        }
        s = findIn(ns, n);
    } else s = findSym(n, m, te->span);
    if (!s) {
        static const std::map<std::string, std::string> hints = {
            {"Int32", "int"}, {"Integer", "int"}, {"Int64", "long"}, {"String", "string"}, {"Boolean", "bool"},
            {"boolean", "bool"}, {"Single", "float"}, {"Double", "double"}, {"Char", "char"}, {"Byte", "byte"},
            {"uint", "long"}, {"short", "int"}, {"usize", "long"}, {"i32", "int"}, {"i64", "long"}, {"f32", "float"}, {"f64", "double"},
        };
        auto& d = g_diag.error(te->span, "unknown type `" + n + "`");
        auto h = hints.find(n);
        if (h != hints.end()) d.help("in Axon this type is called `" + h->second + "`");
        else {
            std::vector<std::string> cands = {"int", "long", "byte", "float", "double", "bool", "char", "string"};
            for (auto& v : visibleNames(m)) cands.push_back(v);
            std::string c = closestName(n, cands);
            if (!c.empty()) d.help("did you mean `" + c + "`?");
        }
        return g_types.error();
    }
    if (s->isPrivate && s->mod != m) {
        g_diag.error(te->span, "`" + n + "` is private to `" + s->mod->name + "`");
        return g_types.error();
    }
    GenericInfo* gi = s->k == Sym::Struct ? &((StructDecl*)s->d)->gen : s->k == Sym::Enum ? &((EnumDecl*)s->d)->gen : nullptr;
    if (te->kind == TypeExpr::Generic) {
        if (!gi || !gi->isGeneric()) {
            g_diag.error(te->span, "`" + n + "` doesn't take type arguments").help("write just `" + n + "`");
            return g_types.error();
        }
        std::vector<Type*> args;
        for (auto* a : te->args) {
            Type* t = resolveType(a, m);
            if (isErr(t)) return t;
            if (t->k == TK::Void || t->k == TK::Interface) {
                g_diag.error(a->span, "`" + typeName(t) + "` can't be used as a type argument")
                    .help(t->k == TK::Interface ? "use `Box<" + typeName(t) + ">`" : "");
                return g_types.error();
            }
            args.push_back(t);
        }
        if (s->k == Sym::Struct) return instantiateStruct((StructDecl*)s->d, args, te->span);
        return instantiateEnum((EnumDecl*)s->d, args, te->span);
    }
    if (gi && gi->isGeneric()) {
        std::string ps;
        for (size_t i = 0; i < gi->typeParams.size(); i++) ps += (i ? ", " : "") + gi->typeParams[i].name;
        g_diag.error(te->span, "`" + n + "` needs type arguments").help("write `" + n + "<...>` with a type for each of " + ps);
        return g_types.error();
    }
    switch (s->k) {
        case Sym::Struct: return ((StructDecl*)s->d)->type;
        case Sym::Enum: return ((EnumDecl*)s->d)->type;
        case Sym::Iface: return ((InterfaceDecl*)s->d)->type;
        default:
            g_diag.error(te->span, "`" + n + "` is not a type");
            return g_types.error();
    }
}

// ======================================================================== generics

std::string Checker::instKey(const std::vector<Type*>& args) {
    std::string k;
    for (auto* t : args) k += std::to_string((uintptr_t)t) + ",";
    return k;
}

std::string Checker::instName(const std::string& base, const std::vector<Type*>& args) {
    std::string n = base + "<";
    for (size_t i = 0; i < args.size(); i++) n += (i ? ", " : "") + typeName(args[i]);
    return n + ">";
}

std::string Checker::instCName(const std::string& base, const std::vector<Type*>& args) {
    std::string n = base;
    for (auto* t : args) n += "_" + t->mangle;
    return uniqueC(n);
}

bool Checker::checkArity(const GenericInfo& g, const std::vector<Type*>& args, const std::string& name, Span sp) {
    if (args.size() == g.typeParams.size()) return true;
    g_diag.error(sp, "`" + name + "` takes " + std::to_string(g.typeParams.size()) + " type argument" +
                         (g.typeParams.size() == 1 ? "" : "s") + ", but " + std::to_string(args.size()) + " were given");
    return false;
}

// Returns false if a bound is known to fail (then the instance body isn't checked, to avoid confusing errors).
bool Checker::queueBounds(const GenericInfo& g, const std::vector<Type*>& args, Module* m, Span sp) {
    bool ok = true;
    for (size_t i = 0; i < g.typeParams.size() && i < args.size(); i++)
        for (auto* b : g.typeParams[i].bounds) {
            BoundCheck bc{args[i], b, m, sp, g.typeParams[i].name};
            if (implsReady) ok &= checkBound(bc);
            else pendingBounds.push_back(bc);
        }
    return ok;
}

bool Checker::checkBound(const BoundCheck& b) {
    Type* bt = resolveType(b.bound, b.mod);
    if (isErr(bt)) return false;
    if (bt->k != TK::Interface) {
        g_diag.error(b.bound->span, "a bound must be an interface, but `" + typeName(bt) + "` isn't one");
        return false;
    }
    if (b.type != bt && !findImpl(P, b.type, bt->id)) {
        g_diag.error(b.span, "`" + typeName(b.type) + "` doesn't implement `" + typeName(bt) + "`, which `" + b.param + "` requires")
            .help("implement it with `extend " + typeName(b.type) + " : " + typeName(bt) + " { ... }`");
        return false;
    }
    return true;
}

// Runs `f` as if inside the generic declaration: its module, with type parameters mapped.
template <class F>
void Checker::inGenericScope(Module* m, const std::map<std::string, Type*>& subst, F f) {
    Module* savedMod = mod;
    FuncDecl* savedFn = fn;
    mod = m;
    substStack.push_back(subst);
    f();
    substStack.pop_back();
    mod = savedMod;
    fn = savedFn;
}

Type* Checker::instantiateStruct(StructDecl* g, const std::vector<Type*>& args, Span sp) {
    if (!checkArity(g->gen, args, g->name, sp)) return g_types.error();
    std::string key = instKey(args);
    auto it = g->gen.instances.find(key);
    if (it != g->gen.instances.end()) return ((StructDecl*)it->second)->type;
    if (++instDepth > 40) {
        instDepth--;
        g_diag.error(sp, "generic types nest too deeply here (instantiating `" + g->name + "`)").help("a generic probably uses itself with a bigger type argument");
        return g_types.error();
    }
    StructDecl* s = cloneStruct(g);
    s->gen.origin = g;
    s->gen.typeArgs = args;
    for (size_t i = 0; i < args.size(); i++) s->gen.subst[g->gen.typeParams[i].name] = args[i];
    s->name = instName(g->name, args);
    s->cname = instCName(g->cname, args);
    typeNamesC.insert(s->cname);
    g->gen.instances[key] = s;
    P.structs.push_back(s);
    s->type = g_types.structT(s);
    queueBounds(g->gen, args, g->mod, sp);
    inGenericScope(g->mod, s->gen.subst, [&] { resolveStruct(s); });
    for (auto* i : s->ifaces) buildImpl(s->type, i, s->span);
    for (auto* f : s->methods) pendingFuncs.push_back(f);
    pendingStructs.push_back(s);
    instDepth--;
    return s->type;
}

Type* Checker::instantiateEnum(EnumDecl* g, const std::vector<Type*>& args, Span sp) {
    if (!checkArity(g->gen, args, g->name, sp)) return g_types.error();
    std::string key = instKey(args);
    auto it = g->gen.instances.find(key);
    if (it != g->gen.instances.end()) return ((EnumDecl*)it->second)->type;
    if (++instDepth > 40) {
        instDepth--;
        g_diag.error(sp, "generic types nest too deeply here (instantiating `" + g->name + "`)");
        return g_types.error();
    }
    EnumDecl* e = cloneEnum(g);
    e->gen.origin = g;
    e->gen.typeArgs = args;
    for (size_t i = 0; i < args.size(); i++) e->gen.subst[g->gen.typeParams[i].name] = args[i];
    e->name = instName(g->name, args);
    e->cname = instCName(g->cname, args);
    typeNamesC.insert(e->cname);
    g->gen.instances[key] = e;
    P.enums.push_back(e);
    e->type = g_types.enumT(e);
    queueBounds(g->gen, args, g->mod, sp);
    inGenericScope(g->mod, e->gen.subst, [&] { resolveEnum(e); });
    instDepth--;
    return e->type;
}

FuncDecl* Checker::instantiateFunc(FuncDecl* g, const std::vector<Type*>& args, Span sp) {
    if (!checkArity(g->gen, args, g->name, sp)) return nullptr;
    std::string key = instKey(args);
    auto it = g->gen.instances.find(key);
    if (it != g->gen.instances.end()) return (FuncDecl*)it->second;
    FuncDecl* f = cloneFunc(g);
    f->gen.origin = g;
    f->gen.typeArgs = args;
    for (size_t i = 0; i < args.size(); i++) f->gen.subst[g->gen.typeParams[i].name] = args[i];
    f->name = instName(g->name, args);
    g->gen.instances[key] = f;
    bool boundsOk = queueBounds(g->gen, args, g->mod, sp);
    inGenericScope(g->mod, f->gen.subst, [&] { setupFunc(f, nullptr); });
    f->cname = instCName(g->cname, args);
    if (boundsOk) pendingFuncs.push_back(f);
    return f;
}

// Matches a parameter type as written (`List<T>`) against an argument's type, binding type parameters.
void Checker::unify(TypeExpr* te, Type* actual, const std::set<std::string>& params, std::map<std::string, Type*>& out) {
    if (!te || !actual || isErr(actual)) return;
    switch (te->kind) {
        case TypeExpr::Named:
            if (te->ns.empty() && params.count(te->name)) {
                if (!out.count(te->name) && actual->k != TK::None) out[te->name] = actual;
            }
            return;
        case TypeExpr::Array:
            if (actual->k == TK::Array) unify(te->args[0], actual->a, params, out);
            return;
        case TypeExpr::Optional:
            if (actual->k == TK::Optional) unify(te->args[0], actual->a, params, out);
            else unify(te->args[0], actual, params, out);
            return;
        case TypeExpr::Generic: {
            if (te->name == "List" && actual->k == TK::List) unify(te->args[0], actual->a, params, out);
            else if (te->name == "Box" && actual->k == TK::Box) unify(te->args[0], actual->a, params, out);
            else if (te->name == "Result" && actual->k == TK::Result && te->args.size() == 2) {
                unify(te->args[0], actual->a, params, out);
                unify(te->args[1], actual->b, params, out);
            } else if (te->name == "Map" && actual->k == TK::Map && te->args.size() == 2) {
                unify(te->args[0], actual->a, params, out);
                unify(te->args[1], actual->b, params, out);
            } else {
                GenericInfo* gi = actual->k == TK::Struct ? &actual->sd->gen : actual->k == TK::Enum ? &actual->ed->gen : nullptr;
                if (gi && gi->origin) {
                    std::string base = actual->k == TK::Struct ? ((StructDecl*)gi->origin)->name : ((EnumDecl*)gi->origin)->name;
                    if (base == te->name)
                        for (size_t i = 0; i < te->args.size() && i < gi->typeArgs.size(); i++) unify(te->args[i], gi->typeArgs[i], params, out);
                }
            }
            return;
        }
    }
}

// A call to a generic function: check the arguments, infer the type arguments, then check against the instance.
Type* Checker::checkGenericCall(Expr* e, FuncDecl* g, std::vector<TypeExpr*>* explicitArgs, Type* expected) {
    std::vector<Type*> targs;
    if (explicitArgs) {
        for (auto* a : *explicitArgs) targs.push_back(resolveType(a, mod));
        for (auto* t : targs)
            if (isErr(t)) {
                for (auto*& a : e->args) check(a, nullptr, false);
                return g_types.error();
            }
    }
    if (e->args.size() != g->params.size()) {
        g_diag.error(e->span, "`" + g->name + "` takes " + std::to_string(g->params.size()) + " argument" + (g->params.size() == 1 ? "" : "s") +
                                  " but " + std::to_string(e->args.size()) + " were given")
            .note(g->span, "`" + g->name + "` is declared here");
        for (auto*& a : e->args) check(a, nullptr, false);
        return g_types.error();
    }
    // check each argument once, without an expected type
    for (size_t i = 0; i < e->args.size(); i++) {
        Expr*& a = e->args[i];
        if (a->kind == EK::RefArg) {
            a->type = check(a->a, nullptr, false);
        } else check(a, nullptr, g->params[i].mode == 0);
    }
    if (!explicitArgs) {
        std::set<std::string> params;
        for (auto& tp : g->gen.typeParams) params.insert(tp.name);
        std::map<std::string, Type*> bound;
        for (size_t i = 0; i < e->args.size(); i++) {
            Expr* a = e->args[i];
            Type* at = a->kind == EK::RefArg ? a->a->type : a->type;
            unify(g->params[i].type, at, params, bound);
        }
        if (expected && bound.size() < params.size()) unify(g->ret, expected, params, bound);  // `int v = make();`
        for (auto& tp : g->gen.typeParams) {
            auto it = bound.find(tp.name);
            if (it == bound.end()) {
                g_diag.error(e->span, "can't work out the type `" + tp.name + "` for `" + g->name + "`")
                    .help("write it explicitly: `" + g->name + "<...>(...)`");
                return g_types.error();
            }
            targs.push_back(it->second);
        }
    }
    FuncDecl* f = instantiateFunc(g, targs, e->span);
    if (!f) return g_types.error();
    e->callKind = CallKind::Func;
    e->fn = f;
    checkArgs(e, f, 0, false, true);
    return f->retType;
}

// ======================================================================== declarations

void Checker::registerModule(Module* m) {
    for (auto* s : m->structs) {
        addSym(m, s->name, {Sym::Struct, s, s->isPrivate, m, s->span});
        s->cname = s->isExtern ? s->name : uniqueC(s->name);
        typeNamesC.insert(s->cname);
        P.structs.push_back(s);
    }
    for (auto* e : m->enums) {
        addSym(m, e->name, {Sym::Enum, e, e->isPrivate, m, e->span});
        e->cname = uniqueC(e->name);
        typeNamesC.insert(e->cname);
        P.enums.push_back(e);
    }
    for (auto* i : m->ifaces) {
        addSym(m, i->name, {Sym::Iface, i, i->isPrivate, m, i->span});
        i->cname = uniqueC(i->name);
        P.ifaces.push_back(i);
    }
    for (auto* f : m->funcs) {
        if (isBuiltinFuncName(f->name) && !m->isStd) {
            g_diag.error(f->span, "`" + f->name + "` is a built-in function and can't be redefined").help("pick another name");
            continue;
        }
        addSym(m, f->name, {Sym::Func, f, f->isPrivate, m, f->span});
        if (f->isExtern) f->cname = f->name;
        else if (f->isAxonRt) f->cname = "axrt_" + f->name;
        else f->cname = uniqueC("ax_" + f->name);
    }
    for (auto* v : m->globals) {
        addSym(m, v->name, {Sym::Global, v, v->isPrivate, m, v->span});
        v->cname = v->isExtern ? v->name : uniqueC("ax_" + v->name);
    }
}

void Checker::setupFunc(FuncDecl* f, Type* self) {
    f->retType = resolveType(f->ret, mod);
    f->selfType = self;
    if (f->retType->k == TK::Interface) {
        g_diag.error(f->ret->span, "functions can't return an interface").help("return the concrete struct type");
        f->retType = g_types.error();
    }
    std::set<std::string> seen;
    for (auto& p : f->params) {
        VarDecl* v = new VarDecl();
        v->kind = VarDecl::Param;
        v->name = p.name;
        v->span = p.span;
        v->mode = p.mode;
        v->mod = mod;
        v->type = resolveType(p.type, mod);
        if (v->type->k == TK::Void) {
            g_diag.error(p.type->span, "parameters can't be `void`");
            v->type = g_types.error();
        }
        if (v->type->k == TK::Interface && p.mode == 0) {
            g_diag.error(p.span, "interface parameters must be borrowed")
                .lbl("`" + typeName(v->type) + "` has no single size, so it can't be passed by value")
                .help("write `ref " + typeName(v->type) + " " + p.name + "` (or `mut ref` if the function changes it)");
        }
        if (!seen.insert(p.name).second) g_diag.error(p.span, "two parameters are named `" + p.name + "`");
        v->cname = localC(p.name);
        p.var = v;
    }
    if (self && !f->ownerIface) {
        VarDecl* t = new VarDecl();
        t->kind = VarDecl::This;
        t->name = "this";
        t->type = self;
        t->mode = f->isMut ? 2 : 1;
        t->cname = "self";
        t->span = f->span;
        f->thisVar = t;
    }
}

void Checker::checkExternSig(FuncDecl* f) {
    auto ok = [&](Type* t, bool isRef) {
        if (isErr(t)) return true;
        switch (t->k) {
            case TK::Byte: case TK::Int: case TK::Long: case TK::Float: case TK::Double: case TK::Bool:
            case TK::Char: case TK::Void:
                return true;
            case TK::String: return !isRef;
            case TK::Optional: return !isRef && t->a->k == TK::String;  // a `const char*` that may be NULL
            case TK::Struct: return t->sd->isExtern;
            default: return false;
        }
    };
    if (!ok(f->retType, false))
        g_diag.error(f->ret->span, "C functions can't return `" + typeName(f->retType) + "`")
            .help("C functions can use numbers, bool, string, string? and extern structs");
    for (auto& p : f->params)
        if (!ok(p.var->type, p.mode != 0))
            g_diag.error(p.span, "C functions can't take `" + typeName(p.var->type) + "`")
                .help("C functions can use numbers, bool, string, string? (may be NULL) and extern structs (and `ref`/`mut ref` of those)");
}

void Checker::resolveSignatures(Module* m) {
    mod = m;
    for (auto* s : m->structs)
        if (!s->gen.isGeneric()) resolveStruct(s);
    for (auto* e : m->enums)
        if (!e->gen.isGeneric()) resolveEnum(e);
    resolveRest(m);
}

void Checker::resolveStruct(StructDecl* s) {
    Module* m = mod;
    {
        std::set<std::string> names;
        for (auto* f : s->fields) {
            f->rtype = resolveType(f->type, m);
            f->cname = s->isExtern ? f->name : localC(f->name);
            if (f->rtype->k == TK::Void) { g_diag.error(f->type->span, "fields can't be `void`"); f->rtype = g_types.error(); }
            if (f->rtype->k == TK::Interface) {
                g_diag.error(f->type->span, "a struct can't hold an interface value")
                    .help("store the concrete struct type; interfaces can only be borrowed with `ref`");
                f->rtype = g_types.error();
            }
            if (s->isExtern && !isErr(f->rtype)) {
                Type* t = f->rtype;
                bool okT = isNumeric(t) || t->k == TK::Bool || t->k == TK::Char || (t->k == TK::Struct && t->sd->isExtern);
                if (!okT) g_diag.error(f->type->span, "C structs can only contain numbers, bool and other extern structs");
            }
            if (!names.insert(f->name).second) g_diag.error(f->span, "`" + s->name + "` has two fields named `" + f->name + "`");
        }
        for (auto* ie : s->ifaceExprs) {
            Type* it = resolveType(ie, m);
            if (isErr(it)) continue;
            if (it->k != TK::Interface) {
                g_diag.error(ie->span, "`" + typeName(it) + "` is not an interface")
                    .help("Axon has no inheritance: list interfaces after `:` and use fields for composition");
                continue;
            }
            s->ifaces.push_back(it->id);
        }
        for (auto* f : s->methods) {
            setupFunc(f, s->type);
            f->cname = uniqueC(s->cname + "_" + f->name);
            if (!names.insert(f->name).second)
                g_diag.error(f->span, "`" + s->name + "` already has a field or method named `" + f->name + "`");
        }
    }
}

void Checker::resolveEnum(EnumDecl* e) {
    Module* m = mod;
    {
        std::set<std::string> vn;
        for (auto& v : e->variants) {
            if (!vn.insert(v.name).second) g_diag.error(v.span, "`" + e->name + "` has two variants named `" + v.name + "`");
            for (auto* f : v.fields) {
                f->rtype = resolveType(f->type, m);
                f->cname = localC(f->name);
                if (f->rtype->k == TK::Void || f->rtype->k == TK::Interface) {
                    g_diag.error(f->type->span, "an enum value can't be `" + typeName(f->rtype) + "`");
                    f->rtype = g_types.error();
                }
            }
        }
    }
}

void Checker::resolveRest(Module* m) {
    mod = m;
    for (auto* i : m->ifaces)
        for (auto* f : i->methods) setupFunc(f, i->type);
    for (auto* f : m->funcs) {
        if (f->gen.isGeneric()) {
            if (f->isExtern || f->isAxonRt) g_diag.error(f->span, "C functions can't be generic");
            if (f->isTest) g_diag.error(f->span, "test functions can't be generic");
            continue;
        }
        setupFunc(f, nullptr);
        if (f->isExtern) checkExternSig(f);
        if (f->isTest) {
            if (!f->params.empty() || f->retType->k != TK::Void)
                g_diag.error(f->span, "test functions must look like `@test void " + f->name + "()`");
            P.tests.push_back(f);
        }
        if (f->name == "main" && m == P.mainModule) P.mainFn = f;
    }
    for (auto* x : m->extends) {
        x->type = resolveType(x->target, m);
        if (isErr(x->type)) continue;
        if (x->type->k == TK::Interface || x->type->k == TK::Void) {
            g_diag.error(x->target->span, "can't extend `" + typeName(x->type) + "`");
            continue;
        }
        for (auto* ie : x->ifaceExprs) {
            Type* it = resolveType(ie, m);
            if (isErr(it)) continue;
            if (it->k != TK::Interface) { g_diag.error(ie->span, "`" + typeName(it) + "` is not an interface"); continue; }
            x->ifaces.push_back(it->id);
        }
        for (auto* f : x->methods) {
            setupFunc(f, x->type);
            std::string base = x->type->k == TK::Struct || x->type->k == TK::Enum ? x->type->cname : "ext_" + x->type->mangle;
            f->cname = uniqueC(base + "_" + f->name);
            auto& lst = P.extMethods[x->type];
            for (auto* o : lst)
                if (o->name == f->name) g_diag.error(f->span, "`" + typeName(x->type) + "` already has a method `" + f->name + "`").note(o->span, "it is defined here");
            if (x->type->k == TK::Struct)
                for (auto* o : x->type->sd->methods)
                    if (o->name == f->name) g_diag.error(f->span, "`" + typeName(x->type) + "` already has a method `" + f->name + "`").note(o->span, "it is defined here");
            lst.push_back(f);
        }
    }
    for (auto* v : m->globals) {
        if (v->texpr) v->type = resolveType(v->texpr, m);
        if (v->isExtern && v->type && !isErr(v->type)) {
            Type* t = v->type;
            if (!(isNumeric(t) || t->k == TK::Bool || t->k == TK::Char || (t->k == TK::Struct && t->sd->isExtern)))
                g_diag.error(v->span, "C constants can only be numbers, bool or extern structs");
        }
    }
}

FuncDecl* findMethodImpl(Program& P, Type* t, const std::string& name) {
    if (t->k == TK::Struct)
        for (auto* f : t->sd->methods)
            if (f->name == name) return f;
    auto it = P.extMethods.find(t);
    if (it != P.extMethods.end())
        for (auto* f : it->second)
            if (f->name == name) return f;
    return nullptr;
}

void Checker::buildImpl(Type* t, InterfaceDecl* iface, Span sp) {
    if (findImpl(P, t, iface)) return;
    Impl im{t, iface, {}};
    std::vector<std::string> missing;
    bool ok = true;
    for (auto* m : iface->methods) {
        FuncDecl* f = findMethodImpl(P, t, m->name);
        if (!f) {
            std::string sig = std::string(m->isMut ? "mut " : "") + typeName(m->retType) + " " + m->name + "(";
            for (size_t i = 0; i < m->params.size(); i++) {
                if (i) sig += ", ";
                auto& p = m->params[i];
                sig += std::string(p.mode == 2 ? "mut ref " : p.mode == 1 ? "ref " : "") + typeName(p.var->type) + " " + p.name;
            }
            sig += ")";
            missing.push_back(sig);
            ok = false;
            continue;
        }
        bool same = sameType(f->retType, m->retType) && f->params.size() == m->params.size();
        for (size_t i = 0; same && i < f->params.size(); i++)
            same = sameType(f->params[i].var->type, m->params[i].var->type) && f->params[i].mode == m->params[i].mode;
        if (!same) {
            g_diag.error(f->span, "`" + f->name + "` doesn't match the signature required by `" + iface->name + "`")
                .note(m->span, "the interface declares it here");
            ok = false;
        } else if (f->isMut && !m->isMut) {
            g_diag.error(f->span, "`" + f->name + "` is `mut` here but read-only in `" + iface->name + "`")
                .note(m->span, "declared here without `mut`")
                .help("remove `mut`, or add `mut` to the interface method");
            ok = false;
        }
        im.methods.push_back(f);
    }
    if (!missing.empty()) {
        auto& d = g_diag.error(sp, "`" + typeName(t) + "` doesn't implement everything in `" + iface->name + "`");
        for (auto& s : missing) d.help("add the method: `" + s + " { ... }`");
    }
    if (ok) P.impls.push_back(im);
}

void Checker::checkContainment() {
    // A struct may not contain itself by value (directly or through optionals/enums/results).
    std::function<bool(Type*, Type*, std::set<Type*>&)> contains = [&](Type* cur, Type* target, std::set<Type*>& seen) -> bool {
        if (!cur || isErr(cur)) return false;
        if (cur == target) return true;
        if (!seen.insert(cur).second) return false;
        switch (cur->k) {
            case TK::Struct:
                for (auto* f : cur->sd->fields) if (contains(f->rtype, target, seen)) return true;
                return false;
            case TK::Enum:
                for (auto& v : cur->ed->variants)
                    for (auto* f : v.fields) if (contains(f->rtype, target, seen)) return true;
                return false;
            case TK::Optional: return contains(cur->a, target, seen);
            case TK::Result: return contains(cur->a, target, seen) || contains(cur->b, target, seen);
            default: return false;
        }
    };
    auto checkOne = [&](Type* t, const std::vector<FieldDecl*>& fields, Span sp, const std::string& name) {
        for (auto* f : fields) {
            std::set<Type*> seen;
            if (contains(f->rtype, t, seen)) {
                g_diag.error(f->span, "`" + name + "` contains itself through `" + f->name + "`")
                    .lbl("this would make `" + name + "` infinitely big")
                    .help("put it in a `Box<" + typeName(f->rtype) + ">` (or a `List`), which keeps the value on the heap");
                return;
            }
        }
        (void)sp;
    };
    for (auto* s : P.structs)
        if (!s->isExtern && !s->gen.isGeneric()) checkOne(s->type, s->fields, s->span, s->name);
    for (auto* e : P.enums)
        if (!e->gen.isGeneric())
            for (auto& v : e->variants) checkOne(e->type, v.fields, e->span, e->name);

}

// ======================================================================== bodies

void Checker::checkGlobal(VarDecl* v) {
    if (v->isExtern || globalsDone.count(v)) return;
    if (globalsBusy.count(v)) {
        g_diag.error(v->span, "constant `" + v->name + "` depends on itself");
        return;
    }
    globalsBusy.insert(v);
    Module* savedMod = mod;
    FuncDecl* savedFn = fn;
    auto savedScopes = std::move(scopes);
    Flow savedFlow = flow;
    scopes.clear();
    scopes.emplace_back();
    mod = v->mod;
    fn = nullptr;
    if (v->init) {
        Type* t = checkValue(v->init, v->type);
        if (!v->type) v->type = t;
        if (v->type && v->type->k == TK::None) {
            g_diag.error(v->span, "can't tell the type of `" + v->name + "`").help("write the type instead of `var`");
            v->type = g_types.error();
        }
    }
    if (!v->type) v->type = g_types.error();
    scopes = std::move(savedScopes);
    flow = savedFlow;
    mod = savedMod;
    fn = savedFn;
    globalsBusy.erase(v);
    globalsDone.insert(v);
    P.globalOrder.push_back(v);
}

void Checker::checkFieldDefaults(StructDecl* s) {
    const std::map<std::string, Type*> none;
    inGenericScope(s->mod, s->gen.origin ? s->gen.subst : none, [&] {
        for (auto* f : s->fields) {
            if (!f->def) continue;
            fn = nullptr;
            scopes.clear();
            scopes.emplace_back();
            checkValue(f->def, f->rtype, ("the default of `" + f->name + "`").c_str());
        }
    });
}

void Checker::drainGenerics() {
    while (!pendingFuncs.empty() || !pendingStructs.empty()) {
        auto fs = std::move(pendingFuncs);
        pendingFuncs.clear();
        for (auto* f : fs) checkFunc(f);
        auto ss = std::move(pendingStructs);
        pendingStructs.clear();
        for (auto* s : ss) checkFieldDefaults(s);
    }
}

void Checker::checkFunc(FuncDecl* f) {
    if (!f->body || f->checked || f->gen.isGeneric()) return;
    if (f->ownerStruct && f->ownerStruct->gen.isGeneric()) return;
    const std::map<std::string, Type*>* subst = nullptr;
    if (f->gen.origin) subst = &f->gen.subst;
    else if (f->ownerStruct && f->ownerStruct->gen.origin) subst = &f->ownerStruct->gen.subst;
    if (subst) {
        substStack.push_back(*subst);
        checkFuncBody(f);
        substStack.pop_back();
    } else checkFuncBody(f);
}

void Checker::checkFuncBody(FuncDecl* f) {
    f->checked = true;
    fn = f;
    mod = f->mod;
    scopes.clear();
    scopes.emplace_back();
    flow = Flow();
    moveSite.clear();
    declLoop.clear();
    ctxs.clear();
    frozen.clear();
    loopDepth = 0;
    for (auto& p : f->params) declare(p.var);
    checkBlock(f->body, true);
    if (f->retType->k != TK::Void && !isErr(f->retType) && !flow.dead) {
        Span end = f->body->span;
        // point at the closing brace
        const auto& txt = g_diag.file(f->span.file).text;
        int depth = 0;
        for (int i = f->body->span.offset; i < (int)txt.size(); i++) {
            if (txt[i] == '{') depth++;
            else if (txt[i] == '}' && --depth == 0) { end = g_diag.makeSpan(f->span.file, i, 1); break; }
        }
        g_diag.error(end, "`" + f->name + "` doesn't return a value on every path")
            .lbl("it can reach the end without a `return`")
            .help("add `return ...;` before the closing `}`");
    }
    P.funcs.push_back(f);
    fn = nullptr;
}

void Checker::checkBlock(Stmt* s, bool newScope) {
    size_t fz = frozen.size();
    if (newScope) scopes.emplace_back();
    for (auto* st : s->stmts) checkStmt(st);
    if (newScope) scopes.pop_back();
    if (frozen.size() > fz) frozen.erase(frozen.begin() + fz, frozen.end());
}

Type* Checker::checkCond(Expr*& e) {
    Type* t = check(e, g_types.boolT(), false);
    if (!isErr(t) && t->k != TK::Bool) {
        auto& d = g_diag.error(e->span, "a condition must be a `bool`, but this is a `" + typeName(t) + "`");
        if (isNumeric(t)) d.help("compare it explicitly, e.g. `x != 0`");
        else if (t->k == TK::Optional) d.help("test it with `x is " + typeName(t->a) + " value` or `x != none`");
    }
    return t;
}

void Checker::checkLoopBody(Stmt* body, const Flow& entry, Flow* nextIter) {
    ctxs.push_back({false, {}, {}});
    loopDepth++;
    flow = entry;
    checkBlock(body, true);
    Flow iter = flow;
    for (auto& c : ctxs.back().conts) iter = mergeFlows(iter, c);
    // anything moved in the body that was declared outside the loop would be gone on the next pass
    if (!iter.dead) {
        for (VarDecl* v : iter.moved) {
            if (entry.moved.count(v)) continue;
            auto it = declLoop.find(v);
            if (it != declLoop.end() && it->second < loopDepth) {
                g_diag.error(moveSite[v], "`" + v->name + "` is moved inside a loop")
                    .lbl("after the first pass `" + v->name + "` would already be gone")
                    .help("pass `ref " + v->name + "` to lend it, use `" + v->name + ".clone()`, or `break` right after moving it");
            }
        }
    }
    loopDepth--;
    *nextIter = iter;
    Flow exit = entry;
    exit = mergeFlows(exit, iter);
    for (auto& b : ctxs.back().breaks) exit = mergeFlows(exit, b);
    exit.dead = false;
    flow = exit;
    ctxs.pop_back();
}

static bool isTrueLit(Expr* e) { return e && e->kind == EK::BoolLit && e->bval; }

void Checker::checkStmt(Stmt* s) {
    switch (s->kind) {
        case SK::Block: checkBlock(s); break;
        case SK::Var: {
            VarDecl* v = s->var;
            v->kind = VarDecl::Local;
            v->mod = mod;
            Type* declared = v->texpr ? resolveType(v->texpr, mod) : nullptr;
            if (declared && declared->k == TK::Void) {
                g_diag.error(v->texpr->span, "variables can't be `void`");
                declared = g_types.error();
            }
            if (declared && declared->k == TK::Interface) {
                g_diag.error(v->texpr->span, "a variable can't hold an interface value")
                    .help("use the concrete struct type; interfaces can only be borrowed with `ref` in parameters");
                declared = g_types.error();
            }
            Type* t = declared;
            if (v->init) {
                Type* it = checkValue(v->init, declared);
                if (!declared) {
                    t = it;
                    if (t->k == TK::None) {
                        g_diag.error(v->init->span, "can't tell what type `none` is here")
                            .help("write the type: `Player? " + v->name + " = none;`");
                        t = g_types.error();
                    } else if (t->k == TK::Void) {
                        g_diag.error(v->init->span, "this expression doesn't produce a value");
                        t = g_types.error();
                    }
                }
            }
            v->type = t ? t : g_types.error();
            declare(v);
            break;
        }
        case SK::ExprStmt: checkExprStmt(s->expr, false); break;
        case SK::If: {
            scopes.emplace_back();
            auto* savedSink = patSink;
            auto* savedNeg = negSink;
            bool savedNegated = patNegated;
            patSink = &s->patVars;
            negSink = &s->negPatVars;
            patNegated = false;
            checkCond(s->expr);
            patSink = savedSink;
            negSink = savedNeg;
            patNegated = savedNegated;
            Flow afterCond = flow;
            size_t fz = frozen.size();
            freezeBorrows(s->patVars);
            checkBlock(s->then);
            frozen.erase(frozen.begin() + fz, frozen.end());
            Flow thenF = flow;
            scopes.pop_back();
            flow = afterCond;
            if (s->els) {
                scopes.emplace_back();
                for (VarDecl* v : s->negPatVars) declare(v);
                freezeBorrows(s->negPatVars);
                checkStmt(s->els);
                frozen.erase(frozen.begin() + fz, frozen.end());
                scopes.pop_back();
            }
            flow = mergeFlows(thenF, flow);
            // `if (!(x is T v)) return;` - v is usable after the if
            if (thenF.dead && !s->negPatVars.empty()) {
                for (VarDecl* v : s->negPatVars) declare(v);
                freezeBorrows(s->negPatVars);
            }
            break;
        }
        case SK::While: {
            scopes.emplace_back();
            auto* savedSink = patSink;
            patSink = &s->patVars;
            checkCond(s->expr);
            patSink = savedSink;
            Flow next;
            Flow entry = flow;
            checkLoopBody(s->body, entry, &next);
            if (isTrueLit(s->expr) && !ctxs.size()) {}
            scopes.pop_back();
            break;
        }
        case SK::DoWhile: {
            Flow next;
            Flow entry = flow;
            checkLoopBody(s->body, entry, &next);
            checkCond(s->expr);
            break;
        }
        case SK::For: {
            scopes.emplace_back();
            if (s->init) {
                if (s->init->kind == SK::Var) checkStmt(s->init);
                else checkExprStmt(s->init->expr, true);
            }
            if (s->expr) checkCond(s->expr);
            Flow entry = flow;
            Flow next;
            // check the step as part of the loop body flow
            ctxs.push_back({false, {}, {}});
            loopDepth++;
            flow = entry;
            checkBlock(s->body, true);
            Flow iter = flow;
            for (auto& c : ctxs.back().conts) iter = mergeFlows(iter, c);
            flow = iter;
            if (s->step) checkExprStmt(s->step, true);
            iter = flow;
            if (!iter.dead) {
                for (VarDecl* v : iter.moved) {
                    if (entry.moved.count(v)) continue;
                    auto it = declLoop.find(v);
                    if (it != declLoop.end() && it->second < loopDepth)
                        g_diag.error(moveSite[v], "`" + v->name + "` is moved inside a loop")
                            .lbl("after the first pass `" + v->name + "` would already be gone")
                            .help("pass `ref " + v->name + "` to lend it, or use `" + v->name + ".clone()`");
                }
            }
            loopDepth--;
            Flow exit = mergeFlows(entry, iter);
            for (auto& b : ctxs.back().breaks) exit = mergeFlows(exit, b);
            exit.dead = !s->expr && ctxs.back().breaks.empty();
            ctxs.pop_back();
            flow = exit;
            scopes.pop_back();
            break;
        }
        case SK::Foreach: {
            Type* ct = check(s->expr, nullptr, false);
            Type* elem = g_types.error();
            if (!isErr(ct)) {
                if (ct->k == TK::Array || ct->k == TK::List) elem = ct->a;
                else {
                    auto& d = g_diag.error(s->expr->span, "can't loop over a `" + typeName(ct) + "`");
                    if (ct->k == TK::String) d.help("loop over `text.split(\"\")` or use a `for` loop with `byteAt(i)`");
                    else d.help("`foreach` works on arrays (`T[]`) and `List<T>`");
                }
            }
            if (s->foreachMode == 2) checkMutablePlace(s->expr, "change items of");
            if (s->foreachMode == 1) s->foreachMode = 0;
            VarDecl* v = s->var;
            if (v->texpr) {
                Type* dt = resolveType(v->texpr, mod);
                if (!sameType(dt, elem))
                    g_diag.error(v->texpr->span, "the items are `" + typeName(elem) + "`, not `" + typeName(dt) + "`")
                        .help("write `foreach (var " + v->name + " in ...)` to use the right type");
            }
            v->type = elem;
            v->mode = s->foreachMode == 2 ? 2 : (isCopy(elem) ? 0 : 1);
            scopes.emplace_back();
            declare(v);
            VarDecl* root = rootVar(s->expr);
            if (root) frozen.push_back({root, s->expr->span, ""});
            Flow entry = flow;
            Flow next;
            checkLoopBody(s->body, entry, &next);
            if (root) frozen.pop_back();
            scopes.pop_back();
            break;
        }
        case SK::Switch: checkSwitch(s); break;
        case SK::Break:
        case SK::Continue: {
            int idx = (int)ctxs.size() - 1;
            if (s->kind == SK::Continue)
                while (idx >= 0 && ctxs[idx].isSwitch) idx--;
            if (idx < 0) {
                g_diag.error(s->span, std::string("`") + (s->kind == SK::Break ? "break" : "continue") + "` must be inside a loop" +
                                          (s->kind == SK::Break ? " or switch" : ""));
            } else {
                if (s->kind == SK::Break) ctxs[idx].breaks.push_back(flow);
                else ctxs[idx].conts.push_back(flow);
            }
            flow.dead = true;
            break;
        }
        case SK::Return: {
            Type* rt = fn ? fn->retType : g_types.voidT();
            if (s->expr) {
                if (rt->k == TK::Void) {
                    g_diag.error(s->expr->span, "`" + (fn ? fn->name : std::string("this")) + "` returns `void`, so it can't return a value")
                        .help("remove the value, or change the return type");
                    check(s->expr, nullptr, false);
                } else checkValue(s->expr, rt, "the return value");
            } else if (rt->k != TK::Void && !isErr(rt)) {
                g_diag.error(s->span, "`" + fn->name + "` must return a `" + typeName(rt) + "`").help("write `return value;`");
            }
            flow.dead = true;
            break;
        }
    }
}

void Checker::checkExprStmt(Expr*& e, bool isStep) {
    switch (e->kind) {
        case EK::Assign: checkAssign(e); return;
        case EK::IncDec:
        case EK::Call:
        case EK::Try:
            check(e, nullptr, false);
            return;
        default: break;
    }
    check(e, nullptr, false);
    if (!isErr(e->type))
        g_diag.error(e->span, isStep ? "this step doesn't do anything" : "this expression doesn't do anything")
            .help("a statement should be an assignment, a call, or `++`/`--`");
}

Expr* Checker::boxDeref(Expr* box) {
    Expr* d = new Expr();
    d->kind = EK::BoxDeref;
    d->span = box->span;
    d->a = box;
    d->type = box->type->a;
    return d;
}

bool Checker::isPlace(Expr* e) {
    switch (e->kind) {
        case EK::Name: return e->var != nullptr;
        case EK::This: return true;
        case EK::BoxDeref: return true;
        case EK::Member: return e->field >= 0 && isPlace(e->a);
        case EK::Index: return isPlace(e->a);
        default: return false;
    }
}

VarDecl* Checker::rootVar(Expr* e) {
    switch (e->kind) {
        case EK::Name: return e->var;
        case EK::This: return fn ? fn->thisVar : nullptr;
        case EK::Member:
        case EK::Index:
        case EK::BoxDeref: return rootVar(e->a);
        case EK::RefArg: return rootVar(e->a);
        default: return nullptr;
    }
}

void Checker::checkFrozen(Expr* e, const std::string& action) {
    VarDecl* r = rootVar(e);
    if (!r) return;
    for (auto& fz : frozen)
        if (fz.var == r) {
            bool loop = fz.why.empty();
            g_diag.error(e->span, "can't " + action + " `" + r->name + "` " + (loop ? "while a `foreach` is looping over it" : fz.why))
                .note(fz.span, loop ? "the loop over it starts here" : "the borrow starts here")
                .help(loop ? "collect the changes in another list and apply them after the loop"
                           : "finish using the borrowed value first, or copy it out with `.clone()` before changing `" + r->name + "`");
            return;
        }
}

bool Checker::checkMutablePlace(Expr* e, const std::string& action) {
    switch (e->kind) {
        case EK::Name: {
            VarDecl* v = e->var;
            if (!v) return false;
            if (v->isConst || v->kind == VarDecl::Global) {
                g_diag.error(e->span, "can't " + action + " `" + v->name + "` because it is a constant");
                return false;
            }
            if (v->kind == VarDecl::ForeachBind && v->mode != 2) {
                g_diag.error(e->span, "can't " + action + " `" + v->name + "`: foreach variables are read-only")
                    .help("loop with `foreach (mut ref var " + v->name + " in ...)` to change the items in place");
                return false;
            }
            if (v->kind == VarDecl::PatBind && v->mode == 1) {
                g_diag.error(e->span, "can't " + action + " `" + v->name + "`: it borrows the value inside the pattern");
                return false;
            }
            if (v->mode == 1) {
                g_diag.error(e->span, "can't " + action + " `" + v->name + "` because it is borrowed read-only")
                    .help("declare the parameter as `mut ref " + typeName(v->type) + " " + v->name + "` and pass it with `mut ref`");
                return false;
            }
            checkFrozen(e, action);
            return true;
        }
        case EK::This: {
            if (fn && fn->thisVar && fn->thisVar->mode != 2) {
                g_diag.error(e->span, "can't " + action + " this struct inside `" + fn->name + "()` because the method isn't marked `mut`")
                    .note(fn->span, "`" + fn->name + "` is declared here")
                    .help("write `mut " + typeName(fn->retType) + " " + fn->name + "(...)` to let it change the struct");
                return false;
            }
            return true;
        }
        case EK::Member: {
            if (e->field < 0) {
                g_diag.error(e->span, "can't " + action + " `" + e->name + "`");
                return false;
            }
            if (e->a->kind == EK::This && fn && fn->thisVar && fn->thisVar->mode != 2) {
                g_diag.error(e->span, "can't " + action + " `" + e->name + "` inside `" + fn->name + "()` because the method isn't marked `mut`")
                    .note(fn->span, "`" + fn->name + "` is declared here")
                    .help("write `mut " + typeName(fn->retType) + " " + fn->name + "(...)` to let it change the struct");
                return false;
            }
            if (!isPlace(e->a)) {
                g_diag.error(e->span, "can't " + action + " a field of a temporary value").help("store the value in a variable first");
                return false;
            }
            return checkMutablePlace(e->a, action);
        }
        case EK::Index:
        case EK::BoxDeref:
            if (!isPlace(e->a)) {
                g_diag.error(e->span, "can't " + action + (e->kind == EK::Index ? " an item of a temporary value" : " the inside of a temporary box"));
                return false;
            }
            return checkMutablePlace(e->a, action);
        default:
            g_diag.error(e->span, "can't " + action + " this expression").lbl("this isn't a variable, field or item");
            return false;
    }
}

void Checker::checkAssign(Expr* e) {
    Expr*& target = e->a;
    // the left side: resolve without counting it as a read of a moved variable
    Type* tt;
    bool wholeVar = false;
    if (target->kind == EK::Name && e->op == T::Assign) {
        VarDecl* v = lookupLocal(target->name);
        if (v) {
            target->var = v;
            target->type = v->type;
            tt = v->type;
            wholeVar = true;
        } else tt = check(target, nullptr, false);
    } else tt = check(target, nullptr, false);
    if (isErr(tt)) {
        check(e->b, nullptr, false);
        e->type = g_types.voidT();
        return;
    }
    checkMutablePlace(target, "change");
    if (e->op == T::Assign) {
        checkValue(e->b, tt, "the new value");
    } else {
        Type* rt = check(e->b, isNumeric(tt) ? tt : nullptr, false);
        bool ok = false;
        if (isErr(rt)) ok = true;
        else if (e->op == T::PlusEq && tt->k == TK::String) {
            ok = printable(e->b);
            e->isConcat = true;
        } else if (isNumeric(tt) || tt->k == TK::Char) {
            bool intOp = e->op == T::AmpEq || e->op == T::PipeEq || e->op == T::CaretEq || e->op == T::ShlEq || e->op == T::ShrEq || e->op == T::PercentEq;
            if (intOp && e->op != T::PercentEq && (!isInteger(tt) || !isInteger(rt))) ok = false;
            else ok = isNumeric(rt) && (canWiden(rt, tt) || rt == tt || literalFits(e->b, tt));
            if (ok && rt != tt) coerce(e->b, tt);
            if (!ok && isNumeric(rt))
                g_diag.error(e->b->span, "can't combine a `" + typeName(rt) + "` into a `" + typeName(tt) + "` without losing data")
                    .help("convert explicitly: `(" + typeName(tt) + ")(" + srcText(e->b->span) + ")`");
            if (!ok && isNumeric(rt)) ok = true;  // already reported
        } else if (tt->k == TK::Bool && (e->op == T::AmpEq || e->op == T::PipeEq || e->op == T::CaretEq)) {
            ok = rt->k == TK::Bool;
        }
        if (!ok)
            g_diag.error(e->span, "can't use `" + srcText(Span{e->span.file, 0, 0}) + "` this operator on `" + typeName(tt) + "` and `" + typeName(rt) + "`");
    }
    if (wholeVar) flow.moved.erase(target->var);
    e->type = g_types.voidT();
}

// ======================================================================== expressions

bool Checker::literalFits(Expr* e, Type* to) {
    if (e->kind == EK::IntLit) {
        int64_t v = e->ival;
        switch (to->k) {
            case TK::Byte: return v >= 0 && v <= 255;
            case TK::Int: return v >= INT32_MIN && v <= INT32_MAX && !e->isLongLit;
            case TK::Long: return true;
            case TK::Float: case TK::Double: return true;
            case TK::Char: return false;
            default: return false;
        }
    }
    if (e->kind == EK::FloatLit) return to->k == TK::Float || to->k == TK::Double;
    return false;
}

bool Checker::canWiden(Type* from, Type* to) {
    if (from == to) return true;
    if (from->k == TK::Char) return to->k == TK::Int || to->k == TK::Long || to->k == TK::Double;
    int a = numRank(from), b = numRank(to);
    if (a < 0 || b < 0) return false;
    return a < b;
}

bool Checker::coerce(Expr*& e, Type* to, const char* what) {
    Type* from = e->type;
    if (!to || isErr(from) || isErr(to) || from == to) return true;
    auto wrap = [&](ImplicitKind k) {
        Expr* w = new Expr();
        w->kind = EK::Implicit;
        w->span = e->span;
        w->a = e;
        w->type = to;
        w->implicit = k;
        w->consumed = e->consumed;
        e = w;
    };
    if (to->k == TK::Box && to->a->k == TK::Interface && from->k == TK::Box && from->a->k != TK::Interface) {
        if (findImpl(P, from->a, to->a->id)) {
            wrap(ImplicitKind::BoxToDyn);
            return true;
        }
        g_diag.error(e->span, "`" + typeName(from->a) + "` doesn't implement `" + typeName(to->a) + "`")
            .help("add `: " + typeName(to->a) + "` to `" + typeName(from->a) + "` and implement its methods");
        e->type = g_types.error();
        return false;
    }
    if (to->k == TK::Optional) {
        if (from->k == TK::None) { e->type = to; return true; }
        if (from == to->a) { wrap(ImplicitKind::WrapOptional); return true; }
        if ((e->kind == EK::IntLit || e->kind == EK::FloatLit) && literalFits(e, to->a)) {
            e->type = to->a;
            wrap(ImplicitKind::WrapOptional);
            return true;
        }
        if (canWiden(from, to->a)) {
            Type* inner = to->a;
            Expr* w = new Expr();
            w->kind = EK::Implicit;
            w->span = e->span;
            w->a = e;
            w->type = inner;
            w->implicit = ImplicitKind::Numeric;
            e = w;
            wrap(ImplicitKind::WrapOptional);
            return true;
        }
    }
    if ((e->kind == EK::IntLit || e->kind == EK::FloatLit) && literalFits(e, to)) {
        e->type = to;
        return true;
    }
    if (canWiden(from, to)) { wrap(ImplicitKind::Numeric); return true; }

    std::string ctx = what ? std::string(" for ") + what : "";
    auto& d = g_diag.error(e->span, "expected `" + typeName(to) + "`" + ctx + ", found `" + typeName(from) + "`");
    d.lbl("this is a `" + typeName(from) + "`");
    std::string src = srcText(e->span);
    if (e->kind == EK::IntLit && isNumeric(to)) {
        d.msg = "the number " + std::to_string(e->ival) + " doesn't fit in a `" + typeName(to) + "`";
        if (to->k == TK::Byte) d.help("a `byte` holds 0 to 255");
        else if (to->k == TK::Int) d.help("use `long` for bigger numbers");
    } else if (isNumeric(from) && isNumeric(to)) {
        d.help("this conversion can lose data; convert explicitly: `(" + typeName(to) + ")" + (src.find(' ') != std::string::npos ? "(" + src + ")" : src) + "`");
    } else if (from->k == TK::Optional && from->a == to) {
        d.help("`" + typeName(from) + "` might be empty; unwrap it with `if (" + src + " is " + typeName(to) + " value)` or give a fallback: `" + src + " ?? ...`");
    } else if (from->k == TK::None) {
        d.help("only optional types can be `none`; make the type `" + typeName(to) + "?`");
    } else if (to->k == TK::String && (isNumeric(from) || from->k == TK::Bool || from->k == TK::Char)) {
        d.help("turn it into text with `\"\" + " + src + "` or `$\"{" + src + "}\"`");
    } else if (from->k == TK::String && isNumeric(to)) {
        d.help("parse it with `" + src + ".toInt() ?? 0`");
    } else if (to->k == TK::Result && (from == to->a)) {
        d.help("wrap it: `Ok(" + src + ")`");
    } else if (to->k == TK::Result && (from == to->b)) {
        d.help("wrap it: `Err(" + src + ")`");
    } else if (from->k == TK::Array && to->k == TK::List && from->a == to->a) {
        d.help("write the items directly as `[...]` where a `List` is expected");
    }
    e->type = g_types.error();
    return false;
}

Type* Checker::checkValue(Expr*& e, Type* expected, const char* what) {
    Type* t = check(e, expected, true);
    if (expected) {
        coerce(e, expected, what);
        if (isErr(e->type)) return expected;
        return expected;
    }
    return t;
}

void Checker::useAfterMove(Expr* e, VarDecl* v) {
    auto& d = g_diag.error(e->span, "`" + v->name + "` was moved and can't be used anymore").lbl("used here after the move");
    auto it = moveSite.find(v);
    if (it != moveSite.end()) d.note(it->second, "`" + v->name + "` was moved here, so its value now belongs somewhere else");
    d.help("to lend it instead, pass `ref " + v->name + "`; to keep both, use `" + v->name + ".clone()`");
    flow.moved.erase(v);  // report once
}

FuncDecl* Checker::toTextMethod(Type* t) {
    FuncDecl* f = findMethodImpl(P, t, "toText");
    if (f && f->params.empty() && f->retType->k == TK::String) return f;
    return nullptr;
}

Expr* Checker::wrapToText(Expr* e, FuncDecl* f) {
    Expr* m = new Expr();
    m->kind = EK::Member;
    m->span = e->span;
    m->a = e;
    m->name = "toText";
    Expr* c = new Expr();
    c->kind = EK::Call;
    c->span = e->span;
    c->a = m;
    c->callKind = CallKind::Method;
    c->fn = f;
    c->type = g_types.stringT();
    return c;
}

bool Checker::printable(Expr*& e) {
    Type* t = e->type;
    if (isErr(t)) return true;
    switch (t->k) {
        case TK::Byte: case TK::Int: case TK::Long: case TK::Float: case TK::Double: case TK::Bool:
        case TK::Char: case TK::String:
            return true;
        default: break;
    }
    if (FuncDecl* f = toTextMethod(t)) {
        e = wrapToText(e, f);
        return true;
    }
    auto& d = g_diag.error(e->span, "can't turn a `" + typeName(t) + "` into text");
    if (t->k == TK::Struct || t->k == TK::Enum)
        d.help("add a method `string toText() { ... }` to `" + typeName(t) + "` (or in an `extend` block)");
    else if (t->k == TK::Optional)
        d.help("unwrap it first: `x ?? fallback` or `if (x is " + typeName(t->a) + " v)`");
    return false;
}

Type* Checker::promote(Type* a, Type* b) {
    if (isErr(a) || isErr(b)) return g_types.error();
    int ra = a->k == TK::Char ? 1 : numRank(a);
    int rb = b->k == TK::Char ? 1 : numRank(b);
    int r = std::max(std::max(ra, rb), 1);
    static const TK ks[] = {TK::Byte, TK::Int, TK::Long, TK::Float, TK::Double};
    return g_types.prim(ks[r]);
}

static bool isLit(Expr* e) {
    return e->kind == EK::IntLit || e->kind == EK::FloatLit || e->kind == EK::None;
}

Type* Checker::check(Expr*& e, Type* expected, bool consume) {
    // Pattern variables may only come from `is` reached through `!`, `&&` and `||`.
    struct SinkGuard {
        Checker* c;
        std::vector<VarDecl*>*p, *n;
        bool neg;
        ~SinkGuard() { c->patSink = p; c->negSink = n; c->patNegated = neg; }
    } guard{this, patSink, negSink, patNegated};
    bool passes = e->kind == EK::Is || (e->kind == EK::Unary && e->op == T::Bang) ||
                  (e->kind == EK::Binary && (e->op == T::AndAnd || e->op == T::OrOr));
    if (!passes) {
        patSink = nullptr;
        negSink = nullptr;
    }
    Type* t = g_types.error();
    switch (e->kind) {
        case EK::IntLit: {
            Type* hint = expected && expected->k == TK::Optional ? expected->a : expected;
            if (hint && isNumeric(hint) && literalFits(e, hint)) t = hint;
            else if (e->isLongLit || e->ival > INT32_MAX || e->ival < INT32_MIN) t = g_types.longT();
            else t = g_types.intT();
            break;
        }
        case EK::FloatLit: {
            Type* hint = expected && expected->k == TK::Optional ? expected->a : expected;
            t = e->isF32 || (hint && hint->k == TK::Float) ? g_types.prim(TK::Float) : g_types.doubleT();
            break;
        }
        case EK::StrLit: t = g_types.stringT(); break;
        case EK::CharLit: t = g_types.prim(TK::Char); break;
        case EK::BoolLit: t = g_types.boolT(); break;
        case EK::None: t = expected && expected->k == TK::Optional ? expected : g_types.prim(TK::None); break;
        case EK::Interp: {
            for (auto*& a : e->args) {
                check(a, nullptr, false);
                printable(a);
            }
            t = g_types.stringT();
            break;
        }
        case EK::This:
            if (!fn || !fn->thisVar) {
                g_diag.error(e->span, "`this` only exists inside methods");
                break;
            }
            t = fn->selfType;
            if (consume && isOwned(t)) {
                g_diag.error(e->span, "can't move `this` out of a method").help("return `this.clone()` to give away a copy");
            }
            break;
        case EK::Name: t = checkName(e, expected, consume); e->consumed = consume; return t;
        case EK::Member: t = checkMember(e, expected, consume); e->consumed = consume; return t;
        case EK::Call: t = checkCall(e, expected, consume); e->consumed = consume; return t;
        case EK::BoxDeref: t = e->type; break;
        case EK::Index: {
            Type* bt = check(e->a, nullptr, false);
            if (!isErr(bt) && bt->k == TK::Box && bt->a->k != TK::Interface) {
                e->a = boxDeref(e->a);  // indexing a boxed list
                bt = e->a->type;
            }
            if (!isErr(bt) && bt->k == TK::Map) {
                // m[key]: reading needs the key to exist; assigning adds it
                check(e->b, bt->a, false);
                coerce(e->b, bt->a, "the key");
                t = bt->b;
                if (consume && isOwned(t))
                    g_diag.error(e->span, "can't move a value out of a map").help("use `.remove(key)` to take it out, or `.clone()` for a copy");
                break;
            }
            Type* it = check(e->b, g_types.intT(), false);
            if (!isErr(it) && !isInteger(it))
                g_diag.error(e->b->span, "an index must be a whole number, not `" + typeName(it) + "`");
            else if (!isErr(it) && it->k != TK::Int) coerce(e->b, it->k == TK::Long ? it : g_types.intT());
            if (isErr(bt)) break;
            if (bt->k == TK::Array || bt->k == TK::List) t = bt->a;
            else if (bt->k == TK::String) {
                g_diag.error(e->span, "strings can't be indexed with `[]`").help("use `text.byteAt(i)` or `text.substring(i, 1)`");
            } else g_diag.error(e->a->span, "can't index into a `" + typeName(bt) + "`").help("`[i]` works on arrays and lists");
            if (consume && !isErr(t) && isOwned(t))
                g_diag.error(e->span, "can't move an item out of a " + std::string(bt->k == TK::List ? "list" : "array"))
                    .help("use `" + srcText(e->span) + ".clone()` for a copy" +
                          std::string(bt->k == TK::List ? ", or `removeAt(i)` to take it out" : ""));
            break;
        }
        case EK::Unary: {
            if (e->op == T::Bang) {
                std::swap(patSink, negSink);
                patNegated = !patNegated;
            }
            Type* at = check(e->a, expected, false);
            if (e->op == T::Bang) {
                std::swap(patSink, negSink);
                patNegated = !patNegated;
            }
            if (isErr(at)) break;
            if (e->op == T::Bang) {
                if (at->k != TK::Bool) g_diag.error(e->span, "`!` needs a `bool`, found `" + typeName(at) + "`");
                t = g_types.boolT();
            } else if (e->op == T::Minus) {
                if (!isNumeric(at)) g_diag.error(e->span, "`-` needs a number, found `" + typeName(at) + "`");
                else t = promote(at, at);
            } else if (e->op == T::Tilde) {
                if (!isInteger(at)) g_diag.error(e->span, "`~` needs a whole number, found `" + typeName(at) + "`");
                else t = promote(at, at);
            }
            break;
        }
        case EK::Binary: t = checkBinary(e, expected, consume); break;
        case EK::Assign:
            g_diag.error(e->span, "an assignment can't be used as a value").help("put the assignment on its own line; use `==` to compare");
            check(e->b, nullptr, false);
            break;
        case EK::Ternary: {
            checkCond(e->a);
            Flow afterCond = flow;
            Type* tb = check(e->b, expected, consume);
            Flow fb = flow;
            flow = afterCond;
            Type* tc = check(e->c, expected ? expected : (isLit(e->c) ? tb : nullptr), consume);
            flow = mergeFlows(fb, flow);
            if (expected && expected->k != TK::None) {
                coerce(e->b, expected);
                coerce(e->c, expected);
                t = expected;
            } else if (isErr(tb) || isErr(tc)) t = g_types.error();
            else if (tb == tc) t = tb;
            else if (isNumeric(tb) && isNumeric(tc)) {
                t = promote(tb, tc);
                coerce(e->b, t);
                coerce(e->c, t);
            } else if (tb->k == TK::None && tc->k != TK::None) {
                t = tc->k == TK::Optional ? tc : g_types.optOf(tc);
                coerce(e->b, t);
                coerce(e->c, t);
            } else if (tc->k == TK::None && tb->k != TK::None) {
                t = tb->k == TK::Optional ? tb : g_types.optOf(tb);
                coerce(e->b, t);
                coerce(e->c, t);
            } else if (tb->k == TK::Optional && tb->a == tc) {
                t = tb;
                coerce(e->c, t);
            } else if (tc->k == TK::Optional && tc->a == tb) {
                t = tc;
                coerce(e->b, t);
            } else {
                g_diag.error(e->span, "the two sides of `? :` have different types: `" + typeName(tb) + "` and `" + typeName(tc) + "`");
            }
            break;
        }
        case EK::Cast: {
            Type* to = resolveType(e->texpr, mod);
            Type* from = check(e->a, nullptr, false);
            t = to;
            if (isErr(from) || isErr(to)) break;
            bool ok = (isNumeric(from) || from->k == TK::Char) && (isNumeric(to) || to->k == TK::Char);
            if (from == to) ok = true;
            if (!ok) {
                auto& d = g_diag.error(e->span, "can't convert `" + typeName(from) + "` to `" + typeName(to) + "`");
                if (to->k == TK::String) d.help("use `\"\" + x` or `$\"{x}\"` to make text");
                else if (from->k == TK::String) d.help("use `x.toInt()` or `x.toDouble()`, which return an optional");
                else if (to->k == TK::Bool) d.help("compare instead, e.g. `x != 0`");
            }
            break;
        }
        case EK::StructLit: t = checkStructLit(e); break;
        case EK::ArrayLit: t = checkArrayLit(e, expected); break;
        case EK::NewArray: {
            Type* et = resolveType(e->texpr, mod);
            Type* nt = check(e->a, g_types.intT(), false);
            if (!isErr(nt) && !isInteger(nt)) g_diag.error(e->a->span, "an array size must be a whole number");
            else if (!isErr(nt) && nt->k != TK::Int && nt->k != TK::Long) coerce(e->a, g_types.intT());
            if (!isErr(et)) {
                if (et->k == TK::Void || et->k == TK::Interface) g_diag.error(e->texpr->span, "can't make an array of `" + typeName(et) + "`");
                else t = g_types.arrayOf(et);
            }
            break;
        }
        case EK::GenericNew: {
            TypeExpr* te = e->texpr;
            if (te->kind == TypeExpr::Generic && te->name == "List" && te->ns.empty()) {
                t = resolveType(te, mod);
                if (!e->args.empty()) {
                    g_diag.error(e->span, "`List<...>()` takes no arguments").help("start with items using `[a, b, c]`");
                }
                e->callKind = CallKind::ListNew;
            } else if (te->kind == TypeExpr::Generic && te->name == "Map" && te->ns.empty()) {
                t = resolveType(te, mod);
                if (!e->args.empty()) g_diag.error(e->span, "`Map<...>()` takes no arguments");
                e->callKind = CallKind::MapNew;
            } else if (te->kind == TypeExpr::Generic && [&] {
                           Sym* s = te->ns.empty() ? findSym(te->name, mod, te->span, false)
                                                   : (findNs(te->ns, mod) ? findIn(findNs(te->ns, mod), te->name) : nullptr);
                           return s && s->k == Sym::Func;
                       }()) {
                // max<int>(a, b): a generic function with explicit type arguments
                Sym* s = te->ns.empty() ? findSym(te->name, mod, te->span, false) : findIn(findNs(te->ns, mod), te->name);
                FuncDecl* g = (FuncDecl*)s->d;
                if (!g->gen.isGeneric()) {
                    g_diag.error(te->span, "`" + g->name + "` isn't generic").help("remove the `<...>`");
                    for (auto*& a : e->args) check(a, nullptr, false);
                    break;
                }
                t = checkGenericCall(e, g, &te->args);
                e->kind = EK::Call;
                e->type = t;
                e->consumed = consume;
                return t;
            } else {
                Type* rt = resolveType(te, mod);
                if (!isErr(rt)) g_diag.error(e->span, "`" + typeName(rt) + "` can't be created this way").help(rt->k == TK::Struct ? "use `new " + typeName(rt) + " { field = value }`" : "");
            }
            break;
        }
        case EK::Is: {
            auto* sink = patSink;
            bool negated = patNegated;
            Type* st = check(e->a, nullptr, false);
            t = g_types.boolT();
            if (isErr(st)) break;
            curIsSubject = e->a;
            declareBinds = !negated;
            checkPattern(e->pat, st, true, sink);
            declareBinds = true;
            curIsSubject = nullptr;
            break;
        }
        case EK::Try: {
            Type* ot = check(e->a, nullptr, true);
            if (isErr(ot)) break;
            Type* rt = fn ? fn->retType : nullptr;
            if (ot->k == TK::Result) {
                t = ot->a;
                if (!rt || rt->k != TK::Result) {
                    g_diag.error(e->span, "`try` can only be used in a function that returns a `Result`")
                        .help(fn ? "change the return type of `" + fn->name + "` to `Result<..., " + typeName(ot->b) + ">`, or handle the error with `switch`" : "");
                } else if (!sameType(rt->b, ot->b)) {
                    g_diag.error(e->span, "this error is a `" + typeName(ot->b) + "`, but `" + fn->name + "` returns errors of type `" + typeName(rt->b) + "`")
                        .help("handle it with `switch`, or make both use the same error type");
                }
            } else if (ot->k == TK::Optional) {
                t = ot->a;
                if (!rt || rt->k != TK::Optional)
                    g_diag.error(e->span, "`try` on an optional can only be used in a function that returns an optional")
                        .help("use `if (x is T value)` or `x ?? fallback` instead");
            } else {
                g_diag.error(e->span, "`try` needs a `Result` or an optional, found `" + typeName(ot) + "`");
            }
            break;
        }
        case EK::IncDec: {
            Type* at = check(e->a, nullptr, false);
            if (isErr(at)) break;
            if (!isNumeric(at) && at->k != TK::Char) g_diag.error(e->span, "`++`/`--` need a number, found `" + typeName(at) + "`");
            else checkMutablePlace(e->a, "change");
            t = at;
            break;
        }
        case EK::RefArg:
            g_diag.error(e->span, "`ref` is only used when passing an argument to a function").help("remove `ref` here");
            t = check(e->a, expected, false);
            break;
        case EK::Implicit: t = e->type; break;
    }
    e->type = t;
    e->consumed = consume;
    return t;
}

Type* Checker::checkName(Expr*& e, Type* expected, bool consume) {
    (void)expected;
    const std::string& name = e->name;
    if (VarDecl* v = lookupLocal(name)) {
        e->var = v;
        e->type = v->type;
        if (flow.moved.count(v)) useAfterMove(e, v);
        if (consume && !isErr(v->type) && isOwned(v->type)) {
            if (v->mode != 0) {
                g_diag.error(e->span, "can't give away `" + name + "` because it is only borrowed")
                    .help("use `" + name + ".clone()` to pass a copy");
            } else {
                bool isFrozen = false;
                for (auto& fz : frozen)
                    if (fz.var == v) isFrozen = true;
                if (isFrozen) checkFrozen(e, "move");
                else {
                    flow.moved.insert(v);
                    moveSite[v] = e->span;
                }
            }
        }
        return v->type;
    }
    // a field of `this` written without `this.`
    if (fn && fn->thisVar && fn->selfType->k == TK::Struct) {
        for (auto* f : fn->selfType->sd->fields)
            if (f->name == name) {
                Expr* th = new Expr();
                th->kind = EK::This;
                th->span = e->span;
                th->type = fn->selfType;
                Expr* m = new Expr();
                m->kind = EK::Member;
                m->span = e->span;
                m->a = th;
                m->name = name;
                e = m;
                return checkMember(e, expected, consume);
            }
    }
    Sym* s = findSym(name, mod, e->span);
    if (s) {
        if (s->isPrivate && s->mod != mod) {
            g_diag.error(e->span, "`" + name + "` is private to `" + s->mod->name + "`");
            return e->type = g_types.error();
        }
        switch (s->k) {
            case Sym::Global: {
                VarDecl* v = (VarDecl*)s->d;
                if (!v->isExtern) checkGlobal(v);
                e->var = v;
                return e->type = v->type ? v->type : g_types.error();
            }
            case Sym::Func:
                g_diag.error(e->span, "`" + name + "` is a function").help("call it: `" + name + "(...)`");
                return e->type = g_types.error();
            case Sym::Struct:
                g_diag.error(e->span, "`" + name + "` is a type, not a value").help("build one with `new " + name + " { ... }`");
                return e->type = g_types.error();
            case Sym::Enum:
                g_diag.error(e->span, "`" + name + "` is an enum type").help("pick a variant, e.g. `" + name + "." +
                                                                               (((EnumDecl*)s->d)->variants.empty() ? std::string("X") : ((EnumDecl*)s->d)->variants[0].name) + "`");
                return e->type = g_types.error();
            case Sym::Iface:
                g_diag.error(e->span, "`" + name + "` is an interface, not a value");
                return e->type = g_types.error();
        }
    }
    if (findNs(name, mod)) {
        g_diag.error(e->span, "`" + name + "` is a module prefix").help("use it like `" + name + ".Name`");
        return e->type = g_types.error();
    }
    auto& d = g_diag.error(e->span, "unknown name `" + name + "`").lbl("not found in this scope");
    if (name == "Console" || name == "System") d.help("use `print(...)` to write a line of text");
    else if (name == "self") d.help("Axon uses `this` for the current struct");
    else {
        std::vector<std::string> cands = visibleNames(mod);
        if (fn && fn->selfType && fn->selfType->k == TK::Struct)
            for (auto* f : fn->selfType->sd->fields) cands.push_back(f->name);
        std::string c = closestName(name, cands);
        if (!c.empty()) d.help("did you mean `" + c + "`?");
    }
    return e->type = g_types.error();
}

Type* Checker::checkMember(Expr*& e, Type* expected, bool consume) {
    (void)expected;
    Expr* base = e->a;
    // `rl.RAYWHITE`, `Item.Empty`
    if (base->kind == EK::Name && !lookupLocal(base->name)) {
        if (Module* ns = findNs(base->name, mod)) {
            Sym* s = findIn(ns, e->name);
            if (!s) {
                g_diag.error(e->span, "`" + base->name + "` has nothing named `" + e->name + "`");
                return e->type = g_types.error();
            }
            if (s->k == Sym::Global) {
                VarDecl* v = (VarDecl*)s->d;
                if (!v->isExtern) checkGlobal(v);
                Expr* n = new Expr(*e);
                n->kind = EK::Name;
                n->name = e->name;
                n->var = v;
                n->type = v->type;
                n->a = nullptr;
                e = n;
                return e->type;
            }
            g_diag.error(e->span, "`" + base->name + "." + e->name + "` isn't a value");
            return e->type = g_types.error();
        }
        Sym* s = findSym(base->name, mod, base->span, false);
        if (s && s->k == Sym::Enum && !(fn && fn->selfType && fn->selfType->k == TK::Struct && [&] {
                for (auto* f : fn->selfType->sd->fields) if (f->name == base->name) return true;
                return false;
            }())) {
            EnumDecl* ed = (EnumDecl*)s->d;
            if (ed->gen.isGeneric()) {
                // `Option.Nothing`: the instance comes from the expected type
                if (expected && expected->k == TK::Optional) expected = expected->a;
                if (expected && expected->k == TK::Enum && expected->ed->gen.origin == ed) ed = expected->ed;
                else {
                    g_diag.error(e->span, "can't tell which `" + ed->name + "<...>` this is")
                        .help("use it where the type is known, e.g. `" + ed->name + "<int> x = " + ed->name + "." + e->name + ";`");
                    return e->type = g_types.error();
                }
            }
            for (size_t i = 0; i < ed->variants.size(); i++)
                if (ed->variants[i].name == e->name) {
                    if (!ed->variants[i].fields.empty()) {
                        std::string args;
                        for (size_t j = 0; j < ed->variants[i].fields.size(); j++)
                            args += (j ? ", " : "") + ed->variants[i].fields[j]->name;
                        g_diag.error(e->span, "`" + ed->name + "." + e->name + "` needs values").help("write `" + ed->name + "." + e->name + "(" + args + ")`");
                        return e->type = g_types.error();
                    }
                    Expr* c = new Expr(*e);
                    c->kind = EK::Call;
                    c->callKind = CallKind::EnumCtor;
                    c->ed = ed;
                    c->variant = (int)i;
                    c->args.clear();
                    c->type = ed->type;
                    e = c;
                    return e->type;
                }
            std::vector<std::string> vs;
            for (auto& v : ed->variants) vs.push_back(v.name);
            auto& d = g_diag.error(e->span, "`" + ed->name + "` has no variant `" + e->name + "`");
            std::string c = closestName(e->name, vs);
            if (!c.empty()) d.help("did you mean `" + ed->name + "." + c + "`?");
            return e->type = g_types.error();
        }
    }
    Type* bt = check(e->a, nullptr, false);
    if (isErr(bt)) return e->type = g_types.error();
    const std::string& name = e->name;
    if (bt->k == TK::Box && bt->a->k != TK::Interface) {
        Expr* d = boxDeref(e->a);
        if (name == "value") {
            // `b.value` is the boxed value itself
            if (consume && isOwned(d->type))
                g_diag.error(e->span, "can't move the value out of a `Box`").help("move the whole box, or use `.value.clone()`");
            d->span = e->span;
            e = d;
            return e->type;
        }
        e->a = d;  // fields of the boxed value: `node.left` on a Box<Node>
        bt = d->type;
    }
    if (bt->k == TK::Struct) {
        StructDecl* sd = bt->sd;
        for (size_t i = 0; i < sd->fields.size(); i++)
            if (sd->fields[i]->name == name) {
                FieldDecl* f = sd->fields[i];
                if (f->isPrivate && sd->mod != mod) {
                    g_diag.error(e->span, "field `" + name + "` of `" + sd->name + "` is private");
                }
                e->field = (int)i;
                e->type = f->rtype;
                if (consume && isOwned(f->rtype)) {
                    g_diag.error(e->span, "can't move `" + srcText(e->span) + "` out of `" + srcText(e->a->span) + "`")
                        .lbl("this field would be left empty")
                        .help("use `" + srcText(e->span) + ".clone()` to get a copy");
                }
                return e->type;
            }
    }
    if (name == "length" && (bt->k == TK::String || bt->k == TK::Array || bt->k == TK::List || bt->k == TK::Map)) {
        e->builtin = Builtin::PropLength;
        return e->type = g_types.intT();
    }
    if (name == "hasValue" && bt->k == TK::Optional) {
        e->builtin = Builtin::PropHasValue;
        return e->type = g_types.boolT();
    }
    auto& d = g_diag.error(e->span, "`" + typeName(bt) + "` has no field `" + name + "`");
    if (FuncDecl* m = findMethodImpl(P, bt, name)) {
        (void)m;
        d.help("`" + name + "` is a method; call it: `" + srcText(e->span) + "()`");
    } else if ((name == "Length" || name == "Count" || name == "size" || name == "count") &&
               (bt->k == TK::String || bt->k == TK::Array || bt->k == TK::List)) {
        d.help("use `.length`");
    } else if (bt->k == TK::Struct) {
        std::vector<std::string> fs;
        for (auto* f : bt->sd->fields) fs.push_back(f->name);
        std::string c = closestName(name, fs);
        if (!c.empty()) d.help("did you mean `" + c + "`?");
    }
    return e->type = g_types.error();
}

bool Checker::checkArgs(Expr* call, FuncDecl* f, size_t firstArg, bool isExtern, bool pre) {
    size_t n = call->args.size() - firstArg;
    if (n != f->params.size()) {
        auto& d = g_diag.error(call->span, "`" + f->name + "` takes " + std::to_string(f->params.size()) + " argument" +
                                               (f->params.size() == 1 ? "" : "s") + " but " + std::to_string(n) + (n == 1 ? " was" : " were") + " given");
        d.note(f->span, "`" + f->name + "` is declared here");
        for (auto*& a : call->args) check(a, nullptr, false);
        return false;
    }
    std::vector<std::pair<VarDecl*, Expr*>> mutRoots;
    for (size_t i = 0; i < n; i++) {
        Param& p = f->params[i];
        Expr*& a = call->args[firstArg + i];
        Type* pt = p.var->type;
        if (p.mode == 0) {
            if (a->kind == EK::RefArg) {
                g_diag.error(a->span, "`" + f->name + "` takes `" + p.name + "` by value, not as a reference").help("remove `ref`");
                a = a->a;
            }
            if (pre) {
                coerce(a, pt, ("`" + p.name + "`").c_str());
            } else if (isExtern || (f->isAxonRt && pt->k == TK::String)) {
                check(a, pt, false);  // C functions only read strings
                coerce(a, pt, ("`" + p.name + "`").c_str());
            } else {
                std::string what = "`" + p.name + "`";
                checkValue(a, pt, what.c_str());
            }
        } else {
            const char* kw = p.mode == 2 ? "mut ref" : "ref";
            if (a->kind != EK::RefArg) {
                if (!pre) check(a, nullptr, false);
                g_diag.error(a->span, "`" + f->name + "` borrows `" + p.name + "`, so the argument needs `" + kw + "`")
                    .help("write `" + std::string(kw) + " " + srcText(a->span) + "`");
                continue;
            }
            if (a->isMut != (p.mode == 2)) {
                g_diag.error(a->span, p.mode == 2 ? "`" + f->name + "` changes `" + p.name + "`, so pass it with `mut ref`"
                                                  : "`" + f->name + "` only reads `" + p.name + "`, so pass it with `ref`")
                    .help("write `" + std::string(kw) + " " + srcText(a->a->span) + "`");
            }
            Type* it = pre ? a->a->type : check(a->a, nullptr, false);
            a->type = it;
            if (!it || isErr(it) || isErr(pt)) continue;
            // `ref Iface` from a `Box<Iface>`: lend the boxed value
            if (pt->k == TK::Interface && it->k == TK::Box && it->a == pt) {
                a->implicit = ImplicitKind::BoxToDyn;
                a->type = pt;
                if (p.mode == 2) checkMutablePlace(a->a, "lend as `mut ref`");
                continue;
            }
            if (p.mode == 2) {
                checkMutablePlace(a->a, "lend as `mut ref`");
                if (VarDecl* r = rootVar(a->a)) mutRoots.push_back({r, a});
            }
            if (pt->k == TK::Interface && it != pt) {
                if (!findImpl(P, it, pt->id)) {
                    g_diag.error(a->a->span, "`" + typeName(it) + "` doesn't implement `" + typeName(pt) + "`")
                        .help("add `: " + typeName(pt) + "` to the declaration of `" + typeName(it) + "` and implement its methods");
                } else {
                    a->implicit = ImplicitKind::IfaceRef;
                    a->type = pt;
                    a->sd = nullptr;
                }
            } else if (it != pt) {
                g_diag.error(a->a->span, "expected `" + typeName(pt) + "` for `" + p.name + "`, found `" + typeName(it) + "`")
                    .help("references must point at exactly the right type");
            }
        }
    }
    // a `mut ref` argument can't be used anywhere else in the same call
    for (auto& mr : mutRoots) {
        for (size_t i = firstArg; i < call->args.size(); i++) {
            Expr* a = call->args[i];
            if (a == mr.second) continue;
            if (rootVar(a) == mr.first) {
                g_diag.error(a->span, "`" + mr.first->name + "` is lent as `mut ref` in this call, so it can't be used again here")
                    .note(mr.second->span, "lent as `mut ref` here")
                    .help("make a copy first, or split this into two calls");
            }
        }
    }
    return true;
}

bool Checker::checkBuiltinMethod(Expr* e, Expr* recv, Type* rt, const std::string& name) {
    // `own`: the method takes ownership of its arguments (list.add), otherwise strings are only read
    auto args = [&](std::initializer_list<Type*> ts, bool own = false) -> bool {
        if (e->args.size() != ts.size()) {
            g_diag.error(e->span, "`" + name + "` takes " + std::to_string(ts.size()) + " argument" + (ts.size() == 1 ? "" : "s") +
                                      ", but " + std::to_string(e->args.size()) + " were given");
            for (auto*& a : e->args) check(a, nullptr, false);
            return false;
        }
        size_t i = 0;
        for (Type* t : ts) {
            Expr*& a = e->args[i++];
            if (!own && (t->k == TK::String || isCopy(t))) {
                check(a, t, false);
                coerce(a, t);
            } else checkValue(a, t);
        }
        return true;
    };
    auto set = [&](Builtin b, Type* t) {
        e->callKind = CallKind::BuiltinMethod;
        e->builtin = b;
        e->type = t;
        return true;
    };
    Type* S = g_types.stringT();
    Type* I = g_types.intT();
    if (name == "clone" && e->args.empty()) return set(Builtin::Clone, rt);
    if (rt->k == TK::String) {
        if (name == "substring") { args({I, I}); return set(Builtin::StrSubstring, S); }
        if (name == "contains") { args({S}); return set(Builtin::StrContains, g_types.boolT()); }
        if (name == "startsWith") { args({S}); return set(Builtin::StrStartsWith, g_types.boolT()); }
        if (name == "endsWith") { args({S}); return set(Builtin::StrEndsWith, g_types.boolT()); }
        if (name == "indexOf") { args({S}); return set(Builtin::StrIndexOf, I); }
        if (name == "trim") { args({}); return set(Builtin::StrTrim, S); }
        if (name == "toUpper") { args({}); return set(Builtin::StrToUpper, S); }
        if (name == "toLower") { args({}); return set(Builtin::StrToLower, S); }
        if (name == "split") { args({S}); return set(Builtin::StrSplit, g_types.arrayOf(S)); }
        if (name == "toInt") { args({}); return set(Builtin::StrToInt, g_types.optOf(I)); }
        if (name == "toDouble") { args({}); return set(Builtin::StrToDouble, g_types.optOf(g_types.doubleT())); }
        if (name == "byteAt") { args({I}); return set(Builtin::StrByteAt, g_types.prim(TK::Byte)); }
        if (name == "replace") { args({S, S}); return set(Builtin::StrReplace, S); }
        if (name == "chars") { args({}); return set(Builtin::StrChars, g_types.arrayOf(g_types.prim(TK::Char))); }
        if (name == "lastIndexOf") { args({S}); return set(Builtin::StrLastIndexOf, I); }
        if (name == "padLeft") { args({I}); return set(Builtin::StrPadLeft, S); }
        if (name == "padRight") { args({I}); return set(Builtin::StrPadRight, S); }
        if (name == "repeat") { args({I}); return set(Builtin::StrRepeat, S); }
        if (name == "isEmpty") { args({}); return set(Builtin::StrIsEmpty, g_types.boolT()); }
    }
    if (rt->k == TK::List || rt->k == TK::Array) {
        Type* el = rt->a;
        bool equatable = isNumeric(el) || el->k == TK::String || el->k == TK::Char || el->k == TK::Bool ||
                         (el->k == TK::Enum && [&] {
                             for (auto& v : el->ed->variants) if (!v.fields.empty()) return false;
                             return true;
                         }());
        bool sortable = isNumeric(el) || el->k == TK::String || el->k == TK::Char;
        if (name == "contains" || name == "indexOf") {
            if (!equatable) {
                g_diag.error(e->span, "`" + name + "` needs items that can be compared with `==`, but these are `" + typeName(el) + "`")
                    .help("loop over the items and compare the fields you care about");
                return set(Builtin::SeqContains, g_types.error());
            }
            args({el});
            return set(name == "contains" ? Builtin::SeqContains : Builtin::SeqIndexOf, name == "contains" ? g_types.boolT() : I);
        }
        if (name == "sort" || name == "reverse") {
            if (isPlace(recv)) checkMutablePlace(recv, "change");
            checkFrozen(recv, "change");
            if (name == "sort" && !sortable) {
                g_diag.error(e->span, "`sort` works on numbers, chars and strings, not `" + typeName(el) + "`");
                return set(Builtin::SeqSort, g_types.error());
            }
            args({});
            return set(name == "sort" ? Builtin::SeqSort : Builtin::SeqReverse, g_types.voidT());
        }
    }
    if (rt->k == TK::Map) {
        bool mutating = name == "remove" || name == "clear";
        if (mutating) {
            if (isPlace(recv)) checkMutablePlace(recv, "change");
            checkFrozen(recv, "change");
        }
        if (name == "get") { args({rt->a}); return set(Builtin::MapGet, g_types.optOf(rt->b)); }
        if (name == "contains") { args({rt->a}); return set(Builtin::MapContains, g_types.boolT()); }
        if (name == "remove") { args({rt->a}); return set(Builtin::MapRemove, g_types.optOf(rt->b)); }
        if (name == "keys") { args({}); return set(Builtin::MapKeys, g_types.arrayOf(rt->a)); }
        if (name == "values") { args({}); return set(Builtin::MapValues, g_types.arrayOf(rt->b)); }
        if (name == "clear") { args({}); return set(Builtin::MapClear, g_types.voidT()); }
    }
    if (rt->k == TK::List) {
        bool mutating = name == "add" || name == "pop" || name == "removeAt" || name == "insert" || name == "clear";
        if (mutating) {
            if (isPlace(recv)) checkMutablePlace(recv, "change");
            checkFrozen(recv, "change");
        }
        if (name == "add") { args({rt->a}, true); return set(Builtin::ListAdd, g_types.voidT()); }
        if (name == "pop") { args({}); return set(Builtin::ListPop, rt->a); }
        if (name == "removeAt") { args({I}); return set(Builtin::ListRemoveAt, rt->a); }
        if (name == "insert") { args({I, rt->a}, true); return set(Builtin::ListInsert, g_types.voidT()); }
        if (name == "clear") { args({}); return set(Builtin::ListClear, g_types.voidT()); }
    }
    return false;
}

Type* Checker::checkCall(Expr*& e, Type* expected, bool consume) {
    (void)consume;
    Expr*& callee = e->a;
    if (callee->kind == EK::Name && !lookupLocal(callee->name)) {
        const std::string& name = callee->name;
        bool isMethodOfThis = false;
        if (fn && fn->thisVar && findMethodImpl(P, fn->selfType, name)) isMethodOfThis = true;
        if (!isMethodOfThis) {
            if (name == "print" || name == "write") {
                e->callKind = CallKind::Builtin;
                e->builtin = name == "print" ? Builtin::Print : Builtin::Write;
                if (e->args.size() > 1) g_diag.error(e->span, "`" + name + "` takes one value").help("join values with `+` or use `$\"...{x}...\"`");
                for (auto*& a : e->args) {
                    check(a, nullptr, false);
                    printable(a);
                }
                return e->type = g_types.voidT();
            }
            if (name == "assert") {
                e->callKind = CallKind::Builtin;
                e->builtin = Builtin::Assert;
                if (e->args.empty() || e->args.size() > 2) g_diag.error(e->span, "`assert` takes a condition and an optional message");
                if (!e->args.empty()) checkCond(e->args[0]);
                if (e->args.size() > 1) { check(e->args[1], g_types.stringT(), false); coerce(e->args[1], g_types.stringT()); }
                return e->type = g_types.voidT();
            }
            if (name == "panic") {
                e->callKind = CallKind::Builtin;
                e->builtin = Builtin::Panic;
                if (e->args.size() != 1) g_diag.error(e->span, "`panic` takes one message");
                else { check(e->args[0], g_types.stringT(), false); coerce(e->args[0], g_types.stringT()); }
                flow.dead = true;
                return e->type = g_types.voidT();
            }
            if (name == "Ok" || name == "Err") {
                bool ok = name == "Ok";
                e->callKind = ok ? CallKind::ResultOk : CallKind::ResultErr;
                Type* rt = expected && expected->k == TK::Result ? expected : (fn && fn->retType->k == TK::Result ? fn->retType : nullptr);
                if (e->args.size() != 1) {
                    g_diag.error(e->span, "`" + name + "` takes one value");
                    for (auto*& a : e->args) check(a, nullptr, false);
                    return e->type = rt ? rt : g_types.error();
                }
                if (!rt) {
                    check(e->args[0], nullptr, true);
                    g_diag.error(e->span, "can't tell which `Result` type this `" + name + "(...)` is").help("use it where a `Result<..., ...>` is expected");
                    return e->type = g_types.error();
                }
                checkValue(e->args[0], ok ? rt->a : rt->b);
                return e->type = rt;
            }
            if (name == "Box") {
                e->callKind = CallKind::BoxNew;
                if (e->args.size() != 1) {
                    g_diag.error(e->span, "`Box` takes one value: `Box(value)`");
                    for (auto*& a : e->args) check(a, nullptr, false);
                    return e->type = g_types.error();
                }
                Type* want = expected && expected->k == TK::Box && expected->a->k != TK::Interface ? expected->a : nullptr;
                Type* t = checkValue(e->args[0], want);
                if (isErr(t)) return e->type = g_types.error();
                if (t->k == TK::Void || t->k == TK::None) {
                    g_diag.error(e->args[0]->span, "can't box this value");
                    return e->type = g_types.error();
                }
                return e->type = g_types.boxOf(t);
            }
            Sym* s = findSym(name, mod, callee->span);
            if (s && s->k == Sym::Func && ((FuncDecl*)s->d)->gen.isGeneric()) {
                FuncDecl* g = (FuncDecl*)s->d;
                if (g->isPrivate && g->mod != mod) g_diag.error(callee->span, "`" + name + "` is private to `" + g->mod->name + "`");
                return e->type = checkGenericCall(e, g, nullptr, expected);
            }
            if (s && s->k == Sym::Func) {
                FuncDecl* f = (FuncDecl*)s->d;
                if (f->isPrivate && f->mod != mod) g_diag.error(callee->span, "`" + name + "` is private to `" + f->mod->name + "`");
                e->callKind = CallKind::Func;
                e->fn = f;
                checkArgs(e, f, 0, f->isExtern);
                return e->type = f->retType;
            }
            if (s && s->k == Sym::Struct) {
                g_diag.error(e->span, "`" + name + "` is a struct, not a function").help("build it with `new " + name + " { field = value, ... }`");
                for (auto*& a : e->args) check(a, nullptr, false);
                return e->type = g_types.error();
            }
            if (!s) {
                auto& d = g_diag.error(callee->span, "unknown function `" + name + "`");
                if (name == "println" || name == "printf" || name == "puts") d.help("use `print(...)`");
                else if (name == "Map" || name == "List") d.help("create one with `" + name + "<...>()`");
                else {
                    std::vector<std::string> cands = visibleNames(mod);
                    cands.push_back("print");
                    std::string c = closestName(name, cands);
                    if (!c.empty()) d.help("did you mean `" + c + "`?");
                    else if (mod) d.help("if it's in another file, add `using module.name;` at the top");
                }
                for (auto*& a : e->args) check(a, nullptr, false);
                return e->type = g_types.error();
            }
            check(callee, nullptr, false);  // reports "is a ..." errors
            for (auto*& a : e->args) check(a, nullptr, false);
            return e->type = g_types.error();
        }
        // implicit `this.method(...)`
        Expr* th = new Expr();
        th->kind = EK::This;
        th->span = callee->span;
        Expr* m = new Expr();
        m->kind = EK::Member;
        m->span = callee->span;
        m->a = th;
        m->name = name;
        callee = m;
    }
    if (callee->kind != EK::Member) {
        Type* ct = check(callee, nullptr, false);
        if (!isErr(ct)) g_diag.error(callee->span, "this isn't a function, it's a `" + typeName(ct) + "`");
        for (auto*& a : e->args) check(a, nullptr, false);
        return e->type = g_types.error();
    }
    Expr*& recv = callee->a;
    const std::string& mname = callee->name;
    // rl.InitWindow(...) / Item.Sword(10)
    if (recv->kind == EK::Name && !lookupLocal(recv->name)) {
        if (Module* ns = findNs(recv->name, mod)) {
            Sym* s = findIn(ns, mname);
            if (!s || s->k != Sym::Func) {
                g_diag.error(callee->span, "`" + recv->name + "` has no function `" + mname + "`");
                for (auto*& a : e->args) check(a, nullptr, false);
                return e->type = g_types.error();
            }
            FuncDecl* f = (FuncDecl*)s->d;
            if (f->gen.isGeneric()) return e->type = checkGenericCall(e, f, nullptr, expected);
            e->callKind = CallKind::Func;
            e->fn = f;
            checkArgs(e, f, 0, f->isExtern);
            return e->type = f->retType;
        }
        Sym* s = findSym(recv->name, mod, recv->span, false);
        bool fieldNamed = false;
        if (fn && fn->selfType && fn->selfType->k == TK::Struct)
            for (auto* f : fn->selfType->sd->fields) fieldNamed |= f->name == recv->name;
        if (s && s->k == Sym::Enum && !fieldNamed) {
            EnumDecl* ed = (EnumDecl*)s->d;
            int vi = -1;
            for (size_t i = 0; i < ed->variants.size(); i++)
                if (ed->variants[i].name == mname) vi = (int)i;
            if (vi < 0) {
                g_diag.error(callee->span, "`" + ed->name + "` has no variant `" + mname + "`");
                for (auto*& a : e->args) check(a, nullptr, false);
                return e->type = g_types.error();
            }
            if (ed->gen.isGeneric()) {
                // Option.Some(5): the instance comes from the expected type, or from the values
                EnumDecl* inst = nullptr;
                if (expected && expected->k == TK::Enum && expected->ed->gen.origin == ed) inst = expected->ed;
                Variant& gv = ed->variants[vi];
                if (!inst) {
                    if (e->args.size() != gv.fields.size()) {
                        g_diag.error(e->span, "`" + ed->name + "." + gv.name + "` holds " + std::to_string(gv.fields.size()) + " value(s)");
                        for (auto*& a : e->args) check(a, nullptr, false);
                        return e->type = g_types.error();
                    }
                    std::set<std::string> params;
                    for (auto& tp : ed->gen.typeParams) params.insert(tp.name);
                    std::map<std::string, Type*> bound;
                    for (size_t i = 0; i < e->args.size(); i++) unify(gv.fields[i]->type, check(e->args[i], nullptr, true), params, bound);
                    std::vector<Type*> targs;
                    for (auto& tp : ed->gen.typeParams) {
                        if (!bound.count(tp.name)) {
                            g_diag.error(e->span, "can't work out `" + tp.name + "` for `" + ed->name + "." + gv.name + "`")
                                .help("give the variable a type, e.g. `" + ed->name + "<int> x = ...`");
                            return e->type = g_types.error();
                        }
                        targs.push_back(bound[tp.name]);
                    }
                    Type* it = instantiateEnum(ed, targs, e->span);
                    if (isErr(it)) return e->type = it;
                    e->callKind = CallKind::EnumCtor;
                    e->ed = it->ed;
                    e->variant = vi;
                    for (size_t i = 0; i < e->args.size(); i++) coerce(e->args[i], it->ed->variants[vi].fields[i]->rtype);
                    return e->type = it;
                }
                ed = inst;
            }
            Variant& v = ed->variants[vi];
            e->callKind = CallKind::EnumCtor;
            e->ed = ed;
            e->variant = vi;
            if (e->args.size() != v.fields.size()) {
                g_diag.error(e->span, "`" + ed->name + "." + v.name + "` holds " + std::to_string(v.fields.size()) + " value" +
                                          (v.fields.size() == 1 ? "" : "s") + ", but " + std::to_string(e->args.size()) + " were given")
                    .note(v.span, "the variant is declared here");
                for (auto*& a : e->args) check(a, nullptr, false);
            } else {
                for (size_t i = 0; i < v.fields.size(); i++) checkValue(e->args[i], v.fields[i]->rtype);
            }
            return e->type = ed->type;
        }
        if (s && s->k == Sym::Struct && !fieldNamed) {
            g_diag.error(e->span, "Axon structs don't have static methods").help("make `" + mname + "` a regular function");
            for (auto*& a : e->args) check(a, nullptr, false);
            return e->type = g_types.error();
        }
    }
    Type* rt = check(recv, nullptr, false);
    if (isErr(rt)) {
        for (auto*& a : e->args) check(a, nullptr, false);
        return e->type = g_types.error();
    }
    if (rt->k == TK::Box && mname != "clone") {
        if (rt->a->k == TK::Interface) {
            // Box<Drawable>: dynamic dispatch through the vtable
            for (auto* m : rt->a->id->methods)
                if (m->name == mname) {
                    e->callKind = CallKind::DynMethod;
                    e->fn = m;
                    if (m->isMut && isPlace(recv)) checkMutablePlace(recv, "call `mut` method `" + mname + "` on");
                    checkArgs(e, m, 0, false);
                    return e->type = m->retType;
                }
            g_diag.error(callee->span, "interface `" + typeName(rt->a) + "` has no method `" + mname + "`");
            for (auto*& a : e->args) check(a, nullptr, false);
            return e->type = g_types.error();
        }
        recv = boxDeref(recv);  // methods of the boxed value
        rt = recv->type;
    }
    callee->type = rt;
    if (rt->k == TK::Interface) {
        for (auto* m : rt->id->methods)
            if (m->name == mname) {
                e->callKind = CallKind::IfaceMethod;
                e->fn = m;
                if (m->isMut) {
                    VarDecl* r = rootVar(recv);
                    if (r && r->mode != 2)
                        g_diag.error(callee->span, "`" + mname + "` changes the value, but `" + r->name + "` is borrowed read-only")
                            .help("take it as `mut ref " + typeName(rt) + " " + r->name + "`");
                }
                checkArgs(e, m, 0, false);
                return e->type = m->retType;
            }
        g_diag.error(callee->span, "interface `" + typeName(rt) + "` has no method `" + mname + "`");
        for (auto*& a : e->args) check(a, nullptr, false);
        return e->type = g_types.error();
    }
    if (FuncDecl* m = findMethodImpl(P, rt, mname)) {
        if (m->isPrivate && m->mod != mod) g_diag.error(callee->span, "method `" + mname + "` is private");
        e->callKind = CallKind::Method;
        e->fn = m;
        if (m->isMut) {
            if (isPlace(recv)) checkMutablePlace(recv, "call `mut` method `" + mname + "` on");
            else
                g_diag.error(recv->span, "can't call `mut` method `" + mname + "` on a temporary value")
                    .help("store the value in a variable first");
        }
        checkArgs(e, m, 0, false);
        return e->type = m->retType;
    }
    if (checkBuiltinMethod(e, recv, rt, mname)) return e->type;
    auto& d = g_diag.error(callee->span, "`" + typeName(rt) + "` has no method `" + mname + "`");
    std::vector<std::string> cands;
    if (rt->k == TK::Struct) for (auto* f : rt->sd->methods) cands.push_back(f->name);
    auto it = P.extMethods.find(rt);
    if (it != P.extMethods.end()) for (auto* f : it->second) cands.push_back(f->name);
    if (rt->k == TK::String) cands.insert(cands.end(), {"substring", "contains", "startsWith", "endsWith", "indexOf", "trim", "toUpper", "toLower", "split", "toInt", "toDouble", "byteAt", "replace", "clone"});
    if (rt->k == TK::List) cands.insert(cands.end(), {"add", "pop", "removeAt", "insert", "clear", "clone"});
    if (mname == "Add" || mname == "push" || mname == "append") cands.push_back("add");
    std::string c = closestName(mname, cands);
    if (mname == "push" || mname == "append" || mname == "Add") c = rt->k == TK::List ? "add" : c;
    if (!c.empty()) d.help("did you mean `" + c + "`?");
    else if (rt->k == TK::Struct) d.help("add the method to `" + typeName(rt) + "`, or in an `extend " + typeName(rt) + " { ... }` block");
    for (auto*& a : e->args) check(a, nullptr, false);
    return e->type = g_types.error();
}

Type* Checker::checkBinary(Expr*& e, Type* expected, bool consume) {
    T op = e->op;
    if (op == T::AndAnd || op == T::OrOr) {
        // `a && b` true: both true (positive bindings ok). `a || b` false: both false (negative bindings ok).
        auto* savedSink = patSink;
        auto* savedNeg = negSink;
        if (op == T::OrOr) patSink = nullptr;
        else negSink = nullptr;
        checkCond(e->a);
        Flow afterA = flow;
        checkCond(e->b);
        flow = mergeFlows(afterA, flow);
        patSink = savedSink;
        negSink = savedNeg;
        return g_types.boolT();
    }
    auto* savedSink = patSink;
    patSink = nullptr;
    struct Restore { std::vector<VarDecl*>** p; std::vector<VarDecl*>* v; ~Restore() { *p = v; } } restore{&patSink, savedSink};
    if (op == T::QQ) {
        Type* at = check(e->a, nullptr, consume);
        if (isErr(at)) { check(e->b, nullptr, consume); return g_types.error(); }
        if (at->k != TK::Optional) {
            g_diag.error(e->a->span, "`??` needs an optional on the left, but this is a `" + typeName(at) + "`")
                .help("`" + typeName(at) + "` always has a value, so the fallback is never used");
            check(e->b, nullptr, consume);
            return at;
        }
        Flow afterA = flow;
        Type* bt = check(e->b, at->a, consume);
        flow = mergeFlows(afterA, flow);
        if (bt == at) return at;
        coerce(e->b, at->a, "the fallback value");
        return at->a;
    }
    if ((op == T::EqEq || op == T::NotEq) && (e->a->kind == EK::None || e->b->kind == EK::None)) {
        Expr*& other = e->a->kind == EK::None ? e->b : e->a;
        Type* ot = check(other, nullptr, false);
        if (!isErr(ot) && ot->k != TK::Optional)
            g_diag.error(other->span, "`" + typeName(ot) + "` is never `none`").help("only optional types like `" + typeName(ot) + "?` can be `none`");
        e->isNoneCompare = true;
        (e->a->kind == EK::None ? e->a : e->b)->type = ot;
        return g_types.boolT();
    }
    Type *ta, *tb;
    if (isLit(e->a) && !isLit(e->b)) {
        tb = check(e->b, nullptr, false);
        ta = check(e->a, isNumeric(tb) ? tb : nullptr, false);
    } else {
        ta = check(e->a, nullptr, false);
        tb = check(e->b, isNumeric(ta) ? ta : nullptr, false);
    }
    if (isErr(ta) || isErr(tb)) return g_types.error();
    std::string opText = srcText(e->span);
    auto bad = [&](const std::string& hint = "") {
        auto& d = g_diag.error(e->span, "can't use `" + opText + "` on `" + typeName(ta) + "` and `" + typeName(tb) + "`");
        if (!hint.empty()) d.help(hint);
        return g_types.error();
    };
    bool numA = isNumeric(ta) || ta->k == TK::Char, numB = isNumeric(tb) || tb->k == TK::Char;
    switch (op) {
        case T::Plus:
            if (ta->k == TK::String || tb->k == TK::String) {
                if (!printable(e->a) || !printable(e->b)) return g_types.stringT();
                e->isConcat = true;
                return g_types.stringT();
            }
            [[fallthrough]];
        case T::Minus: case T::Star: case T::Slash: case T::Percent:
            if (!numA || !numB) {
                if (ta->k == TK::Optional || tb->k == TK::Optional)
                    return bad("unwrap the optional first: `x ?? 0`");
                return bad();
            }
            return promote(ta, tb);
        case T::Amp: case T::Pipe: case T::Caret:
            if (ta->k == TK::Bool && tb->k == TK::Bool) return g_types.boolT();
            if (!isInteger(ta) || !isInteger(tb)) return bad("bit operations need whole numbers");
            return promote(ta, tb);
        case T::Shl: case T::Shr:
            if (!isInteger(ta) || !isInteger(tb)) return bad("shifts need whole numbers");
            return promote(ta, ta);
        case T::Lt: case T::Gt: case T::Le: case T::Ge:
            if (!numA || !numB) return bad(ta->k == TK::String ? "strings can only be compared with `==` and `!=`" : "");
            return g_types.boolT();
        case T::EqEq: case T::NotEq: {
            if (numA && numB) return g_types.boolT();
            if (ta != tb) return bad();
            if (ta->k == TK::Bool || ta->k == TK::String) return g_types.boolT();
            if (ta->k == TK::Enum) {
                for (auto& v : ta->ed->variants)
                    if (!v.fields.empty())
                        return bad("enums that carry values are compared with `switch` or `is`, e.g. `x is " + v.name + "(...)`");
                return g_types.boolT();
            }
            if (ta->k == TK::Optional) return bad("compare with `none`, or unwrap with `is` first");
            return bad("structs can't be compared with `==`; compare their fields");
        }
        default: return bad();
    }
}

Type* Checker::checkStructLit(Expr* e) {
    Type* t = resolveType(e->texpr, mod);
    for (auto*& a : e->args)
        if (isErr(t)) check(a, nullptr, false);
    if (isErr(t)) return t;
    if (t->k != TK::Struct) {
        auto& d = g_diag.error(e->texpr->span, "`" + typeName(t) + "` isn't a struct");
        if (t->k == TK::Enum) d.help("create enum values with `" + typeName(t) + ".Variant(...)`");
        for (auto*& a : e->args) check(a, nullptr, false);
        return g_types.error();
    }
    StructDecl* sd = t->sd;
    e->sd = sd;
    std::vector<bool> set(sd->fields.size(), false);
    std::vector<int> order;
    for (size_t i = 0; i < e->fieldNames.size(); i++) {
        int fi = -1;
        for (size_t j = 0; j < sd->fields.size(); j++)
            if (sd->fields[j]->name == e->fieldNames[i]) fi = (int)j;
        if (fi < 0) {
            std::vector<std::string> fs;
            for (auto* f : sd->fields) fs.push_back(f->name);
            auto& d = g_diag.error(e->fieldSpans[i], "`" + sd->name + "` has no field `" + e->fieldNames[i] + "`");
            std::string c = closestName(e->fieldNames[i], fs);
            if (!c.empty()) d.help("did you mean `" + c + "`?");
            check(e->args[i], nullptr, false);
            order.push_back(-1);
            continue;
        }
        if (set[fi]) g_diag.error(e->fieldSpans[i], "`" + e->fieldNames[i] + "` is set twice");
        set[fi] = true;
        if (sd->fields[fi]->isPrivate && sd->mod != mod)
            g_diag.error(e->fieldSpans[i], "field `" + e->fieldNames[i] + "` is private");
        std::string what = "field `" + e->fieldNames[i] + "`";
        checkValue(e->args[i], sd->fields[fi]->rtype, what.c_str());
        order.push_back(fi);
    }
    std::vector<std::string> missing;
    for (size_t j = 0; j < sd->fields.size(); j++)
        if (!set[j] && !sd->fields[j]->def) missing.push_back(sd->fields[j]->name);
    if (!missing.empty()) {
        std::string list;
        for (size_t i = 0; i < missing.size(); i++) list += (i ? ", " : "") + ("`" + missing[i] + "`");
        auto& d = g_diag.error(e->span, "missing value" + std::string(missing.size() > 1 ? "s" : "") + " for " + list + " in `" + sd->name + "`");
        d.help("every field must be set; add `" + missing[0] + " = ...`, or give the field a default in the struct (`" +
               typeName(sd->fields[0]->rtype) + " " + missing[0] + " = ...;`)");
    }
    e->field = 0;
    (void)order;
    return t;
}

Type* Checker::checkArrayLit(Expr* e, Type* expected) {
    Type* elem = nullptr;
    bool toList = false;
    if (expected && (expected->k == TK::Array || expected->k == TK::List)) {
        elem = expected->a;
        toList = expected->k == TK::List;
    }
    for (size_t i = 0; i < e->args.size(); i++) {
        if (!elem) {
            elem = check(e->args[i], nullptr, true);
            if (elem->k == TK::None) {
                g_diag.error(e->args[i]->span, "can't tell the type of `none` here").help("write the type: `Player?[] xs = [none];`");
                elem = g_types.error();
            }
        } else checkValue(e->args[i], elem, "an item");
    }
    if (!elem) {
        g_diag.error(e->span, "can't tell the type of an empty array").help("write the type: `int[] xs = [];` or use `List<int>()`");
        return g_types.error();
    }
    if (isErr(elem)) return elem;
    if (elem->k == TK::Void) {
        g_diag.error(e->span, "array items must be values");
        return g_types.error();
    }
    if (toList) {
        e->callKind = CallKind::ListNew;
        return g_types.listOf(elem);
    }
    return g_types.arrayOf(elem);
}

void Checker::bindPatVar(PatBind& b, Type* t, std::vector<VarDecl*>* sink) {
    if (b.name == "_") return;
    if (b.type) {
        Type* dt = resolveType(b.type, mod);
        if (!sameType(dt, t))
            g_diag.error(b.span, "this value is a `" + typeName(t) + "`, not a `" + typeName(dt) + "`").help("write `var " + b.name + "` to use the right type");
    }
    if (!sink) {
        g_diag.error(b.span, "a pattern variable can only be introduced in an `if`/`while` condition or a `case`")
            .help("use `_` if you don't need the value");
        return;
    }
    VarDecl* v = new VarDecl();
    v->kind = VarDecl::PatBind;
    v->name = b.name;
    v->span = b.span;
    v->type = t;
    v->mode = isCopy(t) ? 0 : 1;
    v->mod = mod;
    if (v->mode == 1 && curIsSubject) v->borrowOf = rootVar(curIsSubject);
    v->cname = localC(v->name);
    b.var = v;
    sink->push_back(v);
    if (declareBinds) declare(v);
}

bool Checker::checkPattern(Pattern* pt, Type* subj, bool allowBinds, std::vector<VarDecl*>* sink) {
    if (!allowBinds) sink = nullptr;
    // A bare name or `Enum.Name` written as an expression may really be a variant.
    if (pt->kind == Pattern::ExprPat && pt->expr && subj->k == TK::Enum) {
        Expr* x = pt->expr;
        if (x->kind == EK::Name) { pt->kind = Pattern::Variant; pt->name = x->name; }
        else if (x->kind == EK::Member && x->a->kind == EK::Name) { pt->kind = Pattern::Variant; pt->qual = x->a->name; pt->name = x->name; }
    }
    if (pt->kind == Pattern::ExprPat && pt->expr && subj->k == TK::Result && pt->expr->kind == EK::Name &&
        (pt->expr->name == "Ok" || pt->expr->name == "Err")) {
        pt->kind = Pattern::Variant;
        pt->name = pt->expr->name;
    }
    switch (pt->kind) {
        case Pattern::Variant: {
            if (subj->k == TK::Enum) {
                EnumDecl* ed = subj->ed;
                if (!pt->qual.empty() && pt->qual != ed->name) {
                    g_diag.error(pt->span, "this is a `" + ed->name + "`, not a `" + pt->qual + "`");
                    return false;
                }
                int vi = -1;
                for (size_t i = 0; i < ed->variants.size(); i++)
                    if (ed->variants[i].name == pt->name) vi = (int)i;
                if (vi < 0) {
                    std::vector<std::string> vs;
                    for (auto& v : ed->variants) vs.push_back(v.name);
                    auto& d = g_diag.error(pt->span, "`" + ed->name + "` has no variant `" + pt->name + "`");
                    std::string c = closestName(pt->name, vs);
                    if (!c.empty()) d.help("did you mean `" + c + "`?");
                    return false;
                }
                pt->variant = vi;
                Variant& v = ed->variants[vi];
                if (pt->hasParens && pt->binds.size() != v.fields.size()) {
                    std::string names;
                    for (size_t i = 0; i < v.fields.size(); i++) names += (i ? ", " : "") + std::string("var ") + v.fields[i]->name;
                    g_diag.error(pt->span, "`" + v.name + "` holds " + std::to_string(v.fields.size()) + " value" + (v.fields.size() == 1 ? "" : "s") +
                                               ", but the pattern has " + std::to_string(pt->binds.size()))
                        .help(v.fields.empty() ? "write just `" + v.name + "`" : "write `" + v.name + "(" + names + ")`");
                    return false;
                }
                for (size_t i = 0; i < pt->binds.size(); i++) bindPatVar(pt->binds[i], v.fields[i]->rtype, sink);
                return true;
            }
            if (subj->k == TK::Result) {
                if (pt->name != "Ok" && pt->name != "Err") {
                    g_diag.error(pt->span, "a `Result` is either `Ok(...)` or `Err(...)`");
                    return false;
                }
                pt->variant = pt->name == "Ok" ? 0 : 1;
                if (pt->binds.size() > 1) {
                    g_diag.error(pt->span, "`" + pt->name + "` holds one value");
                    return false;
                }
                if (pt->binds.size() == 1) bindPatVar(pt->binds[0], pt->variant == 0 ? subj->a : subj->b, sink);
                return true;
            }
            if (subj->k == TK::Optional && !pt->hasParens) {
                Sym* s = findSym(pt->name, mod, pt->span, false);
                Type* ty = nullptr;
                if (s && s->k == Sym::Struct) ty = ((StructDecl*)s->d)->type;
                if (s && s->k == Sym::Enum) ty = ((EnumDecl*)s->d)->type;
                if (ty == subj->a) {
                    pt->variant = 0;
                    pt->isOptionalTest = true;
                    return true;
                }
            }
            g_diag.error(pt->span, "a `" + typeName(subj) + "` can't match `" + pt->name + (pt->hasParens ? "(...)" : "") + "`");
            return false;
        }
        case Pattern::TypeBind: {
            Type* ty = resolveType(pt->type, mod);
            if (isErr(ty)) return false;
            if (subj->k == TK::Optional) {
                if (ty != subj->a) {
                    g_diag.error(pt->type->span, "this holds a `" + typeName(subj->a) + "`, not a `" + typeName(ty) + "`");
                    return false;
                }
                pt->variant = 0;
                bindPatVar(pt->bind, ty, sink);
                return true;
            }
            g_diag.error(pt->span, "`is " + typeName(ty) + " name` works on optionals, but this is a `" + typeName(subj) + "`")
                .help(subj == ty ? "it is always a `" + typeName(ty) + "`; just use it directly" : "");
            return false;
        }
        case Pattern::ExprPat: {
            if (pt->expr->kind == EK::None) {
                if (subj->k != TK::Optional) {
                    g_diag.error(pt->span, "`" + typeName(subj) + "` is never `none`");
                    return false;
                }
                pt->variant = 1;
                pt->expr->type = subj;
                return true;
            }
            Type* et = check(pt->expr, subj, false);
            if (isErr(et)) return false;
            coerce(pt->expr, subj);
            bool okT = isNumeric(subj) || subj->k == TK::Char || subj->k == TK::String || subj->k == TK::Bool;
            if (!okT) {
                g_diag.error(pt->span, "can't match a `" + typeName(subj) + "` against a value").help("switch on enums, numbers, chars, strings or bools");
                return false;
            }
            Expr* x = pt->expr->kind == EK::Implicit ? pt->expr->a : pt->expr;
            bool constant = x->kind == EK::IntLit || x->kind == EK::FloatLit || x->kind == EK::StrLit || x->kind == EK::CharLit ||
                            x->kind == EK::BoolLit || (x->kind == EK::Name && x->var && x->var->kind == VarDecl::Global);
            if (!constant) {
                g_diag.error(pt->span, "a `case` must be a literal or a constant").help("use `if` for comparisons with variables");
                return false;
            }
            return true;
        }
    }
    return false;
}

void Checker::checkSwitch(Stmt* s) {
    Type* st = check(s->expr, nullptr, false);
    Flow entry = flow;
    ctxs.push_back({true, {}, {}});
    Flow out;
    out.dead = true;
    bool hasDefault = false;
    std::set<int> covered;
    std::set<std::string> seenLits;
    for (auto& c : s->cases) {
        scopes.emplace_back();
        flow = entry;
        if (c.isDefault) {
            if (hasDefault) g_diag.error(c.span, "this switch already has a `default`");
            hasDefault = true;
        }
        std::vector<VarDecl*> binds;
        size_t fz = frozen.size();
        for (auto* pt : c.pats) {
            if (isErr(st)) continue;
            curIsSubject = s->expr;
            bool ok = checkPattern(pt, st, c.pats.size() == 1, &binds);
            curIsSubject = nullptr;
            if (!ok) continue;
            if (pt->kind == Pattern::Variant || pt->kind == Pattern::TypeBind || (pt->kind == Pattern::ExprPat && pt->expr->kind == EK::None)) {
                int key = pt->variant;
                if (st->k == TK::Optional) key = pt->variant;
                if (covered.count(key))
                    g_diag.warning(pt->span, "this case is already covered above");
                covered.insert(key);
            } else {
                std::string lit = srcText(pt->expr->span);
                if (!seenLits.insert(lit).second) g_diag.error(pt->span, "`" + lit + "` appears in two cases");
                if (st->k == TK::Bool && pt->expr->kind == EK::BoolLit) covered.insert(pt->expr->bval ? 1 : 0);
            }
        }
        freezeBorrows(binds);
        for (auto* st2 : c.body) checkStmt(st2);
        frozen.erase(frozen.begin() + fz, frozen.end());
        out = mergeFlows(out, flow);
        scopes.pop_back();
    }
    for (auto& b : ctxs.back().breaks) out = mergeFlows(out, b);
    ctxs.pop_back();
    bool exhaustive = hasDefault;
    if (!isErr(st) && !hasDefault) {
        std::vector<std::string> missing;
        if (st->k == TK::Enum) {
            for (size_t i = 0; i < st->ed->variants.size(); i++)
                if (!covered.count((int)i)) {
                    auto& v = st->ed->variants[i];
                    std::string n = v.name;
                    if (!v.fields.empty()) {
                        n += "(";
                        for (size_t j = 0; j < v.fields.size(); j++) n += (j ? ", " : "") + std::string("var ") + v.fields[j]->name;
                        n += ")";
                    }
                    missing.push_back(n);
                }
            exhaustive = missing.empty();
        } else if (st->k == TK::Result) {
            if (!covered.count(0)) missing.push_back("Ok(var value)");
            if (!covered.count(1)) missing.push_back("Err(var error)");
            exhaustive = missing.empty();
        } else if (st->k == TK::Optional) {
            if (!covered.count(0)) missing.push_back(typeName(st->a) + " value");
            if (!covered.count(1)) missing.push_back("none");
            exhaustive = missing.empty();
        } else if (st->k == TK::Bool) {
            exhaustive = covered.count(0) && covered.count(1);
        }
        if (!missing.empty()) {
            std::string list;
            for (size_t i = 0; i < missing.size(); i++) list += (i ? ", " : "") + ("`" + missing[i] + "`");
            auto& d = g_diag.error(s->span, "this switch doesn't handle " + list);
            d.lbl("every case of a `" + typeName(st) + "` must be handled");
            d.help("add `case " + missing[0] + ":` or a `default:`");
        }
    }
    if (!exhaustive) out = mergeFlows(out, entry);
    flow = out;
}

}  // namespace

// ======================================================================== public

const Impl* findImpl(Program& prog, Type* t, InterfaceDecl* iface) {
    for (auto& im : prog.impls)
        if (im.type == t && im.iface == iface) return &im;
    return nullptr;
}

FuncDecl* findMethod(Program& prog, Type* t, const std::string& name) { return findMethodImpl(prog, t, name); }

bool Checker::run() {
    // the prelude holds built-in interfaces
    if (!P.prelude) {
        Module* pre = new Module();
        pre->name = "prelude";
        pre->isStd = true;
        InterfaceDecl* disp = new InterfaceDecl();
        disp->name = "Disposable";
        disp->builtin = true;
        disp->mod = pre;
        FuncDecl* m = new FuncDecl();
        m->name = "dispose";
        m->isMut = true;
        m->ownerIface = disp;
        m->ret = new TypeExpr();
        m->ret->name = "void";
        m->mod = pre;
        disp->methods.push_back(m);
        pre->ifaces.push_back(disp);
        P.prelude = pre;
        P.disposable = disp;
        P.modules.insert(P.modules.begin(), pre);
    }
    for (Module* m : P.modules) registerModule(m);
    for (auto* s : P.structs)
        if (!s->gen.isGeneric()) s->type = g_types.structT(s);
    for (auto* e : P.enums)
        if (!e->gen.isGeneric()) e->type = g_types.enumT(e);
    for (auto* i : P.ifaces) i->type = g_types.ifaceT(i);
    for (Module* m : P.modules) resolveSignatures(m);
    // interface implementations
    for (Module* m : P.modules) {
        mod = m;
        for (auto* s : m->structs)
            if (!s->gen.isGeneric())
                for (auto* i : s->ifaces) buildImpl(s->type, i, s->span);
        for (auto* x : m->extends)
            if (x->type && !isErr(x->type))
                for (auto* i : x->ifaces) buildImpl(x->type, i, x->span);
    }
    checkContainment();
    implsReady = true;
    if (g_diag.errorCount() > 0 && !P.tools) return false;
    // bodies
    for (Module* m : P.modules) {
        mod = m;
        for (auto* v : m->globals) checkGlobal(v);
        for (auto* s : m->structs)
            if (!s->gen.isGeneric()) checkFieldDefaults(s);
        for (auto* f : m->funcs) checkFunc(f);
        for (auto* s : m->structs)
            if (!s->gen.isGeneric())
                for (auto* f : s->methods) checkFunc(f);
        for (auto* x : m->extends)
            for (auto* f : x->methods) checkFunc(f);
    }
    drainGenerics();
    // `T : Printable` bounds, checked once every implementation is known
    for (auto& b : pendingBounds) checkBound(b);  // instances made before all implementations were known
    if (g_diag.errorCount() == 0) checkContainment();  // generic instances made while checking bodies
    if (!P.testMode && P.mainModule) {
        if (!P.mainFn) {
            Span sp;
            sp.file = P.mainModule->fileId;
            sp.line = 1;
            sp.col = 1;
            g_diag.error(sp, "there is no `main` function").help("add `int main() { ... return 0; }`");
        } else if (!P.mainFn->params.empty() || (P.mainFn->retType->k != TK::Int && P.mainFn->retType->k != TK::Void)) {
            g_diag.error(P.mainFn->span, "`main` must be `int main()` or `void main()`");
        }
    }
    return g_diag.errorCount() == 0;
}

bool checkProgram(Program& prog) {
    Checker c(prog);
    return c.run();
}
