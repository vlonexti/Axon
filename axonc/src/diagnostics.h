// diagnostics.h - source files, spans and friendly error messages.
#pragma once
#include <deque>
#include <string>
#include <utility>
#include <vector>

struct Span {
    int file = -1;
    int offset = 0;
    int len = 1;
    int line = 0;
    int col = 0;
};

struct SourceFile {
    std::string path;     // real path on disk (or "<std>/io.ax")
    std::string display;  // path shown in messages
    std::string text;
    std::vector<int> lineStarts;
};

struct Diag {
    enum Sev { Error, Warning, Note } sev = Error;
    Span span;
    std::string msg;
    std::string label;
    std::vector<std::string> helps;
    std::vector<std::pair<Span, std::string>> notes;

    Diag& lbl(std::string s) { label = std::move(s); return *this; }
    Diag& help(std::string s) { helps.push_back(std::move(s)); return *this; }
    Diag& note(Span sp, std::string s) { notes.push_back({sp, std::move(s)}); return *this; }
};

class Diagnostics {
public:
    int addFile(const std::string& path, const std::string& display, std::string text);
    const SourceFile& file(int id) const { return files[id]; }
    Span makeSpan(int file, int offset, int len) const;

    Diag& error(Span sp, std::string msg);
    Diag& warning(Span sp, std::string msg);

    void flush();  // print everything not printed yet
    int errorCount() const { return errors; }
    const std::deque<Diag>& all() const { return diags; }
    int fileCount() const { return (int)files.size(); }
    void reset() {
        files.clear();
        diags.clear();
        printed = 0;
        errors = 0;
    }
    bool quiet = false;  // the language server collects messages instead of printing them
    bool useColor = false;

private:
    void print(const Diag& d);
    void printExcerpt(Span sp, const std::string& label, bool primary);
    std::vector<SourceFile> files;
    std::deque<Diag> diags;
    size_t printed = 0;
    int errors = 0;
};

extern Diagnostics g_diag;

int editDistance(const std::string& a, const std::string& b);
// Returns the closest candidate (or "" if nothing is close enough).
std::string closestName(const std::string& name, const std::vector<std::string>& candidates);
