// clone.cpp - deep copies of syntax trees. Only syntactic fields are copied; the checker fills in the rest.
#include "clone.h"

TypeExpr* cloneTypeExpr(const TypeExpr* t) {
    if (!t) return nullptr;
    TypeExpr* c = new TypeExpr(*t);
    for (auto& a : c->args) a = cloneTypeExpr(a);
    return c;
}

static Pattern* clonePattern(const Pattern* p) {
    if (!p) return nullptr;
    Pattern* c = new Pattern(*p);
    for (auto& b : c->binds) {
        b.type = cloneTypeExpr(b.type);
        b.var = nullptr;
    }
    c->type = cloneTypeExpr(p->type);
    c->bind.type = cloneTypeExpr(p->bind.type);
    c->bind.var = nullptr;
    c->expr = cloneExpr(p->expr);
    return c;
}

Expr* cloneExpr(const Expr* e) {
    if (!e) return nullptr;
    Expr* c = new Expr();
    c->kind = e->kind;
    c->span = e->span;
    c->ival = e->ival;
    c->fval = e->fval;
    c->isF32 = e->isF32;
    c->isLongLit = e->isLongLit;
    c->bval = e->bval;
    c->cval = e->cval;
    c->sval = e->sval;
    c->name = e->name;
    c->op = e->op;
    c->prefix = e->prefix;
    c->isMut = e->isMut;
    c->a = cloneExpr(e->a);
    c->b = cloneExpr(e->b);
    c->c = cloneExpr(e->c);
    for (auto* x : e->args) c->args.push_back(cloneExpr(x));
    c->fieldNames = e->fieldNames;
    c->fieldSpans = e->fieldSpans;
    c->texpr = cloneTypeExpr(e->texpr);
    c->pat = clonePattern(e->pat);
    return c;
}

static VarDecl* cloneVar(const VarDecl* v) {
    if (!v) return nullptr;
    VarDecl* c = new VarDecl();
    c->kind = v->kind;
    c->name = v->name;
    c->texpr = cloneTypeExpr(v->texpr);
    c->init = cloneExpr(v->init);
    c->span = v->span;
    c->isConst = v->isConst;
    c->mod = v->mod;
    return c;
}

Stmt* cloneStmt(const Stmt* s) {
    if (!s) return nullptr;
    Stmt* c = new Stmt();
    c->kind = s->kind;
    c->span = s->span;
    for (auto* x : s->stmts) c->stmts.push_back(cloneStmt(x));
    c->var = cloneVar(s->var);
    c->expr = cloneExpr(s->expr);
    c->then = cloneStmt(s->then);
    c->els = cloneStmt(s->els);
    c->init = cloneStmt(s->init);
    c->body = cloneStmt(s->body);
    c->step = cloneExpr(s->step);
    c->foreachMode = s->foreachMode;
    for (auto& k : s->cases) {
        Case nk;
        nk.isDefault = k.isDefault;
        nk.span = k.span;
        for (auto* p : k.pats) nk.pats.push_back(clonePattern(p));
        for (auto* b : k.body) nk.body.push_back(cloneStmt(b));
        c->cases.push_back(nk);
    }
    return c;
}

FuncDecl* cloneFunc(const FuncDecl* f) {
    FuncDecl* c = new FuncDecl();
    c->name = f->name;
    c->ret = cloneTypeExpr(f->ret);
    for (auto& p : f->params) {
        Param np = p;
        np.type = cloneTypeExpr(p.type);
        np.var = nullptr;
        c->params.push_back(np);
    }
    c->body = cloneStmt(f->body);
    c->span = f->span;
    c->isMut = f->isMut;
    c->isPrivate = f->isPrivate;
    c->isTest = false;
    c->mod = f->mod;
    c->attrs = f->attrs;
    c->gen.typeParams = f->gen.typeParams;
    return c;
}

static FieldDecl* cloneField(const FieldDecl* f) {
    FieldDecl* c = new FieldDecl();
    c->name = f->name;
    c->type = cloneTypeExpr(f->type);
    c->def = cloneExpr(f->def);
    c->isPrivate = f->isPrivate;
    c->span = f->span;
    return c;
}

StructDecl* cloneStruct(const StructDecl* s) {
    StructDecl* c = new StructDecl();
    c->name = s->name;
    for (auto* f : s->fields) c->fields.push_back(cloneField(f));
    for (auto* m : s->methods) {
        FuncDecl* nm = cloneFunc(m);
        nm->ownerStruct = c;
        c->methods.push_back(nm);
    }
    for (auto* i : s->ifaceExprs) c->ifaceExprs.push_back(cloneTypeExpr(i));
    c->isPrivate = s->isPrivate;
    c->mod = s->mod;
    c->span = s->span;
    c->gen.typeParams = s->gen.typeParams;
    return c;
}

EnumDecl* cloneEnum(const EnumDecl* e) {
    EnumDecl* c = new EnumDecl();
    c->name = e->name;
    for (auto& v : e->variants) {
        Variant nv;
        nv.name = v.name;
        nv.span = v.span;
        for (auto* f : v.fields) nv.fields.push_back(cloneField(f));
        c->variants.push_back(nv);
    }
    c->isPrivate = e->isPrivate;
    c->mod = e->mod;
    c->span = e->span;
    c->gen.typeParams = e->gen.typeParams;
    return c;
}
