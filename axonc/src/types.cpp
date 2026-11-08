// types.cpp
#include "types.h"

TypeTable g_types;

TypeTable::TypeTable() {
    struct P { TK k; const char* m; const char* c; };
    static const P prims[] = {
        {TK::Error, "error", "int"},     {TK::Void, "void", "void"},       {TK::Byte, "byte", "uint8_t"},
        {TK::Int, "int", "int32_t"},     {TK::Long, "long", "int64_t"},    {TK::Float, "float", "float"},
        {TK::Double, "double", "double"}, {TK::Bool, "bool", "bool"},       {TK::Char, "char", "ax_char"},
        {TK::String, "string", "ax_string"}, {TK::None, "none", "int"},
    };
    for (auto& p : prims) {
        Type t;
        t.k = p.k;
        t.mangle = p.m;
        t.cname = p.c;
        intern(std::string("prim:") + p.m, t);
    }
}

Type* TypeTable::intern(const std::string& key, Type proto) {
    auto it = byKey.find(key);
    if (it != byKey.end()) return it->second;
    Type* t = new Type(proto);
    byKey[key] = t;
    types.push_back(t);
    return t;
}

Type* TypeTable::prim(TK k) {
    static const char* names[] = {"error", "void", "byte", "int", "long", "float", "double", "bool", "char", "string"};
    if (k == TK::None) return byKey["prim:none"];
    return byKey[std::string("prim:") + names[(int)k]];
}



Type* TypeTable::structT(StructDecl* sd) {
    Type t;
    t.k = TK::Struct;
    t.sd = sd;
    t.mangle = sd->isExtern ? sd->name : sd->cname;
    t.cname = sd->isExtern ? sd->name : sd->cname;
    return intern("struct:" + std::to_string((uintptr_t)sd), t);
}

Type* TypeTable::enumT(EnumDecl* ed) {
    Type t;
    t.k = TK::Enum;
    t.ed = ed;
    t.mangle = ed->cname;
    t.cname = ed->cname;
    return intern("enum:" + std::to_string((uintptr_t)ed), t);
}

Type* TypeTable::ifaceT(InterfaceDecl* id) {
    Type t;
    t.k = TK::Interface;
    t.id = id;
    t.mangle = id->name;
    t.cname = "ax_ref_" + id->cname;
    return intern("iface:" + std::to_string((uintptr_t)id), t);
}

Type* TypeTable::arrayOf(Type* e) {
    Type t;
    t.k = TK::Array;
    t.a = e;
    t.mangle = "arr_" + e->mangle;
    t.cname = "ax_" + t.mangle;
    return intern("arr:" + std::to_string((uintptr_t)e), t);
}

Type* TypeTable::listOf(Type* e) {
    Type t;
    t.k = TK::List;
    t.a = e;
    t.mangle = "list_" + e->mangle;
    t.cname = "ax_" + t.mangle;
    return intern("list:" + std::to_string((uintptr_t)e), t);
}

Type* TypeTable::optOf(Type* e) {
    Type t;
    t.k = TK::Optional;
    t.a = e;
    t.mangle = "opt_" + e->mangle;
    t.cname = "ax_" + t.mangle;
    return intern("opt:" + std::to_string((uintptr_t)e), t);
}

Type* TypeTable::resultOf(Type* ok, Type* err) {
    Type t;
    t.k = TK::Result;
    t.a = ok;
    t.b = err;
    t.mangle = "res_" + ok->mangle + "_" + err->mangle;
    t.cname = "ax_" + t.mangle;
    return intern("res:" + std::to_string((uintptr_t)ok) + ":" + std::to_string((uintptr_t)err), t);
}

Type* TypeTable::boxOf(Type* e) {
    Type t;
    t.k = TK::Box;
    t.a = e;
    t.mangle = "box_" + e->mangle;
    t.cname = "ax_" + t.mangle;
    return intern("box:" + std::to_string((uintptr_t)e), t);
}

Type* TypeTable::mapOf(Type* k, Type* v) {
    Type t;
    t.k = TK::Map;
    t.a = k;
    t.b = v;
    t.mangle = "map_" + k->mangle + "_" + v->mangle;
    t.cname = "ax_" + t.mangle;
    return intern("map:" + std::to_string((uintptr_t)k) + ":" + std::to_string((uintptr_t)v), t);
}

std::string typeName(Type* t) {
    if (!t) return "?";
    switch (t->k) {
        case TK::Error: return "<error>";
        case TK::Struct: return t->sd->name;
        case TK::Enum: return t->ed->name;
        case TK::Interface: return t->id->name;
        case TK::Array: return typeName(t->a) + "[]";
        case TK::List: return "List<" + typeName(t->a) + ">";
        case TK::Optional: return typeName(t->a) + "?";
        case TK::Result: return "Result<" + typeName(t->a) + ", " + typeName(t->b) + ">";
        case TK::Box: return "Box<" + typeName(t->a) + ">";
        case TK::Map: return "Map<" + typeName(t->a) + ", " + typeName(t->b) + ">";
        case TK::None: return "none";
        default: return t->mangle;
    }
}

bool isErr(Type* t) { return !t || t->k == TK::Error; }

int numRank(Type* t) {
    switch (t->k) {
        case TK::Byte: return 0;
        case TK::Int: return 1;
        case TK::Long: return 2;
        case TK::Float: return 3;
        case TK::Double: return 4;
        default: return -1;
    }
}

bool isNumeric(Type* t) { return numRank(t) >= 0; }
bool isInteger(Type* t) { return t->k == TK::Byte || t->k == TK::Int || t->k == TK::Long; }

static bool computeCopy(Type* t);

bool isCopy(Type* t) {
    if (t->copyState == 2) return true;
    if (t->copyState == 3) return false;
    if (t->copyState == 1) return true;  // recursion guard (recursive types are rejected elsewhere)
    t->copyState = 1;
    bool c = computeCopy(t);
    t->copyState = c ? 2 : 3;
    return c;
}

static bool computeCopy(Type* t) {
    switch (t->k) {
        case TK::String: case TK::Array: case TK::List: case TK::Box: case TK::Map: return false;
        case TK::Optional: return isCopy(t->a);
        case TK::Result: return isCopy(t->a) && isCopy(t->b);
        case TK::Struct: {
            if (t->sd->isExtern) return true;
            for (auto* i : t->sd->ifaces)
                if (i->name == "Disposable" && i->builtin) return false;
            for (auto* f : t->sd->fields)
                if (f->rtype && !isCopy(f->rtype)) return false;
            return true;
        }
        case TK::Enum:
            for (auto& v : t->ed->variants)
                for (auto* f : v.fields)
                    if (f->rtype && !isCopy(f->rtype)) return false;
            return true;
        default: return true;
    }
}
