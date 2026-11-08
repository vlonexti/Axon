// llvm.cpp - emits LLVM IR text from Axon IR.
//
// Axon IR registers become LLVM SSA values. Stack slots become allocas in the entry block and
// slot/global addresses are used directly (no instruction), so they dominate every use. Calls are
// already lowered to the Windows x64 convention by irgen, which LLVM reproduces exactly for the
// x86_64-pc-windows-msvc target.
#include "llvm.h"

#include <cstring>
#include <filesystem>
#include <map>
#include <set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

const char* ty(IT t) {
    switch (t) {
        case IT::Void: return "void";
        case IT::I8: return "i8";
        case IT::I16: return "i16";
        case IT::I32: return "i32";
        case IT::I64: return "i64";
        case IT::F32: return "float";
        case IT::F64: return "double";
        case IT::Ptr: return "ptr";
    }
    return "i64";
}

std::string hexDouble(double d) {
    uint64_t bits;
    memcpy(&bits, &d, 8);
    char b[32];
    snprintf(b, sizeof b, "0x%016llX", (unsigned long long)bits);
    return b;
}

struct FuncEmit {
    const IRFunc& f;
    std::string& o;
    std::map<int, std::string> alias;  // register -> slot or global it names
    int tmp = 0;

    FuncEmit(const IRFunc& fn, std::string& out) : f(fn), o(out) {}

    std::string v(int r) {
        auto it = alias.find(r);
        if (it != alias.end()) return it->second;
        return "%r" + std::to_string(r);
    }
    std::string t() { return "%t" + std::to_string(tmp++); }
    void ln(const std::string& s) { o += "  " + s + "\n"; }

    void inst(const Inst& i) {
        std::string d = i.dst >= 0 ? "%r" + std::to_string(i.dst) : "";
        const char* T = ty(i.type);
        switch (i.op) {
            case Op::IConst:
                if (i.type == IT::Ptr) ln(d + " = inttoptr i64 " + std::to_string(i.imm) + " to ptr");
                else ln(d + " = freeze " + T + " " + std::to_string(i.imm));
                break;
            case Op::FConst: ln(d + " = freeze " + T + " " + hexDouble(i.type == IT::F32 ? (double)(float)i.fimm : i.fimm)); break;
            case Op::Param: ln(d + " = freeze " + T + " %p" + std::to_string(i.imm)); break;
            case Op::SlotAddr: case Op::Global: break;  // aliased
            case Op::Load: ln(d + " = load " + T + ", ptr " + v(i.a) + ", align 1"); break;
            case Op::Store: ln("store " + std::string(T) + " " + v(i.b) + ", ptr " + v(i.a) + ", align 1"); break;
            case Op::Memcpy: ln("call void @llvm.memcpy.p0.p0.i64(ptr " + v(i.a) + ", ptr " + v(i.b) + ", i64 " + std::to_string(i.imm) + ", i1 false)"); break;
            case Op::Memset0: ln("call void @llvm.memset.p0.i64(ptr " + v(i.a) + ", i8 0, i64 " + std::to_string(i.imm) + ", i1 false)"); break;
            case Op::PtrAdd: ln(d + " = getelementptr i8, ptr " + v(i.a) + ", i64 " + v(i.b)); break;
            case Op::PtrAddI: ln(d + " = getelementptr i8, ptr " + v(i.a) + ", i64 " + std::to_string(i.imm)); break;
            case Op::Add: case Op::Sub: case Op::Mul: case Op::SDiv: case Op::SRem: case Op::UDiv: case Op::URem:
            case Op::And: case Op::Or: case Op::Xor: case Op::Shl: case Op::AShr: case Op::LShr: {
                static const std::map<Op, const char*> m = {{Op::Add, "add"}, {Op::Sub, "sub"}, {Op::Mul, "mul"}, {Op::SDiv, "sdiv"},
                                                            {Op::SRem, "srem"}, {Op::UDiv, "udiv"}, {Op::URem, "urem"}, {Op::And, "and"},
                                                            {Op::Or, "or"}, {Op::Xor, "xor"}, {Op::Shl, "shl"}, {Op::AShr, "ashr"},
                                                            {Op::LShr, "lshr"}};
                ln(d + " = " + m.at(i.op) + " " + T + " " + v(i.a) + ", " + v(i.b));
                break;
            }
            case Op::Neg: ln(d + " = sub " + T + " 0, " + v(i.a)); break;
            case Op::Not: ln(d + " = xor " + T + " " + v(i.a) + ", -1"); break;
            case Op::FAdd: ln(d + " = fadd " + T + " " + v(i.a) + ", " + v(i.b)); break;
            case Op::FSub: ln(d + " = fsub " + T + " " + v(i.a) + ", " + v(i.b)); break;
            case Op::FMul: ln(d + " = fmul " + T + " " + v(i.a) + ", " + v(i.b)); break;
            case Op::FDiv: ln(d + " = fdiv " + T + " " + v(i.a) + ", " + v(i.b)); break;
            case Op::FNeg: ln(d + " = fneg " + T + " " + v(i.a)); break;
            case Op::ICmp: case Op::FCmp: {
                static const char* ic[] = {"eq", "ne", "slt", "sle", "sgt", "sge", "ult", "ule", "ugt", "uge"};
                static const char* fc[] = {"oeq", "une", "olt", "ole", "ogt", "oge", "olt", "ole", "ogt", "oge"};
                std::string c = t();
                const char* at = ty(f.regs[i.a]);
                if (i.op == Op::ICmp) ln(c + " = icmp " + ic[(int)i.cond] + " " + at + " " + v(i.a) + ", " + v(i.b));
                else ln(c + " = fcmp " + fc[(int)i.cond] + " " + at + " " + v(i.a) + ", " + v(i.b));
                ln(d + " = zext i1 " + c + " to i8");
                break;
            }
            case Op::SExt: case Op::ZExt: case Op::Trunc: case Op::SIToFP: case Op::UIToFP: case Op::FPToSI: case Op::FPExt: case Op::FPTrunc: {
                static const std::map<Op, const char*> m = {{Op::SExt, "sext"}, {Op::ZExt, "zext"}, {Op::Trunc, "trunc"},
                                                            {Op::SIToFP, "sitofp"}, {Op::UIToFP, "uitofp"}, {Op::FPToSI, "fptosi"},
                                                            {Op::FPExt, "fpext"}, {Op::FPTrunc, "fptrunc"}};
                ln(d + " = " + m.at(i.op) + " " + ty(f.regs[i.a]) + " " + v(i.a) + " to " + T);
                break;
            }
            case Op::Call: case Op::CallPtr: {
                std::string args;
                for (size_t k = 0; k < i.args.size(); k++) args += (k ? ", " : "") + std::string(ty(i.argTypes[k])) + " " + v(i.args[k]);
                std::string callee = i.op == Op::CallPtr ? v(i.a) : "@" + i.sym;
                std::string c = std::string("call ") + T + " " + callee + "(" + args + ")";
                ln(i.dst >= 0 ? d + " = " + c : c);
                break;
            }
            case Op::Br: ln("br label %b" + std::to_string(i.t1)); break;
            case Op::CondBr: {
                std::string c = t();
                ln(c + " = icmp ne i8 " + v(i.a) + ", 0");
                ln("br i1 " + c + ", label %b" + std::to_string(i.t1) + ", label %b" + std::to_string(i.t2));
                break;
            }
            case Op::Ret:
                if (i.a < 0) ln("ret void");
                else ln(std::string("ret ") + ty(f.ret) + " " + v(i.a));
                break;
        }
    }

    void run() {
        for (auto& b : f.blocks)
            for (auto& i : b.insts) {
                if (i.op == Op::SlotAddr) alias[i.dst] = "%s" + std::to_string(i.slot);
                if (i.op == Op::Global) alias[i.dst] = "@" + i.sym;
            }
        std::string ps;
        for (size_t k = 0; k < f.params.size(); k++) ps += (k ? ", " : "") + std::string(ty(f.params[k])) + " %p" + std::to_string(k);
        o += "define " + std::string(ty(f.ret)) + " @" + f.name + "(" + ps + ") {\n";
        o += "entry:\n";
        for (size_t s = 0; s < f.slots.size(); s++)
            ln("%s" + std::to_string(s) + " = alloca [" + std::to_string(f.slots[s].size) + " x i8], align " + std::to_string(f.slots[s].align));
        ln("br label %b0");
        for (size_t b = 0; b < f.blocks.size(); b++) {
            o += "b" + std::to_string(b) + ":\n";
            for (auto& i : f.blocks[b].insts) inst(i);
        }
        o += "}\n\n";
    }
};

}  // namespace

std::string emitLLVM(const IRModule& m) {
    std::string o = "; Generated by axonc (LLVM backend)\ntarget triple = \"x86_64-pc-windows-msvc\"\n\n";
    for (auto& d : m.data) {
        std::string esc;
        for (unsigned char c : d.bytes) {
            if (c >= 32 && c < 127 && c != '"' && c != '\\') esc += (char)c;
            else {
                char b[4];
                snprintf(b, sizeof b, "\\%02X", c);
                esc += b;
            }
        }
        o += "@" + d.name + " = private unnamed_addr constant [" + std::to_string(d.bytes.size() + 1) + " x i8] c\"" + esc + "\\00\"\n";
    }
    // declarations of everything defined elsewhere, using the signature seen at the call sites
    std::set<std::string> defined;
    for (auto& f : m.funcs) defined.insert(f.name);
    std::map<std::string, std::string> decls;
    std::set<std::string> data;
    for (auto& f : m.funcs)
        for (auto& b : f.blocks)
            for (auto& i : b.insts) {
                if (i.op == Op::Call && !defined.count(i.sym) && !decls.count(i.sym)) {
                    std::string ps;
                    for (size_t k = 0; k < i.argTypes.size(); k++) ps += (k ? ", " : "") + std::string(ty(i.argTypes[k]));
                    decls[i.sym] = "declare " + std::string(ty(i.type)) + " @" + i.sym + "(" + ps + ")";
                }
                if (i.op == Op::Global && !defined.count(i.sym)) data.insert(i.sym);
            }
    for (auto& d : m.data) data.erase(d.name);
    for (auto& d : decls) data.erase(d.first);
    o += "\n";
    for (auto& d : decls) o += d.second + "\n";
    for (auto& d : data) o += "@" + d + " = external global i8\n";
    o += "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)\ndeclare void @llvm.memset.p0.i64(ptr, i8, i64, i1)\n\n";
    for (auto& f : m.funcs) {
        FuncEmit e(f, o);
        e.run();
    }
    return o;
}

std::string findLLVMClang() {
#ifdef _WIN32
    char buf[MAX_PATH];
    if (SearchPathA(nullptr, "clang.exe", nullptr, MAX_PATH, buf, nullptr) > 0) return buf;
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    fs::path tools = fs::path(exe).parent_path().parent_path() / "tools" / "llvm";
    std::error_code ec;
    if (fs::exists(tools, ec))
        for (auto& e : fs::directory_iterator(tools, ec)) {
            fs::path c = e.path() / "bin" / "clang.exe";
            if (fs::exists(c)) return c.string();
        }
#endif
    return "";
}
