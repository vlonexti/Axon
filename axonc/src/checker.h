// checker.h - name resolution, type checking, ownership (move) checking.
#pragma once
#include <map>
#include <vector>

#include "ast.h"
#include "types.h"

struct Impl {
    Type* type;
    InterfaceDecl* iface;
    std::vector<FuncDecl*> methods;  // in interface method order
};

struct Program {
    std::vector<Module*> modules;
    Module* mainModule = nullptr;
    Module* prelude = nullptr;
    bool testMode = false;
    bool tools = false;  // editor tools: keep checking after errors to get as much type information as possible

    // collected by the checker
    std::vector<StructDecl*> structs;
    std::vector<EnumDecl*> enums;
    std::vector<InterfaceDecl*> ifaces;
    std::vector<FuncDecl*> funcs;      // free functions + methods with bodies
    std::vector<FuncDecl*> tests;
    std::vector<VarDecl*> globalOrder;  // global constants in initialization order
    std::map<Type*, std::vector<FuncDecl*>> extMethods;
    std::vector<Impl> impls;
    FuncDecl* mainFn = nullptr;
    InterfaceDecl* disposable = nullptr;
};

bool checkProgram(Program& prog);
const Impl* findImpl(Program& prog, Type* t, InterfaceDecl* iface);
FuncDecl* findMethod(Program& prog, Type* t, const std::string& name);
