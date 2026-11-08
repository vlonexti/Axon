// emit_c.h - turns the checked program into one C file.
#pragma once
#include <string>

#include "checker.h"

struct EmitOptions {
    bool release = false;
    bool testMode = false;
    // Support mode (for the native backends): emit the runtime, type helpers, vtables, globals and
    // main, but no user function bodies; nothing is `static`, so the IR-generated object links to it.
    bool support = false;
};

std::string emitProgram(Program& prog, const EmitOptions& opt);
