// clone.h - deep copies of (not yet checked) declarations, used to instantiate generics.
#pragma once
#include "ast.h"

TypeExpr* cloneTypeExpr(const TypeExpr* t);
Expr* cloneExpr(const Expr* e);
Stmt* cloneStmt(const Stmt* s);
FuncDecl* cloneFunc(const FuncDecl* f);
StructDecl* cloneStruct(const StructDecl* s);
EnumDecl* cloneEnum(const EnumDecl* e);
