// runtime.h - C source of the Axon runtime and the embedded standard library.
#pragma once

extern const char* const AXON_RT_CORE_1;
extern const char* const AXON_RT_CORE_2;
extern const char* const AXON_RT_AFTER_TYPES;

// Axon source of the std modules (std.io, std.math, ...). Returns nullptr if unknown.
const char* stdModuleSource(const char* name);
