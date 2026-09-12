// sema.h — 意味解析（③ 型検査・名前解決）
//
// ★ 3 つ目のパスです。
//
//   ① 字句解析  … 文字の並びが「単語」に分けられるか
//   ② 構文解析  … 単語の並びが「文として成り立つ」か
//   ③ 意味解析  … 成り立つ文だが「意味が通る」か   ← ここ
//   ④ コード生成
//
// 構文的には正しいのに意味が通らないものを弾きます。
//
//     y = 1        構文は正しい。でも y は宣言されていない
//     x: foo = 1   構文は正しい。でも foo という型は無い
//     7 / 2        構文は正しい。でも int に '/' は使えない
//
// このパスは AST を書き換えません（型の注釈を書き込むだけ）。
// 各ノードの `type` フィールドを埋めるのが主な仕事で、
// コード生成器はそれを見て命令を選びます。
//
// 型付け規則の一覧は docs/spec/type-system.md にあります。
#ifndef PLC_SEMA_H
#define PLC_SEMA_H

#include <stdbool.h>

#include "ast.h"
#include "module.h"

// 組み込み関数の候補。
// ★ sema と codegen が同じ表を見ます。
//   sema は「型が合う候補があるか」、codegen は「どの C 関数を呼ぶか」。
typedef struct Builtin_ Builtin;
struct Builtin_ {
    const char *name;   // 本言語側の名前（print, len, ...）
    int arg;            // 引数の TypeKind
    int ret;            // 戻り値の TypeKind
    const char *impl;   // 呼び出すランタイム関数（pl_print_int など）
};

extern const Builtin BUILTINS[];
bool is_builtin_name(const char *name);

// 低レベルの組み込み（unsafe: の中でだけ使える）
bool is_lowlevel_name(const char *name);


// 全モジュールを検査し、各ノードの type / ir_name を埋める。
// 問題があればエラーを表示して終了する（戻ってこない）。
//
// ★ 単位が「1 つの AST」から「依存順に並んだモジュール列」になりました。
void sema_program(Module *mods, Module *entry);

#endif  // PLC_SEMA_H
