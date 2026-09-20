// ownck.h — 所有権検査（④ ownck）
//
// ★ 4 つ目のパスです。
//
//   ① 字句解析  … 文字の並びが「単語」に分けられるか
//   ② 構文解析  … 単語の並びが「文として成り立つ」か
//   ③ 意味解析  … 型と名前が合っているか
//   ④ 所有権検査 … **移動済みの値を使っていないか**   ← ここ
//   ⑤ コード生成
//
// なぜ sema と分けるのか（docs/design/ownership.md 1 節）
//   sema は「型が合うか」を見るパスで、実行順序を考えません。
//   所有権の検査は逆に「どの文が先に走るか」が本質です（データフロー解析）。
//   性質の違う解析を 1 つのパスに混ぜると、両方が読めなくなります。
//
// use-after-move（S1）、借用の保存・返却の禁止（S2）、
// 可変性と借用の衝突（S5）を見ます。
#ifndef PLC_OWNCK_H
#define PLC_OWNCK_H

#include <stdbool.h>

#include "module.h"
#include "types.h"

// 検査の振る舞い（コマンドライン option がそのまま入る）。
//
// ★ 昇格のスイッチは**検査ごとに分けます**（決定 D12）。
//   既存コードを直すとき、通ったものから順にエラーへ上げるためです。
typedef struct {
    bool deny_move;    // E-MOVE-*   をエラーにする（既定 true）
    bool deny_borrow;  // E-BORROW-* をエラーにする（既定 true）
    bool deny_mut;     // E-MUT-*    をエラーにする（既定 true）
    // ★ 決定 D12：スイッチは検査ごとに分けたままにします。
    //   既定が全部 true になった今も（A-24）、--warn-own で落としてから
    //   検査ごとに戻せる形が、古いコードを直していく唯一の順序だからです。
    bool deny_store_borrow;  // E-BORROW-7 をエラーにする（既定 true）
    bool explain_mut;  // --explain-mut （診断を出さず、変更される実引数を並べる）
} OwnckOptions;

// 全モジュールの関数本体を解析する。
//
// deny_* が true（**既定**）なら最初の 1 件でエラー終了、
// false（--warn-own）なら警告として出して先へ進みます。
//
// ★ A-24 で既定を入れ替えました。selfhost/ と lib/ が v2 の書き方へ
//   移り終わり（0.16.0）、コンパイラ自身が 4 つの検査すべてを通るように
//   なったので、「既定は警告」を続ける理由が無くなりました。
void ownck_program(Module *mods, const OwnckOptions *opt);

// 所有型か（言語仕様 v2 §2 の分類）。
//   コピー型 : int / bool          … 代入しても元が使える
//   所有型   : str / list[T] / class … 代入で移動する
//
// ★ 型そのものには所有の情報を持たせません（Type はシングルトン共有なので、
//   フラグを足すとポインタ比較による型の同一性が壊れる）。
//   「所有型かどうか」は、このように **型から導く**だけにします。
bool ty_is_owned(Type *t);
bool ty_is_rc(Type *t);   // rc[T] / rc[T] | None（共有型）

#endif  // PLC_OWNCK_H
