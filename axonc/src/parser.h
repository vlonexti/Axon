// parser.h - recursive descent parser (Pratt parsing for expressions).
#pragma once
#include <vector>

#include "ast.h"
#include "lexer.h"

class Parser {
public:
    Parser(std::vector<Token> tokens, Module* mod);
    void parseModule();
    Expr* parseStandaloneExpr();  // used for $"...{expr}..." parts

private:
    struct ParseError {};

    std::vector<Token> t;
    size_t p = 0;
    Module* mod;
    int pendingGt = 0;

    const Token& cur() const { return t[p]; }
    const Token& peekTok(int n = 1) const { return t[p + n < t.size() ? p + n : t.size() - 1]; }
    bool at(T k) const { return t[p].type == k; }
    bool atIdent(const char* s) const { return t[p].type == T::Ident && t[p].text == s; }
    bool accept(T k);
    const Token& expect(T k, const char* what);
    [[noreturn]] void fail(const std::string& msg, const std::string& help = "");
    void syncTop();
    void syncStmt();
    Span spanFrom(const Span& start) const;

    // declarations
    void parseTopDecl();
    void parseUsing();
    StructDecl* parseStruct(bool isPrivate);
    InterfaceDecl* parseInterface(bool isPrivate);
    EnumDecl* parseEnum(bool isPrivate);
    ExtendDecl* parseExtend();
    void parseExtern();
    FuncDecl* parseFuncRest(TypeExpr* ret, const Token& nameTok, bool needBody);
    std::vector<Param> parseParams();
    std::vector<TypeParam> parseTypeParams();
    VarDecl* parseConst(bool global);

    // types
    bool scanType(size_t& i);
    bool isTypeStart(size_t i) const;
    TypeExpr* parseType();
    TypeExpr* parseBaseType();
    void closeAngle();

    // statements
    Stmt* parseStmt();
    Stmt* parseBlock();
    Stmt* asBlock(Stmt* s);
    Stmt* parseVarDecl(bool requireSemi);
    Stmt* parseSimpleStmt(bool requireSemi);  // var decl or expression statement
    bool looksLikeDecl() const;
    Stmt* parseIf();
    Stmt* parseSwitch();

    // expressions
    Expr* parseExpr(int minPrec = 0);
    Expr* parseUnary();
    Expr* parsePostfix(Expr* e);
    Expr* parsePrimary();
    Expr* parseInterp(const Token& tok);
    Expr* parseNew();
    Pattern* parseIsPattern();
    Pattern* parseCasePattern();
    std::vector<PatBind> parseBinds();
    Expr* mk(EK k, Span sp);
};

void parseModuleSource(Module* mod);
