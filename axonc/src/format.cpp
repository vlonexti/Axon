// format.cpp - a token-based formatter.
//
// It keeps the author's line breaks (it never joins or splits lines), and decides:
//  * indentation: 4 spaces per open `{`, one extra level inside unclosed `(`/`[`, and
//    `case` bodies one level deeper than their `case` label;
//  * spacing between tokens on a line;
//  * blank lines: at most one in a row, none right after `{` or before `}`.
// The result is re-lexed and compared with the original tokens, so formatting can never change
// what a program means.
#include "format.h"

#include <vector>

#include "diagnostics.h"
#include "lexer.h"

namespace {

bool isOperand(const Token& t) {
    switch (t.type) {
        case T::Ident: case T::IntLit: case T::FloatLit: case T::StrLit: case T::CharLit: case T::InterpStr:
        case T::RParen: case T::RBracket: case T::KwThis: case T::KwTrue: case T::KwFalse: case T::KwNone:
        case T::PlusPlus: case T::MinusMinus: case T::Gt:
            return true;
        default:
            return false;
    }
}

bool isBinaryOp(T t) {
    switch (t) {
        case T::Assign: case T::EqEq: case T::NotEq: case T::Le: case T::Ge: case T::AndAnd: case T::OrOr:
        case T::QQ: case T::Star: case T::Slash: case T::Percent: case T::Amp: case T::Pipe: case T::Caret:
        case T::Shl: case T::PlusEq: case T::MinusEq: case T::StarEq: case T::SlashEq: case T::PercentEq:
        case T::AmpEq: case T::PipeEq: case T::CaretEq: case T::ShlEq: case T::ShrEq: case T::Arrow:
            return true;
        default:
            return false;
    }
}

bool isControlKw(T t) {
    return t == T::KwIf || t == T::KwWhile || t == T::KwFor || t == T::KwForeach || t == T::KwSwitch;
}

struct Fmt {
    const std::string& src;
    std::vector<Token> toks;

    explicit Fmt(const std::string& s) : src(s) {}

    bool adjacent(const Token& a, const Token& b) const { return a.span.offset + a.span.len == b.span.offset; }

    // `prevPrev` helps decide whether `-` / `+` is unary.
    bool spaceBetween(const Token& pp, const Token& p, const Token& c, bool caseLine, bool havePP) const {
        T pt = p.type, ct = c.type;
        if (ct == T::Comment) return true;
        if (ct == T::Comma || ct == T::Semi || ct == T::RParen || ct == T::RBracket || ct == T::Dot) return false;
        if (pt == T::LParen || pt == T::LBracket || pt == T::Dot || pt == T::At || pt == T::Tilde || pt == T::Bang) return false;
        // unary minus / plus: no space after
        if ((pt == T::Minus || pt == T::Plus) && (!havePP || !isOperand(pp))) return false;
        // ++x / x++
        if (ct == T::PlusPlus || ct == T::MinusMinus) return !isOperand(p);
        if (pt == T::PlusPlus || pt == T::MinusMinus) return !(c.type == T::Ident || c.type == T::KwThis) || isOperand(pp);
        // generics and optional types keep the author's spacing: List<int>, Player?
        if (ct == T::Lt || ct == T::Gt || ct == T::Shr || ct == T::Question || pt == T::Lt || pt == T::Question) return !adjacent(p, c);
        if (pt == T::Gt || pt == T::Shr) {
            if (ct == T::LParen || ct == T::LBracket || ct == T::Dot) return false;
            return true;
        }
        if (ct == T::Colon) return !caseLine;
        if (ct == T::LParen) {
            if (isControlKw(pt) || pt == T::KwReturn || pt == T::KwIn || pt == T::KwIs || pt == T::KwTry || pt == T::KwRef ||
                pt == T::KwCase || pt == T::Comma || pt == T::Colon || pt == T::Semi || pt == T::Assign || isBinaryOp(pt) ||
                pt == T::Plus || pt == T::Minus || pt == T::Lt || pt == T::Gt || pt == T::Question)
                return true;
            if (pt == T::RParen) return !adjacent(p, c);
            return false;  // a call: f(x)
        }
        if (ct == T::LBracket) return !(pt == T::Ident || pt == T::RParen || pt == T::RBracket || pt == T::Gt || pt == T::KwInt ||
                                        pt == T::KwLong || pt == T::KwByte || pt == T::KwFloat || pt == T::KwDouble || pt == T::KwBool ||
                                        pt == T::KwChar || pt == T::KwString || pt == T::KwThis || pt == T::StrLit);
        if (pt == T::LBrace && ct == T::RBrace) return false;
        if (pt == T::RParen) {
            // `(int)x` keeps the author's choice; anything else gets a space
            if (ct == T::Ident || ct == T::IntLit || ct == T::FloatLit || ct == T::LParen) return !adjacent(p, c);
            return true;
        }
        return true;
    }

    bool run(std::string& out, std::string& err) {
        bool crlf = src.find("\r\n") != std::string::npos;
        const char* nl = crlf ? "\r\n" : "\n";
        // group tokens by the line they start on
        std::vector<std::vector<Token>> lines;
        std::vector<int> lineNo;
        for (auto& t : toks) {
            if (t.type == T::Eof) break;
            if (lines.empty() || t.span.line != lineNo.back()) {
                // a token that starts on the line where a multi-line block comment ended stays with it
                if (!lines.empty()) {
                    const Token& last = lines.back().back();
                    int lastEndLine = last.span.line;
                    for (char ch : last.text) lastEndLine += ch == '\n';
                    if (last.type == T::Comment && lastEndLine == t.span.line && lastEndLine != last.span.line) {
                        lines.back().push_back(t);
                        continue;
                    }
                }
                lines.push_back({});
                lineNo.push_back(t.span.line);
            }
            lines.back().push_back(t);
        }
        struct Open {
            char kind;  // '{' block, 's' switch block, '(' paren/bracket
        };
        std::vector<Open> stack;
        bool nextBraceIsSwitch = false;
        int prevLine = -1;
        bool prevEndedWithOpen = false;
        for (size_t li = 0; li < lines.size(); li++) {
            auto& ln = lines[li];
            // blank lines
            if (prevLine >= 0) {
                int gap = lineNo[li] - prevLine - 1;
                // the previous token line may have been a multi-line comment
                const Token& last = lines[li - 1].back();
                for (char ch : last.text) gap -= ch == '\n';
                bool closing = ln[0].type == T::RBrace;
                if (gap > 0 && !prevEndedWithOpen && !closing) out += nl;
            }
            // indentation: closers at the start of the line close before indenting
            std::vector<Open> tmp = stack;
            size_t k = 0;
            while (k < ln.size() && (ln[k].type == T::RBrace || ln[k].type == T::RParen || ln[k].type == T::RBracket)) {
                if (!tmp.empty()) tmp.pop_back();
                k++;
            }
            int indent = 0;
            for (auto& o : tmp) indent += 1;
            bool caseLine = ln[0].type == T::KwCase || ln[0].type == T::KwDefault;
            if (!tmp.empty() && tmp.back().kind == 's' && !caseLine && k == 0) indent += 1;
            // a switch's closing brace lines up with `switch`
            out += std::string(indent * 4, ' ');
            for (size_t i = 0; i < ln.size(); i++) {
                const Token& t = ln[i];
                if (i > 0) {
                    const Token& p = ln[i - 1];
                    bool sp;
                    if (t.type == T::Comment) {
                        int gap = t.span.offset - (p.span.offset + p.span.len);
                        sp = true;
                        out += std::string(gap >= 1 && gap < 40 ? gap : 1, ' ');
                        out += t.text;
                        continue;
                    }
                    sp = spaceBetween(i > 1 ? ln[i - 2] : p, p, t, caseLine, i > 1);
                    if (sp) out += ' ';
                }
                out += t.text;
                // track nesting
                switch (t.type) {
                    case T::KwSwitch: nextBraceIsSwitch = true; break;
                    case T::LBrace:
                        stack.push_back({nextBraceIsSwitch ? 's' : '{'});
                        nextBraceIsSwitch = false;
                        break;
                    case T::LParen: case T::LBracket: stack.push_back({'('}); break;
                    case T::RBrace: case T::RParen: case T::RBracket:
                        if (!stack.empty()) stack.pop_back();
                        break;
                    default: break;
                }
            }
            out += nl;
            prevLine = lineNo[li];
            prevEndedWithOpen = ln.back().type == T::LBrace;
        }
        (void)err;
        return true;
    }
};

}  // namespace

bool formatSource(int fileId, std::string& out, std::string& error) {
    int errorsBefore = g_diag.errorCount();
    auto toks = lexFile(fileId, 0, -1, true);
    if (g_diag.errorCount() > errorsBefore) {
        error = "the file has syntax errors; fix them first";
        return false;
    }
    Fmt f(g_diag.file(fileId).text);
    f.toks = toks;
    std::string result;
    if (!f.run(result, error)) return false;
    // safety: the formatted text must have exactly the same tokens
    int check = g_diag.addFile(g_diag.file(fileId).path + " (formatted)", "formatted", result);
    auto again = lexFile(check, 0, -1, true);
    size_t n = 0, m = 0;
    while (n < toks.size() && m < again.size()) {
        if (toks[n].type != again[m].type || (toks[n].type != T::Comment && toks[n].text != again[m].text)) {
            error = "internal formatter error near line " + std::to_string(toks[n].span.line) + "; the file was left unchanged";
            return false;
        }
        n++;
        m++;
    }
    if (n != toks.size() || m != again.size()) {
        error = "internal formatter error; the file was left unchanged";
        return false;
    }
    out = result;
    return true;
}
