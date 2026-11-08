// x64.cpp - a simple, direct x86-64 code generator for Axon IR (Windows x64 calling convention).
//
// Every virtual register gets an 8-byte home in the stack frame; each IR instruction loads its
// operands into scratch registers (rax, rcx, rdx, r10, r11, xmm0, xmm1), computes, and stores the
// result. There is no register allocation: the point of this backend is compile speed and
// simplicity, while the C and LLVM backends produce optimized code.
//
// Frame (no frame pointer; everything is rsp-relative):
//   [rsp + 0 .. out)          outgoing arguments (at least the 32-byte shadow space)
//   [rsp + out .. size)       stack slots and register homes
//   [rsp + size]              return address
//   [rsp + size + 8 + 8*i]    incoming argument i (registers are spilled here on entry)
#include "x64.h"

#include <cstring>
#include <map>
#include <set>

namespace {

const char* argReg64[] = {"rcx", "rdx", "r8", "r9"};

std::string sizePtr(int bytes) {
    switch (bytes) {
        case 1: return "BYTE PTR";
        case 2: return "WORD PTR";
        case 4: return "DWORD PTR";
        default: return "QWORD PTR";
    }
}

// rax-family register name of a width
std::string rax(int bytes) { return bytes == 1 ? "al" : bytes == 2 ? "ax" : bytes == 4 ? "eax" : "rax"; }
std::string rcx(int bytes) { return bytes == 1 ? "cl" : bytes == 2 ? "cx" : bytes == 4 ? "ecx" : "rcx"; }

struct FuncGen {
    const IRFunc& f;
    std::string& o;
    int fi;
    std::vector<int> slotOff, regOff;
    int outArea = 32, frame = 0;

    FuncGen(const IRFunc& fn, std::string& out, int index) : f(fn), o(out), fi(index) {}

    void ln(const std::string& s) { o += "    " + s + "\n"; }
    std::string mem(int off, int bytes = 8) { return sizePtr(bytes) + " [rsp+" + std::to_string(off) + "]"; }
    std::string R(int r, int bytes = 8) { return mem(regOff[r], bytes); }
    std::string label(int b) { return "L" + std::to_string(fi) + "_" + std::to_string(b); }
    int paramOff(int i) { return frame + 8 + 8 * i; }

    void layout() {
        for (auto& b : f.blocks)
            for (auto& i : b.insts)
                if (i.op == Op::Call || i.op == Op::CallPtr) outArea = std::max(outArea, (int)i.args.size() * 8);
        outArea = (outArea + 15) / 16 * 16;
        int off = outArea;
        for (auto& s : f.slots) {
            int a = std::min(std::max(s.align, 1), 16);
            off = (off + a - 1) / a * a;
            slotOff.push_back(off);
            off += s.size;
        }
        off = (off + 7) / 8 * 8;
        for (size_t r = 0; r < f.regs.size(); r++) {
            regOff.push_back(off);
            off += 8;
        }
        // at entry rsp is 8 mod 16 (return address), so the frame is 8 mod 16 to align calls
        frame = (off + 15) / 16 * 16 + 8;
    }

    void copyBytes(const std::string& dst, const std::string& src, int64_t n) {
        // dst/src are registers holding addresses (r10, r11)
        if (n > 256) {
            ln("mov rcx, " + dst);
            ln("mov rdx, " + src);
            ln("mov r8, " + std::to_string(n));
            ln("call memcpy");
            return;
        }
        int64_t k = 0;
        for (int w : {8, 4, 2, 1})
            while (n - k >= w) {
                ln("mov " + rax(w) + ", " + sizePtr(w) + " [" + src + "+" + std::to_string(k) + "]");
                ln("mov " + sizePtr(w) + " [" + dst + "+" + std::to_string(k) + "], " + rax(w));
                k += w;
            }
    }

    void zeroBytes(const std::string& dst, int64_t n) {
        if (n > 256) {
            ln("mov rcx, " + dst);
            ln("xor edx, edx");
            ln("mov r8, " + std::to_string(n));
            ln("call memset");
            return;
        }
        ln("xor eax, eax");
        int64_t k = 0;
        for (int w : {8, 4, 2, 1})
            while (n - k >= w) {
                ln("mov " + sizePtr(w) + " [" + dst + "+" + std::to_string(k) + "], " + rax(w));
                k += w;
            }
    }

    void loadImm(int64_t v) {
        if (v >= INT32_MIN && v <= INT32_MAX) ln("mov rax, " + std::to_string(v));
        else ln("mov rax, " + std::to_string(v));
    }

    void ret(const Inst& i) {
        if (i.a >= 0) {
            if (i.type == IT::F64) ln("movsd xmm0, " + R(i.a));
            else if (i.type == IT::F32) ln("movss xmm0, " + R(i.a, 4));
            else ln("mov rax, " + R(i.a));
        }
        ln("add rsp, " + std::to_string(frame));
        ln("ret");
    }

    void call(const Inst& i) {
        // stack arguments first (they use rax), then register arguments
        for (size_t k = 4; k < i.args.size(); k++) {
            ln("mov rax, " + R(i.args[k]));
            ln("mov " + mem((int)k * 8) + ", rax");
        }
        if (i.op == Op::CallPtr) ln("mov r11, " + R(i.a));
        for (size_t k = 0; k < i.args.size() && k < 4; k++) {
            IT t = i.argTypes[k];
            if (t == IT::F64) ln("movsd xmm" + std::to_string(k) + ", " + R(i.args[k]));
            else if (t == IT::F32) ln("movss xmm" + std::to_string(k) + ", " + R(i.args[k], 4));
            else ln("mov " + std::string(argReg64[k]) + ", " + R(i.args[k]));
        }
        if (i.op == Op::CallPtr) ln("call r11");
        else ln("call " + i.sym);
        if (i.dst >= 0) {
            if (i.type == IT::F64) ln("movsd " + R(i.dst) + ", xmm0");
            else if (i.type == IT::F32) ln("movss " + R(i.dst, 4) + ", xmm0");
            else ln("mov " + R(i.dst) + ", rax");
        }
    }

    void inst(const Inst& i) {
        int w = itSize(i.type);
        switch (i.op) {
            case Op::IConst: loadImm(i.imm); ln("mov " + R(i.dst) + ", rax"); break;
            case Op::FConst: {
                if (i.type == IT::F32) {
                    float fv = (float)i.fimm;
                    uint32_t bits;
                    memcpy(&bits, &fv, 4);
                    ln("mov eax, " + std::to_string(bits));
                } else {
                    uint64_t bits;
                    memcpy(&bits, &i.fimm, 8);
                    ln("mov rax, " + std::to_string((int64_t)bits));
                }
                ln("mov " + R(i.dst) + ", rax");
                break;
            }
            case Op::Param: ln("mov rax, " + mem(paramOff((int)i.imm))); ln("mov " + R(i.dst) + ", rax"); break;
            case Op::SlotAddr: ln("lea rax, [rsp+" + std::to_string(slotOff[i.slot]) + "]"); ln("mov " + R(i.dst) + ", rax"); break;
            case Op::Global: ln("lea rax, " + i.sym); ln("mov " + R(i.dst) + ", rax"); break;
            case Op::Load:
                ln("mov rcx, " + R(i.a));
                if (w == 1) ln("movzx eax, BYTE PTR [rcx]");
                else if (w == 2) ln("movzx eax, WORD PTR [rcx]");
                else if (w == 4) ln("mov eax, DWORD PTR [rcx]");
                else ln("mov rax, QWORD PTR [rcx]");
                ln("mov " + R(i.dst) + ", rax");
                break;
            case Op::Store:
                ln("mov rcx, " + R(i.a));
                ln("mov rax, " + R(i.b));
                ln("mov " + sizePtr(w) + " [rcx], " + rax(w));
                break;
            case Op::Memcpy:
                ln("mov r10, " + R(i.a));
                ln("mov r11, " + R(i.b));
                copyBytes("r10", "r11", i.imm);
                break;
            case Op::Memset0:
                ln("mov r10, " + R(i.a));
                zeroBytes("r10", i.imm);
                break;
            case Op::PtrAdd: ln("mov rax, " + R(i.a)); ln("add rax, " + R(i.b)); ln("mov " + R(i.dst) + ", rax"); break;
            case Op::PtrAddI:
                ln("mov rax, " + R(i.a));
                if (i.imm >= INT32_MIN && i.imm <= INT32_MAX) ln("add rax, " + std::to_string(i.imm));
                else { ln("mov rcx, " + std::to_string(i.imm)); ln("add rax, rcx"); }
                ln("mov " + R(i.dst) + ", rax");
                break;
            case Op::Add: case Op::Sub: case Op::And: case Op::Or: case Op::Xor: {
                const char* m = i.op == Op::Add ? "add" : i.op == Op::Sub ? "sub" : i.op == Op::And ? "and" : i.op == Op::Or ? "or" : "xor";
                ln("mov rax, " + R(i.a));
                ln(std::string(m) + " rax, " + R(i.b));
                ln("mov " + R(i.dst) + ", rax");
                break;
            }
            case Op::Mul:
                ln("mov rax, " + R(i.a));
                ln("imul rax, " + R(i.b));
                ln("mov " + R(i.dst) + ", rax");
                break;
            case Op::SDiv: case Op::SRem: case Op::UDiv: case Op::URem: {
                bool sign = i.op == Op::SDiv || i.op == Op::SRem;
                int ww = w < 4 ? 4 : w;
                if (w < 4) {  // widen small operands first
                    ln(std::string(sign ? "movsx" : "movzx") + " eax, " + R(i.a, w));
                    ln(std::string(sign ? "movsx" : "movzx") + " ecx, " + R(i.b, w));
                } else {
                    ln("mov " + rax(ww) + ", " + R(i.a, ww));
                    ln("mov " + rcx(ww) + ", " + R(i.b, ww));
                }
                if (sign) ln(ww == 8 ? "cqo" : "cdq");
                else ln("xor edx, edx");
                ln(std::string(sign ? "idiv " : "div ") + rcx(ww));
                bool rem = i.op == Op::SRem || i.op == Op::URem;
                ln("mov " + R(i.dst) + ", " + (rem ? (ww == 8 ? "rdx" : "rdx") : "rax"));
                break;
            }
            case Op::Shl: case Op::AShr: case Op::LShr: {
                const char* m = i.op == Op::Shl ? "shl" : i.op == Op::AShr ? "sar" : "shr";
                ln("mov rax, " + R(i.a));
                ln("mov rcx, " + R(i.b));
                ln(std::string(m) + " " + rax(w) + ", cl");
                ln("mov " + R(i.dst) + ", rax");
                break;
            }
            case Op::Neg: case Op::Not:
                ln("mov rax, " + R(i.a));
                ln(std::string(i.op == Op::Neg ? "neg " : "not ") + rax(w));
                ln("mov " + R(i.dst) + ", rax");
                break;
            case Op::FAdd: case Op::FSub: case Op::FMul: case Op::FDiv: {
                const char* base = i.op == Op::FAdd ? "add" : i.op == Op::FSub ? "sub" : i.op == Op::FMul ? "mul" : "div";
                bool d = i.type == IT::F64;
                ln(std::string(d ? "movsd" : "movss") + " xmm0, " + R(i.a, d ? 8 : 4));
                ln(std::string(base) + (d ? "sd" : "ss") + " xmm0, " + R(i.b, d ? 8 : 4));
                ln(std::string(d ? "movsd " : "movss ") + R(i.dst, d ? 8 : 4) + ", xmm0");
                break;
            }
            case Op::FNeg:
                ln("mov rax, " + R(i.a));
                ln(i.type == IT::F64 ? "btc rax, 63" : "btc eax, 31");
                ln("mov " + R(i.dst) + ", rax");
                break;
            case Op::ICmp: {
                int ow = 8;
                // the operand width is the type of the compared registers
                ow = itSize(f.regs[i.a]);
                if (ow == 0) ow = 8;
                ln("mov " + rax(ow) + ", " + R(i.a, ow));
                ln("cmp " + rax(ow) + ", " + R(i.b, ow));
                static const char* cc[] = {"e", "ne", "l", "le", "g", "ge", "b", "be", "a", "ae"};
                ln(std::string("set") + cc[(int)i.cond] + " al");
                ln("movzx eax, al");
                ln("mov " + R(i.dst) + ", rax");
                break;
            }
            case Op::FCmp: {
                bool d = f.regs[i.a] == IT::F64;
                std::string mov = d ? "movsd" : "movss", ucom = d ? "ucomisd" : "ucomiss";
                int bw = d ? 8 : 4;
                switch (i.cond) {
                    case Cond::Lt: case Cond::Le:  // b > a, b >= a (false when unordered)
                        ln(mov + " xmm0, " + R(i.b, bw));
                        ln(ucom + " xmm0, " + R(i.a, bw));
                        ln(i.cond == Cond::Lt ? "seta al" : "setae al");
                        break;
                    case Cond::Gt: case Cond::Ge:
                        ln(mov + " xmm0, " + R(i.a, bw));
                        ln(ucom + " xmm0, " + R(i.b, bw));
                        ln(i.cond == Cond::Gt ? "seta al" : "setae al");
                        break;
                    case Cond::Eq:
                        ln(mov + " xmm0, " + R(i.a, bw));
                        ln(ucom + " xmm0, " + R(i.b, bw));
                        ln("sete al");
                        ln("setnp cl");
                        ln("and al, cl");
                        break;
                    default:
                        ln(mov + " xmm0, " + R(i.a, bw));
                        ln(ucom + " xmm0, " + R(i.b, bw));
                        ln("setne al");
                        ln("setp cl");
                        ln("or al, cl");
                        break;
                }
                ln("movzx eax, al");
                ln("mov " + R(i.dst) + ", rax");
                break;
            }
            case Op::SExt: {
                int sw = itSize(f.regs[i.a]);
                if (sw == 4) ln("movsxd rax, " + R(i.a, 4));
                else if (sw < 4) ln("movsx rax, " + R(i.a, sw));
                else ln("mov rax, " + R(i.a));
                ln("mov " + R(i.dst) + ", rax");
                break;
            }
            case Op::ZExt: {
                int sw = itSize(f.regs[i.a]);
                if (sw == 4) ln("mov eax, " + R(i.a, 4));
                else if (sw < 4) ln("movzx eax, " + R(i.a, sw));
                else ln("mov rax, " + R(i.a));
                ln("mov " + R(i.dst) + ", rax");
                break;
            }
            case Op::Trunc: ln("mov rax, " + R(i.a)); ln("mov " + R(i.dst) + ", rax"); break;
            case Op::SIToFP: case Op::UIToFP: {
                IT st = f.regs[i.a];
                int sw = itSize(st);
                if (sw == 8) ln("mov rax, " + R(i.a));
                else if (i.op == Op::SIToFP) ln((sw == 4 ? "movsxd rax, " : "movsx rax, ") + R(i.a, sw));
                else ln((sw == 4 ? std::string("mov eax, ") : std::string("movzx eax, ")) + R(i.a, sw));
                if (i.type == IT::F64) { ln("cvtsi2sd xmm0, rax"); ln("movsd " + R(i.dst) + ", xmm0"); }
                else { ln("cvtsi2ss xmm0, rax"); ln("movss " + R(i.dst, 4) + ", xmm0"); }
                break;
            }
            case Op::FPToSI: {
                bool d = f.regs[i.a] == IT::F64;
                ln(std::string(d ? "cvttsd2si rax, " : "cvttss2si rax, ") + R(i.a, d ? 8 : 4));
                ln("mov " + R(i.dst) + ", rax");
                break;
            }
            case Op::FPExt: ln("cvtss2sd xmm0, " + R(i.a, 4)); ln("movsd " + R(i.dst) + ", xmm0"); break;
            case Op::FPTrunc: ln("cvtsd2ss xmm0, " + R(i.a)); ln("movss " + R(i.dst, 4) + ", xmm0"); break;
            case Op::Call: case Op::CallPtr: call(i); break;
            case Op::Br: ln("jmp " + label(i.t1)); break;
            case Op::CondBr:
                ln("mov al, " + R(i.a, 1));
                ln("test al, al");
                ln("jnz " + label(i.t1));
                ln("jmp " + label(i.t2));
                break;
            case Op::Ret: ret(i); break;
        }
    }

    void run() {
        layout();
        o += f.name + " PROC FRAME\n";
        if (frame >= 4096) {
            // touch every page of a big frame (Windows grows the stack one guard page at a time)
            ln("mov eax, " + std::to_string(frame));
            ln("call __chkstk");
            ln("sub rsp, rax");
        } else ln("sub rsp, " + std::to_string(frame));
        ln(".allocstack " + std::to_string(frame));
        ln(".endprolog");
        // spill register arguments to their home slots in the caller's shadow space
        for (size_t k = 0; k < f.params.size() && k < 4; k++) {
            if (f.params[k] == IT::F64) ln("movsd " + mem(paramOff((int)k)) + ", xmm" + std::to_string(k));
            else if (f.params[k] == IT::F32) ln("movss " + mem(paramOff((int)k), 4) + ", xmm" + std::to_string(k));
            else ln("mov " + mem(paramOff((int)k)) + ", " + argReg64[k]);
        }
        for (size_t b = 0; b < f.blocks.size(); b++) {
            o += label((int)b) + ":\n";
            for (auto& i : f.blocks[b].insts) inst(i);
        }
        o += f.name + " ENDP\n\n";
    }
};

}  // namespace

std::string emitX64(const IRModule& m) {
    std::string o = "; Generated by axonc (x64 backend). Assemble with ml64.\nOPTION CASEMAP:NONE\n\n";
    std::set<std::string> dataNames;
    for (auto& d : m.data) dataNames.insert(d.name);
    // functions vs data among the external symbols
    std::set<std::string> called;
    for (auto& f : m.funcs)
        for (auto& b : f.blocks)
            for (auto& i : b.insts)
                if (i.op == Op::Call) called.insert(i.sym);
    for (auto& e : m.externs) o += "EXTERN " + e + (called.count(e) ? ":PROC\n" : ":BYTE\n");
    o += "EXTERN memcpy:PROC\nEXTERN memset:PROC\nEXTERN __chkstk:PROC\n\n";
    for (auto& f : m.funcs) o += "PUBLIC " + f.name + "\n";
    o += "\n.const\n";
    for (auto& d : m.data) {
        std::string bytes = d.bytes + std::string(1, '\0');
        o += d.name + " DB ";
        for (size_t i = 0; i < bytes.size(); i++) {
            if (i && i % 24 == 0) o += "\n    DB ";
            else if (i) o += ",";
            o += std::to_string((unsigned char)bytes[i]);
        }
        o += "\n";
    }
    o += "\n.code\n";
    for (size_t i = 0; i < m.funcs.size(); i++) {
        FuncGen g(m.funcs[i], o, (int)i);
        g.run();
    }
    o += "END\n";
    return o;
}
