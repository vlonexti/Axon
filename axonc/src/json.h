// json.h - a small JSON value with a parser and a writer (for the language server).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct Json {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool v) : kind(Bool), b(v) {}
    Json(int v) : kind(Num), n(v) {}
    Json(int64_t v) : kind(Num), n((double)v) {}
    Json(double v) : kind(Num), n(v) {}
    Json(const char* v) : kind(Str), s(v) {}
    Json(std::string v) : kind(Str), s(std::move(v)) {}

    static Json array() { Json j; j.kind = Arr; return j; }
    static Json object() { Json j; j.kind = Obj; return j; }

    Json& set(const std::string& k, Json v) {
        kind = Obj;
        for (auto& kv : obj)
            if (kv.first == k) { kv.second = std::move(v); return *this; }
        obj.push_back({k, std::move(v)});
        return *this;
    }
    Json& push(Json v) { kind = Arr; arr.push_back(std::move(v)); return *this; }
    const Json& operator[](const std::string& k) const {
        static const Json none;
        for (auto& kv : obj)
            if (kv.first == k) return kv.second;
        return none;
    }
    bool has(const std::string& k) const {
        for (auto& kv : obj) if (kv.first == k) return true;
        return false;
    }
    int asInt() const { return (int)n; }

    std::string dump() const {
        std::string o;
        write(o);
        return o;
    }
    void write(std::string& o) const {
        switch (kind) {
            case Null: o += "null"; break;
            case Bool: o += b ? "true" : "false"; break;
            case Num: {
                if (n == (double)(int64_t)n) o += std::to_string((int64_t)n);
                else o += std::to_string(n);
                break;
            }
            case Str: writeStr(o, s); break;
            case Arr:
                o += '[';
                for (size_t i = 0; i < arr.size(); i++) {
                    if (i) o += ',';
                    arr[i].write(o);
                }
                o += ']';
                break;
            case Obj:
                o += '{';
                for (size_t i = 0; i < obj.size(); i++) {
                    if (i) o += ',';
                    writeStr(o, obj[i].first);
                    o += ':';
                    obj[i].second.write(o);
                }
                o += '}';
                break;
        }
    }
    static void writeStr(std::string& o, const std::string& v) {
        o += '"';
        for (unsigned char c : v) {
            switch (c) {
                case '"': o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n"; break;
                case '\r': o += "\\r"; break;
                case '\t': o += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char b[8];
                        snprintf(b, sizeof b, "\\u%04x", c);
                        o += b;
                    } else o += (char)c;
            }
        }
        o += '"';
    }

    static bool parse(const std::string& t, Json& out) {
        size_t i = 0;
        if (!parseValue(t, i, out)) return false;
        ws(t, i);
        return i == t.size();
    }

private:
    static void ws(const std::string& t, size_t& i) {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) i++;
    }
    static void utf8(std::string& o, uint32_t c) {
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { o += (char)(0xE0 | (c >> 12)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
        else { o += (char)(0xF0 | (c >> 18)); o += (char)(0x80 | ((c >> 12) & 0x3F)); o += (char)(0x80 | ((c >> 6) & 0x3F)); o += (char)(0x80 | (c & 0x3F)); }
    }
    static bool parseStr(const std::string& t, size_t& i, std::string& o) {
        if (t[i] != '"') return false;
        i++;
        while (i < t.size() && t[i] != '"') {
            if (t[i] == '\\') {
                i++;
                if (i >= t.size()) return false;
                char e = t[i++];
                switch (e) {
                    case 'n': o += '\n'; break;
                    case 't': o += '\t'; break;
                    case 'r': o += '\r'; break;
                    case 'b': o += '\b'; break;
                    case 'f': o += '\f'; break;
                    case 'u': {
                        if (i + 4 > t.size()) return false;
                        uint32_t c = (uint32_t)strtoul(t.substr(i, 4).c_str(), nullptr, 16);
                        i += 4;
                        if (c >= 0xD800 && c < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                            uint32_t lo = (uint32_t)strtoul(t.substr(i + 2, 4).c_str(), nullptr, 16);
                            i += 6;
                            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        utf8(o, c);
                        break;
                    }
                    default: o += e;
                }
            } else o += t[i++];
        }
        if (i >= t.size()) return false;
        i++;
        return true;
    }
    static bool parseValue(const std::string& t, size_t& i, Json& out) {
        ws(t, i);
        if (i >= t.size()) return false;
        char c = t[i];
        if (c == '{') {
            out = object();
            i++;
            ws(t, i);
            if (i < t.size() && t[i] == '}') { i++; return true; }
            while (true) {
                ws(t, i);
                std::string k;
                if (i >= t.size() || !parseStr(t, i, k)) return false;
                ws(t, i);
                if (i >= t.size() || t[i] != ':') return false;
                i++;
                Json v;
                if (!parseValue(t, i, v)) return false;
                out.obj.push_back({k, std::move(v)});
                ws(t, i);
                if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == '}') { i++; return true; }
                return false;
            }
        }
        if (c == '[') {
            out = array();
            i++;
            ws(t, i);
            if (i < t.size() && t[i] == ']') { i++; return true; }
            while (true) {
                Json v;
                if (!parseValue(t, i, v)) return false;
                out.arr.push_back(std::move(v));
                ws(t, i);
                if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == ']') { i++; return true; }
                return false;
            }
        }
        if (c == '"') {
            out.kind = Str;
            return parseStr(t, i, out.s);
        }
        if (t.compare(i, 4, "true") == 0) { out = Json(true); i += 4; return true; }
        if (t.compare(i, 5, "false") == 0) { out = Json(false); i += 5; return true; }
        if (t.compare(i, 4, "null") == 0) { out = Json(); i += 4; return true; }
        size_t s = i;
        while (i < t.size() && (isdigit((unsigned char)t[i]) || t[i] == '-' || t[i] == '+' || t[i] == '.' || t[i] == 'e' || t[i] == 'E')) i++;
        if (s == i) return false;
        out = Json(strtod(t.substr(s, i - s).c_str(), nullptr));
        return true;
    }
};
