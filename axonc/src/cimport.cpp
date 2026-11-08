// cimport.cpp - generates Axon bindings from a C header using libclang (loaded at run time, so axon
// works without LLVM and only needs libclang.dll when a header has no bindings file).
//
// What gets imported:
//   * functions whose types Axon can express (numbers, bool, `const char*` as string, structs by
//     value, pointers to structs as `ref` / `mut ref`); variadic functions are skipped
//   * structs (pointer fields become opaque `long` slots so the layout stays exact; structs with
//     arrays or unions are skipped), and typedef aliases of structs (`typedef Texture Texture2D`)
//   * enum constants and simple macros (`#define KEY 32`, `#define RED CLITERAL(Color){ ... }`)
#include "cimport.h"

#include <filesystem>
#include <map>
#include <set>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

// ---- the parts of the libclang C API we use (declared here, so no clang headers are needed)
typedef void* CXIndex;
typedef void* CXTranslationUnit;
struct CXString { const void* data; unsigned flags; };
struct CXCursor { int kind; int xdata; const void* data[3]; };
struct CXType { int kind; void* data[2]; };
struct CXSourceLocation { const void* ptr[2]; unsigned intData; };
struct CXSourceRange { const void* ptr[2]; unsigned begin, end; };
struct CXToken { unsigned intData[4]; void* ptr; };
struct CXUnsavedFile { const char* name; const char* contents; unsigned long length; };
enum { VisitBreak = 0, VisitContinue = 1, VisitRecurse = 2 };
typedef int (*CXVisitor)(CXCursor, CXCursor, void*);

enum CursorKind { K_Struct = 2, K_Union = 3, K_Enum = 5, K_Field = 6, K_EnumConst = 7, K_Function = 8, K_Param = 10, K_Typedef = 20, K_Macro = 501 };
enum TypeKind {
    TY_Void = 2, TY_Bool = 3, TY_CharU = 4, TY_UChar = 5, TY_UShort = 8, TY_UInt = 9, TY_ULong = 10, TY_ULongLong = 11,
    TY_CharS = 13, TY_SChar = 14, TY_Short = 16, TY_Int = 17, TY_Long = 18, TY_LongLong = 19, TY_Float = 21, TY_Double = 22,
    TY_Pointer = 101, TY_Record = 105, TY_Enum = 106, TY_Typedef = 107, TY_ConstArray = 112, TY_Elaborated = 119,
};

struct API {
    CXIndex (*createIndex)(int, int);
    CXTranslationUnit (*parse)(CXIndex, const char*, const char* const*, int, CXUnsavedFile*, unsigned, unsigned);
    CXCursor (*tuCursor)(CXTranslationUnit);
    unsigned (*visit)(CXCursor, CXVisitor, void*);
    CXString (*spelling)(CXCursor);
    const char* (*cstr)(CXString);
    void (*dispose)(CXString);
    CXType (*cursorType)(CXCursor);
    CXType (*resultType)(CXType);
    int (*numArgs)(CXType);
    CXType (*argType)(CXType, unsigned);
    unsigned (*variadic)(CXType);
    CXType (*canonical)(CXType);
    CXType (*pointee)(CXType);
    unsigned (*isConst)(CXType);
    CXString (*typeSpelling)(CXType);
    CXCursor (*typeDecl)(CXType);
    CXType (*typedefUnderlying)(CXCursor);
    long long (*enumValue)(CXCursor);
    CXSourceLocation (*location)(CXCursor);
    int (*inSystemHeader)(CXSourceLocation);
    int (*fromMainFile)(CXSourceLocation);
    CXCursor (*argument)(CXCursor, unsigned);
    unsigned (*macroFunctionLike)(CXCursor);
    CXSourceRange (*extent)(CXCursor);
    void (*tokenize)(CXTranslationUnit, CXSourceRange, CXToken**, unsigned*);
    CXString (*tokenSpelling)(CXTranslationUnit, CXToken);
    void (*disposeTokens)(CXTranslationUnit, CXToken*, unsigned);
    long long (*sizeOf)(CXType);
};

#ifdef _WIN32
HMODULE g_lib = nullptr;

std::string findLibclang() {
    char buf[MAX_PATH];
    if (SearchPathA(nullptr, "libclang.dll", nullptr, MAX_PATH, buf, nullptr) > 0) return buf;
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    fs::path tools = fs::path(exe).parent_path().parent_path() / "tools" / "llvm";
    std::error_code ec;
    if (fs::exists(tools, ec))
        for (auto& e : fs::directory_iterator(tools, ec)) {
            fs::path p = e.path() / "bin" / "libclang.dll";
            if (fs::exists(p)) return p.string();
        }
    return "";
}
#endif

bool load(API& a, std::string& err) {
#ifdef _WIN32
    if (!g_lib) {
        std::string path = findLibclang();
        if (path.empty()) {
            err = "libclang.dll wasn't found (install LLVM, or unpack it into tools/llvm next to axon)";
            return false;
        }
        // libclang's own dependencies live next to it
        SetDllDirectoryA(fs::path(path).parent_path().string().c_str());
        g_lib = LoadLibraryA(path.c_str());
        SetDllDirectoryA(nullptr);
        if (!g_lib) {
            err = "couldn't load " + path;
            return false;
        }
    }
    bool ok = true;
    auto get = [&](auto& fnp, const char* name) {
        fnp = reinterpret_cast<std::remove_reference_t<decltype(fnp)>>(GetProcAddress(g_lib, name));
        if (!fnp) ok = false;
    };
    get(a.createIndex, "clang_createIndex");
    get(a.parse, "clang_parseTranslationUnit");
    get(a.tuCursor, "clang_getTranslationUnitCursor");
    get(a.visit, "clang_visitChildren");
    get(a.spelling, "clang_getCursorSpelling");
    get(a.cstr, "clang_getCString");
    get(a.dispose, "clang_disposeString");
    get(a.cursorType, "clang_getCursorType");
    get(a.resultType, "clang_getResultType");
    get(a.numArgs, "clang_getNumArgTypes");
    get(a.argType, "clang_getArgType");
    get(a.variadic, "clang_isFunctionTypeVariadic");
    get(a.canonical, "clang_getCanonicalType");
    get(a.pointee, "clang_getPointeeType");
    get(a.isConst, "clang_isConstQualifiedType");
    get(a.typeSpelling, "clang_getTypeSpelling");
    get(a.typeDecl, "clang_getTypeDeclaration");
    get(a.typedefUnderlying, "clang_getTypedefDeclUnderlyingType");
    get(a.enumValue, "clang_getEnumConstantDeclValue");
    get(a.location, "clang_getCursorLocation");
    get(a.inSystemHeader, "clang_Location_isInSystemHeader");
    get(a.fromMainFile, "clang_Location_isFromMainFile");
    get(a.argument, "clang_Cursor_getArgument");
    get(a.macroFunctionLike, "clang_Cursor_isMacroFunctionLike");
    get(a.extent, "clang_getCursorExtent");
    get(a.tokenize, "clang_tokenize");
    get(a.tokenSpelling, "clang_getTokenSpelling");
    get(a.disposeTokens, "clang_disposeTokens");
    get(a.sizeOf, "clang_Type_getSizeOf");
    if (!ok) err = "this libclang is missing functions axon needs";
    return ok;
#else
    (void)a;
    err = "importing C headers is only implemented on Windows so far";
    return false;
#endif
}

const std::set<std::string>& axonKeywords() {
    static const std::set<std::string> k = {
        "int", "long", "byte", "float", "double", "bool", "char", "string", "void", "var", "const", "if", "else", "while", "do", "for",
        "foreach", "in", "return", "break", "continue", "switch", "case", "default", "struct", "interface", "enum", "extend", "mut",
        "ref", "new", "this", "true", "false", "none", "null", "is", "try", "using", "extern", "private", "class", "List", "Map", "Box",
        "Result", "Ok", "Err", "print", "write", "assert", "panic"};
    return k;
}

struct Importer {
    API& a;
    CXTranslationUnit tu = nullptr;
    std::map<std::string, std::string> structs;  // name -> "struct Name { ... }"
    std::vector<std::string> structOrder;
    std::set<std::string> badStructs;            // seen but not representable
    std::map<std::string, std::string> aliases;  // typedef alias -> struct it names
    std::vector<std::string> consts, funcs;
    std::set<std::string> names;
    CImportResult res;

    explicit Importer(API& api) : a(api) {}

    std::string str(CXString s) {
        const char* c = a.cstr(s);
        std::string r = c ? c : "";
        a.dispose(s);
        return r;
    }
    std::string name(CXCursor c) { return str(a.spelling(c)); }

    // the struct name a record type is known by in C (typedef name for anonymous structs)
    std::string recordName(CXType t) {
        std::string s = str(a.typeSpelling(t));
        if (s.rfind("const ", 0) == 0) s = s.substr(6);
        if (s.rfind("struct ", 0) == 0) s = s.substr(7);
        return s;
    }

    // Axon spelling of a C type, or "" if Axon can't express it. `pointerOk`: pointers to structs are allowed (params)
    std::string axonType(CXType t, bool isParam, int* refMode = nullptr) {
        CXType c = a.canonical(t);
        switch (c.kind) {
            case TY_Void: return "void";
            case TY_Bool: return "bool";
            case TY_CharU: case TY_UChar: case TY_CharS: case TY_SChar: return "byte";
            case TY_Int: case TY_UInt: case TY_Long: case TY_ULong: case TY_Enum: return "int";
            case TY_LongLong: case TY_ULongLong: return "long";
            case TY_Float: return "float";
            case TY_Double: return "double";
            case TY_Record: {
                std::string n = recordName(t);
                if (aliases.count(n)) return n;
                if (!structs.count(n) && !aliases.count(n)) {
                    // the typedef name may differ from the record's tag
                    std::string canon = recordName(c);
                    if (structs.count(canon)) return canon;
                    return "";
                }
                return n;
            }
            case TY_Pointer: {
                CXType p = a.pointee(c);
                CXType pc = a.canonical(p);
                if (pc.kind == TY_CharS || pc.kind == TY_CharU) return a.isConst(pc) || a.isConst(p) ? "string" : "";
                if (isParam && refMode && pc.kind == TY_Record) {
                    std::string n = axonType(p, false);
                    if (n.empty()) return "";
                    *refMode = a.isConst(p) || a.isConst(pc) ? 1 : 2;
                    return n;
                }
                return "";
            }
            default: return "";
        }
    }

    bool fromHeader(CXCursor c) {
        CXSourceLocation l = a.location(c);
        return !a.inSystemHeader(l) && !a.fromMainFile(l);
    }

    std::string safeName(const std::string& n) { return axonKeywords().count(n) ? n + "_" : n; }

    // first pass: structs and typedef aliases
    void addStruct(const std::string& n, CXCursor decl) {
        if (n.empty() || structs.count(n) || badStructs.count(n) || axonKeywords().count(n)) return;
        struct Ctx { Importer* self; std::string body; bool ok = true; } ctx{this, "", true};
        a.visit(decl, [](CXCursor c, CXCursor, void* d) -> int {
            auto* x = (Ctx*)d;
            Importer* me = x->self;
            if (c.kind == K_Struct || c.kind == K_Union) {
                x->ok = false;  // nested records: the layout isn't something Axon can describe
                return VisitContinue;
            }
            if (c.kind != K_Field) return VisitContinue;
            CXType t = me->a.cursorType(c);
            CXType ct = me->a.canonical(t);
            std::string at = me->axonType(t, false);
            if (at == "string") at = "";
            if (at.empty() && ct.kind == TY_Pointer) at = "long";  // opaque pointer: keeps the layout
            if (at.empty() || at == "void") {
                x->ok = false;
                return VisitContinue;
            }
            x->body += "        " + at + " " + me->safeName(me->name(c)) + ";\n";
            return VisitContinue;
        }, &ctx);
        if (!ctx.ok || ctx.body.empty()) {
            badStructs.insert(n);
            res.skipped++;
            return;
        }
        structs[n] = "    struct " + n + " {\n" + ctx.body + "    }\n";
        structOrder.push_back(n);
    }

    static int topVisit(CXCursor c, CXCursor, void* d) {
        auto* me = (Importer*)d;
        if (!me->fromHeader(c)) return VisitContinue;
        if (c.kind == K_Struct) me->addStruct(me->name(c), c);
        if (c.kind == K_Typedef) {
            std::string n = me->name(c);
            CXType u = me->a.typedefUnderlying(c);
            CXType cu = me->a.canonical(u);
            if (cu.kind == TY_Record) {
                std::string target = me->recordName(cu);
                CXCursor decl = me->a.typeDecl(cu);
                if (target.empty() || target.find("unnamed") != std::string::npos || target.find("anonymous") != std::string::npos ||
                    target.find('(') != std::string::npos)
                    me->addStruct(n, decl);  // typedef struct { ... } Name;
                else if (target != n) {
                    me->addStruct(target, decl);
                    if (me->structs.count(target)) me->aliases[n] = target;  // typedef Texture Texture2D;
                }
            }
        }
        return VisitContinue;
    }

    static int secondVisit(CXCursor c, CXCursor, void* d) {
        auto* me = (Importer*)d;
        if (c.kind == K_Macro) {
            if (!me->fromHeader(c) || me->a.macroFunctionLike(c)) return VisitContinue;
            me->macro(c);
            return VisitContinue;
        }
        if (!me->fromHeader(c)) return VisitContinue;
        if (c.kind == K_Enum) {
            me->a.visit(c, [](CXCursor k, CXCursor, void* dd) -> int {
                auto* m = (Importer*)dd;
                if (k.kind == K_EnumConst) {
                    std::string n = m->name(k);
                    if (!axonKeywords().count(n) && m->names.insert(n).second) {
                        m->consts.push_back("    const int " + n + ";\n");
                        m->res.constants++;
                    }
                }
                return VisitContinue;
            }, me);
        }
        if (c.kind == K_Function) me->function(c);
        return VisitContinue;
    }

    void function(CXCursor c) {
        std::string n = name(c);
        if (axonKeywords().count(n) || !names.insert(n).second) return;
        CXType ft = a.cursorType(c);
        if (a.variadic(ft)) { res.skipped++; return; }
        std::string ret = axonType(a.resultType(ft), false);
        if (ret.empty()) { res.skipped++; return; }
        int count = a.numArgs(ft);
        std::string params;
        for (int i = 0; i < count; i++) {
            int mode = 0;
            std::string t = axonType(a.argType(ft, (unsigned)i), true, &mode);
            if (t.empty() || t == "void") { res.skipped++; return; }
            std::string pn = name(a.argument(c, (unsigned)i));
            if (pn.empty()) pn = "a" + std::to_string(i);
            pn = safeName(pn);
            params += std::string(i ? ", " : "") + (mode == 2 ? "mut ref " : mode == 1 ? "ref " : "") + t + " " + pn;
        }
        funcs.push_back("    " + ret + " " + n + "(" + params + ");\n");
        res.functions++;
    }

    // `#define NAME 42`, `#define NAME (-1)`, `#define NAME 1.5f`, `#define RED CLITERAL(Color){ 230, 41, 55, 255 }`
    void macro(CXCursor c) {
        std::string n = name(c);
        if (axonKeywords().count(n) || names.count(n)) return;
        CXToken* toks = nullptr;
        unsigned count = 0;
        a.tokenize(tu, a.extent(c), &toks, &count);
        std::vector<std::string> t;
        for (unsigned i = 1; i < count; i++) t.push_back(str(a.tokenSpelling(tu, toks[i])));
        a.disposeTokens(tu, toks, count);
        std::vector<std::string> body;
        for (auto& s : t) if (s != "(" && s != ")") body.push_back(s);
        auto isNum = [](const std::string& s) { return !s.empty() && (isdigit((unsigned char)s[0]) || (s[0] == '.' && s.size() > 1)); };
        std::string type;
        if (body.size() == 1 && isNum(body[0])) type = body[0].find_first_of(".eE") != std::string::npos && body[0].find("0x") != 0 ? "double" : "int";
        else if (body.size() == 2 && body[0] == "-" && isNum(body[1])) type = body[1].find('.') != std::string::npos ? "double" : "int";
        else {
            // a compound literal of a known struct: find `Name ) {`
            for (size_t i = 0; i + 2 < t.size(); i++)
                if (t[i + 1] == ")" && t[i + 2] == "{" && (structs.count(t[i]) || aliases.count(t[i]))) type = t[i];
        }
        if (type.empty()) return;
        if (type == "int") {
            // only values that fit in an int
            std::string v = body.back();
            char* end = nullptr;
            long long x = strtoll(v.c_str(), &end, 0);
            if (x > 2147483647LL || x < -2147483648LL) type = "long";
        }
        names.insert(n);
        consts.push_back("    const " + type + " " + n + ";\n");
        res.constants++;
    }

    void run(const std::string& header, const std::vector<std::string>& includeDirs, const std::vector<std::string>& defines) {
        CXIndex index = a.createIndex(0, 0);
        std::string mainText = "#include \"" + header + "\"\n";
        CXUnsavedFile unsaved{"axon_import.c", mainText.c_str(), (unsigned long)mainText.size()};
        std::vector<std::string> args = {"-x", "c", "-std=c11"};
        for (auto& d : includeDirs) args.push_back("-I" + d);
        for (auto& d : defines) args.push_back("-D" + d);
        std::vector<const char*> argv;
        for (auto& s : args) argv.push_back(s.c_str());
        const unsigned DetailedPreprocessingRecord = 0x01, SkipFunctionBodies = 0x40;
        tu = a.parse(index, "axon_import.c", argv.data(), (int)argv.size(), &unsaved, 1, DetailedPreprocessingRecord | SkipFunctionBodies);
        if (!tu) {
            res.error = "libclang couldn't parse " + header;
            return;
        }
        CXCursor root = a.tuCursor(tu);
        a.visit(root, topVisit, this);
        a.visit(root, secondVisit, this);
        res.structs = (int)(structs.size() + aliases.size());
        std::ostringstream o;
        o << "// Axon bindings for " << header << ", generated by `axon` with libclang.\n";
        o << "// " << res.functions << " functions, " << res.structs << " structs, " << res.constants << " constants ("
          << res.skipped << " declarations skipped because Axon can't express their types).\n";
        o << "extern \"" << header << "\" {\n";
        for (auto& n : structOrder) o << structs[n];
        for (auto& al : aliases) {
            // an alias is the same struct under another C name
            std::string s = structs[al.second];
            size_t p = s.find("struct " + al.second + " {");
            if (p != std::string::npos) s.replace(p, 7 + al.second.size(), "struct " + al.first);
            o << s;
        }
        for (auto& c : consts) o << c;
        for (auto& f : funcs) o << f;
        o << "}\n";
        res.axon = o.str();
        res.ok = true;
    }
};

}  // namespace

bool libclangAvailable() {
#ifdef _WIN32
    return g_lib || !findLibclang().empty();
#else
    return false;
#endif
}

CImportResult importCHeader(const std::string& header, const std::vector<std::string>& includeDirs, const std::vector<std::string>& defines) {
    static API api;
    CImportResult r;
    if (!load(api, r.error)) return r;
    Importer im(api);
    im.run(header, includeDirs, defines);
    return im.res;
}
