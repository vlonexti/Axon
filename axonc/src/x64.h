// x64.h - Axon IR -> x86-64 assembly (MASM syntax, for ml64.exe). The fast-to-compile debug backend.
#pragma once
#include <string>

#include "ir.h"

std::string emitX64(const IRModule& m);
