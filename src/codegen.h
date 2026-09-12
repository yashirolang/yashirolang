// codegen.h — コード生成（④ AST → LLVM IR テキスト）
//
// 生成規約は docs/design/ir-conventions.md にあります。
// 特に重要な規約：
//   R1  ローカル変数はすべて entry ブロックで alloca する
//   R4  一時値には英字始まりの名前を付ける（%t0）
//   R6  すべての基本ブロックは終端命令で終わる
//   R11 target triple を必ず出力する
#ifndef PLC_CODEGEN_H
#define PLC_CODEGEN_H

#include "ast.h"
#include "module.h"

// モジュール 1 つぶんの LLVM IR テキストを生成して返す。
//   main_ir_name : 入口モジュールなら「本言語の main の IR 名」。
//                  他のモジュールでは NULL（C の main を出すのは入口だけ）。
//   drop         : 解放（drop）を挿入するか（--drop）。
//
// ⚠️ drop = false のときの出力は v1 と 1 バイトも変わりません。
//    既存コード（selfhost/ / lib/）はまだ v1 の参照セマンティクス前提なので、
//    移行を終えるまで、解放は **opt-in** にしてあります（決定 D16）。
//   triple       : 生成する IR の target triple（--target / pragma target）
char *codegen(Module *mod, const char *main_ir_name, bool drop, bool no_ovf,
              const char *triple);

#endif  // PLC_CODEGEN_H
