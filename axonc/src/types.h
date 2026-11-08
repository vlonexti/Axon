// types.h - semantic types used by the checker and the C emitter.
#pragma once
#include <map>
#include <string>
#include <vector>

#include "ast.h"

enum class TK {
    Error, Void, Byte, Int, Long, Float, Double, Bool, Char, String,
    Struct, Enum, Interface, Array, List, Optional, Result, None, Box, Map,
};

struct Type {
    TK k = TK::Error;
    Type* a = nullptr;  // element / inner / Ok type
    Type* b = nullptr;  // Err type
    StructDecl* sd = nullptr;
    EnumDecl* ed = nullptr;
    InterfaceDecl* id = nullptr;
    std::string mangle;  // used to build C helper names
    std::string cname;   // C type name
    int copyState = 0;   // 0 unknown, 1 computing, 2 copy, 3 owned
};

class TypeTable {
public:
    TypeTable();
    Type* prim(TK k);
    Type* error() { return prim(TK::Error); }
    Type* voidT() { return prim(TK::Void); }
    Type* intT() { return prim(TK::Int); }
    Type* longT() { return prim(TK::Long); }
    Type* boolT() { return prim(TK::Bool); }
    Type* doubleT() { return prim(TK::Double); }
    Type* stringT() { return prim(TK::String); }
    Type* structT(StructDecl* sd);
    Type* enumT(EnumDecl* ed);
    Type* ifaceT(InterfaceDecl* id);
    Type* arrayOf(Type* t);
    Type* listOf(Type* t);
    Type* optOf(Type* t);
    Type* resultOf(Type* ok, Type* err);
    Type* boxOf(Type* t);
    Type* mapOf(Type* k, Type* v);
    const std::vector<Type*>& all() const { return types; }

private:
    Type* intern(const std::string& key, Type proto);
    std::map<std::string, Type*> byKey;
    std::vector<Type*> types;
};

extern TypeTable g_types;

std::string typeName(Type* t);
bool isNumeric(Type* t);
bool isInteger(Type* t);
int numRank(Type* t);  // byte 0, int 1, long 2, float 3, double 4; -1 otherwise
bool isCopy(Type* t);
inline bool isOwned(Type* t) { return !isCopy(t); }
bool isErr(Type* t);
