// irgen.h - lowers checked Axon code to Axon IR (for the native backends).
#pragma once
#include "checker.h"
#include "ir.h"

// Builds IR for every user function and the global initializer. Runtime functions and the
// per-type helpers are not in the IR: they come from the C support object (emit_c in support mode).
IRModule generateIR(Program& prog);

// Memory layout of a type, following the C rules the support object is compiled with.
int typeSize(Type* t);
int typeAlign(Type* t);
