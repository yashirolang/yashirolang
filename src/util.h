// util.h — 全モジュールが使う基礎部品
//
// このファイルは他の本言語モジュールに依存しません（依存グラフの最下層）。
// ここに置くもの：メモリ確保、文字列バッファ、ファイル入出力、エラー終了。
#ifndef PLC_UTIL_H
#define PLC_UTIL_H

#include <stdarg.h>
#include <stddef.h>

// 標準出力・標準エラーを binary モードにする（Windows で \r\n に化けるのを防ぐ）。
// ★ main の最初に 1 回だけ呼びます。
void plc_use_binary_streams(void);

// ── メモリ確保 ──────────────────────────────────────────────
// free() は一切呼びません（docs/design/memory-model.md 第8節）。
// calloc を使うので、確保された領域は必ず 0 / NULL で初期化されています。
void *xmalloc(size_t size);

// s の先頭 n バイトを複製し、NUL 終端した新しい文字列を返す
char *xstrndup(const char *s, size_t n);

// ── 伸長する文字列バッファ ────────────────────────────────────
// LLVM IR を組み立てるための道具。printf と同じ書式で追記できます。
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

void sb_init(StrBuf *sb);
void sb_printf(StrBuf *sb, const char *fmt, ...);
// バッファの中身を NUL 終端された文字列として返す（内部バッファをそのまま返す）
char *sb_str(StrBuf *sb);

// ── ファイル入出力 ──────────────────────────────────────────
// ファイル全体を読み込む。\r\n は \n に正規化し、末尾に改行を保証する。
char *read_file(const char *path);
void write_file(const char *path, const char *text);

// ── エラー終了 ──────────────────────────────────────────────
//
// ★ 位置情報のないエラー専用です。
//   （コマンドライン引数の誤り、ファイルが開けない、メモリ確保失敗など）
//
// ソース上の位置を示せるエラーは diag.h の error_at() / diag_fail() を使います。
//
// 🤔 なぜ診断機能を全部 diag.c に移さないのか
//   xmalloc() は確保に失敗したらエラー終了する必要があります。
//   もし error() を diag.c に置くと util → diag の依存が生まれ、
//   diag.c も xmalloc / StrBuf を使うので **循環依存**になります。
//   「位置を示せない低レベルなエラーは util、位置つき診断は diag」と
//   分けることで、util.c を依存グラフの最下層に保っています。
_Noreturn void error(const char *fmt, ...);

// コンパイラ自身のバグ（ユーザーのミスではない）を報告して終了する
_Noreturn void internal_error(const char *file, int line, const char *fmt, ...);

// switch の default など「来ないはず」の場所に置く
#define UNREACHABLE() \
    internal_error(__FILE__, __LINE__, "到達しないはずのコードに来ました")

#endif  // PLC_UTIL_H
