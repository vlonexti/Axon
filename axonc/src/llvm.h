// llvm.h - Axon IR -> LLVM IR (textual .ll), compiled with clang/llc. The optimizing release backend.
#pragma once
#include <string>

#include "ir.h"

std::string emitLLVM(const IRModule& m);
std::string findLLVMClang();  // clang on PATH, or tools/llvm/*/bin/clang next to axon; "" if none
