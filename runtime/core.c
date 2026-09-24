// core.c — ランタイムの「核」（ runtime.c から分離）
//
// ★ ここには **libc に依存しないもの**だけを置きます。
//   OS を書くとき、リンクできるのはこのファイルだけです。
//
// なぜ分けるのか
//   v1 のランタイムは printf / malloc / fopen を直接呼んでいました。
//   ベアメタルにはそのどれもありません。かといって「OS 用の別ランタイム」を
//   もう 1 本書くと、同じ処理が 2 か所に増えて必ずずれます。
//   **libc に触る所だけを 4 つのフック関数に追い出せば、核は 1 本で済みます。**
//
//   ┌─────────────┐        ┌──────────────────┐
//   │  core.c      │──呼ぶ──▶│ pl_hook_alloc     │  hosted.c（PC 上）
//   │（この file） │        │ pl_hook_free      │    → calloc / free / stdout
//   │              │        │ pl_hook_write     │  kernel/（ベアメタル）
//   │              │        │ pl_hook_panic     │    → UART / 停止
//   └─────────────┘        └──────────────────┘
//
// 注意: このファイルでは <stdio.h> / <stdlib.h> / <string.h> を include しません。
//    必要な小道具（memcpy 相当）は自分で持ちます。

#include "core.h"

// ★ libc を include しないので、必要な定義は自分で持ちます。
#ifndef NULL
#define NULL ((void *)0)
#endif

// long long の最小値。<limits.h> を引かずに済ませます
// （-9223372036854775808 と直に書くと、まず正の定数を作ってから否定する規則の
//   ため、そのままでは long long に収まりません）。
#define PL_LLONG_MIN (-9223372036854775807LL - 1)

// ── 自前の小道具（libc の代わり）────────────────────────────
//
// ★ freestanding でも clang は memcpy / memset の呼び出しを生成することが
//   あります。ベアメタル向けにビルドするときだけ、自分で用意します。
static void *pl_memcpy(void *dst, const void *src, long long n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    for (long long i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static long long pl_cstr_len(const char *s) {
    long long n = 0;
    while (s[n]) n++;
    return n;
}

#ifdef PL_FREESTANDING
// ★ clang は「明らかにコピー」と見たループを memcpy の呼び出しに変えることが
//   あります。libc の無い世界では、その相手も自分で用意しておきます。
void *memcpy(void *dst, const void *src, unsigned long n) {
    return pl_memcpy(dst, src, (long long)n);
}
void *memset(void *dst, int c, unsigned long n) {
    unsigned char *d = dst;
    for (unsigned long i = 0; i < n; i++) d[i] = (unsigned char)c;
    return dst;
}
#endif

// 整数を 10 進の文字列にする（snprintf の代わり）。返すのは書いた長さ。
static long long pl_itoa(long long v, char *out) {
    char tmp[24];
    int n = 0;
    int neg = v < 0;
    unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1ULL
                               : (unsigned long long)v;
    if (u == 0) tmp[n++] = '0';
    while (u > 0) {
        tmp[n++] = (char)('0' + (u % 10));
        u /= 10;
    }
    long long len = 0;
    if (neg) out[len++] = '-';
    while (n > 0) out[len++] = tmp[--n];
    out[len] = '\0';
    return len;
}


// ── エラー ─────────────────────────────────────────────────

// 回復不能なエラー。stderr に出して終了コード 1 で死ぬ。
// 例外機構（try / except）は v1 では採用しません（言語仕様 8 節）。
_Noreturn void pl_panic(const char *msg) {
    // ★ 「どう死ぬか」は環境ごとに違うので、フックに任せます。
    pl_hook_panic(msg);
    for (;;) {  // フックは戻ってこない約束（戻ってきたらここで止まる）
    }
}

// ── メモリ ─────────────────────────────────────────────────

// ★ calloc でゼロ初期化し、失敗したら即終了する。
//   即終了にすることで、生成する IR に NULL チェックを入れずに済みます。
void *pl_alloc(long long size) {
    void *p = pl_hook_alloc(size);
    if (!p) pl_panic("out of memory");
    return p;
}

// ── None（null ポインタ）の検査 ────────────────────────
//
// ★ クラス型のフィールドは calloc により NULL から始まります
//   （既定値を作ろうとすると無限再帰するため）。
//   init で入れ忘れたまま参照すると segfault しますが、
//   ここを通しておけば「何が起きたか分かるメッセージ」に変わります。
//
// 注意: 本来の解決策は型システム側（T | None と narrowing）です。のちに塞ぎます。
//
// ★ 検査そのものは codegen が IR に展開するようになりました
//   （gen_not_none）。フィールド参照はこの言語で最も回数の多い操作で、
//   ここを呼び出しのままにしておくと -O2 でも 1 回ずつ call を払います。
//   ランタイムは別にリンクされるのでインライン化されません。

// None だったときだけ呼ばれる出口。
// 注意: **戻ってきません**。呼び出し側の IR は直後に unreachable を置きます。
void pl_none_fail(void) {
    pl_panic("field access on None (uninitialized reference field?)");
}

// ★ 展開後の codegen はもう呼びませんが、ランタイム内から使います。
void *pl_check_not_none(void *p) {
    if (!p) pl_none_fail();
    return p;
}

// ── 出力（print のオーバーロード）──────────────────────────

// ── double を 10 進の文字列にする ────────────────────────
//
// 注意: **libc は使えません。** このランタイムはベアメタルでも動く必要があり、
//    snprintf("%g") に頼れません（runtime/README.md）。そこで最小限の
//    変換を自前で持ちます。
//
// 出力の規則（言語仕様 7 節。**この 1 か所が唯一の定義**です）:
//   - NaN → "nan"、無限大 → "inf" / "-inf"
//   - **元に戻る最短の桁数**で書きます。末尾の 0 は落とし、
//     最低 1 桁は必ず残します（1.0）
//   - |v| >= 1e18 または 0 < |v| < 1e-6 は指数表記（1.5e-9）
//
// ★ **6 桁の丸めをやめました。**
//   `str(0.1 + 0.2)` が `0.3` に見えるのは、数値計算の言語として害でした
//   （自分の誤差が見えない）。いまは Python と同じく `0.30000000000000004` です。
//
//   注意: **「17 桁出す」ではありません。** double → 文字列 → double が元に戻る、
//     いちばん短い表記を選びます（1.0 は "1.0"、0.1 は "0.1"）。
//
//   注意: **10 のべき乗で割って桁を作る素朴なやり方では駄目でした。**
//     試作して 200 件中 116 件しか往復しませんでした（1 ulp ずれる）。
//     正しくやるには多倍長整数が要ります（Steele & White / Burger–Dybvig の
//     いわゆる Dragon4）。下の pl_shortest_digits がそれです。
static long long pl_utoa(unsigned long long u, char *out) {
    char tmp[24];
    int n = 0;
    if (u == 0) tmp[n++] = '0';
    while (u > 0) {
        tmp[n++] = (char)('0' + (u % 10));
        u /= 10;
    }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

// ── 最短で往復する 10 進の桁を作る（Dragon4） ─────────
//
// ★ **libc は使いません。** 32bit の桁を並べた多倍長整数を自前で持ちます。
//   R / S が値そのもの、M± が「隣の double との中点までの距離」です。
//   R/S を 10 倍しながら 1 桁ずつ取り出し、**残りが中点より内側に入った
//   時点で打ち切る**ので、元に戻る最短の桁数になります。
//
// 注意: 桁数の上限（PL_BN_LIMBS）は余裕を見てあります。表示（pl_ftoa）に
//   要るのは 1e308 や非正規化数でも 40 桁ほどですが（32bit × 40 = 1280bit）、
//   **読み取り（pl_str_to_float）のほうが要ります**。十進 780 桁を
//   10^-1104 で割る形になるので 3700 ビットほど必要です。
//   注意: ここを縮めると、pl_bn_shl が黙って何もしなくなります（あふれ検査で
//     return するため）。縮めるときは PL_MAX_SIG も一緒に見てください。
//   注意: ベアメタルのスタックは 16KB です（kernel/boot.s）。PlBn 1 つが
//     516 バイトで、いちばん多く使う pl_shortest_digits が 6 つです。
#define PL_BN_LIMBS 128

typedef struct {
    int n;
    unsigned int d[PL_BN_LIMBS];  // d[0] が下位
} PlBn;

static void pl_bn_set(PlBn *a, unsigned long long v) {
    for (int i = 0; i < PL_BN_LIMBS; i++) a->d[i] = 0;
    a->d[0] = (unsigned int)(v & 0xffffffffu);
    a->d[1] = (unsigned int)(v >> 32);
    a->n = a->d[1] ? 2 : (a->d[0] ? 1 : 0);
}

static void pl_bn_norm(PlBn *a) {
    while (a->n > 0 && a->d[a->n - 1] == 0) a->n--;
}

static void pl_bn_mul_small(PlBn *a, unsigned int m) {
    unsigned long long carry = 0;
    for (int i = 0; i < a->n; i++) {
        unsigned long long t = (unsigned long long)a->d[i] * m + carry;
        a->d[i] = (unsigned int)t;
        carry = t >> 32;
    }
    while (carry && a->n < PL_BN_LIMBS) {
        a->d[a->n++] = (unsigned int)carry;
        carry >>= 32;
    }
}

static void pl_bn_shl(PlBn *a, int k) {
    int words = k / 32, bits = k % 32;
    if (words) {
        if (a->n + words > PL_BN_LIMBS) return;  // 注意: 起きない大きさに取ってある
        for (int i = a->n - 1; i >= 0; i--) a->d[i + words] = a->d[i];
        for (int i = 0; i < words; i++) a->d[i] = 0;
        a->n += words;
    }
    if (bits) {
        unsigned int carry = 0;
        for (int i = 0; i < a->n; i++) {
            unsigned long long t = ((unsigned long long)a->d[i] << bits) | carry;
            a->d[i] = (unsigned int)t;
            carry = (unsigned int)(t >> 32);
        }
        if (carry && a->n < PL_BN_LIMBS) a->d[a->n++] = carry;
    }
}

static int pl_bn_cmp(const PlBn *a, const PlBn *b) {
    if (a->n != b->n) return a->n > b->n ? 1 : -1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->d[i] != b->d[i]) return a->d[i] > b->d[i] ? 1 : -1;
    return 0;
}

static void pl_bn_sub(PlBn *a, const PlBn *b) {  // a >= b であること
    long long borrow = 0;
    for (int i = 0; i < a->n; i++) {
        long long t = (long long)a->d[i] - (i < b->n ? b->d[i] : 0) - borrow;
        if (t < 0) {
            t += 0x100000000LL;
            borrow = 1;
        } else {
            borrow = 0;
        }
        a->d[i] = (unsigned int)t;
    }
    pl_bn_norm(a);
}

static void pl_bn_add(PlBn *a, const PlBn *b) {
    unsigned long long carry = 0;
    int n = a->n > b->n ? a->n : b->n;
    if (n > PL_BN_LIMBS) n = PL_BN_LIMBS;
    for (int i = 0; i < n; i++) {
        unsigned long long t = (unsigned long long)a->d[i] +
                               (i < b->n ? b->d[i] : 0) + carry;
        a->d[i] = (unsigned int)t;
        carry = t >> 32;
    }
    a->n = n;
    if (carry && a->n < PL_BN_LIMBS) a->d[a->n++] = (unsigned int)carry;
}

// double を f × 2^e に分解する（非正規化数も扱います）
static void pl_decompose(double v, unsigned long long *f, int *e) {
    union { double d; unsigned long long u; } cv;
    cv.d = v;
    unsigned long long frac = cv.u & 0xfffffffffffffULL;
    int be = (int)((cv.u >> 52) & 0x7ff);
    if (be == 0) {
        *f = frac;
        *e = -1074;
    } else {
        *f = frac | (1ULL << 52);
        *e = be - 1075;
    }
}

// digits に '0'〜'9' を並べ、桁数を返す。*exp10 は先頭の桁の 10 の指数。
// 注意: v は **正の有限値**であること（呼び出し側で確かめています）。
static int pl_shortest_digits(double v, char *digits, int *exp10) {
    unsigned long long f;
    int e;
    pl_decompose(v, &f, &e);

    PlBn R, S, Mp, Mm, tmp;
    if (e >= 0) {
        pl_bn_set(&R, f);
        pl_bn_shl(&R, e + 1);
        pl_bn_set(&S, 2);
        pl_bn_set(&Mp, 1);
        pl_bn_shl(&Mp, e);
        Mm = Mp;
    } else {
        pl_bn_set(&R, f);
        pl_bn_shl(&R, 1);
        pl_bn_set(&S, 1);
        pl_bn_shl(&S, 1 - e);
        pl_bn_set(&Mp, 1);
        Mm = Mp;
    }
    // 仮数がちょうど 2^52 のときは、下側の隣までの距離が半分になります
    if (f == (1ULL << 52)) {
        pl_bn_shl(&R, 1);
        pl_bn_shl(&S, 1);
        pl_bn_shl(&Mp, 1);
    }

    // 10 の指数を合わせる（見積りではなく比較で決めます）
    int k = 0;
    for (int guard = 0; guard < 400; guard++) {
        tmp = R;
        pl_bn_add(&tmp, &Mp);
        if (pl_bn_cmp(&tmp, &S) > 0) {
            pl_bn_mul_small(&S, 10);
            k++;
        } else {
            break;
        }
    }
    for (int guard = 0; guard < 400; guard++) {
        tmp = R;
        pl_bn_add(&tmp, &Mp);
        pl_bn_mul_small(&tmp, 10);
        if (pl_bn_cmp(&tmp, &S) <= 0) {
            pl_bn_mul_small(&R, 10);
            pl_bn_mul_small(&Mp, 10);
            pl_bn_mul_small(&Mm, 10);
            k--;
        } else {
            break;
        }
    }

    int n = 0;
    for (;;) {
        pl_bn_mul_small(&R, 10);
        pl_bn_mul_small(&Mp, 10);
        pl_bn_mul_small(&Mm, 10);
        int d = 0;
        while (pl_bn_cmp(&R, &S) >= 0) {
            pl_bn_sub(&R, &S);
            d++;
        }
        int low = pl_bn_cmp(&R, &Mm) < 0;
        tmp = R;
        pl_bn_add(&tmp, &Mp);
        int high = pl_bn_cmp(&tmp, &S) > 0;

        if (!low && !high) {
            digits[n++] = (char)('0' + d);
            if (n >= 18) break;   // 注意: double は 17 桁で必ず往復します
            continue;
        }
        if (low && !high) {
            digits[n++] = (char)('0' + d);
        } else if (high && !low) {
            digits[n++] = (char)('0' + d + 1);
        } else {
            PlBn r2 = R;
            pl_bn_mul_small(&r2, 2);
            digits[n++] = (char)('0' + (pl_bn_cmp(&r2, &S) >= 0 ? d + 1 : d));
        }
        break;
    }
    *exp10 = k - 1;
    return n;
}

static long long pl_ftoa(double v, char *out) {
    long long n = 0;

    // NaN は自分自身と等しくない、という性質で見分けます
    if (v != v) {
        out[0] = 'n'; out[1] = 'a'; out[2] = 'n';
        return 3;
    }
    // 無限大は ∞ - ∞ = NaN になる、という性質で見分けます
    if (v - v != v - v) {
        if (v < 0) out[n++] = '-';
        out[n++] = 'i'; out[n++] = 'n'; out[n++] = 'f';
        return n;
    }
    // 注意: -0.0 は `v < 0` が偽なので、ここだけビットで符号を見ます。
    //   落とすと float(str(-0.0)) が +0.0 になり、往復しなくなります。
    {
        union { double d; unsigned long long u; } cv;
        cv.d = v;
        if (cv.u >> 63) {
            out[n++] = '-';
            v = -v;
        }
    }
    if (v == 0.0) {
        out[n++] = '0'; out[n++] = '.'; out[n++] = '0';
        return n;
    }

    // ★ 元に戻る最短の桁を作る（Dragon4）
    char dg[24];
    int lead = 0;                                  // 先頭の桁の 10 の指数
    int nd = pl_shortest_digits(v, dg, &lead);

    // 指数表記にするか（今までと同じ境目：1e18 以上、または 1e-6 未満）
    if (lead >= 18 || lead < -6) {
        out[n++] = dg[0];
        out[n++] = '.';
        if (nd == 1) {
            out[n++] = '0';
        } else {
            for (int i = 1; i < nd; i++) out[n++] = dg[i];
        }
        out[n++] = 'e';
        int ex = lead;
        if (ex < 0) {
            out[n++] = '-';
            ex = -ex;
        }
        n += pl_utoa((unsigned long long)ex, out + n);
        return n;
    }

    if (lead >= 0) {
        // 整数部（桁が足りなければ 0 を足す）
        for (int i = 0; i <= lead; i++) out[n++] = (i < nd) ? dg[i] : '0';
        out[n++] = '.';
        if (nd > lead + 1) {
            for (int i = lead + 1; i < nd; i++) out[n++] = dg[i];
        } else {
            out[n++] = '0';   // 最低 1 桁は残す（1 → "1.0"）
        }
        return n;
    }

    // 0.000… の形
    out[n++] = '0';
    out[n++] = '.';
    for (int i = 0; i < -lead - 1; i++) out[n++] = '0';
    for (int i = 0; i < nd; i++) out[n++] = dg[i];
    return n;
}

void pl_print_float(double v) {
    char buf[64];
    long long n = pl_ftoa(v, buf);
    buf[n++] = '\n';
    pl_hook_write(buf, n);
}

void pl_print_int(long long v) {
    char buf[26];
    long long n = pl_itoa(v, buf);
    buf[n++] = '\n';
    pl_hook_write(buf, n);
}

// stdout にそのまま書く（改行を足さない）。
// ★ print は改行を足すので、IR の出力には使えません。
void pl_print_raw(const char *s) { pl_hook_write(s, pl_cstr_len(s)); }


void pl_print_str(const char *s) {
    pl_hook_write(s, pl_cstr_len(s));
    pl_hook_write("\n", 1);
}

void pl_print_bool(long long v) {
    if (v) pl_hook_write("True\n", 5);
    else pl_hook_write("False\n", 6);
}

// ── 文字列 ─────────────────────────────────────────────────
//
// ★ str の表現に「長さ」を持たせました。
//
//     [ i64 長さ ][ バイト列 ... ][ '\0' ]
//                  ^ str の値が指すのはここ
//
//   注意: なぜ変えたか
//     それまでの str は「ただの NUL 終端文字列」でした。すると
//     len(s) も s[i] も毎回 strlen する＝ O(n) になり、
//     字句解析器のように 1 文字ずつ回るコードが O(n^2) になります。
//     4000 行のソースを読むだけで数秒かかる計算で、セルフホストできません。
//
//   ★ 値が指すのは「データの先頭」のままなので、C から見ると今までどおり
//     NUL 終端の char * です（extern に渡してもそのまま使えます）。
//     長さは p[-1] の位置（8 バイト手前）にあります。

// 文字列リテラルの印。
//
// ★ なぜ必要か
//   `s: str = "abc"` の "abc" は **プログラムに埋め込まれた定数**（.rodata）で、
//   ヒープではありません。解放しようとすると落ちます。
//   実行時にポインタだけを見て「ヒープか定数か」を判定する移植性のある方法は
//   無いので、**長さのヘッダに 1 ビットの印**を付けて区別します。
//
//   ヘッダ（8 バイト手前）:  [ 静的ビット | 長さ ]
//
// 注意: 印を付けるのは codegen（--drop のとき）です。ランタイム側で作る文字列は
//    すべてヒープなので、印は付きません。
#define PL_STR_STATIC (1LL << 62)

// 長さ len のバイト列を置ける str を確保する（NUL の分も含めて確保）。
char *pl_str_alloc(long long len) {
    char *base = pl_alloc(8 + len + 1);
    *(long long *)base = len;
    return base + 8;
}

// C 文字列から str を作る（argv など、ヘッダを持たない文字列から作るとき）
char *pl_str_from_cstr(const char *s) {
    long long n = (long long)pl_cstr_len(s);
    char *p = pl_str_alloc(n);
    pl_memcpy(p, s, (long long)n + 1);
    return p;
}

// ★ O(1) になりました。
//
// 注意: ヘッダの最上位ビットの 1 つを「静的な文字列」の印に使うので、
//    長さを読むときは必ず落とします（下の PL_STR_STATIC を参照）。
long long pl_str_len(const char *s) {
    return ((const long long *)s)[-1] & ~PL_STR_STATIC;
}

// 文字列の繰り返し（"ab" * 3）
char *pl_str_repeat(const char *s, long long n) {
    if (n < 0) n = 0;
    long long m = pl_str_len(s);
    char *out = pl_str_alloc(m * n);
    for (long long k = 0; k < n; k++) pl_memcpy(out + k * m, s, m);
    out[m * n] = '\0';
    return out;
}

char *pl_str_concat(const char *a, const char *b) {
    long long la = pl_str_len(a);
    long long lb = pl_str_len(b);
    char *p = pl_str_alloc(la + lb);
    pl_memcpy(p, a, (long long)la);
    pl_memcpy(p + la, b, (long long)lb);
    p[la + lb] = '\0';
    return p;
}

// strcmp の符号をそのまま返す。
// ★ これ 1 つで == != < <= > >= の 6 種類すべてに使えます
//   （生成側は結果を 0 と比べる述語を変えるだけ）。
long long pl_str_cmp(const char *a, const char *b) {
    // 注意: libc の strcmp は使えません（core は libc に触らない）。
    //    符号だけ分かればよいので、自分で比べます。
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (*x && *x == *y) {
        x++;
        y++;
    }
    if (*x == *y) return 0;
    return *x < *y ? -1 : 1;
}

char *pl_str_from_int(long long v) {
    char buf[26];
    long long n = pl_itoa(v, buf);
    char *p = pl_str_alloc(n);
    pl_memcpy(p, buf, n + 1);
    return p;
}

char *pl_str_from_float(double v);   // 下で定義

// ── f-string の書式指定（A-45）─────────────────────────────
//
// ★ 表示幅で数えます（全角は 2 桁）。バイト数で数えると日本語の表が崩れます。
//   判定の範囲は lib/strings の is_wide と**同じもの**です。
static int pl_is_wide_cp(long long cp) {
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
           (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6);
}

long long pl_str_width(const char *s) {
    long long n = pl_str_len(s), w = 0, i = 0;
    while (i < n) {
        unsigned char b = (unsigned char)s[i];
        if (b < 0x80) { w++; i++; continue; }
        long long len = b >= 0xF0 ? 4 : (b >= 0xE0 ? 3 : 2);
        long long cp = b & (b >= 0xF0 ? 0x07 : (b >= 0xE0 ? 0x0F : 0x1F));
        for (long long k = 1; k < len && i + k < n; k++)
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        w += pl_is_wide_cp(cp) ? 2 : 1;
        i += len;
    }
    return w;
}

// 表示幅 width まで詰める。align は 0=左寄せ / 1=右寄せ / 2=中央。
//
// 注意: **既に width を超えていたら、そのまま返します**（切りません）。
//   切ると、表の桁は揃っても中身が読めなくなります。
char *pl_str_pad(const char *s, long long width, long long align, long long fill) {
    long long n = pl_str_len(s);
    long long w = pl_str_width(s);
    if (w >= width) {
        char *c = pl_str_alloc(n);
        pl_memcpy(c, s, n + 1);
        return c;
    }
    long long pad = width - w;
    long long left = align == 1 ? pad : (align == 2 ? pad / 2 : 0);
    long long right = pad - left;
    char *out = pl_str_alloc(n + pad);
    long long k = 0;
    for (long long i = 0; i < left; i++) out[k++] = (char)fill;
    pl_memcpy(out + k, s, n);
    k += n;
    for (long long i = 0; i < right; i++) out[k++] = (char)fill;
    out[k] = '\0';
    return out;
}

// 小数点以下を prec 桁に固定して文字列にする（四捨五入。0 から遠いほうへ）。
//
// 注意: **大きすぎる値は最短表現に逃がします**（i64 に入らないため）。
//   表を出すための機能なので、そこまでの値は想定しません。
char *pl_fmt_f64(double v, long long prec) {
    if (v != v || v - v != v - v) return pl_str_from_float(v);
    if (prec < 0) prec = 0;
    if (prec > 17) prec = 17;

    int neg = 0;
    {
        union { double d; unsigned long long u; } cv;
        cv.d = v;
        if (cv.u >> 63) { neg = 1; v = -v; }
    }
    double scale = 1.0;
    for (long long i = 0; i < prec; i++) scale *= 10.0;
    double scaled = v * scale + 0.5;
    if (scaled >= 9.2233720368547758e18) return pl_str_from_float(neg ? -v : v);

    unsigned long long fixed = (unsigned long long)scaled;
    char digits[32];
    long long nd = 0;
    if (fixed == 0) digits[nd++] = '0';
    while (fixed > 0) { digits[nd++] = (char)('0' + (int)(fixed % 10)); fixed /= 10; }
    while (nd <= prec) digits[nd++] = '0';   // 0.0x のぶんを足す

    long long total = neg + nd + (prec > 0 ? 1 : 0);
    char *out = pl_str_alloc(total);
    long long k = 0;
    if (neg) out[k++] = '-';
    for (long long i = nd - 1; i >= 0; i--) {
        out[k++] = digits[i];
        if (prec > 0 && i == prec) out[k++] = '.';
    }
    out[k] = '\0';
    return out;
}

char *pl_str_from_float(double v) {
    char buf[64];
    long long n = pl_ftoa(v, buf);
    buf[n] = '\0';
    char *p = pl_str_alloc(n);
    pl_memcpy(p, buf, n + 1);
    return p;
}

// int ↔ float の変換（言語仕様 7 節）。
//
// ★ 命令 1 つ（sitofp / fptosi）で済みますが、**組み込み関数の仕組みに
//   そのまま乗せる**ためにランタイム関数にしています。コード生成器に
//   float 専用の分岐を増やさずに済みます。
// 注意: float → int は **0 方向への切り捨て**です（-1.7 → -1）。
//    Python の int() と同じで、round() ではありません。
double pl_float_from_int(long long v) { return (double)v; }
// float → int（0 方向へ切り捨て）
//
// ★ **範囲の外を黙って通しません。** C の (long long) キャストは
//   範囲外だと未定義動作で、実際には int の最小値が返っていました
//   （int(1.0e30) が -9223372036854775808）。NaN も同じです。
//   注意: 境界は正確です。9223372036854775808.0（＝ int の最大値 + 1）は
//     double でちょうど表せるので、これ以上を弾けば足ります。
//     最小値のほうは -9223372036854775808.0 が表せるので、そのものは通します。
long long pl_int_from_float(double v) {
    if (v != v) pl_panic("int(): not a number (NaN)");
    if (v >= 9223372036854775808.0 || v < -9223372036854775808.0)
        pl_panic("int(): out of range");
    return (long long)v;
}

char *pl_str_from_bool(long long v) {
    return pl_str_from_cstr(v ? "True" : "False");
}

// 文字列を整数にする。パースできなければ実行時エラー（言語仕様 7 節）。
long long pl_str_to_int(const char *s) {
    // 注意: libc の strtoll は使えないので、自分で読みます。
    //    ★ v1 と同じ規則：符号 1 個 + 数字 1 個以上、余りがあれば panic。
    const char *p = s;
    int neg = 0;
    if (*p == '-' || *p == '+') {
        neg = *p == '-';
        p++;
    }
    if (*p < '0' || *p > '9') pl_panic("int(): not a number");

    // ★ **桁があふれたら panic します。**
    //   int("99999999999999999999") が黙って 7766279631452241919 に
    //   なっていました。
    //   注意: 負の側は 1 つ広い（-9223372036854775808 まで）ので、
    //     符号を見て上限を変えます。累算は符号なしで行い、
    //     1 桁進めるたびに上限と比べます。
    unsigned long long limit = neg ? 9223372036854775808ULL
                                   : 9223372036854775807ULL;
    unsigned long long v = 0;
    while (*p >= '0' && *p <= '9') {
        unsigned long long d = (unsigned long long)(*p - '0');
        if (v > (limit - d) / 10ULL)
            pl_panic("int(): out of range");
        v = v * 10ULL + d;
        p++;
    }
    if (*p != '\0') pl_panic("int(): not a number");
    // 注意: -9223372036854775808 は long long の正の側に無いので、
    //   符号なしのまま否定してから変換します。
    if (neg) return (long long)(0ULL - v);
    return (long long)v;
}

// ── 文字列 → float ────────────────────────────────────
//
// ★ **正しく丸めます**（いちばん近い double を返します）。素朴に
//   「桁を足しながら 10 倍」すると誤差が積もり、`float(str(x))` が x に
//   戻らなくなります。ここでは十進を M × 10^E の**分数**として持ち、
//   多倍長で割って仮数を取り出します（pl_ftoa の裏返しです）。
//
// 注意: 有効数字は **780 桁**まで見ます。double の丸めの境目（中点）を
//    十進で書くのに要るのは高々 767 桁なので、これで**どんな入力でも
//    正しく丸まります**。落とした桁に 0 でないものがあれば sticky を
//    立てるので、「ちょうど中点」を取り違えることもありません。
#define PL_MAX_SIG 780

static double pl_bits_to_double(unsigned long long bits, int neg) {
    union { double d; unsigned long long u; } cv;
    cv.u = neg ? (bits | 0x8000000000000000ULL) : bits;
    return cv.d;
}

static double pl_inf_of(int neg) {
    return pl_bits_to_double(0x7ff0000000000000ULL, neg);
}

static int pl_bn_bitlen(const PlBn *a) {
    if (a->n == 0) return 0;
    unsigned int top = a->d[a->n - 1];
    int b = 0;
    while (top) {
        b++;
        top >>= 1;
    }
    return (a->n - 1) * 32 + b;
}

// dig[0..nd) を整数 M とみて、M × 10^exp10 に**いちばん近い** double を返す。
// sticky は「これより下に 0 でない桁がある」ことを表します。
static double pl_dec_to_double(const unsigned char *dig, int nd, int exp10,
                               int sticky, int neg) {
    if (nd == 0) return pl_bits_to_double(0, neg);

    // 10 進のけた数だけで決まるものを先にはじきます
    //   10^(nd-1+exp10) <= 値 < 10^(nd+exp10)
    if (nd + exp10 > 310) return pl_inf_of(neg);
    if (nd + exp10 < -324) return pl_bits_to_double(0, neg);

    PlBn num, den, t;
    pl_bn_set(&num, 0);
    for (int i = 0; i < nd; i++) {
        PlBn d;
        pl_bn_mul_small(&num, 10);
        pl_bn_set(&d, (unsigned long long)dig[i]);
        pl_bn_add(&num, &d);
    }
    pl_bn_norm(&num);
    if (num.n == 0) return pl_bits_to_double(0, neg);

    pl_bn_set(&den, 1);
    if (exp10 > 0)
        for (int i = 0; i < exp10; i++) pl_bn_mul_small(&num, 10);
    else if (exp10 < 0)
        for (int i = 0; i < -exp10; i++) pl_bn_mul_small(&den, 10);

    // num / den を [1, 2) に入れる。以後 値 = (num/den) × 2^e2
    int e2 = pl_bn_bitlen(&num) - pl_bn_bitlen(&den);
    if (e2 > 0)
        pl_bn_shl(&den, e2);
    else if (e2 < 0)
        pl_bn_shl(&num, -e2);
    // 桁数から求めた e2 は高々 1 ビットずれます。比べて合わせます。
    if (pl_bn_cmp(&num, &den) < 0) {
        pl_bn_shl(&num, 1);
        e2--;
    } else {
        t = den;
        pl_bn_shl(&t, 1);
        if (pl_bn_cmp(&num, &t) >= 0) {
            pl_bn_shl(&den, 1);
            e2++;
        }
    }

    if (e2 > 1023) return pl_inf_of(neg);

    // 取り出すビット数。非正規化数の範囲では 53 より少なくします
    //   （いちばん下の桁の重みを 2^-1074 に固定するため）
    int nbits = 53;
    if (e2 < -1022) nbits = e2 + 1075;
    if (nbits < 1) {
        // 2^-1075（最小の非正規化数の半分）との比較だけで決まります
        if (e2 == -1075 && (pl_bn_cmp(&num, &den) > 0 || sticky))
            return pl_bits_to_double(1, neg);
        return pl_bits_to_double(0, neg);
    }

    // 二進の長除法で nbits ビットを取り出す
    unsigned long long q = 0;
    for (int i = 0; i < nbits; i++) {
        q <<= 1;
        if (pl_bn_cmp(&num, &den) >= 0) {
            pl_bn_sub(&num, &den);
            q |= 1;
        }
        if (i != nbits - 1) pl_bn_shl(&num, 1);
    }

    // 端数を丸める（最近接・偶数寄せ。sticky があれば中点より上）
    t = num;
    pl_bn_shl(&t, 1);
    int c = pl_bn_cmp(&t, &den);
    if (c > 0 || (c == 0 && (sticky || (q & 1)))) q++;

    if (nbits < 53) {
        // 非正規化数。q がそのまま仮数で、指数部は 0。
        // 注意: 丸めで q が 2^52 に達したときは、そのビット列が
        //   ちょうど「最小の正規化数」になります（何もしなくて正しい）。
        return pl_bits_to_double(q, neg);
    }
    if (q == (1ULL << 53)) {
        q >>= 1;
        e2++;
        if (e2 > 1023) return pl_inf_of(neg);
    }
    unsigned long long bits =
        ((unsigned long long)(e2 + 1023) << 52) | (q & 0xfffffffffffffULL);
    return pl_bits_to_double(bits, neg);
}

static int pl_is_space_ch(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

static int pl_ci_match(const char *p, const char *w) {
    for (; *w; p++, w++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != *w) return 0;
    }
    return 1;
}

// 文字列を float にする。読めなければ実行時エラー（int(str) と同じ規則）。
//
// 受け付ける形（Python の float() に合わせています）：
//   [空白] [+|-] ( 数字[.数字][e[+|-]数字] | "inf" | "infinity" | "nan" ) [空白]
double pl_str_to_float(const char *s) {
    const char *p = s;
    while (pl_is_space_ch(*p)) p++;

    int neg = 0;
    if (*p == '+' || *p == '-') {
        neg = *p == '-';
        p++;
    }

    if (pl_ci_match(p, "infinity")) {
        p += 8;
        goto tail;
    }
    if (pl_ci_match(p, "inf")) {
        p += 3;
        goto tail;
    }
    if (pl_ci_match(p, "nan")) {
        p += 3;
        while (pl_is_space_ch(*p)) p++;
        if (*p != '\0') pl_panic("float(): not a number");
        return pl_bits_to_double(0x7ff8000000000000ULL, 0);
    }

    {
        unsigned char dig[PL_MAX_SIG];
        int nd = 0;      // 取り込んだ有効桁の数
        int exp10 = 0;   // dig を整数とみたときに掛ける 10 の冪
        int sticky = 0;  // 落とした桁に 0 でないものがあったか
        int seen = 0;    // 数字を 1 つでも見たか
        int lead = 1;    // まだ先頭の 0 の並びの中か

        for (; *p >= '0' && *p <= '9'; p++) {
            seen = 1;
            if (*p == '0' && lead) continue;
            lead = 0;
            if (nd < PL_MAX_SIG) {
                dig[nd++] = (unsigned char)(*p - '0');
            } else {
                if (*p != '0') sticky = 1;
                exp10++;  // 落とした整数の桁は 10 の冪として残す
            }
        }
        if (*p == '.') {
            p++;
            for (; *p >= '0' && *p <= '9'; p++) {
                seen = 1;
                if (*p == '0' && lead) {
                    exp10--;  // 0.000123 の先頭側の 0
                    continue;
                }
                lead = 0;
                if (nd < PL_MAX_SIG) {
                    dig[nd++] = (unsigned char)(*p - '0');
                    exp10--;
                } else if (*p != '0') {
                    sticky = 1;
                }
            }
        }
        if (!seen) pl_panic("float(): not a number");

        if (*p == 'e' || *p == 'E') {
            p++;
            int eneg = 0;
            if (*p == '+' || *p == '-') {
                eneg = *p == '-';
                p++;
            }
            if (*p < '0' || *p > '9') pl_panic("float(): not a number");
            long long ev = 0;
            for (; *p >= '0' && *p <= '9'; p++)
                if (ev < 1000000) ev = ev * 10 + (*p - '0');
            if (eneg) ev = -ev;
            // 注意: exp10 は int なので、極端な指数は先に潰しておきます
            //   （100000 を超えれば、どのみち inf か 0 です）
            if (ev > 100000) ev = 100000;
            if (ev < -100000) ev = -100000;
            exp10 += (int)ev;
        }

        while (pl_is_space_ch(*p)) p++;
        if (*p != '\0') pl_panic("float(): not a number");
        return pl_dec_to_double(dig, nd, exp10, sticky, neg);
    }

tail:
    while (pl_is_space_ch(*p)) p++;
    if (*p != '\0') pl_panic("float(): not a number");
    return pl_inf_of(neg);
}

// ★ float の 0 除算。整数の `//` `%` と同じく止めます。
//   注意: 「inf を返す」ほうが IEEE754 の既定ですが、それだと
//     「どこで壊れたか」が分からないまま nan が伝わります。
_Noreturn void pl_fdiv_zero_fail(void) {
    pl_panic("float division by zero");
}

long long pl_ord(const char *s) {
    if (s[0] == '\0') pl_panic("ord(): empty string");
    return (long long)(unsigned char)s[0];
}

char *pl_chr(long long v) {
    if (v < 0 || v > 255) pl_panic("chr(): out of range");
    char *p = pl_str_alloc(1);
    p[0] = (char)v;
    p[1] = '\0';
    return p;
}

// ── 検査つきの算術（規約 R10）──────────────────────────────
//
// ★ 0 除算は SIGFPE でプロセスが死にます。何が起きたか分からないより、
//   メッセージを出して死ぬほうがずっと親切です。
//   分岐を IR に出さず、ランタイム関数に押し込むのが R10 の実践です。

// ★ '//' は **切り捨て（truncate）ではなく切り下げ（floor）** です。
//
//   C の '/' は 0 の方向へ丸めるので -7 / 2 == -3 になります。本言語は
//   Python と同じく負の無限大の方向へ丸め、-7 // 2 == -4 とします。
//   「Python の書きやすさは絶対」（ロードマップ §0）に従った判断です。
//
//   注意: 商だけ直して余りを直さないと a == (a // b) * b + a % b が崩れます。
//     この等式が成り立つことは tests/cases/floordiv_sign で固定しています。
//
//   注意: 検査は 1 つ増えますが、どのみち 0 除算のためにこの関数を通るので
//     命令数の増分だけです（呼び出しは元から 1 回）。
long long pl_floordiv(long long a, long long b) {
    if (b == 0) pl_panic("division by zero");
    // ★ PL_LLONG_MIN / -1 は C では未定義動作で、実際には SIGFPE で落ちます。
    //   何が起きたか分からないまま死ぬより、名前を付けて死にます（規約 R10）。
    if (b == -1 && a == PL_LLONG_MIN) pl_panic("integer overflow in //");
    long long q = a / b;
    // 符号が食い違っていて、割り切れていないときだけ 1 つ下げる。
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

// 余りは **除数と同じ符号** になります（Python と同じ）。
//   -7 % 2 == 1 / 7 % -2 == -1
// ── 桁あふれ ────────────────────────────────────────
//
// ★ codegen が出す `llvm.sadd/ssub/smul.with.overflow` の失敗側から呼ばれます。
//   注意: **戻りません。** IR は直後に unreachable を置きます。
//   注意: 宣言には noreturn と cold の両方が付きます。付けないと
//     この呼び出しの費用が、囲む関数のインライン化の見積りに入ります。
//
// 引数は演算の種類です。文字列を渡すと演算のたびに大域定数が増えるので、
// 番号にしてメッセージはこちら側に持ちます。
void pl_overflow_fail(long long op) {
    if (op == 0) pl_panic("integer overflow in +");
    if (op == 1) pl_panic("integer overflow in -");
    if (op == 2) pl_panic("integer overflow in *");
    // 4 は添字の計算。演算子の種類は言いません
    //   — 添字の式の中の + - * を 1 つの旗にまとめて見ているためです。
    if (op == 4) pl_panic("integer overflow in index computation");
    pl_panic("integer overflow in unary -");
}

// ── 範囲型（部分型。A-28）──
//
// ★ 範囲の外の値を入れようとしたときに呼ばれます。
//   注意: **戻りません**（IR は直後に unreachable を置きます）。
//
// 型の名前は大域定数として IR に 1 つ置かれ、そのポインタが渡ります。
// 数は 3 つとも i64 です（メッセージの組み立てはこちら側の仕事）。
void pl_range_fail(const char *name, long long v, long long lo, long long hi) {
    // ★ snprintf は使いません（ベアメタルでも同じコードが動くように）。
    char buf[256];
    long long k = 0;
    const char *head = "value out of range: ";
    for (const char *q = head; *q && k < 200; q++) buf[k++] = *q;
    for (const char *q = name; *q && k < 200; q++) buf[k++] = *q;
    const char *mid = " accepts ";
    for (const char *q = mid; *q && k < 220; q++) buf[k++] = *q;
    k += pl_itoa(lo, buf + k);
    buf[k++] = '.';
    buf[k++] = '.';
    k += pl_itoa(hi, buf + k);
    const char *mid2 = " but got ";
    for (const char *q = mid2; *q && k < 240; q++) buf[k++] = *q;
    k += pl_itoa(v, buf + k);
    buf[k] = '\0';
    pl_panic(buf);
}

// ── 契約（事前条件・事後条件。A-29）──
//
// ★ メッセージはコンパイル時に組み立てて大域定数に置いてあります
//   （"requires of divide (line 12)" のような文字列）。
//   注意: **戻りません**（IR は直後に unreachable を置きます）。
void pl_contract_fail(const char *what) {
    char buf[256];
    long long k = 0;
    const char *head = "contract violated: ";
    for (const char *q = head; *q && k < 200; q++) buf[k++] = *q;
    for (const char *q = what; *q && k < 250; q++) buf[k++] = *q;
    buf[k] = '\0';
    pl_panic(buf);
}

// ── 証明の誤りを捕まえる（A-34 の --verify-prove）──
//
// ★ 「消せる」と判断した検査を**残したまま**建てたときに、その検査が
//   外れたら呼ばれます。つまり **証明器が間違えた**ということです。
//   注意: 利用者のコードの誤りではないので、ふつうの診断と分けます。
void pl_prove_fail(const char *what) {
    char buf[256];
    long long k = 0;
    const char *head = "prover was wrong (this is a compiler bug): ";
    for (const char *q = head; *q && k < 200; q++) buf[k++] = *q;
    for (const char *q = what; *q && k < 250; q++) buf[k++] = *q;
    buf[k] = '\0';
    pl_panic(buf);
}

long long pl_mod(long long a, long long b) {
    if (b == 0) pl_panic("division by zero");
    // ★ こちらの答えは 0 で確定していますが、a % b の計算自体が
    //   PL_LLONG_MIN % -1 で落ちるので、割る前に返します。
    if (b == -1) return 0;
    long long r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

// 掛け算があふれたかを見る。あふれたら 1 を返す。
//
// ★ ハードウェアの桁あふれフラグを使う組み込み（__builtin_mul_overflow）は
//   使いません。ベアメタル（RISC-V）でライブラリ呼び出しに化けないことを
//   保証したいので、割り戻して確かめる形にしてあります。
// 注意: -1 を先に外すのは、p / b が b == -1 で未定義動作になるためです。
static int pl_mul_ovf(long long a, long long b, long long *out) {
    if (a == 0 || b == 0) { *out = 0; return 0; }
    if (a == -1) { if (b == PL_LLONG_MIN) return 1; *out = -b; return 0; }
    if (b == -1) { if (a == PL_LLONG_MIN) return 1; *out = -a; return 0; }
    // 折り返しは符号なしで行う（符号付きの桁あふれは未定義動作のため）
    long long p = (long long)((unsigned long long)a * (unsigned long long)b);
    if (p / b != a) return 1;
    *out = p;
    return 0;
}

// 繰り返し二乗法。ループがあるので当然ランタイム側（R10）。
// 負の指数は int で表せないので実行時エラーにします（先送りしていた宿題）。
//
// ★ **あふれたら panic します。** 2 ** 64 が黙って 0 を返していました。
//   注意: 二乗は「次の周がある」ときだけ行います。最後の周でも二乗していた
//     元の形だと、答えは正しいのに途中の二乗だけがあふれて
//     **誤検出**になります（2 ** 62 など）。
long long pl_ipow(long long base, long long exp) {
    if (exp < 0) pl_panic("negative exponent");
    long long r = 1;
    while (exp > 0) {
        if (exp & 1) {
            if (pl_mul_ovf(r, base, &r)) pl_panic("integer overflow in **");
        }
        exp >>= 1;
        if (exp > 0) {
            if (pl_mul_ovf(base, base, &base)) pl_panic("integer overflow in **");
        }
    }
    return r;
}

// float のべき乗
//
// ★ **libc の pow は使えません**（ベアメタルで動く必要があるため）。
//   lib/math と同じ手で自前に持ちます:
//     指数が整数なら **繰り返し二乗法**（負の底も扱える／誤差も小さい）
//     そうでなければ exp(y·log(x))
//
// 注意: math の exp / log と **同じアルゴリズム**にしてあります。
//   片方だけ直すと `2.0 ** 0.5` と `math.pow(2.0, 0.5)` がずれます。
static double pl_exp_(double x) {
    if (x != x) return x;
    if (x > 710.0) return 1.0e308 * 10.0;
    if (x < -746.0) return 0.0;

    const double LN2 = 0.6931471805599453;
    double q = x / LN2;
    long long k = (long long)(q >= 0 ? q + 0.5 : q - 0.5);
    double r = x - (double)k * LN2;

    double term = 1.0, s = 1.0;
    for (int n = 1; n <= 14; n++) {
        term = term * r / (double)n;
        s += term;
    }
    // 2^k を掛ける
    double out = s;
    if (k >= 0) for (long long i = 0; i < k; i++) out *= 2.0;
    else        for (long long i = 0; i < -k; i++) out /= 2.0;
    return out;
}

static double pl_log_(double x) {
    double inf = 1.0e308 * 10.0;
    if (x != x || x < 0.0) return inf - inf;   // NaN
    if (x == 0.0) return -inf;
    if (x > 1.0e308) return x;

    const double LN2 = 0.6931471805599453;
    double m = x;
    long long k = 0;
    while (m >= 2.0) { m /= 2.0; k++; }
    while (m < 1.0)  { m *= 2.0; k--; }

    double z = (m - 1.0) / (m + 1.0);
    double z2 = z * z, term = z, s = z;
    for (int n = 3; n <= 33; n += 2) {
        term *= z2;
        s += term / (double)n;
    }
    return 2.0 * s + (double)k * LN2;
}

double pl_fpow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x != x || y != y) return x != x ? x : y;   // NaN
    if (x == 1.0) return 1.0;

    // 指数が整数なら繰り返し二乗法（負の底もここで扱える）
    double ty = y < 0 ? -y : y;
    if (ty <= 1024.0 && (double)(long long)ty == ty) {
        long long n = (long long)ty;
        double base = x, r = 1.0;
        while (n > 0) {
            if (n & 1) r *= base;
            base *= base;
            n >>= 1;
        }
        return y < 0.0 ? 1.0 / r : r;
    }

    if (x < 0.0) {
        double inf = 1.0e308 * 10.0;
        return inf - inf;      // 負の数の非整数乗は実数にならない → NaN
    }
    if (x == 0.0) return y > 0.0 ? 0.0 : 1.0e308 * 10.0;
    return pl_exp_(y * pl_log_(x));
}

// ── プロセス ───────────────────────────────────────────────


// ── ファイル入出力 ────────────────────────────────────
//
// ★ ここは「C でしか書けないもの」です。本言語で書けるものは lib/* に置きます
//
// 注意: 失敗したら panic で落とします。エラー値を返して利用者に検査させる形は
//    `T | None` がまだ無いので書けません。




// ── コマンドライン引数と外部コマンド ──────────────────
//
// ★ この 2 つが無いと、セルフホスト版コンパイラは「コマンド」になれません
//   （docs/design/self-hosting.md 3.4）。






// ── list[T] ────────────────────────────────────────────
//
// ★ 要素はすべて 8 バイトに統一します（int/bool は i64、str/list はポインタ）。
//   要素サイズが型ごとに違うと getelementptr のオフセット計算が型ごとに
//   変わりますが、8 バイト固定なら「long long の配列」と「void* の配列」の
//   2 種類だけで済みます。bool で 7 バイト無駄になりますが、
//   実装の単純さと引き換えにするなら安い代償です。

typedef struct {
    void *data;  // 要素の配列（8 バイト × cap）
    long long len;
    long long cap;
} PlList;

PlList *pl_list_new(void) {
    PlList *l = pl_alloc((long long)sizeof(PlList));
    l->cap = 4;
    l->data = pl_alloc(l->cap * 8);
    l->len = 0;
    return l;
}

long long pl_list_len(PlList *l) { return l->len; }

// 要素の並びの先頭を返す（A-33）。
//
// ★ **C のライブラリに配列をそのまま渡すための口**です。要素は 8 バイトずつ
//   連続して並んでいるので、`list[float]` の中身は C から見れば `double*`、
//   `list[int]` は `long long*` です。写す必要はありません。
//
// 注意: 返すのは**借りもの**です。`append` で伸びると別の場所へ移ることがあるので、
//   **渡したあいだは list を変えないでください**（設計 ffi.md）。
void *pl_list_data(PlList *l) { return l->data; }

// 注意: realloc を使わないのは「一度渡したポインタは永久に有効」という
//    方針（メモリモデル 3 節）と噛み合わないためです。
//    memcpy して古い領域を捨てるほうが、方針と一貫します。
//
// ★ 倍々に増やすので、n 回の append の総コストは O(n) です。
static void pl_list_grow(PlList *l) {
    if (l->len < l->cap) return;
    long long ncap = l->cap * 2;
    void *nd = pl_alloc(ncap * 8);
    pl_memcpy(nd, l->data, (long long)l->len * 8);
    l->data = nd;
    l->cap = ncap;
}

// 範囲検査（規約 R10）。
//
// 注意: 負の添字は **末尾からの位置**です（Python と同じ）。
//   xs[-1] が最後の要素。呼ぶ側で正規化してから渡します。
//
// ★ **ここはもう「検査の置き場所」ではありません。**
//   以前は「検査をここに置くので、生成する IR に分岐が 1 つも出ない」と
//   書いていましたが、その呼び出しこそが最大のボトルネックでした。
//   いまは codegen が検査ごと IR に展開します（gen_index_addr）。
//   **IR には分岐が出ます。** そのぶん LLVM が検査をループの外へ
//   持ち上げたり、範囲が自明なときに消したりできます。

// 範囲外だったときだけ呼ばれる出口。
// 注意: **戻ってきません**。呼び出し側の IR は直後に unreachable を置きます。
// 注意: 3 つめの引数は「添字の計算で桁があふれたか」です。
//   あふれたときは i に意味がない（折り返した値）ので、数を出しません。
void pl_index_fail(long long i, long long len, long long overflowed) {
    if (overflowed) pl_panic("integer overflow in index computation");
    char buf[80];
    char *w = buf;
    const char *m = "index out of range: ";
    while (*m) *w++ = *m++;
    w += pl_itoa(i, w);
    *w++ = ' ';
    *w++ = '(';
    w += pl_itoa(len, w);
    *w++ = ')';
    *w = '\0';
    pl_panic(buf);
}

// ★ 展開後の codegen はもう呼びませんが、ランタイム内から使います。
static void pl_list_check(PlList *l, long long i) {
    if (i < 0 || i >= l->len) pl_index_fail(i, l->len, 0);
}

// 部分文字列の位置（'in' 演算子）。無ければ -1
//
// 注意: 素朴な走査（O(n·m)）です。KMP のような高速化はしていません。
//   'in' の用途では入力が短いことがほとんどで、コードの短さを優先しました。
// ★ 空文字列はどこにでも含まれるので 0 を返します（Python と同じ）。
long long pl_str_find(const char *hay, const char *needle) {
    long long n = pl_str_len(hay);
    long long m = pl_str_len(needle);
    if (m == 0) return 0;
    if (m > n) return -1;
    for (long long i = 0; i + m <= n; i++) {
        long long k = 0;
        while (k < m && hay[i + k] == needle[k]) k++;
        if (k == m) return i;
    }
    return -1;
}

// 部分文字列。**新しい文字列を作って返します**
//
// 注意: 範囲は Python と同じ規則で丸めます。**範囲外でも落ちません**
//   （添字と違い、スライスは「はみ出したぶんは無い」と読むのが自然なため）。
//     負の値 → 0、長さを超える → 長さ、開始 > 終端 → 空
char *pl_str_slice(const char *s, long long lo, long long hi) {
    long long n = pl_str_len(s);
    // ★ 負の端は末尾から数えます（s[:-1] で最後の 1 文字を落とす）
    if (lo < 0) lo += n;
    if (hi < 0) hi += n;
    if (lo < 0) lo = 0;
    if (hi > n) hi = n;
    if (lo > hi) lo = hi;
    long long m = hi - lo;
    char *out = pl_str_alloc(m);
    pl_memcpy(out, s + lo, m);
    out[m] = '\0';
    return out;
}

// ── 探索（'in' 演算子と list のメソッド） ────────────────
//
// ★ 見つかった位置を返し、無ければ -1。'in' はこの結果を >= 0 と比べます。
//   注意: 位置を返す形にしておくと index() にもそのまま使えます。

long long pl_list_index_i64(PlList *l, long long v) {
    for (long long i = 0; i < l->len; i++)
        if (((long long *)l->data)[i] == v) return i;
    return -1;
}

// 注意: float は **ビットではなく数値として**比べます。ビットで比べると
//    0.0 と -0.0 が別物になり、NaN が自分自身と一致してしまいます。
long long pl_list_index_f64(PlList *l, double v) {
    for (long long i = 0; i < l->len; i++) {
        double x;
        long long bits = ((long long *)l->data)[i];
        pl_memcpy(&x, &bits, 8);
        if (x == v) return i;
    }
    return -1;
}

// ポインタの同一性で比べます（クラス・list の 'in' はこちら。== と同じ意味）
long long pl_list_index_ptr(PlList *l, void *v) {
    for (long long i = 0; i < l->len; i++)
        if (((void **)l->data)[i] == v) return i;
    return -1;
}

// str は **中身**で比べます（== と同じ）
long long pl_list_index_str(PlList *l, const char *v) {
    for (long long i = 0; i < l->len; i++) {
        const char *x = ((const char **)l->data)[i];
        if (x == v) return i;
        if (x && v && pl_str_cmp(x, v) == 0) return i;
    }
    return -1;
}

void pl_list_push_i64(PlList *l, long long v);   // 下で定義

// ── list を文字列にする ──────────────────────────────────
//
// ★ `print(xs)` / `str(xs)` のためのものです。要素の型ごとに 1 本ずつ
//   用意します。**要素の型はコンパイル時に決まっている**ので、
//   どれを呼ぶかは codegen が選べます。
//
// 注意: 形は Python に寄せます: [1, 2, 3] / ["a", "b"] / [True, False]
//   文字列だけ引用符で囲むのは、空文字や空白を含む要素が見えるようにするためです。
//
// 注意: **入れ子（list[list[int]]）は対象外です。** 要素をさらに文字列に
//   する手立てが要るためで、sema が先に弾きます。

// 組み立て用の可変長バッファ（この節の中だけで使う）
typedef struct {
    char *p;
    long long len;
    long long cap;
} SBuf;

static void sb_need(SBuf *b, long long n) {
    if (b->len + n <= b->cap) return;
    long long ncap = b->cap ? b->cap * 2 : 64;
    while (ncap < b->len + n) ncap *= 2;
    char *np = pl_alloc(ncap);
    pl_memcpy(np, b->p, b->len);
    b->p = np;
    b->cap = ncap;
}

static void sb_put(SBuf *b, const char *s, long long n) {
    sb_need(b, n);
    pl_memcpy(b->p + b->len, s, n);
    b->len += n;
}

static void sb_putc(SBuf *b, char c) { sb_put(b, &c, 1); }

static char *sb_finish(SBuf *b) {
    char *out = pl_str_alloc(b->len);
    pl_memcpy(out, b->p, b->len);
    out[b->len] = '\0';
    return out;
}

// 要素の種類。codegen が数で渡します
//   0 = int / 1 = float / 2 = str / 3 = bool
char *pl_list_str(PlList *l, long long kind) {
    SBuf b = {0};
    sb_putc(&b, '[');
    for (long long i = 0; i < l->len; i++) {
        if (i) sb_put(&b, ", ", 2);
        long long raw = ((long long *)l->data)[i];
        char tmp[64];
        if (kind == 0) {
            sb_put(&b, tmp, pl_itoa(raw, tmp));
        } else if (kind == 1) {
            double d;
            pl_memcpy(&d, &raw, 8);
            sb_put(&b, tmp, pl_ftoa(d, tmp));
        } else if (kind == 2) {
            const char *s = (const char *)raw;
            sb_putc(&b, '"');
            if (s) sb_put(&b, s, pl_str_len(s));
            sb_putc(&b, '"');
        } else {
            const char *t = raw ? "True" : "False";
            sb_put(&b, t, pl_cstr_len(t));
        }
    }
    sb_putc(&b, ']');
    return sb_finish(&b);
}

void pl_print_list(PlList *l, long long kind) {
    char *s = pl_list_str(l, kind);
    pl_print_str(s);
}

// ── ハッシュ ────────────────────────────────────────────
//
// ★ FNV-1a。短い鍵に強く、実装が数行で済みます。
//   注意: **暗号用ではありません。** 敵が鍵を選べる場面（外部入力を鍵にする
//     サーバなど）では、衝突を狙われて線形探索に落とされます。
//   注意: 返す値は **非負**にします（剰余で添字にするため）。
long long pl_hash_str(const char *s) {
    unsigned long long h = 14695981039346656037ULL;
    long long n = pl_str_len(s);
    for (long long i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return (long long)(h & 0x7fffffffffffffffULL);
}

// 整数は「散らす」だけ（そのままだと下位ビットに偏りが出る）
long long pl_hash_i64(long long v) {
    unsigned long long h = (unsigned long long)v;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return (long long)(h & 0x7fffffffffffffffULL);
}

// float はビット列を整数として散らす
// 注意: 0.0 と -0.0 はビットが違うので別の値になります。鍵にするときは注意。
long long pl_hash_f64(double v) {
    long long bits;
    pl_memcpy(&bits, &v, 8);
    return pl_hash_i64(bits);
}

// 組み込み関数
long long pl_iabs(long long v) { return v < 0 ? -v : v; }
double pl_fabs(double v) { return v < 0.0 ? -v : v; }

// ── 負の添字の正規化 ────────────────────────────────────
//
// ★ Python と同じく、負の添字は **末尾から**数えます（-1 が最後）。
//   注意: 正規化だけで、範囲の検査はしません（後段の pl_list_check / pl_str_index
//     がそのまま担当します）。-100 のような値は負のまま渡り、そこで落ちます。
long long pl_norm_index(long long i, long long len) {
    if (i < 0) return i + len;
    return i;
}

// list[int] の総和
// 注意: **int のリストだけ**です。float の総和は要素の型で命令が変わるので、
//   linalg.vsum を使ってください（sema が型を見て弾きます）。
long long pl_list_sum(PlList *l) {
    long long s = 0;
    for (long long i = 0; i < l->len; i++) s += ((long long *)l->data)[i];
    return s;
}

// list の連結（新しい list を作る）
PlList *pl_list_concat(PlList *a, PlList *b) {
    PlList *out = pl_list_new();
    for (long long i = 0; i < a->len; i++)
        pl_list_push_i64(out, ((long long *)a->data)[i]);
    for (long long i = 0; i < b->len; i++)
        pl_list_push_i64(out, ((long long *)b->data)[i]);
    return out;
}

// list の繰り返し（負や 0 なら空）
PlList *pl_list_repeat(PlList *a, long long n) {
    PlList *out = pl_list_new();
    for (long long k = 0; k < n; k++)
        for (long long i = 0; i < a->len; i++)
            pl_list_push_i64(out, ((long long *)a->data)[i]);
    return out;
}


// list のスライス。**新しい list を作ります**（借用ではありません）
//
// 注意: 要素をそのまま写すので、参照型なら「同じものを指す 2 つのリスト」に
//   なります。所有権の観点では借用と同じ扱いが要るため、複製した中身の
//   解放は行いません（仕様 §6 の一時値と同じ扱い）。
PlList *pl_list_slice(PlList *l, long long lo, long long hi) {
    // ★ 負の端は末尾から数えます
    if (lo < 0) lo += l->len;
    if (hi < 0) hi += l->len;
    if (lo < 0) lo = 0;
    if (hi > l->len) hi = l->len;
    if (lo > hi) lo = hi;
    PlList *out = pl_list_new();
    for (long long i = lo; i < hi; i++)
        pl_list_push_i64(out, ((long long *)l->data)[i]);
    return out;
}

// ── list のメソッド ────────────────────────────────────

// 末尾を取り出す（空なら panic）
long long pl_list_pop(PlList *l) {
    if (l->len == 0) pl_panic("pop from empty list");
    return ((long long *)l->data)[--l->len];
}

// i の位置に差し込む（後ろへずらす）。i == len なら末尾に足すのと同じ
void pl_list_insert(PlList *l, long long i, long long v) {
    if (i < 0 || i > l->len) {
        char buf[80];
        char *w = buf;
        const char *m = "insert index out of range: ";
        while (*m) *w++ = *m++;
        w += pl_itoa(i, w);
        *w = '\0';
        pl_panic(buf);
    }
    pl_list_grow(l);
    for (long long k = l->len; k > i; k--)
        ((long long *)l->data)[k] = ((long long *)l->data)[k - 1];
    ((long long *)l->data)[i] = v;
    l->len++;
}

// i の位置を取り除いて、その値を返す
long long pl_list_remove_at(PlList *l, long long i) {
    pl_list_check(l, i);
    long long v = ((long long *)l->data)[i];
    for (long long k = i; k + 1 < l->len; k++)
        ((long long *)l->data)[k] = ((long long *)l->data)[k + 1];
    l->len--;
    return v;
}

void pl_list_reverse(PlList *l) {
    for (long long i = 0, j = l->len - 1; i < j; i++, j--) {
        long long t = ((long long *)l->data)[i];
        ((long long *)l->data)[i] = ((long long *)l->data)[j];
        ((long long *)l->data)[j] = t;
    }
}

void pl_list_clear(PlList *l) { l->len = 0; }

// 中身を丸ごと写した新しい list
PlList *pl_list_copy(PlList *l) { return pl_list_slice(l, 0, l->len); }

// 要素も複製して写した新しい list（A-44）。
//
// ★ elem_copy は「要素 1 つを複製する関数」。要素がコピー型（int / bool /
//   float）なら NULL を渡します——そのときは中身をそのまま写すだけです
//   （pl_drop_list と同じ形にしてあります）。
//
// 注意: **浅い写し（pl_list_copy）と別物です。** あちらは要素の中身を
//   共有するので、str の list を写すと同じ文字列を 2 つの list が指します。
void pl_list_push_ptr(PlList *l, void *v);   // 下で定義

PlList *pl_list_copy_with(PlList *l, void *(*elem_copy)(void *)) {
    if (!l) return NULL;
    if (!elem_copy) return pl_list_copy(l);
    PlList *out = pl_list_new();
    void **items = (void **)l->data;
    for (long long i = 0; i < l->len; i++) pl_list_push_ptr(out, elem_copy(items[i]));
    return out;
}

// 別の list の中身を末尾に足す
void pl_list_extend(PlList *l, PlList *o) {
    for (long long i = 0; i < o->len; i++)
        pl_list_push_i64(l, ((long long *)o->data)[i]);
}

void pl_list_push_i64(PlList *l, long long v) {
    pl_list_grow(l);
    ((long long *)l->data)[l->len++] = v;
}

void pl_list_push_ptr(PlList *l, void *v) {
    pl_list_grow(l);
    ((void **)l->data)[l->len++] = v;
}

long long pl_list_get_i64(PlList *l, long long i) {
    pl_list_check(l, i);
    return ((long long *)l->data)[i];
}

void *pl_list_get_ptr(PlList *l, long long i) {
    pl_list_check(l, i);
    return ((void **)l->data)[i];
}

void pl_list_set_i64(PlList *l, long long i, long long v) {
    pl_list_check(l, i);
    ((long long *)l->data)[i] = v;
}

void pl_list_set_ptr(PlList *l, long long i, void *v) {
    pl_list_check(l, i);
    ((void **)l->data)[i] = v;
}

// list[str] を sep でつないだ str を作る。
//
// ★ 本言語側で out = out + xs[i] と書くと O(n^2) になります
//   （1 回ごとに全部コピーするため）。文字列を「組み立てる」のは
//   セルフホストの IR 出力で毎回やることなので、ここだけは C で用意します。
//   利用者は list[str] に溜めて最後に join する、という形で O(n) になります。
char *pl_str_join(PlList *xs, const char *sep) {
    long long n = xs->len;
    long long sep_len = pl_str_len(sep);

    long long total = n > 0 ? sep_len * (n - 1) : 0;
    for (long long i = 0; i < n; i++)
        total += pl_str_len(((char **)xs->data)[i]);

    char *p = pl_str_alloc(total);
    long long at = 0;
    for (long long i = 0; i < n; i++) {
        if (i > 0) {
            pl_memcpy(p + at, sep, (long long)sep_len);
            at += sep_len;
        }
        const char *e = ((char **)xs->data)[i];
        long long el = pl_str_len(e);
        pl_memcpy(p + at, e, (long long)el);
        at += el;
    }
    p[total] = '\0';
    return p;
}


// 文字列の i 番目のバイトを int で返す。
//
// ★ pl_str_index と違い、確保しません。字句解析器のように 1 文字ずつ回る
//   コードでは、1 文字ごとの 2 バイト確保が効いてきます（実測）。
// 注意: 言語には足していません。lib/strings から extern で呼ぶだけです
//   （引いた境界線のとおり）。
long long pl_byte_at(const char *s, long long i) {
    long long n = pl_str_len(s);
    if (i < 0 || i >= n) {
        char buf[80];
        char *w = buf;
        const char *m = "index out of range: ";
        while (*m) *w++ = *m++;
        w += pl_itoa(i, w);
        *w++ = ' ';
        *w++ = '(';
        w += pl_itoa(n, w);
        *w++ = ')';
        *w = '\0';
        pl_panic(buf);
    }
    return (long long)(unsigned char)s[i];
}

// 文字列の添字：1 文字の str を返す（char 型は作らない。型システム 5.8）
char *pl_str_index(const char *s, long long i) {
    long long n = pl_str_len(s);
    if (i < 0 || i >= n) {
        char buf[80];
        char *w = buf;
        const char *m = "index out of range: ";
        while (*m) *w++ = *m++;
        w += pl_itoa(i, w);
        *w++ = ' ';
        *w++ = '(';
        w += pl_itoa(n, w);
        *w++ = ')';
        *w = '\0';
        pl_panic(buf);
    }
    char *p = pl_str_alloc(1);
    p[0] = s[i];
    p[1] = '\0';
    return p;
}

// ── 複製 ───────────────────────────────────────────────
//
// ★ 借りたものを保存したいときの逃げ道です（決定 D8）。
//   `copy(s)` は **ヒープに新しい文字列**を作るので、呼び出し側が所有できます。
//   リテラルを複製しても「静的」の印は付きません（複製はヒープにあるため）。
char *pl_str_copy(const char *s) {
    if (!s) return NULL;
    long long n = pl_str_len(s);
    char *p = pl_str_alloc(n);
    pl_memcpy(p, s, (long long)n + 1);
    return p;
}

// ── 共有所有 rc[T] ─────────────────────────────────────
//
// ★ 所有者を 1 つに決められないデータのための逃げ道です（仕様 v2 §7）。
//
//   ┌──────────┬──────────┬──────────────┐
//   │ strong   │ borrow   │ 中身へのポインタ │
//   │ i64      │ i64      │ ptr           │
//   └──────────┴──────────┴──────────────┘
//
// 注意: 設計（ownership.md §7）では「中身を埋め込む」形にしていましたが、
//    本言語の所有型はすべてポインタなので、**ポインタを 1 本持つ**ほうが
//    型ごとのレイアウト計算が要らず、どの型でも同じ形になります。
typedef struct {
    long long strong;
    long long borrow;
    void *value;
} PlRc;

void *pl_rc_new(void *value) {
    PlRc *r = pl_alloc((long long)sizeof(PlRc));
    r->strong = 1;
    r->borrow = 0;
    r->value = value;
    return r;
}

// ★ codegen は rc の中身読み出しを **IR に展開**します
//   （オフセット 16 の load）。外れの経路だけがここに来ます。
_Noreturn void pl_rc_none_fail(void) {
    pl_panic("rc: None の中身は読めません");
}

// 注意: ランタイムの中からはまだ使います（codegen は呼びません）。
void *pl_rc_get(void *p) {
    if (!p) pl_panic("rc: None の中身は読めません");
    return ((PlRc *)p)->value;
}

void *pl_rc_retain(void *p) {
    if (p) ((PlRc *)p)->strong++;
    return p;
}

// カウントを 1 減らし、0 になったら中身を解放する。
// ★ 中身の解放のしかたは型ごとに違うので、関数ポインタで受け取ります
//   （pl_drop_list と同じ形）。
void pl_rc_release(void *p, void (*value_drop)(void *)) {
    if (!p) return;
    PlRc *r = p;
    r->strong--;
    if (r->strong > 0) return;
    if (r->borrow > 0) pl_panic("rc: 借用したまま解放されました");
    if (value_drop) value_drop(r->value);
    pl_hook_free(r);
}

// 借用の数え札（仕様 §7.2。検査は**実行時**）
void *pl_rc_borrow(void *p) {
    if (!p) pl_panic("rc: None は借用できません");
    ((PlRc *)p)->borrow++;
    return ((PlRc *)p)->value;
}

void *pl_rc_borrow_mut(void *p) {
    if (!p) pl_panic("rc: None は借用できません");
    PlRc *r = p;
    if (r->borrow != 0)
        pl_panic("rc: 既に借用されているので、可変で借りられません");
    r->borrow++;
    return r->value;
}

void pl_rc_unborrow(void *p) {
    if (p) ((PlRc *)p)->borrow--;
}

// ── 解放 ───────────────────────────────────────────────
//
// ★ v1 は「解放しない」設計でした（docs/design/memory-model.md）。
//   所有権で「誰が所有者か」が静的に決まったので、
//   ここで初めて pl_hook_free() を入れます。
//
// 注意: どれも「NULL を渡してよい」ようにしてあります。
//    T | None のフィールドや、まだ入っていない値をそのまま渡せるからです。

void pl_drop_str(char *s) {
    if (!s) return;
    // ★ 文字列リテラル（.rodata）は解放しない
    if (((long long *)s)[-1] & PL_STR_STATIC) return;
    pl_hook_free(s - 8);  // 確保したのはヘッダの先頭（pl_str_alloc を参照）
}

// リストを解放する。
//
// ★ elem_drop は「要素 1 つを解放する関数」。要素がコピー型（int / bool）なら
//   NULL を渡します。所有型なら codegen が適切な関数を渡します
//   （str なら pl_drop_str、クラスなら生成した @drop.C）。
void pl_drop_list(PlList *l, void (*elem_drop)(void *)) {
    if (!l) return;
    if (elem_drop) {
        void **items = (void **)l->data;
        for (long long i = 0; i < l->len; i++) elem_drop(items[i]);
    }
    pl_hook_free(l->data);
    pl_hook_free(l);
}

// クラスのインスタンスそのものを解放する。
// ★ フィールドの解放と drop メソッドの呼び出しは、codegen が生成する
//   @drop.C の中で先に済ませてあります。
void pl_drop_obj(void *p) {
    if (!p) return;
    pl_hook_free(p);
}
