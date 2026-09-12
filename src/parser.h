// parser.h — 構文解析（② トークン列 → AST）
//
// 手法は「再帰下降構文解析」です。
// 文法規則 1 つ ＝ static 関数 1 つ に対応させます。
// 文法は docs/spec/grammar.md にあります。
//
// 当初の文法（暫定）：
//     program ::= expr EOF
//     expr    ::= INT
//
// ⚠️ この文法は正式な言語仕様とは違います。当初は
//    「パイプラインを端から端まで通す」ことだけを目的にしているためです。
//    のちに正式な形（def / インデントブロック）に置き換えます。
#ifndef PLC_PARSER_H
#define PLC_PARSER_H

#include "ast.h"
#include "lexer.h"

// トークン列を構文解析して AST の根を返す
Node *parse(TokenVec toks);

#endif  // PLC_PARSER_H
