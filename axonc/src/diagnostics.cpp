// diagnostics.cpp - prints Rust-style error messages with source excerpts.
#include "diagnostics.h"

#include <algorithm>
#include <cstdio>

Diagnostics g_diag;

int Diagnostics::addFile(const std::string& path, const std::string& display, std::string text) {
    SourceFile f;
    f.path = path;
    f.display = display;
    f.text = std::move(text);
    f.lineStarts.push_back(0);
    for (size_t i = 0; i < f.text.size(); i++)
        if (f.text[i] == '\n') f.lineStarts.push_back((int)i + 1);
    files.push_back(std::move(f));
    return (int)files.size() - 1;
}

Span Diagnostics::makeSpan(int fileId, int offset, int len) const {
    Span s;
    s.file = fileId;
    s.offset = offset;
    s.len = len < 1 ? 1 : len;
    const auto& ls = files[fileId].lineStarts;
    auto it = std::upper_bound(ls.begin(), ls.end(), offset);
    int lineIdx = (int)(it - ls.begin()) - 1;
    if (lineIdx < 0) lineIdx = 0;
    s.line = lineIdx + 1;
    s.col = offset - ls[lineIdx] + 1;
    return s;
}

Diag& Diagnostics::error(Span sp, std::string msg) {
    Diag d;
    d.sev = Diag::Error;
    d.span = sp;
    d.msg = std::move(msg);
    diags.push_back(std::move(d));
    errors++;
    return diags.back();
}

Diag& Diagnostics::warning(Span sp, std::string msg) {
    Diag d;
    d.sev = Diag::Warning;
    d.span = sp;
    d.msg = std::move(msg);
    diags.push_back(std::move(d));
    return diags.back();
}

static const char* C_RED = "\x1b[1;31m";
static const char* C_YEL = "\x1b[1;33m";
static const char* C_BLUE = "\x1b[1;34m";
static const char* C_CYAN = "\x1b[1;36m";
static const char* C_BOLD = "\x1b[1m";
static const char* C_OFF = "\x1b[0m";

void Diagnostics::printExcerpt(Span sp, const std::string& label, bool primary) {
    if (sp.file < 0) return;
    const SourceFile& f = files[sp.file];
    const char* blue = useColor ? C_BLUE : "";
    const char* off = useColor ? C_OFF : "";
    const char* mark = useColor ? (primary ? C_RED : C_CYAN) : "";
    std::string lineNo = std::to_string(sp.line);
    std::string pad(lineNo.size(), ' ');
    fprintf(stderr, "%s%s-->%s %s:%d:%d\n", pad.c_str(), blue, off, f.display.c_str(), sp.line, sp.col);
    int start = f.lineStarts[sp.line - 1];
    int end = start;
    while (end < (int)f.text.size() && f.text[end] != '\n' && f.text[end] != '\r') end++;
    std::string raw = f.text.substr(start, end - start);
    // expand tabs so the caret lines up
    std::string text;
    int caretCol = 0, visCol = 0;
    for (int i = 0; i < (int)raw.size(); i++) {
        if (i == sp.col - 1) caretCol = visCol;
        if (raw[i] == '\t') { text += "    "; visCol += 4; }
        else { text += raw[i]; visCol++; }
    }
    if (sp.col - 1 >= (int)raw.size()) caretCol = visCol + (sp.col - 1 - (int)raw.size());
    int len = sp.len;
    if (sp.col - 1 + len > (int)raw.size() + 1) len = std::max(1, (int)raw.size() - (sp.col - 1));
    fprintf(stderr, "%s%s |%s\n", pad.c_str(), blue, off);
    fprintf(stderr, "%s%s |%s %s\n", lineNo.c_str(), blue, off, text.c_str());
    fprintf(stderr, "%s%s |%s %s%s%s", pad.c_str(), blue, off, std::string(caretCol, ' ').c_str(), mark,
            std::string(len, primary ? '^' : '-').c_str());
    if (!label.empty()) fprintf(stderr, " %s", label.c_str());
    fprintf(stderr, "%s\n", off);
}

void Diagnostics::print(const Diag& d) {
    const char* sevColor = useColor ? (d.sev == Diag::Error ? C_RED : C_YEL) : "";
    const char* bold = useColor ? C_BOLD : "";
    const char* off = useColor ? C_OFF : "";
    const char* blue = useColor ? C_BLUE : "";
    const char* sev = d.sev == Diag::Error ? "error" : d.sev == Diag::Warning ? "warning" : "note";
    fprintf(stderr, "%s%s%s%s: %s%s\n", sevColor, sev, off, bold, d.msg.c_str(), off);
    printExcerpt(d.span, d.label, true);
    std::string pad = d.span.file >= 0 ? std::string(std::to_string(d.span.line).size(), ' ') : "";
    for (auto& n : d.notes) {
        if (n.first.file >= 0) {
            fprintf(stderr, "%s%s =%s note: %s\n", pad.c_str(), blue, off, n.second.c_str());
            printExcerpt(n.first, "", false);
        } else {
            fprintf(stderr, "%s%s =%s note: %s\n", pad.c_str(), blue, off, n.second.c_str());
        }
    }
    for (auto& h : d.helps) fprintf(stderr, "%s%s =%s %shelp%s: %s\n", pad.c_str(), blue, off, bold, off, h.c_str());
    fprintf(stderr, "\n");
}

void Diagnostics::flush() {
    if (quiet) return;
    for (; printed < diags.size(); printed++) {
        if (printed >= 60) {
            if (printed == 60) fprintf(stderr, "(stopping after 60 messages)\n");
            continue;
        }
        print(diags[printed]);
    }
    fflush(stderr);
}

int editDistance(const std::string& a, const std::string& b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = (int)j;
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= b.size(); j++) {
            int cost = tolower(a[i - 1]) == tolower(b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

std::string closestName(const std::string& name, const std::vector<std::string>& candidates) {
    std::string best;
    int bestD = 1 << 30;
    int limit = name.size() <= 3 ? 1 : name.size() <= 6 ? 2 : 3;
    for (auto& c : candidates) {
        if (c == name) continue;
        int d = editDistance(name, c);
        if (d < bestD) { bestD = d; best = c; }
    }
    return bestD <= limit ? best : "";
}
