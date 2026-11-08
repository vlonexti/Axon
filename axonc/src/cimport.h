// cimport.h - reads a C header with libclang and writes Axon bindings for it (Phase 5 "stage 2" C import).
#pragma once
#include <string>
#include <vector>

struct CImportResult {
    bool ok = false;
    std::string axon;     // the generated `extern "header" { ... }` source
    std::string error;    // why it failed (e.g. libclang not found)
    int functions = 0, structs = 0, constants = 0, skipped = 0;
};

// `header` is what follows `using c` (e.g. "raylib.h"); it is found through includeDirs.
CImportResult importCHeader(const std::string& header, const std::vector<std::string>& includeDirs,
                            const std::vector<std::string>& defines);

bool libclangAvailable();
