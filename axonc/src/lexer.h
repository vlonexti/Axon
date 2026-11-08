// lexer.h - turns Axon source text into tokens.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "diagnostics.h"

enum class T {
    Eof, Ident, IntLit, FloatLit, StrLit, CharLit, InterpStr, Comment,
    // keywords
    KwInt, KwLong, KwByte, KwFloat, KwDouble, KwBool, KwChar, KwString, KwVoid, KwVar, KwConst,
    KwIf, KwElse, KwWhile, KwDo, KwFor, KwForeach, KwIn, KwReturn, KwBreak, KwContinue,
    KwSwitch, KwCase, KwDefault, KwStruct, KwInterface, KwEnum, KwExtend, KwMut, KwRef,
    KwNew, KwThis, KwTrue, KwFalse, KwNone, KwNull, KwIs, KwTry, KwUsing, KwExtern, KwPrivate,
    KwClass,
    // punctuation
    LParen, RParen, LBrace, RBrace, LBracket, RBracket, Comma, Semi, Colon, Dot, Question, QQ, At,
    Plus, Minus, Star, Slash, Percent, Amp, Pipe, Caret, Tilde, Bang, Shl, Shr,
    Assign, EqEq, NotEq, Lt, Gt, Le, Ge, AndAnd, OrOr, PlusPlus, MinusMinus,
    PlusEq, MinusEq, StarEq, SlashEq, PercentEq, AmpEq, PipeEq, CaretEq, ShlEq, ShrEq, Arrow,
};

struct InterpPart {
    bool isExpr = false;
    std::string text;  // decoded literal text
    int start = 0, end = 0;  // byte offsets of the expression source
};

struct Token {
    T type = T::Eof;
    std::string text;  // raw source text
    Span span;
    int64_t ival = 0;
    double fval = 0;
    bool isF32 = false;
    bool isLong = false;
    uint32_t cval = 0;
    std::string sval;  // decoded string literal
    std::vector<InterpPart> parts;
};

const char* tokName(T t);
std::string tokDisplay(const Token& t);

// Lexes [start, end) of a file. end = -1 means to the end of the file.
std::vector<Token> lexFile(int fileId, int start = 0, int end = -1, bool keepComments = false);
