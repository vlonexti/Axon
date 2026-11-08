// lexer.cpp
#include "lexer.h"

#include <cstring>
#include <unordered_map>

static const std::unordered_map<std::string, T>& keywords() {
    static const std::unordered_map<std::string, T> kw = {
        {"int", T::KwInt}, {"long", T::KwLong}, {"byte", T::KwByte}, {"float", T::KwFloat},
        {"double", T::KwDouble}, {"bool", T::KwBool}, {"char", T::KwChar}, {"string", T::KwString},
        {"void", T::KwVoid}, {"var", T::KwVar}, {"const", T::KwConst}, {"if", T::KwIf},
        {"else", T::KwElse}, {"while", T::KwWhile}, {"do", T::KwDo}, {"for", T::KwFor},
        {"foreach", T::KwForeach}, {"in", T::KwIn}, {"return", T::KwReturn}, {"break", T::KwBreak},
        {"continue", T::KwContinue}, {"switch", T::KwSwitch}, {"case", T::KwCase},
        {"default", T::KwDefault}, {"struct", T::KwStruct}, {"interface", T::KwInterface},
        {"enum", T::KwEnum}, {"extend", T::KwExtend}, {"mut", T::KwMut}, {"ref", T::KwRef},
        {"new", T::KwNew}, {"this", T::KwThis}, {"true", T::KwTrue}, {"false", T::KwFalse},
        {"none", T::KwNone}, {"null", T::KwNull}, {"is", T::KwIs}, {"try", T::KwTry},
        {"using", T::KwUsing}, {"extern", T::KwExtern}, {"private", T::KwPrivate},
        {"class", T::KwClass},
    };
    return kw;
}

const char* tokName(T t) {
    switch (t) {
        case T::Eof: return "end of file";
        case T::Ident: return "name";
        case T::IntLit: return "integer";
        case T::FloatLit: return "number";
        case T::StrLit: return "string";
        case T::CharLit: return "character";
        case T::InterpStr: return "string";
        case T::LParen: return "`(`";
        case T::RParen: return "`)`";
        case T::LBrace: return "`{`";
        case T::RBrace: return "`}`";
        case T::LBracket: return "`[`";
        case T::RBracket: return "`]`";
        case T::Comma: return "`,`";
        case T::Semi: return "`;`";
        case T::Colon: return "`:`";
        case T::Dot: return "`.`";
        case T::Assign: return "`=`";
        case T::Lt: return "`<`";
        case T::Gt: return "`>`";
        default: break;
    }
    for (auto& kv : keywords())
        if (kv.second == t) {
            static thread_local std::string s;
            s = "`" + kv.first + "`";
            return s.c_str();
        }
    return "symbol";
}

std::string tokDisplay(const Token& t) {
    if (t.type == T::Eof) return "end of file";
    return "`" + t.text + "`";
}

namespace {

struct Lexer {
    int file;
    const std::string& src;
    int pos, end;
    std::vector<Token> out;
    bool keepComments = false;

    Lexer(int f, int s, int e) : file(f), src(g_diag.file(f).text), pos(s), end(e < 0 ? (int)g_diag.file(f).text.size() : e) {}

    char peek(int o = 0) const { return pos + o < end ? src[pos + o] : '\0'; }

    Span span(int start, int len) const { return g_diag.makeSpan(file, start, len); }

    void add(T type, int start) {
        Token t;
        t.type = type;
        t.text = src.substr(start, pos - start);
        t.span = span(start, pos - start);
        out.push_back(std::move(t));
    }

    static void appendUtf8(std::string& s, uint32_t c) {
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
        else { s += (char)(0xF0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 0x3F)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
    }

    // Reads one (possibly escaped) character at pos and appends its UTF-8 form. Returns code point.
    uint32_t readChar(std::string& outStr) {
        unsigned char c = (unsigned char)peek();
        if (c == '\\') {
            int escStart = pos;
            pos++;
            char e = peek();
            pos++;
            uint32_t v = 0;
            switch (e) {
                case 'n': v = '\n'; break;
                case 't': v = '\t'; break;
                case 'r': v = '\r'; break;
                case '0': v = 0; break;
                case '\\': v = '\\'; break;
                case '"': v = '"'; break;
                case '\'': v = '\''; break;
                case '{': v = '{'; break;
                case '}': v = '}'; break;
                case 'x': {
                    for (int i = 0; i < 2 && isxdigit((unsigned char)peek()); i++) v = v * 16 + hexVal(src[pos++]);
                    outStr += (char)v;
                    return v;
                }
                case 'u': {
                    bool brace = peek() == '{';
                    if (brace) pos++;
                    int n = 0;
                    while (isxdigit((unsigned char)peek()) && n < 8) { v = v * 16 + hexVal(src[pos++]); n++; }
                    if (brace) { if (peek() == '}') pos++; }
                    break;
                }
                default:
                    g_diag.error(span(escStart, 2), std::string("unknown escape sequence `\\") + e + "`")
                        .help("use one of \\n \\t \\r \\0 \\\\ \\\" \\' \\xHH \\u{HHHH}");
                    v = (unsigned char)e;
            }
            appendUtf8(outStr, v);
            return v;
        }
        // decode UTF-8 so char literals get the right code point
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        uint32_t v = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        outStr += (char)c;
        pos++;
        for (int i = 1; i < n && pos < end; i++) {
            v = (v << 6) | ((unsigned char)src[pos] & 0x3F);
            outStr += src[pos++];
        }
        return v;
    }

    static int hexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
    }

    void lexNumber(int start) {
        bool isFloat = false;
        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
            pos += 2;
            while (isxdigit((unsigned char)peek()) || peek() == '_') pos++;
        } else if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
            pos += 2;
            while (peek() == '0' || peek() == '1' || peek() == '_') pos++;
        } else {
            while (isdigit((unsigned char)peek()) || peek() == '_') pos++;
            if (peek() == '.' && isdigit((unsigned char)peek(1))) {
                isFloat = true;
                pos++;
                while (isdigit((unsigned char)peek()) || peek() == '_') pos++;
            }
            if (peek() == 'e' || peek() == 'E') {
                int save = pos;
                pos++;
                if (peek() == '+' || peek() == '-') pos++;
                if (isdigit((unsigned char)peek())) {
                    isFloat = true;
                    while (isdigit((unsigned char)peek())) pos++;
                } else pos = save;
            }
        }
        std::string digits;
        for (int i = start; i < pos; i++) if (src[i] != '_') digits += src[i];
        Token t;
        t.span = span(start, pos - start);
        if (peek() == 'f' || peek() == 'F') { pos++; isFloat = true; t.isF32 = true; }
        else if (peek() == 'd' || peek() == 'D') { pos++; isFloat = true; }
        else if (!isFloat && (peek() == 'L' || peek() == 'l')) { pos++; t.isLong = true; }
        t.text = src.substr(start, pos - start);
        t.span = span(start, pos - start);
        if (isalnum((unsigned char)peek()) || peek() == '_') {
            int s2 = pos;
            while (isalnum((unsigned char)peek()) || peek() == '_') pos++;
            g_diag.error(span(s2, pos - s2), "unexpected letters after a number");
        }
        if (isFloat) {
            t.type = T::FloatLit;
            t.fval = strtod(digits.c_str(), nullptr);
        } else {
            t.type = T::IntLit;
            errno = 0;
            unsigned long long v;
            if (digits.size() > 2 && (digits[1] == 'b' || digits[1] == 'B')) v = strtoull(digits.c_str() + 2, nullptr, 2);
            else v = strtoull(digits.c_str(), nullptr, 0);
            if (errno == ERANGE || v > 9223372036854775807ULL)
                g_diag.error(t.span, "this number is too big for a `long`");
            t.ival = (int64_t)v;
        }
        out.push_back(std::move(t));
    }

    void lexString(int start, bool interp) {
        pos++;  // opening quote
        Token t;
        t.type = interp ? T::InterpStr : T::StrLit;
        InterpPart cur;
        while (true) {
            if (pos >= end || peek() == '\n') {
                g_diag.error(span(start, 1), "this string is never closed").help("add a closing `\"`");
                break;
            }
            if (peek() == '"') { pos++; break; }
            if (interp && peek() == '{') {
                if (peek(1) == '{') { cur.text += '{'; pos += 2; continue; }
                t.parts.push_back(cur);
                cur = InterpPart();
                pos++;
                InterpPart ex;
                ex.isExpr = true;
                ex.start = pos;
                int depth = 0;
                while (pos < end && peek() != '\n') {
                    if (peek() == '{') depth++;
                    else if (peek() == '}') { if (depth == 0) break; depth--; }
                    else if (peek() == '"') { pos++; while (pos < end && peek() != '"' && peek() != '\n') { if (peek() == '\\') pos++; pos++; } }
                    pos++;
                }
                ex.end = pos;
                if (peek() != '}') {
                    g_diag.error(span(ex.start - 1, 1), "this `{` in the string is never closed")
                        .help("close it with `}`, or write `{{` for a literal brace");
                    break;
                }
                pos++;
                t.parts.push_back(ex);
                continue;
            }
            if (interp && peek() == '}' && peek(1) == '}') { cur.text += '}'; pos += 2; continue; }
            readChar(interp ? cur.text : t.sval);
        }
        if (interp) t.parts.push_back(cur);
        t.text = src.substr(start, pos - start);
        t.span = span(start, pos - start);
        out.push_back(std::move(t));
    }

    void run() {
        while (true) {
            // whitespace and comments
            while (pos < end) {
                char c = peek();
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n') pos++;
                else if (c == '/' && peek(1) == '/') {
                    int s = pos;
                    while (pos < end && peek() != '\n' && peek() != '\r') pos++;
                    if (keepComments) add(T::Comment, s);
                } else if (c == '/' && peek(1) == '*') {
                    int s = pos;
                    pos += 2;
                    while (pos < end && !(peek() == '*' && peek(1) == '/')) pos++;
                    if (pos >= end) g_diag.error(span(s, 2), "this comment is never closed").help("add `*/` to end it");
                    else pos += 2;
                    if (keepComments) add(T::Comment, s);
                } else break;
            }
            if (pos >= end) break;
            int start = pos;
            char c = peek();
            if (isalpha((unsigned char)c) || c == '_') {
                while (isalnum((unsigned char)peek()) || peek() == '_') pos++;
                std::string word = src.substr(start, pos - start);
                auto it = keywords().find(word);
                add(it != keywords().end() ? it->second : T::Ident, start);
                continue;
            }
            if (isdigit((unsigned char)c)) { lexNumber(start); continue; }
            if (c == '"') { lexString(start, false); continue; }
            if (c == '$' && peek(1) == '"') { pos++; lexString(start, true); out.back().span = span(start, pos - start); continue; }
            if (c == '\'') {
                pos++;
                Token t;
                t.type = T::CharLit;
                if (peek() == '\'') {
                    g_diag.error(span(start, 2), "empty character literal");
                } else {
                    std::string tmp;
                    t.cval = readChar(tmp);
                }
                if (peek() != '\'') {
                    g_diag.error(span(start, pos - start), "a character literal holds exactly one character")
                        .help("use double quotes for text: \"...\"");
                    while (pos < end && peek() != '\'' && peek() != '\n') pos++;
                }
                if (peek() == '\'') pos++;
                t.text = src.substr(start, pos - start);
                t.span = span(start, pos - start);
                out.push_back(std::move(t));
                continue;
            }
            struct Sym { const char* s; T t; };
            static const Sym syms[] = {
                {"<<=", T::ShlEq}, {">>=", T::ShrEq}, {"??", T::QQ}, {"==", T::EqEq}, {"!=", T::NotEq},
                {"<=", T::Le}, {">=", T::Ge}, {"&&", T::AndAnd}, {"||", T::OrOr}, {"++", T::PlusPlus},
                {"--", T::MinusMinus}, {"+=", T::PlusEq}, {"-=", T::MinusEq}, {"*=", T::StarEq},
                {"/=", T::SlashEq}, {"%=", T::PercentEq}, {"&=", T::AmpEq}, {"|=", T::PipeEq},
                {"^=", T::CaretEq}, {"<<", T::Shl}, {">>", T::Shr}, {"=>", T::Arrow},
                {"(", T::LParen}, {")", T::RParen}, {"{", T::LBrace}, {"}", T::RBrace}, {"[", T::LBracket},
                {"]", T::RBracket}, {",", T::Comma}, {";", T::Semi}, {":", T::Colon}, {".", T::Dot},
                {"?", T::Question}, {"@", T::At}, {"+", T::Plus}, {"-", T::Minus}, {"*", T::Star},
                {"/", T::Slash}, {"%", T::Percent}, {"&", T::Amp}, {"|", T::Pipe}, {"^", T::Caret},
                {"~", T::Tilde}, {"!", T::Bang}, {"=", T::Assign}, {"<", T::Lt}, {">", T::Gt},
            };
            bool matched = false;
            for (auto& s : syms) {
                size_t n = strlen(s.s);
                if (pos + (int)n <= end && src.compare(pos, n, s.s) == 0) {
                    pos += (int)n;
                    add(s.t, start);
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
            // unknown character
            std::string tmp;
            readChar(tmp);
            g_diag.error(span(start, pos - start), "unexpected character `" + tmp + "`")
                .lbl("Axon doesn't use this character");
        }
        Token eof;
        eof.type = T::Eof;
        eof.span = span(end > 0 && end <= (int)src.size() ? end - (end > 0 ? 1 : 0) : 0, 1);
        if (end <= (int)src.size()) eof.span = span(end > 0 ? end - 1 : 0, 1);
        out.push_back(eof);
    }
};

}  // namespace

std::vector<Token> lexFile(int fileId, int start, int end, bool keepComments) {
    Lexer lx(fileId, start, end);
    lx.keepComments = keepComments;
    if (g_diag.file(fileId).text.empty()) {
        Token eof;
        eof.type = T::Eof;
        eof.span.file = fileId;
        eof.span.line = 1;
        eof.span.col = 1;
        return {eof};
    }
    lx.run();
    return std::move(lx.out);
}
