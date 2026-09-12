#include "diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── 書式化ヘルパ ────────────────────────────────────────────

char *diag_fmt(const char *fmt, ...) {
    va_list ap;

    // 必要バイト数を測ってから確保する（StrBuf と同じ手口）
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) error("diag_fmt: 書式化に失敗しました");

    char *buf = xmalloc((size_t)need + 1);
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)need + 1, fmt, ap);
    va_end(ap);
    return buf;
}

// ── 罫線の桁揃え ────────────────────────────────────────────
//
// 行番号の桁数に合わせて罫線の位置をそろえます。
//
//      |            ← gutter 幅 = 1 のとき
//    9 | code
//      | ^
//
//       |           ← gutter 幅 = 2 のとき（10 行目以降）
//    10 | code
//       | ^
//
// 主要な位置と関連する位置で行番号の桁数が違っても、
// 両方のブロックで同じ幅を使うと縦線が一直線にそろって読みやすくなります。

static int num_width(int n) {
    int w = 1;
    while (n >= 10) {
        n /= 10;
        w++;
    }
    return w;
}

static int gutter_width(const Diag *d) {
    int w = num_width(d->primary.tok->line);
    if (d->related.tok) {
        int w2 = num_width(d->related.tok->line);
        if (w2 > w) w = w2;
    }
    return w;
}

// ── UTF-8 と表示幅 ──────────────────────────────────────────
//
// ★ なぜこれが必要か
//
//   Token.col は「行頭からのバイト数」です。スライスに使うにはそれが正しい
//   のですが、そのまま人間向けに使うと日本語を含む行で破綻します。
//
//     # ERROR: 空のプログラムです      ← 36 バイト / 27 文字 / 表示幅 27
//                                 ^ 36 バイト目に空白 36 個を送ると大きくずれる
//
//   そこで 2 種類の数え方を使い分けます。
//
//     ・報告する桁番号  … **文字数**（人間が「何文字目」と数える単位）
//     ・キャレットの字下げ … **表示幅**（全角文字は 2 桁分を占める）
//
//   この 2 つは別物です。全角 3 文字なら「3 文字目」だが「表示幅 6」です。

// 全角（East Asian Wide / Fullwidth）として扱うコードポイントか
static int is_wide_cp(unsigned cp) {
    return (cp >= 0x1100 && cp <= 0x115F) ||  // ハングル字母
           (cp >= 0x2E80 && cp <= 0x303E) ||  // CJK 記号
           (cp >= 0x3041 && cp <= 0x33FF) ||  // かな・カタカナ・CJK 互換
           (cp >= 0x3400 && cp <= 0x4DBF) ||  // CJK 拡張A
           (cp >= 0x4E00 && cp <= 0x9FFF) ||  // CJK 統合漢字
           (cp >= 0xA000 && cp <= 0xA4CF) ||  // イ文字
           (cp >= 0xAC00 && cp <= 0xD7A3) ||  // ハングル音節
           (cp >= 0xF900 && cp <= 0xFAFF) ||  // CJK 互換漢字
           (cp >= 0xFE10 && cp <= 0xFE19) ||  // 縦書き記号
           (cp >= 0xFE30 && cp <= 0xFE6F) ||  // CJK 互換形
           (cp >= 0xFF00 && cp <= 0xFF60) ||  // 全角英数
           (cp >= 0xFFE0 && cp <= 0xFFE6);    // 全角記号
}

// s の先頭 1 文字を調べる。*bytes にバイト長を入れ、表示幅を返す。
//
// ⚠️ 不正なバイト列でも必ず前進する（*bytes >= 1）ようにします。
//    さもないと無限ループになります。エラー表示中に固まるのは最悪です。
static int utf8_char(const char *s, int *bytes) {
    unsigned char c = (unsigned char)s[0];

    if (c < 0x80) {  // ASCII
        *bytes = 1;
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF && (s[1] & 0xC0) == 0x80) {  // 2 バイト
        *bytes = 2;
        return 1;  // ラテン拡張・キリル等は半角扱い
    }
    if (c >= 0xE0 && c <= 0xEF && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        unsigned cp = (unsigned)(c & 0x0F) << 12 |
                      (unsigned)(s[1] & 0x3F) << 6 |
                      (unsigned)(s[2] & 0x3F);
        *bytes = 3;
        return is_wide_cp(cp) ? 2 : 1;
    }
    if (c >= 0xF0 && c <= 0xF4 && (s[1] & 0xC0) == 0x80) {  // 4 バイト（絵文字等）
        *bytes = 4;
        return 2;
    }

    *bytes = 1;  // 不正なバイト：1 バイト進めて半角扱い
    return 1;
}

// ── ソース抜粋の描画 ────────────────────────────────────────

// その行の長さ（改行まで）をバイト数で数える
static int line_length(const char *line_start) {
    int n = 0;
    while (line_start[n] != '\n' && line_start[n] != '\0') n++;
    return n;
}

// 行頭から byte_off バイト目までに何文字あるか（1 起算の桁番号を返す）
static int char_column(const char *ls, int byte_off) {
    int col = 1;
    for (int i = 0; i < byte_off;) {
        int bytes;
        utf8_char(ls + i, &bytes);
        i += bytes;
        col++;
    }
    return col;
}

// 1 箇所ぶんのブロックを描画する。
//
//     --> file:line:col
//      |
//    2 | (1 + 2
//      | ^^^ label
//
// gw は行番号欄の幅。全フィールドで幅 gw+1 を使うとそろいます。
static void render_block(const DiagLabel *lb, int gw) {
    Token *tok = lb->tok;
    const char *ls = tok->line_start;

    // 位置情報が無い場合はファイル名だけ示して抜粋を省略する
    if (!ls) {
        fprintf(stderr, "%*s--> %s:%d:%d\n", gw + 1, "", tok->file, tok->line, tok->col);
        return;
    }

    int len = line_length(ls);

    // byte 単位の col を、行内に収まる範囲に丸めておく
    int off = tok->col - 1;
    if (off > len) off = len;
    if (off < 0) off = 0;

    // ★ 報告する桁番号は「文字数」で出す（バイト数ではなく）
    fprintf(stderr, "%*s--> %s:%d:%d\n", gw + 1, "", tok->file, tok->line,
            char_column(ls, off));
    fprintf(stderr, "%*s |\n", gw + 1, "");

    // ソース行そのもの
    fprintf(stderr, "%*d | %.*s\n", gw + 1, tok->line, len, ls);

    // 下線の行
    fprintf(stderr, "%*s | ", gw + 1, "");

    // ★ キャレットの手前は「表示幅」ぶんの空白を送る（全角は 2 個）
    // ⚠️ タブはタブで送ると、端末上の桁が元の行とそろいます。
    for (int i = 0; i < off;) {
        if (ls[i] == '\t') {
            fputc('\t', stderr);
            i++;
            continue;
        }
        int bytes;
        int w = utf8_char(ls + i, &bytes);
        for (int k = 0; k < w; k++) fputc(' ', stderr);
        i += bytes;
    }

    // 下線も表示幅で数える。行の末尾を越えないように切り詰め、最低 1 文字は引く。
    int end = off + tok->len;
    if (end > len) end = len;
    int under = 0;
    for (int i = off; i < end;) {
        int bytes;
        under += utf8_char(ls + i, &bytes);
        i += bytes;
    }
    if (under < 1) under = 1;
    for (int i = 0; i < under; i++) fputc('^', stderr);

    if (lb->label) fprintf(stderr, " %s", lb->label);
    fputc('\n', stderr);
}

// ── 本体 ───────────────────────────────────────────────────

void diag_emit(const Diag *d) {
    const char *sev = d->severity ? d->severity : "error";
    int gw = gutter_width(d);

    // ① 主メッセージ（診断コードがあれば error[E-MOVE-1]: の形で出す）
    if (d->code) fprintf(stderr, "%s[%s]: %s\n", sev, d->code, d->message);
    else fprintf(stderr, "%s: %s\n", sev, d->message);

    // ② 主要な位置
    render_block(&d->primary, gw);

    // ③ 関連する位置（別ブロックとして note: で出す）
    //
    // 🤔 なぜ 1 つのブロックに 2 本の下線をまとめないのか
    //    rustc は同じ行なら 1 ブロックに複数ラベルを描きますが、
    //    「同じ行か / 別の行か」「ラベルが重なるか」で場合分けが増え、
    //    描画コードが一気に複雑になります。
    //    ブロックを分ければ行の位置関係に関係なく常に正しく描けるので、
    //    この教材では分割方式を採用します。
    if (d->related.tok) {
        fprintf(stderr, "%*s |\n", gw + 1, "");
        fprintf(stderr, "note: %s\n", d->related.label ? d->related.label : "関連する位置");
        DiagLabel rel = {d->related.tok, NULL};  // note 側に重ねてラベルは出さない
        render_block(&rel, gw);
    }

    // ④ ヒント
    if (d->hint) {
        fprintf(stderr, "%*s |\n", gw + 1, "");
        fprintf(stderr, "%*s = ヒント: %s\n", gw + 1, "", d->hint);
    }
}

_Noreturn void diag_fail(const Diag *d) {
    diag_emit(d);
    exit(1);
}

// ── 簡易版 ──────────────────────────────────────────────────

_Noreturn void error_at(Token *tok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    Diag d = {0};
    d.message = msg;
    d.primary.tok = tok;
    diag_fail(&d);
}

_Noreturn void error_at_hint(Token *tok, const char *hint, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    Diag d = {0};
    d.message = msg;
    d.primary.tok = tok;
    d.hint = hint;
    diag_fail(&d);
}
