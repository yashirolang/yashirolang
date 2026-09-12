#include "util.h"

// ⚠️ Windows の標準出力・標準エラーは既定で \n を \r\n に書き換えます。
//    診断や --dump-ast の出力が OS によって変わってしまうので、binary に揃えます。
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "langinfo.h"

// ── メモリ確保 ──────────────────────────────────────────────

// 標準出力・標準エラーを binary モードにする（Windows だけの都合）
void plc_use_binary_streams(void) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
}

void *xmalloc(size_t size) {
    // calloc でゼロ初期化する。Node のような大きな構造体で
    // 「フィールドの初期化忘れ」が不定値バグにならないようにするため。
    void *p = calloc(1, size);
    if (!p) error("メモリ確保に失敗しました (%zu バイト)", size);
    return p;
}

char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

// ── StrBuf ─────────────────────────────────────────────────

void sb_init(StrBuf *sb) {
    sb->cap = 256;
    sb->len = 0;
    sb->data = xmalloc(sb->cap);
    sb->data[0] = '\0';
}

void sb_printf(StrBuf *sb, const char *fmt, ...) {
    va_list ap;

    // まず「書き込むと何バイト必要か」を測る。
    // vsnprintf は size に 0 を渡すと、書き込まずに必要バイト数だけを返す。
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) error("sb_printf: 書式化に失敗しました");

    // 足りなければ、必要量を満たすまで容量を 2 倍にしていく
    size_t required = sb->len + (size_t)need + 1;  // +1 は NUL の分
    if (required > sb->cap) {
        while (sb->cap < required) sb->cap *= 2;
        char *newdata = xmalloc(sb->cap);
        memcpy(newdata, sb->data, sb->len + 1);
        sb->data = newdata;  // 古い領域は解放しない（memory-model.md 第8節）
    }

    va_start(ap, fmt);
    vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    sb->len += (size_t)need;
}

char *sb_str(StrBuf *sb) { return sb->data; }

// ── ファイル入出力 ──────────────────────────────────────────

char *read_file(const char *path) {
    FILE *fp;
    if (strcmp(path, "-") == 0) {
        fp = stdin;
    } else {
        fp = fopen(path, "rb");
        if (!fp) error("ファイルを開けません: %s", path);
    }

    // 一気に読む（サイズが分からない stdin でも動くようにチャンク読み）
    StrBuf sb;
    sb_init(&sb);
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        // chunk は NUL 終端されていないので sb_printf は使えない
        size_t required = sb.len + n + 1;
        if (required > sb.cap) {
            while (sb.cap < required) sb.cap *= 2;
            char *newdata = xmalloc(sb.cap);
            memcpy(newdata, sb.data, sb.len);
            sb.data = newdata;
        }
        memcpy(sb.data + sb.len, chunk, n);
        sb.len += n;
        sb.data[sb.len] = '\0';
    }
    if (fp != stdin) fclose(fp);

    // \r\n → \n に正規化する（言語仕様 2.1）
    char *src = sb.data;
    size_t w = 0;
    for (size_t r = 0; r < sb.len; r++) {
        if (src[r] == '\r' && r + 1 < sb.len && src[r + 1] == '\n') continue;
        if (src[r] == '\r') { src[w++] = '\n'; continue; }
        src[w++] = src[r];
    }
    src[w] = '\0';

    // 末尾に改行を保証しておくと、字句解析器の行末処理が単純になる
    if (w == 0 || src[w - 1] != '\n') {
        StrBuf out;
        sb_init(&out);
        sb_printf(&out, "%s\n", src);
        return sb_str(&out);
    }
    return src;
}

void write_file(const char *path, const char *text) {
    FILE *fp = fopen(path, "wb");
    if (!fp) error("ファイルを書き込めません: %s", path);
    fputs(text, fp);
    fclose(fp);
}

// ── エラー報告 ──────────────────────────────────────────────

_Noreturn void error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

_Noreturn void internal_error(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, PLC_LANG_CC " internal error: %s:%d: ", file, line);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fprintf(stderr, "  これはコンパイラ自身のバグです。報告してください。\n");
    va_end(ap);
    exit(2);  // ユーザーのミス(1)と区別するため 2 で終了する
}
