// prove.h — 証明（⑤ 区間解析で実行時検査を消す。A-34 段 1・2）
//
// ★ 5 つ目のパスです。
//
//   ① 字句解析 ② 構文解析 ③ 意味解析 ④ 所有権検査
//   ⑤ **証明**（← ここ）⑥ コード生成
//
// 🤔 何をするパスか
//   各 int の値が「取りうる範囲」を関数の中で流し、**すでに書いてある
//   実行時検査のうち、必ず成り立つと示せたものだけ**に印を付けます。
//   codegen はその印を見て検査を出しません。
//
//   ⚠️ **利用者が書くものは 1 行も増えません。** ループ不変条件も、
//     SMT ソルバも要りません（設計は docs/roadmap.md の A-34）。
//
// ⚠️ **消す方向にしか使いません。** 分からなければ印は立たず、今までどおり
//    実行時に確かめます。印を 1 つ誤ると、そこはメモリ安全でなくなるので、
//    `--verify-prove` で「検査は残したまま、外れたら専用の診断で止める」
//    形を用意してあります。
#ifndef PLC_PROVE_H
#define PLC_PROVE_H

#include <stdbool.h>

#include "module.h"

typedef struct {
    // 消せた検査の数（--prove-report が出します）
    int ovf;       // 桁あふれ
    int range;     // 範囲型（A-28）
    int bounds;    // 添字
    int contract;  // 契約（A-29）
    int div;       // 0 除算（`//` と `%`。呼び出しが命令になります）
    // 残った数（示せなかったもの）
    int ovf_left;
    int range_left;
    int bounds_left;
    int contract_left;
    int div_left;
} ProveStats;

// 全モジュールを解析して、AST に「検査を出さなくてよい」印を立てる。
//
// ⚠️ 2 つの実装で**同じ印**が立たなければなりません（IR が食い違うため）。
//    だから解析は決定的です（表はすべて出現順のリストで、名前で引きます）。
void prove_program(Module *mods, ProveStats *out);

#endif  // PLC_PROVE_H
