// ast.h - the syntax tree produced by the parser and annotated by the checker.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "diagnostics.h"
#include "lexer.h"

struct Type;
struct VarDecl;
struct FuncDecl;
struct StructDecl;
struct EnumDecl;
struct InterfaceDecl;
struct ExtendDecl;
struct Module;
struct Stmt;
struct Expr;

// A type as written in the source: `int`, `Player?`, `List<int>`, `rl.Color[]`.
struct TypeExpr {
    enum Kind { Named, Array, Optional, Generic } kind = Named;
    std::string ns;    // `rl` in `rl.Color`
    std::string name;  // base name for Named / Generic
    std::vector<TypeExpr*> args;
    Span span;
    Type* resolved = nullptr;  // set by the checker (used by editor tools)
};

struct PatBind {
    TypeExpr* type = nullptr;  // null when written `var x` or `_`
    std::string name;          // "_" for ignore
    Span span;
    VarDecl* var = nullptr;
};

// Patterns appear after `case` and `is`.
struct Pattern {
    enum Kind { Variant, TypeBind, ExprPat } kind = ExprPat;
    std::string qual;  // `Item` in `Item.Sword(...)`
    std::string name;  // variant name
    bool hasParens = false;
    std::vector<PatBind> binds;
    TypeExpr* type = nullptr;  // TypeBind: `Player found`
    PatBind bind;
    Expr* expr = nullptr;      // ExprPat: literal or constant
    Span span;
    // checker
    int variant = -1;           // enum variant / 0=Ok 1=Err / 0=has-value for optionals
    bool isOptionalTest = false;
};

enum class EK {
    IntLit, FloatLit, StrLit, CharLit, BoolLit, None, Interp, Name, Member, Call, Index, Unary,
    Binary, Assign, Ternary, Cast, StructLit, ArrayLit, NewArray, Is, Try, This, IncDec, RefArg,
    Implicit, GenericNew, BoxDeref,
};

enum class CallKind {
    Unknown, Func, Method, IfaceMethod, Builtin, BuiltinMethod, EnumCtor, ResultOk, ResultErr, ListNew, MapNew,
    BoxNew, DynMethod,
};

enum class Builtin {
    None, Print, Write, Assert, Panic,
    // string methods
    StrSubstring, StrContains, StrStartsWith, StrEndsWith, StrIndexOf, StrTrim, StrToUpper,
    StrToLower, StrSplit, StrToInt, StrToDouble, StrByteAt, StrReplace, StrChars, StrLastIndexOf,
    StrPadLeft, StrPadRight, StrRepeat, StrIsEmpty,
    // list / array methods
    ListAdd, ListPop, ListRemoveAt, ListInsert, ListClear, SeqContains, SeqIndexOf, SeqSort, SeqReverse,
    // map methods
    MapGet, MapContains, MapRemove, MapKeys, MapValues, MapClear,
    Clone, ToText,
    // properties
    PropLength, PropHasValue,
};

enum class ImplicitKind { Numeric, WrapOptional, IfaceRef, BoxToDyn };

struct Expr {
    EK kind;
    Span span;
    int64_t ival = 0;
    double fval = 0;
    bool isF32 = false;
    bool isLongLit = false;
    bool bval = false;
    uint32_t cval = 0;
    std::string sval;
    std::string name;
    T op = T::Eof;
    bool prefix = false;  // ++x vs x++
    bool isMut = false;   // `mut ref x` argument
    Expr* a = nullptr;
    Expr* b = nullptr;
    Expr* c = nullptr;
    std::vector<Expr*> args;
    std::vector<std::string> fieldNames;
    std::vector<Span> fieldSpans;
    TypeExpr* texpr = nullptr;
    Pattern* pat = nullptr;

    // ---- filled in by the checker ----
    Type* type = nullptr;
    VarDecl* var = nullptr;
    FuncDecl* fn = nullptr;
    StructDecl* sd = nullptr;
    EnumDecl* ed = nullptr;
    int field = -1;
    int variant = -1;
    CallKind callKind = CallKind::Unknown;
    Builtin builtin = Builtin::None;
    ImplicitKind implicit = ImplicitKind::Numeric;
    bool consumed = false;     // value is moved/consumed by its context
    bool isConcat = false;     // string `+`
    bool isNoneCompare = false;  // `x == none`
    std::vector<VarDecl*> patVars;
};

enum class SK { Block, Var, ExprStmt, If, While, DoWhile, For, Foreach, Switch, Break, Continue, Return };

struct Case {
    std::vector<Pattern*> pats;
    bool isDefault = false;
    std::vector<Stmt*> body;
    Span span;
};

struct Stmt {
    SK kind;
    Span span;
    std::vector<Stmt*> stmts;  // Block
    VarDecl* var = nullptr;    // Var / Foreach binding
    Expr* expr = nullptr;      // ExprStmt, Return value, If/While cond, Switch subject, Foreach source
    Stmt* then = nullptr;
    Stmt* els = nullptr;
    Stmt* init = nullptr;
    Stmt* body = nullptr;
    Expr* step = nullptr;
    std::vector<Case> cases;
    int foreachMode = 0;  // 0 = read, 2 = mut ref
    // checker
    std::vector<VarDecl*> patVars;     // bound when the condition is true
    std::vector<VarDecl*> negPatVars;  // bound when it is false: `if (!(x is T v)) return;`
};

struct VarDecl {
    enum Kind { Local, Param, Global, PatBind, ForeachBind, This } kind = Local;
    std::string name;
    TypeExpr* texpr = nullptr;
    Expr* init = nullptr;
    Span span;
    Type* type = nullptr;
    int mode = 0;  // 0 = value, 1 = ref (read-only borrow), 2 = mut ref
    bool isConst = false;
    bool isExtern = false;
    bool isPrivate = false;
    std::string header;
    std::string ns;
    std::string cname;
    Module* mod = nullptr;
    VarDecl* borrowOf = nullptr;  // pattern bindings: the variable they borrow from
};

struct TypeParam {
    std::string name;
    std::vector<TypeExpr*> bounds;  // `T : Printable`
    Span span;
};

// Generic declarations keep their type parameters; each use with concrete types makes an
// instance: a copy of the declaration whose `subst` maps parameter names to types.
struct GenericInfo {
    std::vector<TypeParam> typeParams;
    std::map<std::string, Type*> subst;      // set on instances
    std::vector<Type*> typeArgs;             // set on instances
    std::map<std::string, void*> instances;  // on the original: key -> instance
    void* origin = nullptr;                  // on instances: the generic original
    bool isGeneric() const { return !typeParams.empty() && !origin; }
};

struct Param {
    std::string name;
    TypeExpr* type = nullptr;
    int mode = 0;  // 0 value, 1 ref, 2 mut ref
    Span span;
    VarDecl* var = nullptr;
};

struct FuncDecl {
    std::string name;
    TypeExpr* ret = nullptr;
    std::vector<Param> params;
    Stmt* body = nullptr;
    Span span;
    bool isMut = false;
    bool isPrivate = false;
    bool isTest = false;
    bool isExtern = false;
    bool isAxonRt = false;  // implemented by the Axon runtime (extern "@axon")
    std::string header;
    Module* mod = nullptr;
    StructDecl* ownerStruct = nullptr;
    EnumDecl* ownerEnum = nullptr;
    ExtendDecl* ownerExtend = nullptr;
    InterfaceDecl* ownerIface = nullptr;
    Type* retType = nullptr;
    Type* selfType = nullptr;
    VarDecl* thisVar = nullptr;
    std::string cname;
    std::vector<std::string> attrs;
    bool checked = false;
    GenericInfo gen;
};

struct FieldDecl {
    std::string name;
    TypeExpr* type = nullptr;
    Expr* def = nullptr;
    bool isPrivate = false;
    Span span;
    Type* rtype = nullptr;
    std::string cname;
};

struct StructDecl {
    std::string name;
    std::vector<FieldDecl*> fields;
    std::vector<FuncDecl*> methods;
    std::vector<TypeExpr*> ifaceExprs;
    std::vector<InterfaceDecl*> ifaces;
    bool isExtern = false;
    bool isPrivate = false;
    std::string header;
    Module* mod = nullptr;
    Span span;
    Type* type = nullptr;
    std::string cname;
    GenericInfo gen;
};

struct Variant {
    std::string name;
    std::vector<FieldDecl*> fields;
    Span span;
};

struct EnumDecl {
    std::string name;
    std::vector<Variant> variants;
    bool isPrivate = false;
    Module* mod = nullptr;
    Span span;
    Type* type = nullptr;
    std::string cname;
    GenericInfo gen;
};

struct InterfaceDecl {
    std::string name;
    std::vector<FuncDecl*> methods;
    bool isPrivate = false;
    Module* mod = nullptr;
    Span span;
    Type* type = nullptr;
    std::string cname;
    bool builtin = false;
};

struct ExtendDecl {
    TypeExpr* target = nullptr;
    std::vector<TypeExpr*> ifaceExprs;
    std::vector<InterfaceDecl*> ifaces;
    std::vector<FuncDecl*> methods;
    Module* mod = nullptr;
    Span span;
    Type* type = nullptr;
};

struct UsingDecl {
    bool isC = false;
    std::vector<std::string> path;  // game.player
    std::string header;             // raylib.h
    std::string alias;
    Span span;
};

struct Module {
    std::string name;  // "main", "game.player", "std.io", "c:raylib.h"
    std::string path;
    int fileId = -1;
    bool isStd = false;
    std::vector<UsingDecl> usings;
    std::vector<StructDecl*> structs;
    std::vector<EnumDecl*> enums;
    std::vector<InterfaceDecl*> ifaces;
    std::vector<FuncDecl*> funcs;
    std::vector<VarDecl*> globals;
    std::vector<ExtendDecl*> extends;
    std::vector<std::string> cIncludes;  // headers named in extern blocks
    // resolved by the driver/checker
    std::vector<Module*> imports;
    std::map<std::string, Module*> namespaces;  // `using c "raylib.h" as rl`
};
