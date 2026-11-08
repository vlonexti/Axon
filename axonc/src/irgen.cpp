// irgen.cpp - lowers checked Axon code to Axon IR.
//
// Model: every expression evaluates to a *pointer to memory* holding its value. Places (variables,
// fields, items) give their own address; computed values get a fresh stack slot. Scalars are
// loaded into registers only to compute with them. This keeps the lowering simple and uniform;
// the C backend or LLVM do the optimizing.
//
// Ownership follows the C emitter exactly: moving out of a local copies its bytes and zeroes the
// source; temporaries that are only borrowed are dropped at the end of the statement; every owned
// local is dropped when its scope ends (or on return / break / continue).
#include "irgen.h"

#include <functional>
#include <map>
#include <set>

namespace {

int alignUp(int x, int a) { return a <= 1 ? x : (x + a - 1) / a * a; }

struct Layout {
    int size = 0, align = 1;
    std::vector<int> offs;                // struct fields
    std::vector<std::vector<int>> voffs;  // enum: field offsets per variant
    int valOff = 0;                       // optional: offset of val; enum/result: offset of the union
};

std::map<Type*, Layout> g_layouts;
Program* g_P = nullptr;

bool isDisposableT(Type* t) {
    if (t->k != TK::Struct || t->sd->isExtern) return false;
    for (auto* i : t->sd->ifaces)
        if (i == g_P->disposable) return true;
    return false;
}

const Layout& layout(Type* t);

void place(Layout& L, int size, int align, int& off) {
    off = alignUp(L.size, align);
    L.size = off + size;
    if (align > L.align) L.align = align;
}

const Layout& layout(Type* t) {
    auto it = g_layouts.find(t);
    if (it != g_layouts.end()) return it->second;
    Layout L;
    switch (t->k) {
        case TK::Void: L.size = 0; break;
        case TK::Byte: case TK::Bool: L.size = 1; break;
        case TK::Int: case TK::Char: case TK::Float: case TK::Error: case TK::None: L.size = 4; L.align = 4; break;
        case TK::Long: case TK::Double: L.size = 8; L.align = 8; break;
        case TK::String: L.size = 24; L.align = 8; break;
        case TK::Array: case TK::Interface: L.size = 16; L.align = 8; break;
        case TK::List: L.size = 24; L.align = 8; break;
        case TK::Map: L.size = 56; L.align = 8; break;
        case TK::Box: L.size = t->a->k == TK::Interface ? 16 : 8; L.align = 8; break;
        case TK::Struct: {
            for (auto* f : t->sd->fields) {
                int off;
                place(L, typeSize(f->rtype), typeAlign(f->rtype), off);
                L.offs.push_back(off);
            }
            if (isDisposableT(t)) { int o; place(L, 1, 1, o); }
            if (L.size == 0) L.size = 1;
            break;
        }
        case TK::Enum: {
            int ualign = 1, usize = 0;
            std::vector<Layout> vs;
            for (auto& v : t->ed->variants) {
                Layout V;
                for (auto* f : v.fields) {
                    int off;
                    place(V, typeSize(f->rtype), typeAlign(f->rtype), off);
                    V.offs.push_back(off);
                }
                V.size = alignUp(V.size, V.align);
                if (!v.fields.empty()) {
                    ualign = std::max(ualign, V.align);
                    usize = std::max(usize, V.size);
                }
                vs.push_back(V);
            }
            if (usize == 0) usize = 1;
            L.align = std::max(4, ualign);
            L.valOff = alignUp(4, ualign);
            L.size = L.valOff + usize;
            for (auto& V : vs) {
                std::vector<int> o;
                for (int x : V.offs) o.push_back(L.valOff + x);
                L.voffs.push_back(o);
            }
            break;
        }
        case TK::Optional: {
            L.size = 1;
            int off;
            place(L, typeSize(t->a), typeAlign(t->a), off);
            L.valOff = off;
            break;
        }
        case TK::Result: {
            int ua = std::max(typeAlign(t->a), typeAlign(t->b));
            int us = std::max(typeSize(t->a), typeSize(t->b));
            L.align = std::max(4, ua);
            L.valOff = alignUp(4, ua);
            L.size = L.valOff + us;
            break;
        }
    }
    L.size = alignUp(L.size, L.align);
    return g_layouts[t] = L;
}

// ---------------------------------------------------------------- calling convention (Windows x64)

enum class Pass { Void, Int, Float, Ref };  // Ref: passed as a pointer to a copy / returned through a hidden pointer

struct Abi {
    Pass pass;
    IT it;
};

Abi classify(Type* t) {
    switch (t->k) {
        case TK::Void: return {Pass::Void, IT::Void};
        case TK::Float: return {Pass::Float, IT::F32};
        case TK::Double: return {Pass::Float, IT::F64};
        case TK::Byte: case TK::Bool: return {Pass::Int, IT::I8};
        case TK::Int: case TK::Char: return {Pass::Int, IT::I32};
        case TK::Long: return {Pass::Int, IT::I64};
        case TK::Box: if (t->a->k != TK::Interface) return {Pass::Int, IT::Ptr}; break;
        default: break;
    }
    switch (typeSize(t)) {
        case 1: return {Pass::Int, IT::I8};
        case 2: return {Pass::Int, IT::I16};
        case 4: return {Pass::Int, IT::I32};
        case 8: return {Pass::Int, IT::I64};
        default: return {Pass::Ref, IT::Ptr};
    }
}

IT scalarIT(Type* t) {
    switch (t->k) {
        case TK::Byte: case TK::Bool: return IT::I8;
        case TK::Int: case TK::Char: return IT::I32;
        case TK::Long: return IT::I64;
        case TK::Float: return IT::F32;
        case TK::Double: return IT::F64;
        case TK::Box: return IT::Ptr;
        default: return IT::I64;
    }
}
bool isUnsigned(Type* t) { return t->k == TK::Byte || t->k == TK::Char || t->k == TK::Bool; }

struct CArg {
    IT it;
    int reg;
};

// ---------------------------------------------------------------- the generator

class Gen {
public:
    Gen(Program& p, IRModule& m) : P(p), M(m) {}
    void run();

private:
    Program& P;
    IRModule& M;
    IRFunc* F = nullptr;
    int cur = 0;
    FuncDecl* fn = nullptr;
    std::map<VarDecl*, int> varSlot;
    std::map<int, int> tempSlot;  // pointer register -> its temp slot
    int selfSlot = -1, sretSlot = -1;
    std::map<std::string, std::string> strData;
    int dataN = 0;

    struct Drop {
        std::string fn;
        int slot;
    };
    struct Ctx {
        std::vector<Drop> post;
    };
    std::vector<Ctx*> ctx;
    struct Scope {
        int kind = 0;  // 0 block, 1 loop, 2 switch
        std::vector<Drop> drops;
        int brk = -1, cont = -1;
    };
    std::vector<Scope> scopes;

    // ---- emission primitives
    int reg(IT t) {
        F->regs.push_back(t);
        return (int)F->regs.size() - 1;
    }
    int slot(int size, int align) {
        F->slots.push_back({std::max(size, 1), std::max(align, 1)});
        return (int)F->slots.size() - 1;
    }
    int block() {
        F->blocks.emplace_back();
        return (int)F->blocks.size() - 1;
    }
    void setBlock(int b) { cur = b; }
    void put(Inst i) { F->blocks[cur].insts.push_back(std::move(i)); }
    bool ended() const {
        auto& v = F->blocks[cur].insts;
        if (v.empty()) return false;
        Op o = v.back().op;
        return o == Op::Br || o == Op::CondBr || o == Op::Ret;
    }
    int iconst(IT t, int64_t v) {
        Inst i{Op::IConst};
        i.type = t;
        i.dst = reg(t);
        i.imm = v;
        put(i);
        return i.dst;
    }
    int fconst(IT t, double v) {
        Inst i{Op::FConst};
        i.type = t;
        i.dst = reg(t);
        i.fimm = v;
        put(i);
        return i.dst;
    }
    int slotAddr(int s) {
        Inst i{Op::SlotAddr};
        i.type = IT::Ptr;
        i.dst = reg(IT::Ptr);
        i.slot = s;
        put(i);
        return i.dst;
    }
    int global(const std::string& sym) {
        Inst i{Op::Global};
        i.type = IT::Ptr;
        i.dst = reg(IT::Ptr);
        i.sym = sym;
        put(i);
        return i.dst;
    }
    int load(IT t, int p) {
        Inst i{Op::Load};
        i.type = t;
        i.dst = reg(t);
        i.a = p;
        put(i);
        return i.dst;
    }
    void store(IT t, int p, int v) {
        Inst i{Op::Store};
        i.type = t;
        i.a = p;
        i.b = v;
        put(i);
    }
    void copy(int dst, int src, int n) {
        if (n <= 0) return;
        Inst i{Op::Memcpy};
        i.a = dst;
        i.b = src;
        i.imm = n;
        put(i);
    }
    void zero(int p, int n) {
        Inst i{Op::Memset0};
        i.a = p;
        i.imm = n;
        put(i);
    }
    int addI(int p, int64_t off) {
        if (off == 0) return p;
        Inst i{Op::PtrAddI};
        i.type = IT::Ptr;
        i.dst = reg(IT::Ptr);
        i.a = p;
        i.imm = off;
        put(i);
        return i.dst;
    }
    int bin(Op op, IT t, int a, int b) {
        Inst i{op};
        i.type = t;
        i.dst = reg(t);
        i.a = a;
        i.b = b;
        put(i);
        return i.dst;
    }
    int un(Op op, IT t, int a) {
        Inst i{op};
        i.type = t;
        i.dst = reg(t);
        i.a = a;
        put(i);
        return i.dst;
    }
    int cmp(Op op, Cond c, int a, int b) {
        Inst i{op};
        i.type = IT::I8;
        i.dst = reg(IT::I8);
        i.a = a;
        i.b = b;
        i.cond = c;
        put(i);
        return i.dst;
    }
    void br(int t) {
        if (ended()) return;
        Inst i{Op::Br};
        i.t1 = t;
        put(i);
    }
    void condBr(int c, int t, int f) {
        Inst i{Op::CondBr};
        i.a = c;
        i.t1 = t;
        i.t2 = f;
        put(i);
    }
    int call(const std::string& sym, IT ret, const std::vector<CArg>& args, int fnPtr = -1) {
        Inst i{fnPtr >= 0 ? Op::CallPtr : Op::Call};
        i.type = ret;
        i.sym = sym;
        i.a = fnPtr;
        for (auto& a : args) {
            i.args.push_back(a.reg);
            i.argTypes.push_back(a.it);
        }
        if (ret != IT::Void) i.dst = reg(ret);
        put(i);
        return i.dst;
    }

    // ---- data
    std::string dataSym(const std::string& bytes) {
        auto it = strData.find(bytes);
        if (it != strData.end()) return it->second;
        std::string name = "axs_" + std::to_string(dataN++);
        M.data.push_back({name, bytes});
        strData[bytes] = name;
        return name;
    }
    int locArg(Span sp) {
        if (sp.file < 0) return global(dataSym("?"));
        std::string path = g_diag.file(sp.file).display;
        for (auto& c : path) if (c == '\\') c = '/';
        return global(dataSym(path + ":" + std::to_string(sp.line) + ":" + std::to_string(sp.col)));
    }

    // ---- temporaries and values
    int temp(Type* t) {
        int s = slot(typeSize(t), typeAlign(t));
        int p = slotAddr(s);
        tempSlot[p] = s;
        return p;
    }
    int tempOf(IT it) {
        int s = slot(itSize(it), itSize(it));
        int p = slotAddr(s);
        tempSlot[p] = s;
        return p;
    }
    int storeTemp(Type* t, int v) {
        int p = temp(t);
        store(scalarIT(t), p, v);
        return p;
    }
    void post(Type* t, int p) {
        auto it = tempSlot.find(p);
        if (it == tempSlot.end()) return;
        Drop d{"ax_drop_" + t->mangle, it->second};
        if (ctx.empty()) emitDrop(d);
        else ctx.back()->post.push_back(d);
    }
    void emitDrop(const Drop& d) { call(d.fn, IT::Void, {{IT::Ptr, slotAddr(d.slot)}}); }

    CArg byValue(Type* t, int p) {
        Abi a = classify(t);
        if (a.pass == Pass::Int || a.pass == Pass::Float) return {a.it, load(a.it, p)};
        // passed by pointer: the callee may change it, so it gets its own copy
        int c = temp(t);
        copy(c, p, typeSize(t));
        return {IT::Ptr, c};
    }
    // Calls a function that returns an Axon value; the result lands in a fresh temp.
    int callRet(const std::string& sym, Type* ret, std::vector<CArg> args, int fnPtr = -1) {
        Abi a = classify(ret);
        if (a.pass == Pass::Void) {
            call(sym, IT::Void, args, fnPtr);
            return -1;
        }
        int t = temp(ret);
        if (a.pass == Pass::Ref) {
            args.insert(args.begin(), {IT::Ptr, t});
            call(sym, IT::Ptr, args, fnPtr);
            return t;
        }
        int r = call(sym, a.it, args, fnPtr);
        store(a.it, t, r);
        return t;
    }

    // ---- expressions
    bool needsDrop(Expr* e);
    int gen(Expr* e);
    int val(Expr* e);
    int view(Expr* e);
    int ownedCopy(Expr* e);
    int addr(Expr* e);
    int addrForWrite(Expr* e);
    int scalar(Expr* e) { return load(scalarIT(e->type), gen(e)); }
    int convert(int r, Type* from, Type* to);
    int boolReg(Expr* e) { return load(IT::I8, gen(e)); }
    int genCall(Expr* e);
    std::vector<CArg> callArgs(Expr* e, FuncDecl* f);
    int genBinary(Expr* e);
    int arith(T op, Type* R, int a, int b, Span sp);
    int genConcat(const std::vector<Expr*>& parts);
    void appendPart(int sb, Expr* p);
    int genTernary(Expr* e);
    int genCoalesce(Expr* e);
    int genTry(Expr* e);
    int patternTest(Pattern* pt, Type* subj, int s, bool bind);
    void bindPattern(Pattern* pt, Type* subj, int s);
    void bindVar(PatBind& b, int fieldPtr);
    int fieldPtr(Type* st, int base, int field) { return addI(base, layout(st).offs[field]); }
    int stringLit(const std::string& s);
    int seqData(int seqPtr) { return load(IT::Ptr, seqPtr); }
    int seqLen(int seqPtr) { return load(IT::I64, addI(seqPtr, 8)); }
    template <class F>
    void sub(Ctx& c, F f);

    // ---- statements
    void pushScope(int kind = 0) {
        Scope s;
        s.kind = kind;
        scopes.push_back(s);
    }
    void popScope() {
        auto& s = scopes.back();
        if (!ended())
            for (auto it = s.drops.rbegin(); it != s.drops.rend(); ++it) emitDrop(*it);
        scopes.pop_back();
    }
    void dropScopes(size_t from) {
        for (size_t i = scopes.size(); i-- > from;)
            for (auto it = scopes[i].drops.rbegin(); it != scopes[i].drops.rend(); ++it) emitDrop(*it);
    }
    void flushPost(Ctx& c) {
        for (auto it = c.post.rbegin(); it != c.post.rend(); ++it) emitDrop(*it);
    }
    int declare(VarDecl* v) {
        int s = v->mode != 0 && v->type->k != TK::Interface ? slot(8, 8) : slot(typeSize(v->type), typeAlign(v->type));
        varSlot[v] = s;
        return s;
    }
    void addOwned(VarDecl* v) {
        if (v->mode == 0 && v->type && isOwned(v->type)) scopes.back().drops.push_back({"ax_drop_" + v->type->mangle, varSlot[v]});
    }
    int varPlace(VarDecl* v);
    void genBlock(Stmt* b);
    void genStmt(Stmt* s);
    void genAssign(Expr* e);
    void declPatVars(const std::vector<VarDecl*>& vs) {
        for (auto* v : vs) {
            int s = declare(v);
            zero(slotAddr(s), F->slots[s].size);
        }
    }
    void retValue(int v);
    void genFunc(FuncDecl* f);
    void genInitGlobals();
    void finish();
};

template <class Fn>
void Gen::sub(Ctx& c, Fn f) {
    ctx.push_back(&c);
    f();
    ctx.pop_back();
}

bool Gen::needsDrop(Expr* e) {
    if (!e->type || isCopy(e->type)) return false;
    switch (e->kind) {
        case EK::Name: case EK::This: case EK::Index: case EK::StrLit: case EK::BoxDeref: return false;
        case EK::Implicit: return e->implicit == ImplicitKind::BoxToDyn && needsDrop(e->a);
        case EK::Member: return e->field < 0;
        default: return true;
    }
}

int Gen::varPlace(VarDecl* v) {
    if (v->kind == VarDecl::Global) {
        if (v->isExtern) return callRet("axg_" + v->name, v->type, {});
        return global(v->cname);
    }
    auto it = varSlot.find(v);
    if (it == varSlot.end()) return iconst(IT::Ptr, 0);
    int p = slotAddr(it->second);
    if (v->mode != 0 && v->type->k != TK::Interface) return load(IT::Ptr, p);
    return p;
}

int Gen::val(Expr* e) {
    if (e->kind == EK::Implicit && e->implicit == ImplicitKind::WrapOptional)
        return callRet("ax_some_" + e->a->type->mangle, e->type, {byValue(e->a->type, val(e->a))});
    if (!e->type || isCopy(e->type)) return gen(e);
    switch (e->kind) {
        case EK::Name:
            if (e->var && e->var->kind == VarDecl::Global) return callRet("ax_clone_" + e->type->mangle, e->type, {{IT::Ptr, gen(e)}});
            if (e->var && e->var->mode == 0 && e->var->kind != VarDecl::ForeachBind) {
                int src = gen(e);
                int t = temp(e->type);
                copy(t, src, typeSize(e->type));
                zero(src, typeSize(e->type));
                return t;
            }
            return callRet("ax_clone_" + e->type->mangle, e->type, {{IT::Ptr, addr(e)}});
        case EK::This: case EK::Index: case EK::BoxDeref:
            return callRet("ax_clone_" + e->type->mangle, e->type, {{IT::Ptr, addr(e)}});
        case EK::Member:
            if (e->field >= 0) return callRet("ax_clone_" + e->type->mangle, e->type, {{IT::Ptr, addr(e)}});
            return gen(e);
        default: return gen(e);
    }
}

int Gen::view(Expr* e) {
    if (e->kind == EK::Implicit && e->implicit == ImplicitKind::WrapOptional)
        return callRet("ax_some_" + e->a->type->mangle, e->type, {byValue(e->a->type, view(e->a))});
    int p = gen(e);
    if (e->type && !isCopy(e->type) && needsDrop(e)) post(e->type, p);
    return p;
}

int Gen::ownedCopy(Expr* e) {
    if (e->kind == EK::Implicit && e->implicit == ImplicitKind::WrapOptional)
        return callRet("ax_some_" + e->a->type->mangle, e->type, {byValue(e->a->type, ownedCopy(e->a))});
    if (!e->type || isCopy(e->type)) return gen(e);
    if (!needsDrop(e) && e->kind != EK::StrLit) return callRet("ax_clone_" + e->type->mangle, e->type, {{IT::Ptr, addr(e)}});
    return gen(e);
}

int Gen::addr(Expr* e) {
    int p = gen(e);
    if (e->type && !isCopy(e->type) && needsDrop(e)) post(e->type, p);
    return p;
}

int Gen::addrForWrite(Expr* e) {
    if (e->kind == EK::Index && e->a->type->k == TK::Map) {
        Type* mt = e->a->type;
        int m = addr(e->a);
        CArg k = byValue(mt->a, mt->a->k == TK::String ? view(e->b) : gen(e->b));
        return call("ax_map_slot_" + mt->mangle, IT::Ptr, {{IT::Ptr, m}, k});
    }
    return addr(e);
}

int Gen::convert(int r, Type* from, Type* to) {
    IT f = scalarIT(from), t = scalarIT(to);
    if (f == t) return r;
    bool ff = itFloat(f), tf = itFloat(t);
    if (ff && tf) return un(itSize(t) > itSize(f) ? Op::FPExt : Op::FPTrunc, t, r);
    if (ff) return un(Op::FPToSI, t, r);
    if (tf) return un(isUnsigned(from) ? Op::UIToFP : Op::SIToFP, t, r);
    if (itSize(t) > itSize(f)) return un(isUnsigned(from) ? Op::ZExt : Op::SExt, t, r);
    return un(Op::Trunc, t, r);
}

int Gen::stringLit(const std::string& s) {
    int t = temp(g_types.stringT());
    store(IT::Ptr, t, global(dataSym(s)));
    store(IT::I64, addI(t, 8), iconst(IT::I64, (int64_t)s.size()));
    store(IT::I64, addI(t, 16), iconst(IT::I64, 0));
    return t;
}

int Gen::gen(Expr* e) {
    switch (e->kind) {
        case EK::IntLit: {
            IT it = scalarIT(e->type);
            if (itFloat(it)) return storeTemp(e->type, fconst(it, (double)e->ival));
            return storeTemp(e->type, iconst(it, e->ival));
        }
        case EK::FloatLit: return storeTemp(e->type, fconst(scalarIT(e->type), e->fval));
        case EK::StrLit: return stringLit(e->sval);
        case EK::CharLit: return storeTemp(e->type, iconst(IT::I32, e->cval));
        case EK::BoolLit: return storeTemp(e->type, iconst(IT::I8, e->bval ? 1 : 0));
        case EK::None: return callRet("ax_none_" + e->type->a->mangle, e->type, {});
        case EK::Interp: return genConcat(e->args);
        case EK::Name: return e->var ? varPlace(e->var) : temp(g_types.intT());
        case EK::This: return load(IT::Ptr, slotAddr(selfSlot));
        case EK::Member: {
            if (e->builtin == Builtin::PropLength) {
                int b = view(e->a);
                int off = e->a->type->k == TK::Map ? 16 : 8;
                return storeTemp(g_types.intT(), un(Op::Trunc, IT::I32, load(IT::I64, addI(b, off))));
            }
            if (e->builtin == Builtin::PropHasValue) return storeTemp(g_types.boolT(), load(IT::I8, view(e->a)));
            int base = needsDrop(e->a) ? view(e->a) : gen(e->a);
            return fieldPtr(e->a->type, base, e->field);
        }
        case EK::Call: {
            int r = genCall(e);
            return r < 0 ? temp(g_types.intT()) : r;
        }
        case EK::Index: {
            Type* bt = e->a->type;
            if (bt->k == TK::Map) {
                int m = addr(e->a);
                CArg k = byValue(bt->a, bt->a->k == TK::String ? view(e->b) : gen(e->b));
                return call("ax_map_at_" + bt->mangle, IT::Ptr, {{IT::Ptr, m}, k, {IT::Ptr, locArg(e->span)}});
            }
            int base = addr(e->a);
            int idx = convert(scalar(e->b), e->b->type, g_types.longT());
            std::string fnName = (bt->k == TK::List ? "ax_list_at_" : "ax_arr_at_") + bt->a->mangle;
            return call(fnName, IT::Ptr, {{IT::Ptr, base}, {IT::I64, idx}, {IT::Ptr, locArg(e->span)}});
        }
        case EK::BoxDeref: {
            int b = load(IT::Ptr, needsDrop(e->a) ? view(e->a) : gen(e->a));
            return call("ax_box_deref_" + e->type->mangle, IT::Ptr, {{IT::Ptr, b}, {IT::Ptr, locArg(e->span)}});
        }
        case EK::Unary: {
            int a = scalar(e->a);
            if (e->op == T::Bang) return storeTemp(e->type, bin(Op::Xor, IT::I8, a, iconst(IT::I8, 1)));
            a = convert(a, e->a->type, e->type);
            IT it = scalarIT(e->type);
            if (e->op == T::Tilde) return storeTemp(e->type, un(Op::Not, it, a));
            if (e->type->k == TK::Int) return storeTemp(e->type, call("ax_neg_i32", IT::I32, {{IT::I32, a}, {IT::Ptr, locArg(e->span)}}));
            if (e->type->k == TK::Long) return storeTemp(e->type, call("ax_neg_i64", IT::I64, {{IT::I64, a}, {IT::Ptr, locArg(e->span)}}));
            return storeTemp(e->type, un(Op::FNeg, it, a));
        }
        case EK::Binary: return genBinary(e);
        case EK::Ternary: return genTernary(e);
        case EK::Cast: return storeTemp(e->type, convert(scalar(e->a), e->a->type, e->type));
        case EK::StructLit: {
            StructDecl* sd = e->sd;
            int t = temp(e->type);
            zero(t, typeSize(e->type));
            for (size_t j = 0; j < sd->fields.size(); j++) {
                FieldDecl* f = sd->fields[j];
                int idx = -1;
                for (size_t i = 0; i < e->fieldNames.size(); i++)
                    if (e->fieldNames[i] == f->name) idx = (int)i;
                int v = idx >= 0 ? val(e->args[idx]) : val(f->def);
                copy(fieldPtr(e->type, t, (int)j), v, typeSize(f->rtype));
            }
            if (isDisposableT(e->type)) {
                // ax_live sits right after the last field
                int off = 0;
                const Layout& L = layout(e->type);
                if (!sd->fields.empty()) off = L.offs.back() + typeSize(sd->fields.back()->rtype);
                store(IT::I8, addI(t, off), iconst(IT::I8, 1));
            }
            return t;
        }
        case EK::ArrayLit: {
            Type* t = e->type;
            Type* el = t->a;
            int es = typeSize(el);
            if (t->k == TK::List) {
                int l = callRet("ax_list_new_" + el->mangle, t, {});
                for (auto* x : e->args) call("ax_list_add_" + el->mangle, IT::Void, {{IT::Ptr, l}, byValue(el, val(x))});
                return l;
            }
            int arr = temp(t);
            zero(arr, 16);
            size_t n = e->args.size();
            if (n == 0) return arr;
            int data = call("ax_alloc", IT::Ptr, {{IT::I64, iconst(IT::I64, (int64_t)(n * es))}});
            store(IT::Ptr, arr, data);
            store(IT::I64, addI(arr, 8), iconst(IT::I64, (int64_t)n));
            for (size_t i = 0; i < n; i++) {
                int v = val(e->args[i]);
                copy(addI(load(IT::Ptr, arr), (int64_t)(i * es)), v, es);
            }
            return arr;
        }
        case EK::NewArray: {
            int n = convert(scalar(e->a), e->a->type, g_types.longT());
            return callRet("ax_arr_new_" + e->type->a->mangle, e->type, {{IT::I64, n}, {IT::Ptr, locArg(e->span)}});
        }
        case EK::GenericNew:
            if (e->callKind == CallKind::MapNew) return callRet("ax_map_new_" + e->type->mangle, e->type, {});
            return callRet("ax_list_new_" + e->type->a->mangle, e->type, {});
        case EK::Is: {
            Type* S = e->a->type;
            int s = addr(e->a);
            return storeTemp(g_types.boolT(), patternTest(e->pat, S, s, true));
        }
        case EK::Try: return genTry(e);
        case EK::IncDec: {
            static const std::map<TK, std::string> k = {{TK::Int, "i32"}, {TK::Long, "i64"}, {TK::Byte, "u8"},
                                                        {TK::Char, "u32"}, {TK::Float, "f32"}, {TK::Double, "f64"}};
            std::string which = std::string(e->prefix ? "pre" : "post") + (e->op == T::PlusPlus ? "inc" : "dec");
            int p = addrForWrite(e->a);
            return storeTemp(e->type, call("ax_" + which + "_" + k.at(e->type->k), scalarIT(e->type), {{IT::Ptr, p}, {IT::Ptr, locArg(e->span)}}));
        }
        case EK::Implicit:
            if (e->implicit == ImplicitKind::Numeric) return storeTemp(e->type, convert(scalar(e->a), e->a->type, e->type));
            if (e->implicit == ImplicitKind::WrapOptional)
                return callRet("ax_some_" + e->a->type->mangle, e->type, {byValue(e->a->type, e->consumed ? val(e->a) : ownedCopy(e->a))});
            if (e->implicit == ImplicitKind::BoxToDyn) {
                int t = temp(e->type);
                int b = e->consumed ? val(e->a) : gen(e->a);
                store(IT::Ptr, t, load(IT::Ptr, b));
                store(IT::Ptr, addI(t, 8), global(e->a->type->a->mangle + "__" + e->type->a->id->cname + "_vt"));
                return t;
            }
            return gen(e->a);
        case EK::RefArg: return addr(e->a);
        case EK::Assign: return temp(g_types.intT());
    }
    return temp(g_types.intT());
}

std::vector<CArg> Gen::callArgs(Expr* e, FuncDecl* f) {
    std::vector<CArg> as;
    for (size_t i = 0; i < f->params.size() && i < e->args.size(); i++) {
        Param& p = f->params[i];
        Expr* a = e->args[i];
        Type* pt = p.var->type;
        if (p.mode == 0) {
            bool borrowed = (f->isExtern || f->isAxonRt) && (pt->k == TK::String || pt->k == TK::Optional);
            as.push_back(byValue(pt, borrowed ? view(a) : val(a)));
            continue;
        }
        Expr* inner = a->kind == EK::RefArg ? a->a : a;
        if (pt->k == TK::Interface) {
            int ref = temp(pt);
            if (inner->type == pt) copy(ref, view(inner), 16);
            else if (inner->type->k == TK::Box) {
                int b = callRet("ax_dyn_check_" + inner->type->mangle, inner->type, {byValue(inner->type, view(inner)), {IT::Ptr, locArg(inner->span)}});
                copy(ref, b, 16);
            } else {
                const Impl* im = findImpl(P, inner->type, pt->id);
                store(IT::Ptr, ref, addr(inner));
                store(IT::Ptr, addI(ref, 8), global(im ? im->type->mangle + "__" + pt->id->cname + "_vt" : "ax_null_vt"));
            }
            as.push_back(byValue(pt, ref));
        } else as.push_back({IT::Ptr, addr(inner)});
    }
    return as;
}

int Gen::genCall(Expr* e) {
    switch (e->callKind) {
        case CallKind::Func: {
            FuncDecl* f = e->fn;
            std::string sym = f->isExtern ? "axw_" + f->name : f->cname;
            return callRet(sym, f->retType, callArgs(e, f));
        }
        case CallKind::Method: {
            int self = addr(e->a->a);
            std::vector<CArg> as = {{IT::Ptr, self}};
            for (auto& a : callArgs(e, e->fn)) as.push_back(a);
            return callRet(e->fn->cname, e->fn->retType, as);
        }
        case CallKind::IfaceMethod:
        case CallKind::DynMethod: {
            Expr* recv = e->a->a;
            int r;
            if (e->callKind == CallKind::DynMethod)
                r = callRet("ax_dyn_check_" + recv->type->mangle, recv->type, {byValue(recv->type, view(recv)), {IT::Ptr, locArg(e->span)}});
            else r = view(recv);
            InterfaceDecl* id = e->callKind == CallKind::DynMethod ? recv->type->a->id : recv->type->id;
            int index = 0;
            for (size_t i = 0; i < id->methods.size(); i++)
                if (id->methods[i] == e->fn) index = (int)i;
            int self = load(IT::Ptr, r);
            int vt = load(IT::Ptr, addI(r, 8));
            int fp = load(IT::Ptr, addI(vt, index * 8));
            std::vector<CArg> as = {{IT::Ptr, self}};
            for (auto& a : callArgs(e, e->fn)) as.push_back(a);
            return callRet("", e->fn->retType, as, fp);
        }
        case CallKind::Builtin: {
            switch (e->builtin) {
                case Builtin::Print: case Builtin::Write: {
                    std::string k = e->builtin == Builtin::Print ? "ax_print_" : "ax_write_";
                    if (e->args.empty()) {
                        if (e->builtin == Builtin::Print) call("ax_print_newline", IT::Void, {});
                        return -1;
                    }
                    Expr* a = e->args[0];
                    switch (a->type->k) {
                        case TK::String: call(k + "str", IT::Void, {byValue(a->type, view(a))}); break;
                        case TK::Float: call(k + "f32", IT::Void, {{IT::F32, scalar(a)}}); break;
                        case TK::Double: call(k + "f64", IT::Void, {{IT::F64, scalar(a)}}); break;
                        case TK::Bool: call(k + "bool", IT::Void, {{IT::I8, scalar(a)}}); break;
                        case TK::Char: call(k + "char", IT::Void, {{IT::I32, scalar(a)}}); break;
                        default: call(k + "i64", IT::Void, {{IT::I64, convert(scalar(a), a->type, g_types.longT())}}); break;
                    }
                    return -1;
                }
                case Builtin::Assert:
                    if (e->args.size() > 1) {
                        int c = boolReg(e->args[0]);
                        call("ax_assert_msg", IT::Void, {{IT::I8, c}, byValue(g_types.stringT(), view(e->args[1])), {IT::Ptr, locArg(e->span)}});
                    } else call("ax_assert", IT::Void, {{IT::I8, boolReg(e->args[0])}, {IT::Ptr, locArg(e->span)}});
                    return -1;
                case Builtin::Panic:
                    call("ax_panic_str", IT::Void, {byValue(g_types.stringT(), view(e->args[0])), {IT::Ptr, locArg(e->span)}});
                    return -1;
                default: return -1;
            }
        }
        case CallKind::BuiltinMethod: {
            Expr* recv = e->a->a;
            Type* rt = recv->type;
            Type* S = g_types.stringT();
            Type* I = g_types.intT();
            auto A = [&](int i) {
                Expr* x = e->args[i];
                return byValue(x->type, x->type->k == TK::String ? view(x) : gen(x));
            };
            auto str = [&]() { return byValue(S, view(recv)); };
            CArg loc{IT::Ptr, -1};
            auto L = [&]() { return CArg{IT::Ptr, locArg(e->span)}; };
            (void)loc;
            switch (e->builtin) {
                case Builtin::Clone:
                    if (isCopy(rt)) {
                        int t = temp(rt);
                        copy(t, gen(recv), typeSize(rt));
                        return t;
                    }
                    return callRet("ax_clone_" + rt->mangle, rt, {{IT::Ptr, addr(recv)}});
                case Builtin::StrSubstring: { CArg s = str(); CArg a0 = A(0); CArg a1 = A(1); return callRet("ax_str_substring", S, {s, a0, a1, L()}); }
                case Builtin::StrContains: { CArg s = str(); return callRet("ax_str_contains", g_types.boolT(), {s, A(0)}); }
                case Builtin::StrStartsWith: { CArg s = str(); return callRet("ax_str_startswith", g_types.boolT(), {s, A(0)}); }
                case Builtin::StrEndsWith: { CArg s = str(); return callRet("ax_str_endswith", g_types.boolT(), {s, A(0)}); }
                case Builtin::StrIndexOf: { CArg s = str(); return callRet("ax_str_indexof", I, {s, A(0)}); }
                case Builtin::StrLastIndexOf: { CArg s = str(); return callRet("ax_str_lastindexof", I, {s, A(0)}); }
                case Builtin::StrTrim: return callRet("ax_str_trim", S, {str()});
                case Builtin::StrToUpper: return callRet("ax_str_toupper", S, {str()});
                case Builtin::StrToLower: return callRet("ax_str_tolower", S, {str()});
                case Builtin::StrSplit: { CArg s = str(); return callRet("ax_str_split", e->type, {s, A(0)}); }
                case Builtin::StrToInt: return callRet("ax_str_toint", e->type, {str()});
                case Builtin::StrToDouble: return callRet("ax_str_todouble", e->type, {str()});
                case Builtin::StrByteAt: { CArg s = str(); return callRet("ax_str_byteat", e->type, {s, A(0), L()}); }
                case Builtin::StrReplace: { CArg s = str(); CArg a0 = A(0); return callRet("ax_str_replace", S, {s, a0, A(1)}); }
                case Builtin::StrChars: return callRet("ax_str_chars", e->type, {str()});
                case Builtin::StrPadLeft: { CArg s = str(); return callRet("ax_str_pad", S, {s, A(0), {IT::I8, iconst(IT::I8, 1)}}); }
                case Builtin::StrPadRight: { CArg s = str(); return callRet("ax_str_pad", S, {s, A(0), {IT::I8, iconst(IT::I8, 0)}}); }
                case Builtin::StrRepeat: { CArg s = str(); return callRet("ax_str_repeat", S, {s, A(0), L()}); }
                case Builtin::StrIsEmpty: {
                    int s = view(recv);
                    return storeTemp(g_types.boolT(), cmp(Op::ICmp, Cond::Eq, load(IT::I64, addI(s, 8)), iconst(IT::I64, 0)));
                }
                case Builtin::ListAdd: { int l = addr(recv); call("ax_list_add_" + rt->a->mangle, IT::Void, {{IT::Ptr, l}, byValue(rt->a, val(e->args[0]))}); return -1; }
                case Builtin::ListPop: { int l = addr(recv); return callRet("ax_list_pop_" + rt->a->mangle, rt->a, {{IT::Ptr, l}, L()}); }
                case Builtin::ListRemoveAt: { int l = addr(recv); return callRet("ax_list_removeAt_" + rt->a->mangle, rt->a, {{IT::Ptr, l}, A(0), L()}); }
                case Builtin::ListInsert: {
                    int l = addr(recv);
                    CArg i = A(0);
                    call("ax_list_insert_" + rt->a->mangle, IT::Void, {{IT::Ptr, l}, i, byValue(rt->a, val(e->args[1])), L()});
                    return -1;
                }
                case Builtin::ListClear: call("ax_list_clear_" + rt->a->mangle, IT::Void, {{IT::Ptr, addr(recv)}}); return -1;
                case Builtin::SeqContains: case Builtin::SeqIndexOf: {
                    int p = addr(recv);
                    std::string fnName = std::string("ax_") + (rt->k == TK::List ? "list_" : "arr_") + rt->a->mangle + "_indexOf";
                    int r = call(fnName, IT::I32, {{IT::Ptr, p}, A(0)});
                    if (e->builtin == Builtin::SeqIndexOf) return storeTemp(I, r);
                    return storeTemp(g_types.boolT(), cmp(Op::ICmp, Cond::Ge, r, iconst(IT::I32, 0)));
                }
                case Builtin::SeqSort:
                    call(std::string("ax_") + (rt->k == TK::List ? "list_" : "arr_") + rt->a->mangle + "_sort", IT::Void, {{IT::Ptr, addr(recv)}});
                    return -1;
                case Builtin::SeqReverse: {
                    int p = addr(recv);
                    call("ax_reverse", IT::Void, {{IT::Ptr, seqData(p)}, {IT::I64, seqLen(p)}, {IT::I64, iconst(IT::I64, typeSize(rt->a))}});
                    return -1;
                }
                case Builtin::MapGet: case Builtin::MapContains: case Builtin::MapRemove: {
                    int m = addr(recv);
                    CArg k = A(0);
                    std::string n = e->builtin == Builtin::MapGet ? "get" : e->builtin == Builtin::MapContains ? "contains" : "remove";
                    return callRet("ax_map_" + n + "_" + rt->mangle, e->type, {{IT::Ptr, m}, k});
                }
                case Builtin::MapKeys: return callRet("ax_map_keys_" + rt->mangle, e->type, {{IT::Ptr, addr(recv)}});
                case Builtin::MapValues: return callRet("ax_map_values_" + rt->mangle, e->type, {{IT::Ptr, addr(recv)}});
                case Builtin::MapClear: call("ax_map_clear_" + rt->mangle, IT::Void, {{IT::Ptr, addr(recv)}}); return -1;
                default: return -1;
            }
        }
        case CallKind::EnumCtor: {
            std::vector<CArg> as;
            auto& v = e->ed->variants[e->variant];
            for (size_t i = 0; i < e->args.size(); i++) as.push_back(byValue(v.fields[i]->rtype, val(e->args[i])));
            return callRet(e->ed->cname + "__" + v.name, e->type, as);
        }
        case CallKind::ResultOk: return callRet("ax_ok_" + e->type->mangle, e->type, {byValue(e->type->a, val(e->args[0]))});
        case CallKind::ResultErr: return callRet("ax_err_" + e->type->mangle, e->type, {byValue(e->type->b, val(e->args[0]))});
        case CallKind::BoxNew: return callRet("ax_box_new_" + e->type->a->mangle, e->type, {byValue(e->type->a, val(e->args[0]))});
        default: return -1;
    }
}

int Gen::arith(T op, Type* R, int a, int b, Span sp) {
    IT it = scalarIT(R);
    bool isInt = R->k == TK::Int, isLong = R->k == TK::Long;
    if (isInt || isLong) {
        std::string sfx = isInt ? "i32" : "i64";
        const char* name = nullptr;
        switch (op) {
            case T::Plus: case T::PlusEq: name = "add"; break;
            case T::Minus: case T::MinusEq: name = "sub"; break;
            case T::Star: case T::StarEq: name = "mul"; break;
            case T::Slash: case T::SlashEq: name = "div"; break;
            case T::Percent: case T::PercentEq: name = "mod"; break;
            default: break;
        }
        if (name) return call(std::string("ax_") + name + "_" + sfx, it, {{it, a}, {it, b}, {IT::Ptr, locArg(sp)}});
    }
    switch (op) {
        case T::Plus: case T::PlusEq: return bin(Op::FAdd, it, a, b);
        case T::Minus: case T::MinusEq: return bin(Op::FSub, it, a, b);
        case T::Star: case T::StarEq: return bin(Op::FMul, it, a, b);
        case T::Slash: case T::SlashEq: return bin(Op::FDiv, it, a, b);
        case T::Percent: case T::PercentEq: return call(it == IT::F32 ? "fmodf" : "fmod", it, {{it, a}, {it, b}});
        case T::Amp: case T::AmpEq: return bin(Op::And, it, a, b);
        case T::Pipe: case T::PipeEq: return bin(Op::Or, it, a, b);
        case T::Caret: case T::CaretEq: return bin(Op::Xor, it, a, b);
        case T::Shl: case T::ShlEq: return bin(Op::Shl, it, a, b);
        case T::Shr: case T::ShrEq: return bin(isUnsigned(R) ? Op::LShr : Op::AShr, it, a, b);
        default: return a;
    }
}

void Gen::appendPart(int sb, Expr* p) {
    CArg s{IT::Ptr, sb};
    switch (p->type->k) {
        case TK::String: call("ax_sb_str", IT::Void, {s, byValue(p->type, view(p))}); break;
        case TK::Float: call("ax_sb_f32", IT::Void, {s, {IT::F32, scalar(p)}}); break;
        case TK::Double: call("ax_sb_f64", IT::Void, {s, {IT::F64, scalar(p)}}); break;
        case TK::Bool: call("ax_sb_bool", IT::Void, {s, {IT::I8, scalar(p)}}); break;
        case TK::Char: call("ax_sb_char", IT::Void, {s, {IT::I32, scalar(p)}}); break;
        default: call("ax_sb_i64", IT::Void, {s, {IT::I64, convert(scalar(p), p->type, g_types.longT())}}); break;
    }
}

int Gen::genConcat(const std::vector<Expr*>& parts) {
    int sbs = slot(24, 8);
    int sb = slotAddr(sbs);
    zero(sb, 24);
    for (auto* p : parts) appendPart(sb, p);
    return callRet("ax_sb_done", g_types.stringT(), {{IT::Ptr, slotAddr(sbs)}});
}

int Gen::genBinary(Expr* e) {
    T op = e->op;
    Type* B = g_types.boolT();
    if (op == T::AndAnd || op == T::OrOr) {
        int res = temp(B);
        store(IT::I8, res, boolReg(e->a));
        int rhs = block(), done = block();
        int l = load(IT::I8, res);
        if (op == T::AndAnd) condBr(l, rhs, done);
        else condBr(l, done, rhs);
        setBlock(rhs);
        Ctx c;
        int r;
        sub(c, [&] { r = boolReg(e->b); });
        store(IT::I8, res, r);
        flushPost(c);
        br(done);
        setBlock(done);
        return res;
    }
    if (op == T::QQ) return genCoalesce(e);
    if (e->isNoneCompare) {
        Expr* other = e->a->kind == EK::None ? e->b : e->a;
        int has = load(IT::I8, view(other));
        // `x == none` is true when the optional has no value
        return storeTemp(B, cmp(Op::ICmp, op == T::EqEq ? Cond::Eq : Cond::Ne, has, iconst(IT::I8, 0)));
    }
    if (e->isConcat) {
        std::vector<Expr*> parts;
        std::function<void(Expr*)> collect = [&](Expr* x) {
            if (x->kind == EK::Binary && x->isConcat && x->op == T::Plus) { collect(x->a); collect(x->b); }
            else parts.push_back(x);
        };
        collect(e);
        return genConcat(parts);
    }
    Type* ta = e->a->type;
    Type* tb = e->b->type;
    switch (op) {
        case T::EqEq: case T::NotEq: case T::Lt: case T::Gt: case T::Le: case T::Ge: {
            Cond c = op == T::EqEq ? Cond::Eq : op == T::NotEq ? Cond::Ne : op == T::Lt ? Cond::Lt : op == T::Gt ? Cond::Gt : op == T::Le ? Cond::Le : Cond::Ge;
            if (ta->k == TK::String) {
                CArg a = byValue(ta, view(e->a));
                CArg b = byValue(tb, view(e->b));
                int eq = call("ax_str_eq", IT::I8, {a, b});
                if (op == T::NotEq) eq = bin(Op::Xor, IT::I8, eq, iconst(IT::I8, 1));
                return storeTemp(B, eq);
            }
            if (ta->k == TK::Enum) {
                int a = load(IT::I32, gen(e->a));
                int b = load(IT::I32, gen(e->b));
                return storeTemp(B, cmp(Op::ICmp, c, a, b));
            }
            int a = scalar(e->a);
            int b = scalar(e->b);
            if (ta->k == TK::Bool) return storeTemp(B, cmp(Op::ICmp, c, a, b));
            bool fl = itFloat(scalarIT(ta)) || itFloat(scalarIT(tb));
            Type* W = fl ? g_types.doubleT() : g_types.longT();
            a = convert(a, ta, W);
            b = convert(b, tb, W);
            return storeTemp(B, cmp(fl ? Op::FCmp : Op::ICmp, c, a, b));
        }
        default: {
            Type* R = e->type;
            int a = convert(scalar(e->a), ta, R);
            int b = scalar(e->b);
            b = (op == T::Shl || op == T::Shr) ? convert(b, tb, R) : convert(b, tb, R);
            return storeTemp(R, arith(op, R, a, b, e->span));
        }
    }
}

int Gen::genTernary(Expr* e) {
    int c = boolReg(e->a);
    bool owning = e->consumed || isCopy(e->type);
    int res = temp(e->type);
    int bt = block(), bf = block(), done = block();
    condBr(c, bt, bf);
    int n = typeSize(e->type);
    setBlock(bt);
    {
        Ctx s;
        sub(s, [&] { copy(slotAddr(tempSlot[res]), owning ? val(e->b) : ownedCopy(e->b), n); });
        flushPost(s);
        br(done);
    }
    setBlock(bf);
    {
        Ctx s;
        sub(s, [&] { copy(slotAddr(tempSlot[res]), owning ? val(e->c) : ownedCopy(e->c), n); });
        flushPost(s);
        br(done);
    }
    setBlock(done);
    return slotAddr(tempSlot[res]);
}

int Gen::genCoalesce(Expr* e) {
    Type* RT = e->type;
    Type* O = e->a->type;
    bool resultIsOpt = RT->k == TK::Optional;
    int res = temp(RT);
    int rs = tempSlot[res];
    int n = typeSize(RT);
    int valOff = layout(O).valOff;
    int o;
    std::string cloneFn;
    if (e->consumed || isCopy(RT) || needsDrop(e->a)) o = val(e->a);
    else {
        o = addr(e->a);
        cloneFn = resultIsOpt ? "ax_clone_" + O->mangle : "ax_clone_" + O->a->mangle;
    }
    int has = load(IT::I8, o);
    int yes = block(), no = block(), done = block();
    condBr(has, yes, no);
    setBlock(yes);
    if (cloneFn.empty()) copy(slotAddr(rs), resultIsOpt ? o : addI(o, valOff), n);
    else {
        int c = callRet(cloneFn, RT, {{IT::Ptr, resultIsOpt ? o : addI(o, valOff)}});
        copy(slotAddr(rs), c, n);
    }
    br(done);
    setBlock(no);
    {
        Ctx s;
        sub(s, [&] { copy(slotAddr(rs), (e->consumed || isCopy(RT)) ? val(e->b) : ownedCopy(e->b), n); });
        flushPost(s);
        br(done);
    }
    setBlock(done);
    return slotAddr(rs);
}

int Gen::genTry(Expr* e) {
    Type* R = e->a->type;
    Type* RetT = fn->retType;
    int r = val(e->a);
    int fail = block(), ok = block();
    if (R->k == TK::Result) {
        int isErr = cmp(Op::ICmp, Cond::Ne, load(IT::I32, r), iconst(IT::I32, 0));
        condBr(isErr, fail, ok);
        setBlock(fail);
        int errp = addI(r, layout(R).valOff);
        int out = callRet("ax_err_" + RetT->mangle, RetT, {byValue(R->b, errp)});
        for (size_t i = ctx.size(); i-- > 0;)
            for (auto it = ctx[i]->post.rbegin(); it != ctx[i]->post.rend(); ++it) emitDrop(*it);
        dropScopes(0);
        retValue(out);
        setBlock(ok);
        return addI(r, layout(R).valOff);
    }
    int has = load(IT::I8, r);
    condBr(has, ok, fail);
    setBlock(fail);
    int none = callRet("ax_none_" + RetT->a->mangle, RetT, {});
    for (size_t i = ctx.size(); i-- > 0;)
        for (auto it = ctx[i]->post.rbegin(); it != ctx[i]->post.rend(); ++it) emitDrop(*it);
    dropScopes(0);
    retValue(none);
    setBlock(ok);
    return addI(r, layout(R).valOff);
}

void Gen::bindVar(PatBind& b, int ptr) {
    if (!b.var) return;
    int s = varSlot.count(b.var) ? varSlot[b.var] : declare(b.var);
    if (b.var->mode == 0) copy(slotAddr(s), ptr, typeSize(b.var->type));
    else store(IT::Ptr, slotAddr(s), ptr);
}

// Returns an I8 register: does the value at `s` match? With `bind`, also fills the pattern variables.
int Gen::patternTest(Pattern* pt, Type* subj, int s, bool bind) {
    int res = tempOf(IT::I8);
    int rs = tempSlot[res];
    int test;
    switch (pt->kind) {
        case Pattern::Variant:
            if (subj->k == TK::Enum || subj->k == TK::Result) test = cmp(Op::ICmp, Cond::Eq, load(IT::I32, s), iconst(IT::I32, pt->variant));
            else test = load(IT::I8, s);
            break;
        case Pattern::TypeBind: test = load(IT::I8, s); break;
        case Pattern::ExprPat:
            if (pt->expr->kind == EK::None) test = cmp(Op::ICmp, Cond::Eq, load(IT::I8, s), iconst(IT::I8, 0));
            else if (subj->k == TK::String) {
                CArg a = byValue(subj, s);
                test = call("ax_str_eq", IT::I8, {a, byValue(subj, view(pt->expr))});
            } else {
                Type* et = pt->expr->type;
                bool fl = itFloat(scalarIT(subj)) || itFloat(scalarIT(et));
                Type* W = fl ? g_types.doubleT() : (subj->k == TK::Bool ? g_types.boolT() : g_types.longT());
                int a = convert(load(scalarIT(subj), s), subj, W);
                int b = convert(scalar(pt->expr), et, W);
                test = cmp(fl ? Op::FCmp : Op::ICmp, Cond::Eq, a, b);
            }
            break;
    }
    store(IT::I8, slotAddr(rs), test);
    if (bind) {
        int yes = block(), done = block();
        condBr(test, yes, done);
        setBlock(yes);
        bindPattern(pt, subj, s);
        br(done);
        setBlock(done);
    }
    return load(IT::I8, slotAddr(rs));
}

void Gen::bindPattern(Pattern* pt, Type* subj, int s) {
    if (pt->kind == Pattern::Variant && subj->k == TK::Enum) {
        const Layout& L = layout(subj);
        for (size_t i = 0; i < pt->binds.size(); i++) bindVar(pt->binds[i], addI(s, L.voffs[pt->variant][i]));
    } else if (pt->kind == Pattern::Variant && subj->k == TK::Result) {
        if (!pt->binds.empty()) bindVar(pt->binds[0], addI(s, layout(subj).valOff));
    } else if (pt->kind == Pattern::TypeBind) {
        bindVar(pt->bind, addI(s, layout(subj).valOff));
    }
}

// ---------------------------------------------------------------- statements

void Gen::retValue(int v) {
    Type* rt = fn ? fn->retType : g_types.voidT();
    Abi a = classify(rt);
    Inst i{Op::Ret};
    if (a.pass == Pass::Void) {
        i.a = -1;
    } else if (a.pass == Pass::Ref) {
        int out = load(IT::Ptr, slotAddr(sretSlot));
        copy(out, v, typeSize(rt));
        i.a = out;
        i.type = IT::Ptr;
    } else {
        i.a = load(a.it, v);
        i.type = a.it;
    }
    put(i);
    setBlock(block());  // anything after a return is unreachable
}

void Gen::genBlock(Stmt* b) {
    pushScope(0);
    if (b->kind == SK::Block)
        for (auto* s : b->stmts) genStmt(s);
    else genStmt(b);
    popScope();
}

void Gen::genAssign(Expr* e) {
    Expr* target = e->a;
    Type* T_ = target->type;
    Ctx c;
    sub(c, [&] {
        int n = typeSize(T_);
        if (e->op == T::Assign) {
            if (isCopy(T_)) {
                int v = val(e->b);
                int tmp = temp(T_);
                copy(tmp, v, n);
                copy(addrForWrite(target), tmp, n);
            } else {
                int v = temp(T_);
                copy(v, val(e->b), n);
                int p = addrForWrite(target);
                call("ax_drop_" + T_->mangle, IT::Void, {{IT::Ptr, p}});
                copy(p, v, n);
            }
        } else if (e->isConcat) {
            int v = genConcat({target, e->b});
            int p = addrForWrite(target);
            call("ax_drop_" + T_->mangle, IT::Void, {{IT::Ptr, p}});
            copy(p, v, n);
        } else {
            int p = addrForWrite(target);
            IT it = scalarIT(T_);
            int cur = load(it, p);
            int rhs = convert(scalar(e->b), e->b->type, T_);
            store(it, p, arith(e->op, T_, cur, rhs, e->span));
        }
    });
    flushPost(c);
}

void Gen::genStmt(Stmt* s) {
    switch (s->kind) {
        case SK::Block: genBlock(s); break;
        case SK::Var: {
            VarDecl* v = s->var;
            int sl = declare(v);
            Ctx c;
            sub(c, [&] { copy(slotAddr(sl), val(v->init), typeSize(v->type)); });
            flushPost(c);
            addOwned(v);
            break;
        }
        case SK::ExprStmt: {
            Expr* e = s->expr;
            if (e->kind == EK::Assign) { genAssign(e); break; }
            Ctx c;
            sub(c, [&] {
                if (!e->type || e->type->k == TK::Void || isCopy(e->type)) gen(e);
                else view(e);
            });
            flushPost(c);
            break;
        }
        case SK::If: {
            bool hasNeg = !s->negPatVars.empty();
            bool wrap = !hasNeg;
            if (hasNeg) declPatVars(s->negPatVars);
            if (wrap) pushScope(0);
            declPatVars(s->patVars);
            Ctx c;
            int cond;
            sub(c, [&] { cond = boolReg(s->expr); });
            for (auto& d : c.post) scopes.back().drops.push_back(d);
            int bt = block(), bf = block(), done = block();
            condBr(cond, bt, s->els ? bf : done);
            setBlock(bt);
            genBlock(s->then);
            br(done);
            setBlock(bf);
            if (s->els) genBlock(s->els);
            br(done);
            setBlock(done);
            if (wrap) popScope();
            break;
        }
        case SK::While: case SK::For: case SK::DoWhile: {
            pushScope(0);  // for-init variables
            if (s->kind == SK::For && s->init) genStmt(s->init);
            declPatVars(s->patVars);
            int condB = block(), body = block(), cont = block(), exitDrops = block(), exit = block();
            br(s->kind == SK::DoWhile ? body : condB);
            // condition
            setBlock(condB);
            Ctx c;
            int cv = -1;
            if (s->expr) sub(c, [&] { cv = boolReg(s->expr); });
            if (cv >= 0) condBr(cv, body, exitDrops);
            else br(body);
            setBlock(exitDrops);
            flushPost(c);
            br(exit);
            // body
            setBlock(body);
            pushScope(1);
            scopes.back().brk = exit;
            scopes.back().cont = cont;
            if (s->kind != SK::DoWhile) scopes.back().drops = c.post;  // condition temps live through the body
            genBlock(s->body);
            scopes.back().drops.clear();
            popScope();
            br(cont);
            setBlock(cont);
            if (s->kind != SK::DoWhile) flushPost(c);
            if (s->kind == SK::For && s->step) {
                Stmt step;
                step.kind = SK::ExprStmt;
                step.expr = s->step;
                genStmt(&step);
            }
            br(condB);
            setBlock(exit);
            popScope();
            break;
        }
        case SK::Foreach: {
            pushScope(0);
            Type* CT = s->expr->type;
            Ctx c;
            int coll;
            sub(c, [&] { coll = addr(s->expr); });
            for (auto& d : c.post) scopes.back().drops.push_back(d);
            int collSlot = slot(8, 8);
            store(IT::Ptr, slotAddr(collSlot), coll);
            int iSlot = slot(8, 8);
            store(IT::I64, slotAddr(iSlot), iconst(IT::I64, 0));
            int condB = block(), body = block(), cont = block(), exit = block();
            br(condB);
            setBlock(condB);
            int cp = load(IT::Ptr, slotAddr(collSlot));
            int lt = cmp(Op::ICmp, Cond::Lt, load(IT::I64, slotAddr(iSlot)), seqLen(cp));
            condBr(lt, body, exit);
            setBlock(body);
            pushScope(1);
            scopes.back().brk = exit;
            scopes.back().cont = cont;
            VarDecl* v = s->var;
            int vs = declare(v);
            int es = typeSize(CT->a);
            int data = seqData(load(IT::Ptr, slotAddr(collSlot)));
            int off = bin(Op::Mul, IT::I64, load(IT::I64, slotAddr(iSlot)), iconst(IT::I64, es));
            Inst pa{Op::PtrAdd};
            pa.type = IT::Ptr;
            pa.dst = reg(IT::Ptr);
            pa.a = data;
            pa.b = off;
            put(pa);
            if (v->mode == 0) copy(slotAddr(vs), pa.dst, typeSize(v->type));
            else store(IT::Ptr, slotAddr(vs), pa.dst);
            genBlock(s->body);
            popScope();
            br(cont);
            setBlock(cont);
            store(IT::I64, slotAddr(iSlot), bin(Op::Add, IT::I64, load(IT::I64, slotAddr(iSlot)), iconst(IT::I64, 1)));
            br(condB);
            setBlock(exit);
            popScope();
            break;
        }
        case SK::Switch: {
            pushScope(0);
            Type* S = s->expr->type;
            Ctx c;
            int subj;
            sub(c, [&] { subj = addr(s->expr); });
            for (auto& d : c.post) scopes.back().drops.push_back(d);
            int ss = slot(8, 8);
            store(IT::Ptr, slotAddr(ss), subj);
            int end = block();
            pushScope(2);
            scopes.back().brk = end;
            const Case* def = nullptr;
            for (auto& k : s->cases) {
                if (k.isDefault) { def = &k; continue; }
                int body = block(), next = block();
                for (size_t i = 0; i < k.pats.size(); i++) {
                    int alt = i + 1 < k.pats.size() ? block() : next;
                    Ctx tc;
                    int t;
                    sub(tc, [&] { t = patternTest(k.pats[i], S, load(IT::Ptr, slotAddr(ss)), false); });
                    condBr(t, body, alt);
                    setBlock(alt);
                    if (alt == next) break;
                }
                setBlock(body);
                pushScope(0);
                if (k.pats.size() == 1) {
                    std::vector<VarDecl*> vs;
                    bindPattern(k.pats[0], S, load(IT::Ptr, slotAddr(ss)));
                }
                for (auto* st : k.body) genStmt(st);
                popScope();
                br(end);
                setBlock(next);
            }
            if (def) {
                pushScope(0);
                for (auto* st : def->body) genStmt(st);
                popScope();
            }
            br(end);
            popScope();
            setBlock(end);
            popScope();
            break;
        }
        case SK::Break: {
            int k = (int)scopes.size() - 1;
            while (k >= 0 && scopes[k].kind == 0) k--;
            if (k < 0) break;
            dropScopes(scopes[k].kind == 2 ? k + 1 : k);
            br(scopes[k].brk);
            setBlock(block());
            break;
        }
        case SK::Continue: {
            int k = (int)scopes.size() - 1;
            while (k >= 0 && scopes[k].kind != 1) k--;
            if (k < 0) break;
            dropScopes(k + 1);
            br(scopes[k].cont);
            setBlock(block());
            break;
        }
        case SK::Return: {
            Ctx c;
            int keep = -1;
            Type* rt = fn->retType;
            if (s->expr && rt->k != TK::Void) {
                sub(c, [&] {
                    int v = val(s->expr);
                    keep = temp(rt);
                    copy(keep, v, typeSize(rt));
                });
            } else if (s->expr) {
                sub(c, [&] { gen(s->expr); });
            }
            flushPost(c);
            dropScopes(0);
            if (keep >= 0) retValue(slotAddr(tempSlot[keep]));
            else retValue(-1);
            break;
        }
    }
}

void Gen::genFunc(FuncDecl* f) {
    M.funcs.emplace_back();
    F = &M.funcs.back();
    F->name = f->cname;
    fn = f;
    varSlot.clear();
    tempSlot.clear();
    scopes.clear();
    ctx.clear();
    selfSlot = sretSlot = -1;
    cur = block();
    Abi ra = classify(f->retType);
    F->ret = ra.pass == Pass::Ref ? IT::Ptr : ra.it;
    int pi = 0;
    auto param = [&](IT t) {
        F->params.push_back(t);
        Inst i{Op::Param};
        i.type = t;
        i.dst = reg(t);
        i.imm = pi++;
        put(i);
        return i.dst;
    };
    if (ra.pass == Pass::Ref) {
        sretSlot = slot(8, 8);
        store(IT::Ptr, slotAddr(sretSlot), param(IT::Ptr));
    }
    if (f->selfType && !f->ownerIface) {
        selfSlot = slot(8, 8);
        store(IT::Ptr, slotAddr(selfSlot), param(IT::Ptr));
    }
    pushScope(0);
    for (auto& p : f->params) {
        VarDecl* v = p.var;
        int s = declare(v);
        if (p.mode != 0 && v->type->k != TK::Interface) {
            store(IT::Ptr, slotAddr(s), param(IT::Ptr));
            continue;
        }
        Abi a = classify(v->type);
        if (a.pass == Pass::Ref) copy(slotAddr(s), param(IT::Ptr), typeSize(v->type));
        else store(a.it, slotAddr(s), param(a.it));
        addOwned(v);
    }
    genBlock(f->body);
    popScope();
    if (!ended()) {
        if (f->retType->k == TK::Void) retValue(-1);
        else retValue(temp(f->retType));  // not reachable: the checker requires a return
    }
    finish();
}

void Gen::finish() {
    // every block needs a terminator; unreachable leftovers just return
    for (auto& b : F->blocks) {
        bool term = !b.insts.empty() && (b.insts.back().op == Op::Br || b.insts.back().op == Op::CondBr || b.insts.back().op == Op::Ret);
        if (term) continue;
        Inst r{Op::Ret};
        if (F->ret != IT::Void) {
            Inst z{itFloat(F->ret) ? Op::FConst : Op::IConst};
            z.type = F->ret;
            z.dst = reg(F->ret);
            b.insts.push_back(z);
            r.a = z.dst;
            r.type = F->ret;
        }
        b.insts.push_back(r);
    }
}

void Gen::genInitGlobals() {
    M.funcs.emplace_back();
    F = &M.funcs.back();
    F->name = "ax_init_globals";
    fn = nullptr;
    varSlot.clear();
    tempSlot.clear();
    scopes.clear();
    ctx.clear();
    cur = block();
    for (VarDecl* v : P.globalOrder) {
        Ctx c;
        sub(c, [&] { copy(global(v->cname), val(v->init), typeSize(v->type)); });
        flushPost(c);
    }
    Inst r{Op::Ret};
    put(r);
    finish();
}

void Gen::run() {
    for (FuncDecl* f : P.funcs) genFunc(f);
    genInitGlobals();
    // everything referenced but not defined here comes from the C support object
    std::set<std::string> defined, ext;
    for (auto& f : M.funcs) defined.insert(f.name);
    for (auto& d : M.data) defined.insert(d.name);
    for (auto& f : M.funcs)
        for (auto& b : f.blocks)
            for (auto& i : b.insts)
                if ((i.op == Op::Call || i.op == Op::Global) && !i.sym.empty() && !defined.count(i.sym)) ext.insert(i.sym);
    M.externs.assign(ext.begin(), ext.end());
}

}  // namespace

int typeSize(Type* t) { return layout(t).size; }
int typeAlign(Type* t) { return layout(t).align; }

IRModule generateIR(Program& prog) {
    g_P = &prog;
    g_layouts.clear();
    IRModule m;
    Gen g(prog, m);
    g.run();
    return m;
}

// ---------------------------------------------------------------- text dump

static const char* itName(IT t) {
    switch (t) {
        case IT::Void: return "void";
        case IT::I8: return "i8";
        case IT::I16: return "i16";
        case IT::I32: return "i32";
        case IT::I64: return "i64";
        case IT::F32: return "f32";
        case IT::F64: return "f64";
        case IT::Ptr: return "ptr";
    }
    return "?";
}

std::string irToText(const IRModule& m) {
    static const char* ops[] = {"iconst", "fconst", "param", "slot", "global", "load", "store", "memcpy", "memset0", "ptradd", "ptraddi",
                                "add", "sub", "mul", "sdiv", "srem", "udiv", "urem", "and", "or", "xor", "shl", "ashr", "lshr", "neg", "not",
                                "fadd", "fsub", "fmul", "fdiv", "fneg", "icmp", "fcmp", "sext", "zext", "trunc", "sitofp", "uitofp", "fptosi",
                                "fpext", "fptrunc", "call", "callptr", "br", "condbr", "ret"};
    static const char* conds[] = {"eq", "ne", "lt", "le", "gt", "ge", "ult", "ule", "ugt", "uge"};
    std::string o;
    for (auto& d : m.data) o += "data " + d.name + " = " + std::to_string(d.bytes.size()) + " bytes\n";
    for (auto& f : m.funcs) {
        o += "\nfunc " + f.name + "(";
        for (size_t i = 0; i < f.params.size(); i++) o += std::string(i ? ", " : "") + itName(f.params[i]);
        o += ") " + std::string(itName(f.ret)) + "  ; " + std::to_string(f.slots.size()) + " slots\n";
        for (size_t bi = 0; bi < f.blocks.size(); bi++) {
            o += "b" + std::to_string(bi) + ":\n";
            for (auto& i : f.blocks[bi].insts) {
                o += "    ";
                if (i.dst >= 0) o += "%" + std::to_string(i.dst) + " = ";
                o += ops[(int)i.op];
                if (i.type != IT::Void) o += std::string(".") + itName(i.type);
                if (i.op == Op::ICmp || i.op == Op::FCmp) o += std::string(" ") + conds[(int)i.cond];
                if (!i.sym.empty()) o += " @" + i.sym;
                if (i.slot >= 0) o += " slot" + std::to_string(i.slot);
                if (i.a >= 0) o += " %" + std::to_string(i.a);
                if (i.b >= 0) o += ", %" + std::to_string(i.b);
                if (i.op == Op::IConst || i.op == Op::Param || i.op == Op::PtrAddI || i.op == Op::Memcpy || i.op == Op::Memset0)
                    o += " " + std::to_string(i.imm);
                if (i.op == Op::FConst) o += " " + std::to_string(i.fimm);
                for (size_t k = 0; k < i.args.size(); k++) o += (k ? ", %" : " (%") + std::to_string(i.args[k]) + (k + 1 == i.args.size() ? ")" : "");
                if (i.t1 >= 0) o += " b" + std::to_string(i.t1);
                if (i.t2 >= 0) o += ", b" + std::to_string(i.t2);
                o += "\n";
            }
        }
    }
    return o;
}
