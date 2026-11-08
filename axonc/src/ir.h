// ir.h - Axon IR: a small low-level intermediate representation shared by the native backends.
//
// * Every function has numbered virtual registers (each assigned once), stack slots and blocks.
// * Values have machine types (I8..I64, F32, F64, Ptr). Aggregates (structs, strings, lists...)
//   live in memory and are handled through pointers, Load/Store and Memcpy.
// * Calls are already lowered to the Windows x64 calling convention: small structs travel as
//   integers, bigger ones by pointer, and big return values through a hidden first pointer.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

enum class IT : uint8_t { Void, I8, I16, I32, I64, F32, F64, Ptr };

inline int itSize(IT t) {
    switch (t) {
        case IT::I8: return 1;
        case IT::I16: return 2;
        case IT::I32: case IT::F32: return 4;
        case IT::Void: return 0;
        default: return 8;
    }
}
inline bool itFloat(IT t) { return t == IT::F32 || t == IT::F64; }

enum class Op : uint8_t {
    IConst,   // dst = imm
    FConst,   // dst = fimm
    Param,    // dst = incoming parameter #imm
    SlotAddr, // dst = address of stack slot #slot
    Global,   // dst = address of data/function symbol `sym`
    Load,     // dst = *(type*)a
    Store,    // *(type*)a = b
    Memcpy,   // memcpy(a, b, imm)
    Memset0,  // memset(a, 0, imm)
    PtrAdd,   // dst = a + b (b is I64)
    PtrAddI,  // dst = a + imm
    Add, Sub, Mul, SDiv, SRem, UDiv, URem, And, Or, Xor, Shl, AShr, LShr,
    Neg, Not,  // Not: bitwise
    FAdd, FSub, FMul, FDiv, FNeg,
    ICmp,     // dst(I8) = a <cond> b (signed unless cond is unsigned)
    FCmp,
    SExt, ZExt, Trunc, SIToFP, UIToFP, FPToSI, FPExt, FPTrunc,
    Call,     // dst = sym(args...)        (dst = -1 for void)
    CallPtr,  // dst = (*a)(args...)
    Br,       // goto t1
    CondBr,   // if (a != 0) goto t1 else goto t2   (a is I8)
    Ret,      // return a  (a = -1 for void)
};

enum class Cond : uint8_t { Eq, Ne, Lt, Le, Gt, Ge, ULt, ULe, UGt, UGe };

struct Inst {
    Op op;
    IT type = IT::Void;  // type of dst (or of the value stored/loaded)
    int dst = -1;
    int a = -1, b = -1;
    int64_t imm = 0;
    double fimm = 0;
    Cond cond = Cond::Eq;
    int slot = -1;
    int t1 = -1, t2 = -1;
    std::string sym;
    std::vector<int> args;
    std::vector<IT> argTypes;
};

struct IRBlock {
    std::vector<Inst> insts;
};

struct IRSlot {
    int size;
    int align;
};

struct IRFunc {
    std::string name;
    std::vector<IT> params;  // lowered parameter types (hidden return pointer first if any)
    IT ret = IT::Void;
    std::vector<IRSlot> slots;
    std::vector<IRBlock> blocks;
    std::vector<IT> regs;  // type of each virtual register
};

struct IRData {
    std::string name;
    std::string bytes;  // raw bytes (NUL added by the backend)
};

struct IRModule {
    std::vector<IRFunc> funcs;
    std::vector<IRData> data;
    std::vector<std::string> externs;  // functions/data defined elsewhere (the C support object)
};

std::string irToText(const IRModule& m);  // readable dump, for `axon emit-ir`
