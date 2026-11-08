// lsp.cpp - `axon lsp`, a Language Server Protocol server for editors.
// Features: diagnostics as you type, hover (types and signatures), go to definition,
// completion (members after `.`, names otherwise), document outline and formatting.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "checker.h"
#include "diagnostics.h"
#include "format.h"
#include "json.h"
#include "types.h"

namespace fs = std::filesystem;

std::string normPath(const std::string& p);
bool analyzeForTools(const std::string& file, const std::map<std::string, std::string>& overlay, Program& prog);

namespace {

// ---------------------------------------------------------------- transport

bool readMessage(Json& out) {
    size_t length = 0;
    std::string line;
    while (true) {
        line.clear();
        int c;
        while ((c = fgetc(stdin)) != EOF && c != '\n') line += (char)c;
        if (c == EOF) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        if (line.rfind("Content-Length:", 0) == 0) length = (size_t)strtoul(line.c_str() + 15, nullptr, 10);
    }
    std::string body(length, '\0');
    if (length && fread(&body[0], 1, length, stdin) != length) return false;
    return Json::parse(body, out);
}

void send(const Json& msg) {
    std::string body = msg.dump();
    fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", body.size(), body.c_str());
    fflush(stdout);
}

void reply(const Json& id, Json result) {
    Json m = Json::object();
    m.set("jsonrpc", "2.0").set("id", id).set("result", std::move(result));
    send(m);
}

void notify(const std::string& method, Json params) {
    Json m = Json::object();
    m.set("jsonrpc", "2.0").set("method", method).set("params", std::move(params));
    send(m);
}

// ---------------------------------------------------------------- uris and positions

int hexv(char c) { return isdigit((unsigned char)c) ? c - '0' : (tolower(c) - 'a' + 10); }

std::string uriToPath(const std::string& uri) {
    std::string s = uri;
    if (s.rfind("file://", 0) == 0) s = s.substr(7);
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += (char)(hexv(s[i + 1]) * 16 + hexv(s[i + 2]));
            i += 2;
        } else out += s[i];
    }
#ifdef _WIN32
    if (out.size() > 2 && out[0] == '/' && out[2] == ':') out = out.substr(1);
    for (auto& c : out) if (c == '/') c = '\\';
#endif
    return out;
}

std::map<std::string, std::string> g_uriOf;  // normalized path -> uri the editor used

std::string pathToUri(const std::string& path) {
    auto it = g_uriOf.find(normPath(path));
    if (it != g_uriOf.end()) return it->second;
    std::string p = fs::absolute(path).string();
    std::string out = "file://";
#ifdef _WIN32
    out += "/";
#endif
    for (char c : p) {
        if (c == '\\') out += '/';
        else if (c == ' ') out += "%20";
        else out += c;
    }
    return out;
}

// LSP positions are 0-based lines and UTF-16 columns; Axon spans are 1-based byte columns.
// Counting bytes is exact for ASCII code, which is what identifiers are made of.
int offsetOf(int fileId, int line, int ch) {
    const SourceFile& f = g_diag.file(fileId);
    if (line < 0) return 0;
    if (line >= (int)f.lineStarts.size()) return (int)f.text.size();
    return std::min((int)f.text.size(), f.lineStarts[line] + ch);
}

Json position(int fileId, int offset) {
    Span s = g_diag.makeSpan(fileId, offset, 1);
    Json p = Json::object();
    p.set("line", s.line - 1).set("character", s.col - 1);
    return p;
}

Json rangeOf(Span sp) {
    Json r = Json::object();
    if (sp.file < 0) {
        Json z = Json::object();
        z.set("line", 0).set("character", 0);
        r.set("start", z).set("end", z);
        return r;
    }
    r.set("start", position(sp.file, sp.offset)).set("end", position(sp.file, sp.offset + sp.len));
    return r;
}

Json location(Span sp) {
    Json l = Json::object();
    l.set("uri", pathToUri(g_diag.file(sp.file).path)).set("range", rangeOf(sp));
    return l;
}

// ---------------------------------------------------------------- finding things in the tree

struct Hit {
    Span span;
    Expr* e = nullptr;
    VarDecl* v = nullptr;
    FuncDecl* f = nullptr;
    FieldDecl* fd = nullptr;
    TypeExpr* te = nullptr;
    Type* ftype = nullptr;
    Span target;  // where its definition is
};

std::string sigOf(FuncDecl* f) {
    std::string s = std::string(f->isMut ? "mut " : "") + (f->retType ? typeName(f->retType) : "?") + " " + f->name + "(";
    for (size_t i = 0; i < f->params.size(); i++) {
        auto& p = f->params[i];
        if (i) s += ", ";
        s += std::string(p.mode == 2 ? "mut ref " : p.mode == 1 ? "ref " : "") + (p.var && p.var->type ? typeName(p.var->type) : "?") + " " + p.name;
    }
    return s + ")";
}

Span declSpanOfType(Type* t) {
    if (!t) return Span();
    if (t->k == TK::Struct) {
        StructDecl* sd = t->sd->gen.origin ? (StructDecl*)t->sd->gen.origin : t->sd;
        return sd->span;
    }
    if (t->k == TK::Enum) {
        EnumDecl* ed = t->ed->gen.origin ? (EnumDecl*)t->ed->gen.origin : t->ed;
        return ed->span;
    }
    if (t->k == TK::Interface) return t->id->span;
    return Span();
}

struct Index {
    std::vector<Hit> hits;
    std::vector<VarDecl*> vars;  // every variable declaration, for completion

    void add(Hit h) {
        if (h.span.file >= 0) hits.push_back(h);
    }
    void type(TypeExpr* te) {
        if (!te) return;
        Hit h;
        h.span = te->span;
        h.te = te;
        if (te->resolved) h.target = declSpanOfType(te->resolved);
        add(h);
        for (auto* a : te->args) type(a);
    }
    void var(VarDecl* v) {
        if (!v) return;
        Hit h;
        h.span = v->span;
        h.v = v;
        h.target = v->span;
        add(h);
        vars.push_back(v);
        type(v->texpr);
        if (v->kind != VarDecl::Global) expr(v->init);
    }
    void pattern(Pattern* p) {
        if (!p) return;
        for (auto& b : p->binds) {
            if (b.var) var(b.var);
            type(b.type);
        }
        if (p->bind.var) var(p->bind.var);
        type(p->type);
        expr(p->expr);
    }
    void expr(Expr* e) {
        if (!e) return;
        Hit h;
        h.span = e->span;
        h.e = e;
        if (e->kind == EK::Name && e->var) h.target = e->var->span;
        add(h);
        if (e->kind == EK::Call && e->fn && e->a) {
            Hit c;
            c.f = e->fn;
            c.target = e->fn->gen.origin ? ((FuncDecl*)e->fn->gen.origin)->span : e->fn->span;
            if (e->a->kind == EK::Name) c.span = e->a->span;
            else if (e->a->kind == EK::Member && !e->a->fieldSpans.empty()) c.span = e->a->fieldSpans[0];
            add(c);
        }
        if (e->kind == EK::Member && e->field >= 0 && e->a && e->a->type && e->a->type->k == TK::Struct && !e->fieldSpans.empty()) {
            Hit m;
            m.span = e->fieldSpans[0];
            m.fd = e->a->type->sd->fields[e->field];
            m.ftype = e->type;
            m.target = m.fd->span;
            add(m);
        }
        if (e->kind == EK::Call && e->callKind == CallKind::EnumCtor && e->ed && e->variant >= 0) h.target = e->ed->variants[e->variant].span;
        expr(e->a);
        expr(e->b);
        expr(e->c);
        for (auto* x : e->args) expr(x);
        type(e->texpr);
        pattern(e->pat);
    }
    void stmt(Stmt* s) {
        if (!s) return;
        for (auto* x : s->stmts) stmt(x);
        if (s->var) var(s->var);
        expr(s->expr);
        stmt(s->then);
        stmt(s->els);
        stmt(s->init);
        stmt(s->body);
        expr(s->step);
        for (auto& c : s->cases) {
            for (auto* p : c.pats) pattern(p);
            for (auto* b : c.body) stmt(b);
        }
    }
    void func(FuncDecl* f) {
        Hit h;
        h.span = f->span;
        h.f = f;
        h.target = f->span;
        add(h);
        type(f->ret);
        for (auto& p : f->params) {
            if (p.var) var(p.var);
            type(p.type);
        }
        stmt(f->body);
    }
    void build(Program& P) {
        for (FuncDecl* f : P.funcs) func(f);
        for (Module* m : P.modules) {
            for (auto* f : m->funcs)
                if (!f->checked) func(f);
            for (auto* s : m->structs) {
                for (auto* fd : s->fields) {
                    Hit h;
                    h.span = fd->span;
                    h.fd = fd;
                    h.ftype = fd->rtype;
                    h.target = fd->span;
                    add(h);
                    type(fd->type);
                    expr(fd->def);
                }
                for (auto* ie : s->ifaceExprs) type(ie);
            }
            for (auto* e : m->enums)
                for (auto& v : e->variants)
                    for (auto* fd : v.fields) type(fd->type);
            for (auto* g : m->globals) {
                Hit h;
                h.span = g->span;
                h.v = g;
                h.target = g->span;
                add(h);
                type(g->texpr);
                expr(g->init);
            }
            for (auto* x : m->extends) type(x->target);
        }
    }
    const Hit* at(int fileId, int offset) const {
        const Hit* best = nullptr;
        for (auto& h : hits) {
            if (h.span.file != fileId) continue;
            if (offset < h.span.offset || offset > h.span.offset + h.span.len) continue;
            if (offset == h.span.offset + h.span.len && h.span.len > 0 && best) continue;
            if (!best || h.span.len < best->span.len) best = &h;
        }
        return best;
    }
};

// ---------------------------------------------------------------- server state

struct Server {
    std::map<std::string, std::string> docs;  // normalized path -> text
    Program prog;
    Index index;
    std::set<std::string> reportedUris;
    bool shutdown = false;

    int fileIdOf(const std::string& path) {
        std::string n = normPath(path);
        for (int i = 0; i < g_diag.fileCount(); i++)
            if (normPath(g_diag.file(i).path) == n) return i;
        return -1;
    }

    void analyze(const std::string& path) {
        g_diag.reset();
        g_diag.quiet = true;
        prog = Program();
        analyzeForTools(path, docs, prog);
        index = Index();
        index.build(prog);
        // publish diagnostics per file
        std::map<std::string, Json> perUri;
        perUri[pathToUri(path)] = Json::array();
        for (auto& d : g_diag.all()) {
            if (d.span.file < 0) continue;
            const std::string& p = g_diag.file(d.span.file).path;
            if (p.rfind("<std>", 0) == 0 || p.find("(formatted)") != std::string::npos) continue;
            std::string uri = pathToUri(p);
            if (!perUri.count(uri)) perUri[uri] = Json::array();
            Json diag = Json::object();
            std::string msg = d.msg;
            if (!d.label.empty()) msg += "\n" + d.label;
            for (auto& h : d.helps) msg += "\nhelp: " + h;
            for (auto& n : d.notes) msg += "\nnote: " + n.second;
            diag.set("range", rangeOf(d.span)).set("severity", d.sev == Diag::Error ? 1 : 2).set("source", "axon").set("message", msg);
            perUri[uri].push(diag);
        }
        for (auto& u : reportedUris)
            if (!perUri.count(u)) perUri[u] = Json::array();
        reportedUris.clear();
        for (auto& kv : perUri) {
            Json p = Json::object();
            p.set("uri", kv.first).set("diagnostics", kv.second);
            notify("textDocument/publishDiagnostics", p);
            if (!kv.second.arr.empty()) reportedUris.insert(kv.first);
        }
    }

    Json hover(const std::string& path, int line, int ch) {
        int fid = fileIdOf(path);
        if (fid < 0) return Json();
        const Hit* h = index.at(fid, offsetOf(fid, line, ch));
        if (!h) return Json();
        std::string text;
        if (h->f) text = sigOf(h->f);
        else if (h->v) {
            const char* kind = h->v->kind == VarDecl::Param ? "parameter" : h->v->kind == VarDecl::Global ? "constant" :
                               h->v->isConst ? "constant" : "variable";
            text = std::string("(") + kind + ") " + (h->v->type ? typeName(h->v->type) : "?") + " " + h->v->name;
            if (h->v->mode == 1) text = "(borrowed) " + text;
            if (h->v->mode == 2) text = "(mut borrowed) " + text;
        } else if (h->fd) text = "(field) " + (h->ftype ? typeName(h->ftype) : "?") + " " + h->fd->name;
        else if (h->te && h->te->resolved) {
            Type* t = h->te->resolved;
            text = (t->k == TK::Struct ? "struct " : t->k == TK::Enum ? "enum " : t->k == TK::Interface ? "interface " : "") + typeName(t);
            if (t->k == TK::Struct && !t->sd->isExtern) {
                text += " {";
                for (auto* f : t->sd->fields) text += "\n    " + (f->rtype ? typeName(f->rtype) : "?") + " " + f->name + ";";
                text += "\n}";
            }
        } else if (h->e && h->e->type) {
            if (h->e->kind == EK::Name && h->e->var) {
                VarDecl* v = h->e->var;
                text = (v->type ? typeName(v->type) : "?") + " " + v->name;
            } else text = typeName(h->e->type);
        }
        if (text.empty()) return Json();
        Json contents = Json::object();
        contents.set("kind", "markdown").set("value", "```axon\n" + text + "\n```");
        Json r = Json::object();
        r.set("contents", contents).set("range", rangeOf(h->span));
        return r;
    }

    Json definition(const std::string& path, int line, int ch) {
        int fid = fileIdOf(path);
        if (fid < 0) return Json();
        const Hit* h = index.at(fid, offsetOf(fid, line, ch));
        if (!h || h->target.file < 0) return Json();
        if (g_diag.file(h->target.file).path.rfind("<std>", 0) == 0) return Json();
        return location(h->target);
    }

    // what can follow `<type>.`
    void members(Type* t, Json& items) {
        auto item = [&](const std::string& label, int kind, const std::string& detail) {
            Json it = Json::object();
            it.set("label", label).set("kind", kind).set("detail", detail);
            items.push(it);
        };
        if (!t) return;
        if (t->k == TK::Box && t->a->k != TK::Interface) {
            item("value", 5, typeName(t->a));
            t = t->a;
        }
        if (t->k == TK::Box && t->a->k == TK::Interface) t = t->a;
        if (t->k == TK::Struct) {
            for (auto* f : t->sd->fields) item(f->name, 5, f->rtype ? typeName(f->rtype) : "");
            for (auto* m : t->sd->methods) item(m->name, 2, sigOf(m));
        }
        if (t->k == TK::Interface)
            for (auto* m : t->id->methods) item(m->name, 2, sigOf(m));
        auto it = prog.extMethods.find(t);
        if (it != prog.extMethods.end())
            for (auto* m : it->second) item(m->name, 2, sigOf(m));
        static const char* strM[] = {"length", "substring", "contains", "startsWith", "endsWith", "indexOf", "lastIndexOf", "trim", "toUpper",
                                     "toLower", "split", "toInt", "toDouble", "byteAt", "replace", "chars", "padLeft", "padRight", "repeat",
                                     "isEmpty", "clone"};
        static const char* listM[] = {"length", "add", "pop", "removeAt", "insert", "clear", "contains", "indexOf", "sort", "reverse", "clone"};
        static const char* arrM[] = {"length", "contains", "indexOf", "sort", "reverse", "clone"};
        static const char* mapM[] = {"length", "get", "contains", "remove", "keys", "values", "clear", "clone"};
        if (t->k == TK::String) for (auto* s : strM) item(s, 2, "string");
        if (t->k == TK::List) for (auto* s : listM) item(s, 2, typeName(t));
        if (t->k == TK::Array) for (auto* s : arrM) item(s, 2, typeName(t));
        if (t->k == TK::Map) for (auto* s : mapM) item(s, 2, typeName(t));
        if (t->k == TK::Optional) item("hasValue", 5, "bool");
        if (t->k == TK::Enum) for (auto& v : t->ed->variants) item(v.name, 20, typeName(t));
    }

    Json completion(const std::string& path, int line, int ch) {
        Json items = Json::array();
        std::string n = normPath(path);
        auto dit = docs.find(n);
        if (dit == docs.end()) return items;
        const std::string& text = dit->second;
        // offset of the cursor in the current text
        int off = 0, l = 0;
        while (l < line && off < (int)text.size()) {
            if (text[off] == '\n') l++;
            off++;
        }
        off = std::min((int)text.size(), off + ch);
        int p = off;
        while (p > 0 && (isalnum((unsigned char)text[p - 1]) || text[p - 1] == '_')) p--;
        int fid = fileIdOf(path);
        if (p > 0 && text[p - 1] == '.') {
            // walk back over `a.b.c.`
            std::vector<std::string> chain;
            int q = p - 1;
            while (q > 0 && text[q] == '.') {
                int e = q;
                int s = q;
                while (s > 0 && (isalnum((unsigned char)text[s - 1]) || text[s - 1] == '_')) s--;
                if (s == e) break;
                chain.insert(chain.begin(), text.substr(s, e - s));
                q = s - 1;
            }
            if (chain.empty()) return items;
            Type* t = nullptr;
            // the closest declaration of the first name before the cursor
            VarDecl* best = nullptr;
            for (auto* v : index.vars)
                if (v->name == chain[0] && v->span.file == fid && v->span.offset < off && (!best || v->span.offset > best->span.offset)) best = v;
            if (best) t = best->type;
            if (!t && chain[0] == "this") {
                for (FuncDecl* f : prog.funcs)
                    if (f->selfType && f->span.file == fid && f->span.offset < off) t = f->selfType;
            }
            if (!t) {
                // a module prefix, enum name or constant
                for (Module* m : prog.modules) {
                    if (m->fileId != fid) continue;
                    auto ns = m->namespaces.find(chain[0]);
                    if (ns != m->namespaces.end() && chain.size() == 1) {
                        for (auto* f : ns->second->funcs) {
                            Json it = Json::object();
                            it.set("label", f->name).set("kind", 3).set("detail", sigOf(f));
                            items.push(it);
                        }
                        for (auto* g : ns->second->globals) {
                            Json it = Json::object();
                            it.set("label", g->name).set("kind", 21);
                            items.push(it);
                        }
                        return items;
                    }
                }
                for (auto* e : prog.enums)
                    if (e->name == chain[0] && e->type) {
                        members(e->type, items);
                        return items;
                    }
            }
            for (size_t i = 1; t && i < chain.size(); i++) {
                if (t->k == TK::Box && t->a->k != TK::Interface) t = t->a;
                Type* next = nullptr;
                if (t->k == TK::Struct)
                    for (auto* f : t->sd->fields)
                        if (f->name == chain[i]) next = f->rtype;
                t = next;
            }
            members(t, items);
            return items;
        }
        // names: keywords, variables declared above, and everything global
        static const char* kws[] = {"int", "long", "byte", "float", "double", "bool", "char", "string", "void", "var", "const", "if", "else",
                                    "while", "do", "for", "foreach", "in", "return", "break", "continue", "switch", "case", "default",
                                    "struct", "interface", "enum", "extend", "mut", "ref", "new", "this", "true", "false", "none", "is",
                                    "try", "using", "extern", "private", "List", "Map", "Box", "Result"};
        for (auto* k : kws) {
            Json it = Json::object();
            it.set("label", k).set("kind", 14);
            items.push(it);
        }
        static const char* builtins[] = {"print", "write", "assert", "panic", "Ok", "Err", "Box"};
        for (auto* b : builtins) {
            Json it = Json::object();
            it.set("label", b).set("kind", 3);
            items.push(it);
        }
        std::set<std::string> seen;
        for (auto* v : index.vars)
            if (v->span.file == fid && v->span.offset < off && seen.insert(v->name).second) {
                Json it = Json::object();
                it.set("label", v->name).set("kind", 6).set("detail", v->type ? typeName(v->type) : "");
                items.push(it);
            }
        for (Module* m : prog.modules) {
            if (m->isStd && m->name == "prelude") continue;
            for (auto* f : m->funcs)
                if (seen.insert(f->name).second) {
                    Json it = Json::object();
                    it.set("label", f->name).set("kind", 3).set("detail", f->retType ? sigOf(f) : "");
                    items.push(it);
                }
            for (auto* s : m->structs)
                if (seen.insert(s->name).second) {
                    Json it = Json::object();
                    it.set("label", s->name).set("kind", 22);
                    items.push(it);
                }
            for (auto* e : m->enums)
                if (seen.insert(e->name).second) {
                    Json it = Json::object();
                    it.set("label", e->name).set("kind", 13);
                    items.push(it);
                }
            for (auto* i : m->ifaces)
                if (seen.insert(i->name).second) {
                    Json it = Json::object();
                    it.set("label", i->name).set("kind", 8);
                    items.push(it);
                }
            for (auto* g : m->globals)
                if (seen.insert(g->name).second) {
                    Json it = Json::object();
                    it.set("label", g->name).set("kind", 21);
                    items.push(it);
                }
        }
        return items;
    }

    Json symbols(const std::string& path) {
        Json out = Json::array();
        int fid = fileIdOf(path);
        auto sym = [&](const std::string& name, int kind, Span sp, Json children) {
            Json s = Json::object();
            s.set("name", name).set("kind", kind).set("range", rangeOf(sp)).set("selectionRange", rangeOf(sp));
            if (!children.arr.empty()) s.set("children", children);
            return s;
        };
        for (Module* m : prog.modules) {
            if (m->fileId != fid) continue;
            for (auto* s : m->structs) {
                Json ch = Json::array();
                for (auto* f : s->fields) ch.push(sym(f->name, 8, f->span, Json::array()));
                for (auto* f : s->methods) ch.push(sym(f->name, 6, f->span, Json::array()));
                out.push(sym(s->name, 23, s->span, ch));
            }
            for (auto* e : m->enums) {
                Json ch = Json::array();
                for (auto& v : e->variants) ch.push(sym(v.name, 22, v.span, Json::array()));
                out.push(sym(e->name, 10, e->span, ch));
            }
            for (auto* i : m->ifaces) out.push(sym(i->name, 11, i->span, Json::array()));
            for (auto* f : m->funcs) out.push(sym(f->name, 12, f->span, Json::array()));
            for (auto* g : m->globals) out.push(sym(g->name, 14, g->span, Json::array()));
        }
        return out;
    }

    Json formatting(const std::string& path) {
        auto it = docs.find(normPath(path));
        if (it == docs.end()) return Json();
        int id = g_diag.addFile(path, path, it->second);
        std::string out, err;
        if (!formatSource(id, out, err) || out == it->second) return Json::array();
        int lines = 0;
        for (char c : it->second) lines += c == '\n';
        Json start = Json::object(), end = Json::object(), range = Json::object(), edit = Json::object();
        start.set("line", 0).set("character", 0);
        end.set("line", lines + 1).set("character", 0);
        range.set("start", start).set("end", end);
        edit.set("range", range).set("newText", out);
        Json r = Json::array();
        r.push(edit);
        return r;
    }

    void handle(const Json& msg) {
        const std::string method = msg["method"].s;
        const Json& id = msg["id"];
        const Json& params = msg["params"];
        bool isRequest = msg.has("id");
        auto docPath = [&]() { return uriToPath(params["textDocument"]["uri"].s); };
        auto posLine = [&]() { return params["position"]["line"].asInt(); };
        auto posChar = [&]() { return params["position"]["character"].asInt(); };
        if (method == "initialize") {
            Json caps = Json::object();
            Json sync = Json::object();
            sync.set("openClose", true).set("change", 1).set("save", true);
            Json comp = Json::object();
            Json trig = Json::array();
            trig.push(".");
            comp.set("triggerCharacters", trig);
            caps.set("textDocumentSync", sync)
                .set("hoverProvider", true)
                .set("definitionProvider", true)
                .set("completionProvider", comp)
                .set("documentSymbolProvider", true)
                .set("documentFormattingProvider", true);
            Json info = Json::object();
            info.set("name", "axon-lsp").set("version", "0.5.0");
            Json r = Json::object();
            r.set("capabilities", caps).set("serverInfo", info);
            reply(id, r);
        } else if (method == "initialized") {
        } else if (method == "shutdown") {
            shutdown = true;
            reply(id, Json());
        } else if (method == "exit") {
            exit(shutdown ? 0 : 1);
        } else if (method == "textDocument/didOpen") {
            std::string path = docPath();
            g_uriOf[normPath(path)] = params["textDocument"]["uri"].s;
            docs[normPath(path)] = params["textDocument"]["text"].s;
            analyze(path);
        } else if (method == "textDocument/didChange") {
            std::string path = docPath();
            const Json& changes = params["contentChanges"];
            if (!changes.arr.empty()) docs[normPath(path)] = changes.arr.back()["text"].s;
            analyze(path);
        } else if (method == "textDocument/didSave") {
            analyze(docPath());
        } else if (method == "textDocument/didClose") {
            docs.erase(normPath(docPath()));
        } else if (method == "textDocument/hover") {
            reply(id, hover(docPath(), posLine(), posChar()));
        } else if (method == "textDocument/definition") {
            reply(id, definition(docPath(), posLine(), posChar()));
        } else if (method == "textDocument/completion") {
            reply(id, completion(docPath(), posLine(), posChar()));
        } else if (method == "textDocument/documentSymbol") {
            reply(id, symbols(docPath()));
        } else if (method == "textDocument/formatting") {
            reply(id, formatting(docPath()));
        } else if (isRequest) {
            Json m = Json::object();
            Json err = Json::object();
            err.set("code", -32601).set("message", "not supported: " + method);
            m.set("jsonrpc", "2.0").set("id", id).set("error", err);
            send(m);
        }
    }
};

}  // namespace

int runLanguageServer() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    Server server;
    Json msg;
    while (readMessage(msg)) server.handle(msg);
    return 0;
}
