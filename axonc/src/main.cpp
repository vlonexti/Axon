// main.cpp - the `axon` command-line tool and compiler driver.
//
//   axon new <name>          create a project
//   axon build [--release]   compile the project (or a single .ax file) to an executable
//   axon run   [--release]   build, then run
//   axon test                run every @test function
//   axon check               type-check only
//   axon emit-c              write the generated C
//   axon tokens <file.ax>    print the tokens of a file
//   axonc <file.ax>          compile one file (same as `axon build file.ax`)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <io.h>
#include <windows.h>
#define popen _popen
#define pclose _pclose
#else
#include <unistd.h>
#endif

#include "ast.h"
#include "checker.h"
#include "diagnostics.h"
#include "emit_c.h"
#include "lexer.h"
#include "parser.h"
#include "cimport.h"
#include "format.h"
#include "irgen.h"
#include "llvm.h"
#include "x64.h"
#include "runtime.h"

namespace fs = std::filesystem;

static const char* AXON_VERSION = "0.3.0";

// ---------------------------------------------------------------- console output

static bool g_color = false;

static void status(const char* verb, const std::string& msg) {
    if (g_color) fprintf(stderr, "\x1b[1;32m%12s\x1b[0m %s\n", verb, msg.c_str());
    else fprintf(stderr, "%12s %s\n", verb, msg.c_str());
}

static void fatal(const std::string& msg, const std::string& help = "") {
    if (g_color) fprintf(stderr, "\x1b[1;31merror\x1b[0m\x1b[1m: %s\x1b[0m\n", msg.c_str());
    else fprintf(stderr, "error: %s\n", msg.c_str());
    if (!help.empty()) fprintf(stderr, "  = help: %s\n", help.c_str());
}

static void setupConsole() {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        if (SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) g_color = true;
    }
    SetConsoleOutputCP(CP_UTF8);
#else
    g_color = isatty(2);
#endif
    if (getenv("NO_COLOR")) g_color = false;
    g_diag.useColor = g_color;
}

static bool readFile(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    // strip a UTF-8 BOM
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) out.erase(0, 3);
    return true;
}

static bool writeFile(const fs::path& p, const std::string& text) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f << text;
    return (bool)f;
}

static std::string displayPath(const fs::path& p) {
    std::error_code ec;
    fs::path rel = fs::relative(p, fs::current_path(), ec);
    std::string s = (!ec && !rel.empty() && rel.string().rfind("..", 0) != 0) ? rel.string() : p.string();
    for (auto& c : s) if (c == '\\') c = '/';
    return s;
}

// ---------------------------------------------------------------- tiny TOML reader

struct TomlValue {
    enum Kind { Str, Arr, Table, Other } kind = Other;
    std::string s;
    std::vector<std::string> arr;
    std::map<std::string, TomlValue> table;
};

struct Toml {
    std::map<std::string, std::map<std::string, TomlValue>> sections;
    std::string error;
    int errorLine = 0;

    const TomlValue* get(const std::string& sec, const std::string& key) const {
        auto it = sections.find(sec);
        if (it == sections.end()) return nullptr;
        auto jt = it->second.find(key);
        return jt == it->second.end() ? nullptr : &jt->second;
    }
    std::string str(const std::string& sec, const std::string& key, const std::string& def = "") const {
        const TomlValue* v = get(sec, key);
        return v && v->kind == TomlValue::Str ? v->s : def;
    }
};

namespace {
struct TomlParser {
    const std::string& src;
    size_t i = 0;
    int line = 1;
    std::string err;
    explicit TomlParser(const std::string& s) : src(s) {}
    void ws() {
        while (i < src.size() && (src[i] == ' ' || src[i] == '\t' || src[i] == '\r')) i++;
        if (i < src.size() && src[i] == '#') while (i < src.size() && src[i] != '\n') i++;
    }
    void wsnl() {
        while (true) {
            ws();
            if (i < src.size() && src[i] == '\n') { i++; line++; continue; }
            break;
        }
    }
    std::string key() {
        ws();
        if (i < src.size() && src[i] == '"') return string();
        size_t s = i;
        while (i < src.size() && (isalnum((unsigned char)src[i]) || src[i] == '_' || src[i] == '-' || src[i] == '.')) i++;
        return src.substr(s, i - s);
    }
    std::string string() {
        i++;
        std::string r;
        while (i < src.size() && src[i] != '"' && src[i] != '\n') {
            if (src[i] == '\\' && i + 1 < src.size()) {
                char e = src[++i];
                r += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                i++;
                continue;
            }
            r += src[i++];
        }
        if (i < src.size() && src[i] == '"') i++;
        else err = "unterminated string";
        return r;
    }
    TomlValue value() {
        ws();
        TomlValue v;
        if (i >= src.size()) { err = "missing value"; return v; }
        if (src[i] == '"') { v.kind = TomlValue::Str; v.s = string(); return v; }
        if (src[i] == '\'') {
            i++;
            size_t s = i;
            while (i < src.size() && src[i] != '\'') i++;
            v.kind = TomlValue::Str;
            v.s = src.substr(s, i - s);
            if (i < src.size()) i++;
            return v;
        }
        if (src[i] == '[') {
            i++;
            v.kind = TomlValue::Arr;
            while (true) {
                wsnl();
                if (i < src.size() && src[i] == ']') { i++; break; }
                TomlValue e = value();
                if (!err.empty()) return v;
                v.arr.push_back(e.s);
                wsnl();
                if (i < src.size() && src[i] == ',') { i++; continue; }
                if (i < src.size() && src[i] == ']') { i++; break; }
                err = "expected `,` or `]` in the list";
                return v;
            }
            return v;
        }
        if (src[i] == '{') {
            i++;
            v.kind = TomlValue::Table;
            while (true) {
                ws();
                if (i < src.size() && src[i] == '}') { i++; break; }
                std::string k = key();
                ws();
                if (i >= src.size() || src[i] != '=') { err = "expected `=` after `" + k + "`"; return v; }
                i++;
                v.table[k] = value();
                if (!err.empty()) return v;
                ws();
                if (i < src.size() && src[i] == ',') { i++; continue; }
                if (i < src.size() && src[i] == '}') { i++; break; }
                err = "expected `,` or `}`";
                return v;
            }
            return v;
        }
        size_t s = i;
        while (i < src.size() && src[i] != '\n' && src[i] != '#' && src[i] != ',' && src[i] != '}' && src[i] != ']') i++;
        v.kind = TomlValue::Other;
        v.s = src.substr(s, i - s);
        while (!v.s.empty() && isspace((unsigned char)v.s.back())) v.s.pop_back();
        return v;
    }
};
}  // namespace

static Toml parseToml(const std::string& text) {
    Toml t;
    TomlParser p(text);
    std::string section;
    while (true) {
        p.wsnl();
        if (p.i >= text.size()) break;
        if (text[p.i] == '[') {
            p.i++;
            section = p.key();
            p.ws();
            if (p.i < text.size() && text[p.i] == ']') p.i++;
            else { t.error = "expected `]`"; t.errorLine = p.line; return t; }
            t.sections[section];
            continue;
        }
        std::string k = p.key();
        if (k.empty()) { t.error = "expected a key"; t.errorLine = p.line; return t; }
        p.ws();
        if (p.i >= text.size() || text[p.i] != '=') { t.error = "expected `=` after `" + k + "`"; t.errorLine = p.line; return t; }
        p.i++;
        TomlValue v = p.value();
        if (!p.err.empty()) { t.error = p.err; t.errorLine = p.line; return t; }
        t.sections[section][k] = v;
    }
    return t;
}

// ---------------------------------------------------------------- module loading

struct Library {
    std::string name, include, lib;
    std::vector<std::string> link;
    std::vector<std::string> defines;  // macros the library's headers need, e.g. CIMGUI_DEFINE_ENUMS_AND_STRUCTS
};

struct BuildConfig {
    fs::path entry;
    fs::path projectRoot;  // empty for single-file builds
    fs::path srcRoot;
    std::string name;
    fs::path buildDir;
    std::vector<Library> libs;
    std::string cCompiler;
    bool release = false;
    bool testMode = false;
    std::string backend = "c";  // c (default), x64 (own code generator), llvm
    struct Dep {
        fs::path root;     // the package folder (has its own axon.toml)
        fs::path srcRoot;  // where `using name.module;` looks
        fs::path entry;    // what `using name;` loads
    };
    std::map<std::string, Dep> deps;
};

// Editor buffers that haven't been saved (used by the language server): normalized path -> text.
static const std::map<std::string, std::string>* g_overlay = nullptr;

std::string normPath(const std::string& p) {
    std::error_code ec;
    fs::path a = fs::weakly_canonical(fs::absolute(fs::path(p), ec), ec);
    std::string s = a.string();
    for (auto& c : s) {
        if (c == '/') c = '\\';
#ifdef _WIN32
        c = (char)tolower((unsigned char)c);
#endif
    }
    return s;
}

static bool readSource(const fs::path& p, std::string& out) {
    if (g_overlay) {
        auto it = g_overlay->find(normPath(p.string()));
        if (it != g_overlay->end()) {
            out = it->second;
            return true;
        }
    }
    return readFile(p, out);
}

class Loader {
public:
    Program& prog;
    BuildConfig& cfg;
    std::map<std::string, Module*> byName;
    std::map<Module*, fs::path> pkgSrc;  // which package (source folder) each module belongs to
    explicit Loader(Program& p, BuildConfig& c) : prog(p), cfg(c) {}

    Module* loadSource(const std::string& name, const std::string& path, const std::string& display, const std::string& text, bool isStd,
                       const fs::path& srcRoot = fs::path()) {
        Module* m = new Module();
        m->name = name;
        m->path = path;
        m->isStd = isStd;
        m->fileId = g_diag.addFile(path, display, text);
        byName[name] = m;
        if (!srcRoot.empty()) pkgSrc[m] = srcRoot;
        prog.modules.push_back(m);
        parseModuleSource(m);
        resolveUsings(m);
        return m;
    }

    Module* loadFile(const std::string& name, const fs::path& p, Span from, const fs::path& srcRoot = fs::path()) {
        auto it = byName.find(name);
        if (it != byName.end()) return it->second;
        // the same file reached under two names (e.g. `pkg.x` from inside and outside) is one module
        for (auto& kv : byName)
            if (normPath(kv.second->path) == normPath(p.string())) return kv.second;
        std::string text;
        if (!readSource(p, text)) {
            if (from.file >= 0)
                g_diag.error(from, "can't find the module file `" + displayPath(p) + "`")
                    .help("create the file, or check the spelling of the `using` line");
            else fatal("can't read `" + displayPath(p) + "`");
            return nullptr;
        }
        return loadSource(name, p.string(), displayPath(p), text, false, srcRoot);
    }

    void resolveUsings(Module* m) {
        for (auto& u : m->usings) {
            Module* target = nullptr;
            if (u.isC) {
                std::string stem = fs::path(u.header).stem().string();
                std::string name = "c:" + u.header;
                auto it = byName.find(name);
                if (it != byName.end()) target = it->second;
                else {
                    std::vector<fs::path> dirs;
                    if (!cfg.projectRoot.empty()) dirs.push_back(cfg.projectRoot / "bindings");
                    dirs.push_back(cfg.srcRoot / "bindings");
                    dirs.push_back(cfg.srcRoot);
                    dirs.push_back(fs::path(m->path).parent_path() / "bindings");
                    if (pkgSrc.count(m)) dirs.push_back(pkgSrc[m].parent_path() / "bindings");  // a package's own bindings
                    for (auto& d : dirs) {
                        fs::path p = d / (stem + ".ax");
                        if (fs::exists(p)) {
                            target = loadFile(name, p, u.span);
                            break;
                        }
                    }
                    if (!target) {
                        // no bindings file: read the header itself with libclang (if LLVM is installed)
                        std::vector<std::string> incs, defs;
                        for (auto& l : cfg.libs) {
                            if (!l.include.empty()) incs.push_back(l.include);
                            for (auto& d : l.defines) defs.push_back(d);
                        }
                        incs.push_back(cfg.srcRoot.string());
                        if (!cfg.projectRoot.empty()) incs.push_back(cfg.projectRoot.string());
                        CImportResult imp = importCHeader(u.header, incs, defs);
                        if (imp.ok) {
                            fs::path gen = cfg.buildDir / "bindings" / (stem + ".ax");
                            writeFile(gen, imp.axon);  // kept so you can read what was imported
                            target = loadSource(name, gen.string(), displayPath(gen), imp.axon, false);
                        }
                    }
                    if (!target) {
                        auto& d = g_diag.error(u.span, "no Axon bindings found for `" + u.header + "`");
                        d.help("create `bindings/" + stem + ".ax` with an `extern \"" + u.header + "\" { ... }` block that declares the functions you use");
                        if (!libclangAvailable()) d.help("or install LLVM: with libclang available, axon reads the header itself");
                        else d.help("libclang couldn't import it: check the `include` path of the library in axon.toml");
                        continue;
                    }
                }
            } else if (u.path.size() >= 1 && u.path[0] == "std") {
                if (u.path.size() != 2) {
                    g_diag.error(u.span, "unknown standard module").help("available: std.io, std.math, std.text, std.os, std.random, std.time");
                    continue;
                }
                std::string name = "std." + u.path[1];
                auto it = byName.find(name);
                if (it != byName.end()) target = it->second;
                else {
                    const char* src = stdModuleSource(u.path[1].c_str());
                    if (!src) {
                        g_diag.error(u.span, "there is no module `" + name + "`").help("available: std.io, std.math, std.text, std.os, std.random, std.time");
                        continue;
                    }
                    target = loadSource(name, "<std>/" + u.path[1] + ".ax", "<std>/" + u.path[1] + ".ax", src, true);
                }
            } else if (cfg.deps.count(u.path[0])) {
                // a package from [dependencies]: `using pkg;` or `using pkg.module;`
                const auto& dep = cfg.deps[u.path[0]];
                std::string name = "pkg:";
                fs::path p = dep.srcRoot;
                for (size_t i = 0; i < u.path.size(); i++) name += (i ? "." : "") + u.path[i];
                if (u.path.size() == 1) p = dep.entry;
                else {
                    for (size_t i = 1; i < u.path.size(); i++) p /= u.path[i];
                    p += ".ax";
                }
                target = loadFile(name, p, u.span, dep.srcRoot);
            } else {
                // a module of the same package: relative to that package's source folder
                fs::path root = pkgSrc.count(m) ? pkgSrc[m] : cfg.srcRoot;
                std::string name = root == cfg.srcRoot ? "" : "pkg:" + root.string() + ":";
                fs::path p = root;
                for (size_t i = 0; i < u.path.size(); i++) {
                    name += (i ? "." : "") + u.path[i];
                    p /= u.path[i];
                }
                p += ".ax";
                target = loadFile(name, p, u.span, root);
            }
            if (!target) continue;
            if (target == m) {
                g_diag.error(u.span, "a module can't use itself");
                continue;
            }
            if (!u.alias.empty()) m->namespaces[u.alias] = target;
            else m->imports.push_back(target);
        }
    }
};

// ---------------------------------------------------------------- C compiler

static std::string quote(const std::string& s) {
    if (s.find_first_of(" \t&()") == std::string::npos) return s;
    return "\"" + s + "\"";
}

static bool onPath(const std::string& exe) {
#ifdef _WIN32
    char buf[MAX_PATH];
    std::string name = exe;
    if (name.find('.') == std::string::npos) name += ".exe";
    return SearchPathA(nullptr, name.c_str(), nullptr, MAX_PATH, buf, nullptr) > 0;
#else
    std::string cmd = "command -v " + exe + " >/dev/null 2>&1";
    return system(cmd.c_str()) == 0;
#endif
}

static int runCapture(const std::string& cmd, std::string& output) {
#ifdef _WIN32
    std::string full = "\"" + cmd + " 2>&1\"";
#else
    std::string full = cmd + " 2>&1";
#endif
    FILE* p = popen(full.c_str(), "r");
    if (!p) return -1;
    char buf[4096];
    while (fgets(buf, sizeof buf, p)) output += buf;
    int rc = pclose(p);
#ifndef _WIN32
    if (WIFEXITED(rc)) rc = WEXITSTATUS(rc);
#endif
    return rc;
}

static std::string findVcvars() {
#ifdef _WIN32
    const char* pf = getenv("ProgramFiles(x86)");
    std::string vswhere = std::string(pf ? pf : "C:\\Program Files (x86)") + "\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    if (!fs::exists(vswhere)) return "";
    std::string out;
    runCapture(quote(vswhere) + " -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath", out);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    size_t nl = out.find('\n');
    if (nl != std::string::npos) out = out.substr(0, nl);
    if (out.empty()) return "";
    fs::path bat = fs::path(out) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
    return fs::exists(bat) ? bat.string() : "";
#else
    return "";
#endif
}

struct CCompiler {
    std::string kind;    // "clang", "gcc", "cl"
    std::string exe;
    std::string vcvars;  // set when cl must be run through vcvars64.bat
};

static bool pickCompiler(const std::string& pref, CCompiler& cc) {
    std::vector<std::string> order;
    if (!pref.empty()) order.push_back(pref);
    else order = {"clang", "gcc", "cl"};
    for (auto& c : order) {
        std::string kind = c;
        std::string base = fs::path(c).stem().string();
        if (base.find("clang") != std::string::npos) kind = "clang";
        else if (base.find("gcc") != std::string::npos || base == "cc") kind = "gcc";
        else if (base == "cl") kind = "cl";
        if (onPath(c) || fs::exists(c)) {
            cc.kind = kind;
            cc.exe = c;
            return true;
        }
        if (kind == "cl") {
            std::string vc = findVcvars();
            if (!vc.empty()) {
                cc.kind = "cl";
                cc.exe = "cl";
                cc.vcvars = vc;
                return true;
            }
        }
    }
    return false;
}

static int compileC(const BuildConfig& cfg, const fs::path& cfile, const fs::path& exe, const std::vector<std::string>& preCmds = {},
                    const std::vector<fs::path>& extraObjs = {}) {
    CCompiler cc;
    if (!pickCompiler(cfg.backend == "c" ? cfg.cCompiler : "cl", cc)) {
        fatal(cfg.cCompiler.empty() ? "no C compiler found" : "C compiler `" + cfg.cCompiler + "` not found",
              "install LLVM (clang), MinGW (gcc) or Visual Studio with C++ tools; Axon uses it to build the generated C");
        return 1;
    }
    std::string cmd;
    if (cc.kind == "cl") {
        fs::path obj = cfile;
        obj.replace_extension(".obj");
        // /MD: use the DLL C runtime, like nearly every prebuilt C library (raylib, SDL, ...), so they link cleanly
        cmd = "cl /nologo /std:c11 /w /utf-8 /MD " + std::string(cfg.release ? "/O2 /DNDEBUG" : "/Od /Z7") + " " + quote(cfile.string()) +
              " /Fe:" + quote(exe.string()) + " /Fo:" + quote(obj.string());
        for (auto& x : extraObjs) cmd += " " + quote(x.string());
        for (auto& l : cfg.libs) {
            if (!l.include.empty()) cmd += " /I" + quote(l.include);
            for (auto& d : l.defines) cmd += " /D" + quote(d);
        }
        cmd += " /link /NOLOGO";
        if (!cfg.release) cmd += " /DEBUG";
        for (auto& l : cfg.libs) {
            if (!l.lib.empty()) cmd += " /LIBPATH:" + quote(l.lib);
            for (auto& n : l.link) cmd += " " + (n.size() > 4 && n.substr(n.size() - 4) == ".lib" ? n : n + ".lib");
        }
        if (!cc.vcvars.empty() || !preCmds.empty()) {
            // one batch file: the Visual Studio environment, assembler steps, then cl
            fs::path bat = cfile.parent_path() / "axon_cl.bat";
            std::string text = "@echo off\r\n";
            if (!cc.vcvars.empty()) text += "call " + quote(cc.vcvars) + " >nul 2>nul\r\n";
            for (auto& p : preCmds) text += p + "\r\nif errorlevel 1 exit /b 1\r\n";
            text += cmd + "\r\nexit /b %errorlevel%\r\n";
            writeFile(bat, text);
            cmd = "cmd /c " + quote(bat.string());
        }
    } else {
        cmd = quote(cc.exe) + " -std=c11 -w " + std::string(cfg.release ? "-O2 -DNDEBUG" : "-O0 -g") + " " + quote(cfile.string()) +
              " -o " + quote(exe.string());
        for (auto& l : cfg.libs) {
            if (!l.include.empty()) cmd += " -I" + quote(l.include);
            for (auto& d : l.defines) cmd += " -D" + quote(d);
            if (!l.lib.empty()) cmd += " -L" + quote(l.lib);
            for (auto& n : l.link) cmd += " -l" + n;
        }
#ifndef _WIN32
        cmd += " -lm";
#endif
    }
    std::string out;
    int rc = runCapture(cmd, out);
    if (rc != 0) {
        fatal("the C compiler (" + cc.kind + ") failed on the generated code",
              "this is usually a missing library or header; if it isn't, it's a bug in axonc - please report it");
        fprintf(stderr, "%s\n", out.c_str());
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------- pipeline

struct CompileResult {
    bool ok = false;
    fs::path cfile, exe;
};

static bool frontEnd(BuildConfig& cfg, Program& prog) {
    Loader loader(prog, cfg);
    std::string text;
    if (!readFile(cfg.entry, text)) {
        fatal("can't read `" + displayPath(cfg.entry) + "`");
        return false;
    }
    prog.testMode = cfg.testMode;
    Module* mainMod = loader.loadSource("main", cfg.entry.string(), displayPath(cfg.entry), text, false);
    prog.mainModule = mainMod;
    if (g_diag.errorCount() == 0) checkProgram(prog);
    g_diag.flush();
    int n = g_diag.errorCount();
    if (n > 0) {
        fatal("could not compile `" + cfg.name + "` because of " + std::to_string(n) + " error" + (n == 1 ? "" : "s"));
        return false;
    }
    return true;
}

static CompileResult compile(BuildConfig& cfg, bool emitOnly) {
    CompileResult r;
    auto t0 = std::chrono::steady_clock::now();
    status("Compiling", cfg.name + (cfg.projectRoot.empty() ? "" : " (" + displayPath(cfg.projectRoot) + ")"));
    Program prog;
    if (!frontEnd(cfg, prog)) return r;
    EmitOptions eo;
    eo.release = cfg.release;
    eo.testMode = cfg.testMode;
    eo.support = cfg.backend != "c";
    std::string c = emitProgram(prog, eo);
    std::string stem = cfg.name + (cfg.testMode ? "_test" : "");
    r.cfile = cfg.buildDir / (stem + (eo.support ? "_rt.c" : ".c"));
#ifdef _WIN32
    r.exe = cfg.buildDir / (stem + ".exe");
#else
    r.exe = cfg.buildDir / stem;
#endif
    if (!writeFile(r.cfile, c)) {
        fatal("can't write `" + displayPath(r.cfile) + "`");
        return r;
    }
    std::vector<std::string> pre;
    std::vector<fs::path> objs;
    if (cfg.backend == "ir" || cfg.backend == "x64" || cfg.backend == "llvm") {
        IRModule ir = generateIR(prog);
        writeFile(cfg.buildDir / (stem + ".ir"), irToText(ir));
        if (cfg.backend == "x64") {
            fs::path asmFile = cfg.buildDir / (stem + ".asm"), obj = cfg.buildDir / (stem + "_x64.obj");
            writeFile(asmFile, emitX64(ir));
            pre.push_back("ml64 /nologo /c /Fo" + quote(obj.string()) + " " + quote(asmFile.string()) + " >nul");
            objs.push_back(obj);
        } else if (cfg.backend == "llvm") {
            fs::path ll = cfg.buildDir / (stem + ".ll"), obj = cfg.buildDir / (stem + "_llvm.obj");
            writeFile(ll, emitLLVM(ir));
            std::string clang = findLLVMClang();
            if (clang.empty()) {
                fatal("the LLVM backend needs clang", "install LLVM, or put it in tools/llvm next to axon");
                return r;
            }
            pre.push_back(quote(clang) + " -c " + std::string(cfg.release ? "-O2" : "-O0") + " -Wno-override-module " + quote(ll.string()) +
                          " -o " + quote(obj.string()));
            objs.push_back(obj);
        }
    }
    if (emitOnly) {
        status("Wrote", displayPath(r.cfile));
        r.ok = true;
        return r;
    }
    if (compileC(cfg, r.cfile, r.exe, pre, objs) != 0) return r;
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    char buf[64];
    snprintf(buf, sizeof buf, "%.2fs", secs);
    status("Finished", std::string(cfg.release ? "release" : "debug") + " build in " + buf + " -> " + displayPath(r.exe));
    r.ok = true;
    return r;
}

static int runExe(const fs::path& exe, const std::vector<std::string>& args) {
    std::string cmd = quote(fs::absolute(exe).string());
    for (auto& a : args) cmd += " " + quote(a);
    status("Running", displayPath(exe));
    fflush(stderr);
#ifdef _WIN32
    std::string full = "\"" + cmd + "\"";
    int rc = system(full.c_str());
#else
    int rc = system(cmd.c_str());
    if (WIFEXITED(rc)) rc = WEXITSTATUS(rc);
#endif
    return rc;
}

// Finds axon.toml in the current directory or a parent.
static bool findProject(fs::path& root) {
    fs::path d = fs::current_path();
    while (true) {
        if (fs::exists(d / "axon.toml")) { root = d; return true; }
        if (!d.has_parent_path() || d.parent_path() == d) return false;
        d = d.parent_path();
    }
}

static void addLibraries(const fs::path& root, const Toml& t, BuildConfig& cfg);
static bool resolveDeps(const fs::path& root, const Toml& t, BuildConfig& cfg, const fs::path& topRoot, const Toml& lock,
                        std::map<std::string, std::string>& newLock, std::set<std::string>& seen);

static bool loadProject(const fs::path& root, BuildConfig& cfg) {
    std::string text;
    readFile(root / "axon.toml", text);
    Toml t = parseToml(text);
    if (!t.error.empty()) {
        fatal("axon.toml line " + std::to_string(t.errorLine) + ": " + t.error);
        return false;
    }
    cfg.projectRoot = root;
    cfg.name = t.str("", "name", root.filename().string());
    cfg.entry = root / t.str("", "entry", "src/main.ax");
    cfg.srcRoot = cfg.entry.parent_path();
    cfg.buildDir = root / "build";
    cfg.cCompiler = t.str("build", "c_compiler", "");
    addLibraries(root, t, cfg);
    std::set<std::string> seen;
    Toml lock;
    std::string lockText;
    if (readFile(root / "axon.lock", lockText)) lock = parseToml(lockText);
    std::map<std::string, std::string> newLock;
    bool ok = resolveDeps(root, t, cfg, root, lock, newLock, seen);
    // remember the exact commits of git packages, so every build uses the same code
    if (ok && !newLock.empty()) {
        std::string out = "# axon.lock - exact versions of git packages. Generated by axon; commit it.\n[git]\n";
        for (auto& kv : newLock) out += kv.first + " = \"" + kv.second + "\"\n";
        if (out != lockText) writeFile(root / "axon.lock", out);
    }
    return ok;
}

static bool g_updateDeps = false;  // `axon fetch --update`

// Clones or updates a git package. Returns false on failure.
static bool fetchGit(const std::string& name, const TomlValue& spec, const fs::path& dir, const std::string& locked, std::string& rev) {
    auto get = [&](const char* k) {
        auto j = spec.table.find(k);
        return j == spec.table.end() ? std::string() : j->second.s;
    };
    std::string url = get("git"), tag = get("tag"), branch = get("branch"), want = get("rev");
    std::string out;
    if (!fs::exists(dir / ".git")) {
        status("Fetching", name + " (" + url + ")");
        std::error_code ec;
        fs::create_directories(dir.parent_path(), ec);
        if (runCapture("git clone --quiet " + quote(url) + " " + quote(dir.string()), out) != 0) {
            fatal("couldn't clone package `" + name + "` from " + url, out);
            return false;
        }
    } else if (g_updateDeps) {
        status("Updating", name);
        runCapture("git -C " + quote(dir.string()) + " fetch --quiet --tags origin", out);
    }
    std::string target = !locked.empty() && !g_updateDeps ? locked
                       : !want.empty()                    ? want
                       : !tag.empty()                     ? "tags/" + tag
                       : !branch.empty()                  ? "origin/" + branch
                                                          : "origin/HEAD";
    out.clear();
    if (runCapture("git -C " + quote(dir.string()) + " checkout --quiet " + quote(target), out) != 0) {
        fatal("couldn't check out `" + target + "` of package `" + name + "`", out);
        return false;
    }
    out.clear();
    runCapture("git -C " + quote(dir.string()) + " rev-parse HEAD", out);
    while (!out.empty() && isspace((unsigned char)out.back())) out.pop_back();
    rev = out;
    return true;
}

// Reads [dependencies] of the package at `root`, recursively. Packages are flattened by name.
static bool resolveDeps(const fs::path& root, const Toml& t, BuildConfig& cfg, const fs::path& topRoot, const Toml& lock,
                        std::map<std::string, std::string>& newLock, std::set<std::string>& seen) {
    auto it = t.sections.find("dependencies");
    if (it == t.sections.end()) return true;
    for (auto& kv : it->second) {
        const std::string& name = kv.first;
        if (seen.count(name)) continue;
        seen.insert(name);
        const TomlValue& spec = kv.second;
        fs::path depRoot;
        if (spec.kind == TomlValue::Table && spec.table.count("path")) {
            depRoot = fs::absolute(root / spec.table.at("path").s);
        } else if (spec.kind == TomlValue::Table && spec.table.count("git")) {
            depRoot = topRoot / ".axon" / "deps" / name;
            std::string rev;
            if (!fetchGit(name, spec, depRoot, lock.str("git", name, ""), rev)) return false;
            newLock[name] = rev;
        } else {
            fatal("dependency `" + name + "` needs a `path` or a `git` source",
                  name + " = { path = \"../" + name + "\" }   or   " + name + " = { git = \"https://...\", tag = \"v1.0\" }");
            return false;
        }
        std::string text;
        if (!readFile(depRoot / "axon.toml", text)) {
            fatal("package `" + name + "` has no axon.toml (looked in " + displayPath(depRoot) + ")");
            return false;
        }
        Toml dt = parseToml(text);
        if (!dt.error.empty()) {
            fatal(displayPath(depRoot / "axon.toml") + " line " + std::to_string(dt.errorLine) + ": " + dt.error);
            return false;
        }
        BuildConfig::Dep d;
        d.root = depRoot;
        d.entry = depRoot / dt.str("", "lib", "src/lib.ax");
        d.srcRoot = d.entry.parent_path();
        cfg.deps[name] = d;
        addLibraries(depRoot, dt, cfg);
        if (!resolveDeps(depRoot, dt, cfg, topRoot, lock, newLock, seen)) return false;
    }
    return true;
}

// [libraries] of a package; relative paths are relative to that package.
static void addLibraries(const fs::path& root, const Toml& t, BuildConfig& cfg) {
    auto it = t.sections.find("libraries");
    if (it != t.sections.end()) {
        for (auto& kv : it->second) {
            Library l;
            l.name = kv.first;
            const TomlValue& v = kv.second;
            if (v.kind == TomlValue::Table) {
                auto get = [&](const char* k) {
                    auto j = v.table.find(k);
                    return j == v.table.end() ? std::string() : j->second.s;
                };
                l.include = get("include");
                l.lib = get("lib");
                auto j = v.table.find("link");
                if (j != v.table.end()) l.link = j->second.kind == TomlValue::Arr ? j->second.arr : std::vector<std::string>{j->second.s};
                else l.link = {kv.first};
                auto d = v.table.find("defines");
                if (d != v.table.end()) l.defines = d->second.kind == TomlValue::Arr ? d->second.arr : std::vector<std::string>{d->second.s};
            } else l.link = {kv.first};
            auto absolute = [&](std::string& p) {
                if (!p.empty() && fs::path(p).is_relative()) p = (root / p).string();
            };
            absolute(l.include);
            absolute(l.lib);
            bool dup = false;
            for (auto& o : cfg.libs) dup |= o.name == l.name && o.include == l.include;
            if (!dup) cfg.libs.push_back(l);
        }
    }
}

static void singleFileConfig(const fs::path& file, BuildConfig& cfg) {
    cfg.entry = fs::absolute(file);
    cfg.name = file.stem().string();
    cfg.srcRoot = cfg.entry.parent_path();
    cfg.buildDir = cfg.srcRoot / "build";
}

static int cmdTokens(const std::string& file) {
    std::string text;
    if (!readFile(file, text)) {
        fatal("can't read `" + file + "`");
        return 1;
    }
    int id = g_diag.addFile(file, displayPath(file), text);
    auto toks = lexFile(id);
    for (auto& t : toks) {
        const char* kind = "SYMBOL";
        switch (t.type) {
            case T::Ident: kind = "NAME"; break;
            case T::IntLit: case T::FloatLit: kind = "NUMBER"; break;
            case T::StrLit: case T::InterpStr: kind = "STRING"; break;
            case T::CharLit: kind = "CHAR"; break;
            case T::Eof: kind = "EOF"; break;
            default:
                if (t.type >= T::KwInt && t.type <= T::KwClass) kind = "KEYWORD";
        }
        printf("%-8s %-20s (%d:%d)\n", kind, t.type == T::Eof ? "" : t.text.c_str(), t.span.line, t.span.col);
    }
    g_diag.flush();
    return g_diag.errorCount() ? 1 : 0;
}

static bool findProject(fs::path& root);
static bool loadProject(const fs::path& root, BuildConfig& cfg);

// axon fmt [files or folders] [--check]
static int cmdFmt(const std::vector<std::string>& args) {
    bool check = false;
    std::vector<fs::path> files;
    auto addDir = [&](const fs::path& d) {
        std::error_code ec;
        for (auto& e : fs::recursive_directory_iterator(d, ec)) {
            if (e.is_regular_file() && e.path().extension() == ".ax") {
                bool skip = false;
                for (auto& part : e.path()) skip |= part == "build" || part == ".axon";
                if (!skip) files.push_back(e.path());
            }
        }
    };
    for (auto& a : args) {
        if (a == "--check") check = true;
        else if (fs::is_directory(a)) addDir(a);
        else if (fs::path(a).extension() == ".ax") files.push_back(a);
        else {
            fatal("`" + a + "` isn't an .ax file or a folder");
            return 1;
        }
    }
    if (files.empty()) {
        fs::path root;
        BuildConfig cfg;
        if (!findProject(root) || !loadProject(root, cfg)) {
            fatal("nothing to format", "pass files or folders, or run it inside a project");
            return 1;
        }
        addDir(cfg.srcRoot);
        if (fs::exists(root / "bindings")) addDir(root / "bindings");
    }
    int changed = 0, failed = 0;
    for (auto& f : files) {
        std::string text;
        if (!readFile(f, text)) {
            fatal("can't read `" + displayPath(f) + "`");
            failed++;
            continue;
        }
        int id = g_diag.addFile(f.string(), displayPath(f), text);
        std::string out, err;
        if (!formatSource(id, out, err)) {
            g_diag.flush();
            fatal(displayPath(f) + ": " + err);
            failed++;
            continue;
        }
        if (out == text) continue;
        changed++;
        if (check) status("Unformatted", displayPath(f));
        else if (writeFile(f, out)) status("Formatted", displayPath(f));
        else {
            fatal("can't write `" + displayPath(f) + "`");
            failed++;
        }
    }
    if (check && changed == 0 && failed == 0) status("Checked", std::to_string(files.size()) + " file(s): all formatted");
    if (!check && changed == 0 && failed == 0) status("Formatted", "nothing to change in " + std::to_string(files.size()) + " file(s)");
    return failed || (check && changed) ? 1 : 0;
}

// axon add <name> --path ../lib        or        axon add <name> --git <url> [--tag v1.0 | --branch main]
static int cmdAdd(const std::vector<std::string>& args) {
    if (args.empty()) {
        fatal("`axon add` needs a package name", "axon add vecmath --path ../vecmath   or   axon add utils --git https://example.com/utils.git --tag v1.0");
        return 1;
    }
    std::string name = args[0], path, git, tag, branch;
    for (size_t i = 1; i + 1 < args.size(); i += 2) {
        if (args[i] == "--path") path = args[i + 1];
        else if (args[i] == "--git") git = args[i + 1];
        else if (args[i] == "--tag") tag = args[i + 1];
        else if (args[i] == "--branch") branch = args[i + 1];
        else { fatal("unknown option `" + args[i] + "`"); return 1; }
    }
    if (path.empty() == git.empty()) { fatal("give exactly one of --path or --git"); return 1; }
    fs::path root;
    if (!findProject(root)) { fatal("no axon.toml found"); return 1; }
    std::string text;
    readFile(root / "axon.toml", text);
    for (auto& c : path) if (c == '\\') c = '/';
    std::string spec = !path.empty() ? "{ path = \"" + path + "\" }"
                                     : "{ git = \"" + git + "\"" + (tag.empty() ? "" : ", tag = \"" + tag + "\"") +
                                           (branch.empty() ? "" : ", branch = \"" + branch + "\"") + " }";
    std::string line = name + " = " + spec + "\n";
    size_t sec = text.find("[dependencies]");
    if (sec == std::string::npos) {
        if (!text.empty() && text.back() != '\n') text += "\n";
        text += "\n[dependencies]\n" + line;
    } else {
        size_t eol = text.find('\n', sec);
        text.insert(eol == std::string::npos ? text.size() : eol + 1, line);
    }
    writeFile(root / "axon.toml", text);
    BuildConfig cfg;
    if (!loadProject(root, cfg)) return 1;
    status("Added", name + " " + spec);
    return 0;
}

static int cmdNew(const std::string& name) {
    fs::path root = fs::current_path() / name;
    if (fs::exists(root)) {
        fatal("`" + name + "` already exists");
        return 1;
    }
    writeFile(root / "axon.toml",
              "# axon.toml\n"
              "name = \"" + name + "\"\n"
              "version = \"0.1.0\"\n"
              "entry = \"src/main.ax\"\n"
              "\n"
              "[libraries]\n"
              "# raylib = { include = \"libs/raylib/include\", lib = \"libs/raylib/lib\", link = [\"raylib\", \"winmm\", \"gdi32\", \"user32\", \"shell32\"] }\n"
              "\n"
              "[build]\n"
              "# c_compiler = \"clang\"   # or \"gcc\" or \"cl\"; found automatically if not set\n");
    writeFile(root / "src" / "main.ax",
              "int main() {\n"
              "    print(\"Hello from " + name + "!\");\n"
              "    return 0;\n"
              "}\n");
    writeFile(root / "bindings" / "README.md",
              "Put C library bindings here, e.g. `raylib.ax` with an `extern \"raylib.h\" { ... }` block.\n"
              "Then write `using c \"raylib.h\";` in your code.\n");
    writeFile(root / ".gitignore", "build/\n");
    status("Created", "project `" + name + "`");
    fprintf(stderr, "\n    cd %s\n    axon run\n\n", name.c_str());
    return 0;
}

static void usage() {
    fprintf(stderr,
            "axon %s - the Axon language tool\n\n"
            "usage:\n"
            "  axon new <name>           create a new project\n"
            "  axon build [--release]    build the project in this folder\n"
            "  axon run [--release]      build and run (arguments after `--` go to the program)\n"
            "  axon test                 run every function marked @test\n"
            "  axon check                check for errors without building\n"
            "  axon emit-c               write the generated C to build/\n"
            "  axon emit-ir              write the Axon IR (build/<name>.ir)\n"
            "  --backend c|x64|llvm      code generator: C via a C compiler (default), Axon's own\n"
            "                            x86-64 backend (fast debug builds), or LLVM (needs clang)\n"
            "  axon tokens <file.ax>     print the tokens of a file\n"
            "  axon fmt [--check]        format the project (or the files/folders given)\n"
            "  axon lsp                  run the language server (editors start this for you)\n"
            "  axon add <name> --path <dir> | --git <url> [--tag t]   add a package\n"
            "  axon fetch [--update]     download git packages (--update: newest version)\n"
            "  axon bindgen <h> [-I dir] [-o out.ax]   write Axon bindings for a C header (needs LLVM)\n"
            "  axon run --leak-check     report memory that was never freed when the program ends\n"
            "\n"
            "Every command also accepts a single file instead of a project:\n"
            "  axon run hello.ax\n"
            "  axonc hello.ax [-o out.exe]   (same as `axon build hello.ax`)\n",
            AXON_VERSION);
}

// For editor tools: parse and check the project that contains `file` (or the file by itself).
// Messages are collected in g_diag instead of printed. Returns false if there were errors.
bool analyzeForTools(const std::string& file, const std::map<std::string, std::string>& overlay, Program& prog) {
    g_overlay = &overlay;
    BuildConfig cfg;
    fs::path root;
    fs::path start = fs::absolute(file).parent_path();
    bool inProject = false;
    for (fs::path d = start;; d = d.parent_path()) {
        if (fs::exists(d / "axon.toml")) { root = d; inProject = true; break; }
        if (!d.has_parent_path() || d.parent_path() == d) break;
    }
    if (inProject && loadProject(root, cfg)) {
        // is the file part of the project's module graph? if not, check it on its own
        Program p;
        p.tools = true;
        Loader loader(p, cfg);
        std::string text;
        if (readSource(cfg.entry, text)) {
            loader.loadSource("main", cfg.entry.string(), displayPath(cfg.entry), text, false);
            bool found = false;
            for (Module* m : p.modules) found |= normPath(m->path) == normPath(file);
            if (found) {
                p.mainModule = p.modules.empty() ? nullptr : p.modules[0];
                if (p.mainModule) p.mainModule = loader.byName["main"];
                checkProgram(p);
                prog = std::move(p);
                g_overlay = nullptr;
                return g_diag.errorCount() == 0;
            }
        }
        g_diag.reset();
        BuildConfig single = cfg;
        single.entry = fs::absolute(file);
        cfg = single;
    } else {
        singleFileConfig(file, cfg);
    }
    cfg.testMode = true;  // a lone module doesn't need `main`
    Loader loader(prog, cfg);
    std::string text;
    if (!readSource(cfg.entry, text)) {
        g_overlay = nullptr;
        return false;
    }
    prog.testMode = true;
    prog.tools = true;
    prog.mainModule = loader.loadSource("main", cfg.entry.string(), displayPath(cfg.entry), text, false);
    checkProgram(prog);
    g_overlay = nullptr;
    return g_diag.errorCount() == 0;
}

int runLanguageServer();

int main(int argc, char** argv) {
    setupConsole();
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string self = fs::path(argv[0]).stem().string();

    std::string cmd;
    if (!args.empty() && fs::path(args[0]).extension() == ".ax") cmd = "build";  // `axonc main.ax`
    else if (!args.empty()) { cmd = args[0]; args.erase(args.begin()); }
    if (cmd.empty() || cmd == "help" || cmd == "--help" || cmd == "-h") {
        usage();
        return cmd.empty() ? 1 : 0;
    }
    if (cmd == "version" || cmd == "--version") {
        printf("axon %s\n", AXON_VERSION);
        return 0;
    }
    if (cmd == "new") {
        if (args.empty()) { fatal("`axon new` needs a project name", "axon new mygame"); return 1; }
        return cmdNew(args[0]);
    }
    if (cmd == "tokens") {
        if (args.empty()) { fatal("`axon tokens` needs a file"); return 1; }
        return cmdTokens(args[0]);
    }
    if (cmd == "fmt") return cmdFmt(args);
    if (cmd == "lsp") return runLanguageServer();
    if (cmd == "fetch") {
        // download git packages (again, with --update: move to the newest tag/branch commit)
        for (auto& a : args) if (a == "--update") g_updateDeps = true;
        fs::path root;
        BuildConfig cfg;
        if (!findProject(root)) { fatal("no axon.toml found"); return 1; }
        if (!loadProject(root, cfg)) return 1;
        status("Ready", std::to_string(cfg.deps.size()) + " package(s)");
        return 0;
    }
    if (cmd == "add") return cmdAdd(args);
    if (cmd == "bindgen") {
        // axon bindgen <header> [-I dir]... [-D NAME]... [-o out.ax]   (inside a project, its libraries' paths are used too)
        if (args.empty()) { fatal("`axon bindgen` needs a header", "axon bindgen raylib.h -I libs/raylib/include -o bindings/raylib.ax"); return 1; }
        std::vector<std::string> incs, defs;
        std::string header = args[0], outFile;
        for (size_t i = 1; i + 1 < args.size(); i += 2) {
            if (args[i] == "-I") incs.push_back(args[i + 1]);
            else if (args[i] == "-D") defs.push_back(args[i + 1]);
            else if (args[i] == "-o") outFile = args[i + 1];
            else { fatal("unknown option `" + args[i] + "`"); return 1; }
        }
        fs::path root;
        BuildConfig pc;
        if (findProject(root) && loadProject(root, pc))
            for (auto& l : pc.libs) {
                if (!l.include.empty()) incs.push_back(l.include);
                for (auto& d : l.defines) defs.push_back(d);
            }
        CImportResult imp = importCHeader(header, incs, defs);
        if (!imp.ok) { fatal("couldn't import `" + header + "`", imp.error); return 1; }
        if (outFile.empty()) printf("%s", imp.axon.c_str());
        else {
            writeFile(outFile, imp.axon);
            status("Wrote", outFile + ": " + std::to_string(imp.functions) + " functions, " + std::to_string(imp.structs) + " structs, " +
                                std::to_string(imp.constants) + " constants (" + std::to_string(imp.skipped) + " skipped)");
        }
        return 0;
    }
    if (cmd != "build" && cmd != "run" && cmd != "test" && cmd != "check" && cmd != "emit-c" && cmd != "emit-ir") {
        fatal("unknown command `" + cmd + "`", "run `axon help` to see the commands");
        return 1;
    }

    BuildConfig cfg;
    std::string file, outPath;
    std::vector<std::string> progArgs;
    bool leakCheck = false;
    std::string backend = "c";
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        if (a == "--") { progArgs.assign(args.begin() + i + 1, args.end()); break; }
        if (a == "--release" || a == "-r") cfg.release = true;
        else if (a == "-o" && i + 1 < args.size()) outPath = args[++i];
        else if (a == "--cc" && i + 1 < args.size()) cfg.cCompiler = args[++i];
        else if (a == "--run") cmd = "run";
        else if (a == "--leak-check") leakCheck = true;
        else if (a == "--backend" && i + 1 < args.size()) backend = args[++i];
        else if (a == "--emit-c") cmd = "emit-c";
        else if (fs::path(a).extension() == ".ax") file = a;
        else {
            fatal("unknown option `" + a + "`", "run `axon help` to see the options");
            return 1;
        }
    }
    std::string cc = cfg.cCompiler;
    if (!file.empty()) {
        if (!fs::exists(file)) { fatal("`" + file + "` doesn't exist"); return 1; }
        singleFileConfig(file, cfg);
    } else {
        fs::path root;
        if (!findProject(root)) {
            fatal("no axon.toml found in this folder or its parents",
                  "create a project with `axon new <name>`, or pass a file: `" + self + " " + cmd + " main.ax`");
            return 1;
        }
        if (!loadProject(root, cfg)) return 1;
    }
    if (!cc.empty()) cfg.cCompiler = cc;
    cfg.testMode = cmd == "test";

    if (cmd == "check") {
        Program prog;
        if (!frontEnd(cfg, prog)) return 1;
        status("Checked", cfg.name + ": no errors");
        return 0;
    }
    if (backend != "c" && backend != "x64" && backend != "llvm") {
        fatal("unknown backend `" + backend + "`", "choose c (default), x64 or llvm");
        return 1;
    }
    cfg.backend = cmd == "emit-ir" ? "ir" : backend;
    CompileResult r = compile(cfg, cmd == "emit-c" || cmd == "emit-ir");
    if (!r.ok) return 1;
    if (!outPath.empty() && cmd == "build") {
        std::error_code ec;
        fs::copy_file(r.exe, outPath, fs::copy_options::overwrite_existing, ec);
        if (ec) { fatal("can't write `" + outPath + "`"); return 1; }
    }
    if (leakCheck) {
#ifdef _WIN32
        _putenv("AXON_LEAKCHECK=1");
#else
        setenv("AXON_LEAKCHECK", "1", 1);
#endif
    }
    if (cmd == "run" || cmd == "test") return runExe(r.exe, progArgs);
    return 0;
}
