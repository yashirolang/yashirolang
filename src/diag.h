// diag.h — 診断メッセージ（エラー報告）の整形と出力
//
// ★ このモジュールの存在理由
//
// コンパイラを開発している間、最初のユーザーは自分自身です。
// インデント処理や型検査で何十回もエラーを見ることになるので、
// その表示が親切かどうかが残りの章の開発速度を決めます。
//
// 良い診断メッセージの 3 要素:
//   ① どこで      … ファイル:行:桁 とソース抜粋
//   ② 何が問題か  … 主メッセージと、位置に付ける短いラベル
//   ③ どうすれば  … ヒント / 関連する位置
//
// 出力例:
//
//   error: 閉じ括弧 ')' がありません
//     --> t:3:1
//      |
//    3 |
//      | ^ ここに ')' が必要です
//      |
//   note: 対応する '(' はここです
//     --> t:2:1
//      |
//    2 | (1 + 2
//      | ^
//      |
//      = ヒント: 括弧の対応を確認してください
//
// 位置情報を持たないエラー（コマンドライン引数の誤りなど）は
// util.h の error() を使います。こちらは Token を必要とする診断専用です。
#ifndef PLC_DIAG_H
#define PLC_DIAG_H

#include "lexer.h"
#include "util.h"

// ソース上の 1 箇所と、そこに付ける短い説明。
// label が NULL なら下線だけを引きます。
typedef struct {
    Token *tok;
    const char *label;
} DiagLabel;

// 1 件の診断。
// 使わないフィールドは 0 / NULL のままにしておけます（= {0} で初期化する）。
typedef struct {
    const char *severity;  // "error" / "warning"。NULL なら "error"
    const char *code;      // 診断コード（"E-MOVE-1"）。NULL なら出力しない
                           // ★ 仕様書が診断コードで規則を指せるように
                           //   しました（safety-spec.md §1 の S1〜S8）。
    const char *message;   // 主メッセージ（必須）

    DiagLabel primary;  // 主要な位置（必須）
    DiagLabel related;  // 関連する位置（tok が NULL なら出力しない）
                        // label は note: の見出しとして使われる
    const char *hint;   // "= ヒント: ..." の行（NULL なら出力しない）
} Diag;

// printf 書式で文字列を組み立てて返す（Diag.message を作るのに使う）
char *diag_fmt(const char *fmt, ...);

// 診断を stderr に出力する（終了はしない）
void diag_emit(const Diag *d);

// 診断を出力して exit(1) する
_Noreturn void diag_fail(const Diag *d);

// ── 簡易版 ──────────────────────────────────────────────────
// ほとんどのエラーは「位置 + メッセージ」だけで足りるので、
// Diag を組み立てずに呼べる関数を用意します。

_Noreturn void error_at(Token *tok, const char *fmt, ...);

// ヒント付き。hint は最後の行に "= ヒント: " として出ます。
_Noreturn void error_at_hint(Token *tok, const char *hint, const char *fmt, ...);

// 組み立て済みの文面をそのまま出す版（MSGn で組み立てたものを渡す）
_Noreturn void error_at_m(Token *tok, const char *msg);
_Noreturn void error_at_hint_m(Token *tok, const char *hint, const char *msg);

// ── 言語の切り替え（docs/ja/design/i18n-diagnostics.md）──────────
//
// ★ 日本語が正本で、既定です。英語は msgs/en.tsv を鍵で引きます。
//   文面の中の {0} {1} … は、差し込む値（いつも文字列）で埋めます
//   （{{ と }} は波括弧そのもの）。英語では語順が変わるので番号で書きます。
//
//   MSG2("E-TYPE-3", "型 '{0}' と '{1}' は違います", l, r)
//
// 注意: 英語の表が無い・鍵が無いときは、日本語で出します（診断を出せなく
//   なることはありません）。日本語のときは表を開きません。
// 注意: 日本語の文面は**1 つの文字列リテラル**で書きます（make check-msgs が
//   差し込みの番号を英語と突き合わせるため）。
//
// 言語を決める。spec は "ja" / "en" / "auto"。それ以外なら false。
// 呼ばなければ、最初に文面を作るときに環境変数 PLC_MSG_LANG を見ます。
bool msg_set_lang(const char *spec);
// いまの言語が英語か
bool msg_is_en(void);
char *msgv(const char *key, const char *ja, const char **args, int nargs);
// 静的な表の初期値に並べる形（鍵と日本語の 2 つのフィールドになる。使う側で msgv に渡す）
#define MSGK(k, ja) (k), (ja)
#define MSG0(k, ja) msgv((k), (ja), NULL, 0)
#define MSG1(k, ja, a) msgv((k), (ja), (const char *[]){(a)}, 1)
#define MSG2(k, ja, a, b) msgv((k), (ja), (const char *[]){(a), (b)}, 2)
#define MSG3(k, ja, a, b, c) msgv((k), (ja), (const char *[]){(a), (b), (c)}, 3)
#define MSG4(k, ja, a, b, c, d) msgv((k), (ja), (const char *[]){(a), (b), (c), (d)}, 4)
#define MSG5(k, ja, a, b, c, d, e2) \
    msgv((k), (ja), (const char *[]){(a), (b), (c), (d), (e2)}, 5)
#define MSG7(k, ja, a, b, c, d, e2, f, g) \
    msgv((k), (ja), (const char *[]){(a), (b), (c), (d), (e2), (f), (g)}, 7)

#endif  // PLC_DIAG_H
