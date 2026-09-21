// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Shota Iwamoto
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// ── このファイルが繋がる先のライセンス ───────────────────────
//
// ★ **このファイルのコードはすべて Apache-2.0 です。** 他所のソースは
//   1 行も入っていません（取り込み＝vendoring はしていません）。
//
// ★ PL_TLS_OPENSSL を立てて建てると、**OpenSSL 3.x にリンクします**。
//
//   | | |
//   |---|---|
//   | OpenSSL 3.0 以上 | Apache-2.0。このリポジトリと同じで、摩擦がありません |
//   | OpenSSL 1.1.1 以前 | **旧 OpenSSL/SSLeay ライセンス**（宣伝条項つき）。Apache-2.0 と両立しません |
//
//   だから下の #error で **3.0 未満をビルドの時点で断ります**。技術的には
//   動いてしまうので、気づかないまま両立しないライセンスのライブラリに
//   繋がるほうが困ります。
//
//   注意: 既定のビルドでは OpenSSL に繋がりません（`make TLS=1` のときだけ）。
//   注意: **配る人へ** — 動的リンクなら OpenSSL は配布物に含まれません。
//     静的に取り込むなら、Apache-2.0 の条件（NOTICE の同梱など）を
//     自分の配布物について確かめてください。
//
// 詳しい判断は docs/design/tls<md> にあります。

// tls.c — TLS（暗号化した通信）のランタイム
//
// ★ **暗号もプロトコルも自分では書きません。** 検証済みの実装
//   （OpenSSL 3.x）に任せ、このファイルは
//     ① この言語の extern から呼べる形に直す
//     ② **危険な使い方を書けないようにする**
//   の 2 つだけをします。
//
//   注意: なぜ自前で書かないのか
//     TLS 1.3 のハンドシェイク・X.509 の連鎖検証・定数時間の比較を
//     新しく書けば、「安全な言語」という看板の下で**いちばん危ない部分だけ
//     が未検証**という状態になります。ここは車輪を再発明しない所です。
//
// ★ **無くても建ちます。** OpenSSL が見つからないビルドでも、この
//   ファイルは同じ関数をすべて定義します（中身は「TLS を組み込まずに
//   建てました」と断るだけ）。おかげで
//     ・既定の `make` は今までどおり clang だけで通る
//     ・lib の側に「TLS があるときだけ書く」分岐が要らない
//     ・**黙って平文に落ちることが無い**
//   の 3 つが同時に成り立ちます。
//
// ★ ここが決めている安全側の既定（利用者が緩められません）
//
//   | 事柄 | 既定 |
//   |---|---|
//   | 最低版 | TLS 1.2（1.0 / 1.1 / SSLv3 は繋ぎません） |
//   | 相手の証明書 | **必ず検証します**（外す口はありません） |
//   | 名前の照合 | **必ず行います**（証明書が誰のものか確かめます） |
//   | 部分ワイルドカード | 断ります（`w*.example.com` は通しません） |
//   | 圧縮 | 切ります（CRIME） |
//   | 再ネゴシエーション | 切ります |
//   | 相手が名乗る名前（SNI） | 住所（IP）で繋ぐときは送りません（RFC 6066） |
//
// 対になる定義: lib/tls<ext>（この言語から見える顔）

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>

// ★ **失敗の番号の出どころが OS で違います。** winsock は errno を
//   立てず、WSAGetLastError() に置きます。ここを errno のままにすると、
//   Windows では**待ち時間切れが「壊れた」に化けます**（もう一度待つ、
//   という判断が呼び出し側で書けなくなります）。
//   対になる定義: runtime/hosted.c :: PL_SOCK_ERRNO
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <winsock2.h>
#define PL_TLS_ERRNO WSAGetLastError()
#else
#define PL_TLS_ERRNO errno
#endif

// core.c が持っている文字列の作り方（ヘッダ付きの str を作る）。
//   注意: core.h には載っていません（核の内部表現なので）。ここでは
//     hosted.c と同じように前方宣言して使います。
char *pl_str_alloc(long long len);
char *pl_str_from_cstr(const char *s);
long long pl_str_len(const char *s);
void *pl_hook_alloc(long long size);
void pl_hook_free(void *p);

// ── 直前の失敗 ──────────────────────────────────────────
//
// ★ net と同じ作法です（`pl_sock_error` / `pl_sock_timed_out`）。
//   失敗した関数は -1 か NULL を返し、**理由はここに置きます**。
//   呼び出した直後に取り出してください（次の呼び出しで上書きされます）。
static char g_tls_err[512];
static int g_tls_timeout = 0;

static void pl_tls_say(const char *msg) {
    snprintf(g_tls_err, sizeof(g_tls_err), "%s", msg);
    g_tls_timeout = 0;
}

char *pl_tls_error(void) { return pl_str_from_cstr(g_tls_err); }

// 直前の失敗は待ち時間切れだったか（1 なら yes）。
//
// ★ net.NetError.timed_out と同じ役目です。待ち時間切れは「まだ来ていない」
//   だけで、接続はまだ生きています。「壊れた」と同じ扱いにすると、
//   もう一度待つ／あきらめる の判断が呼び出し側で書けません。
long long pl_tls_timed_out(void) { return g_tls_timeout ? 1 : 0; }

#ifndef PL_TLS_OPENSSL

// ══════════════════════════════════════════════════════════
// TLS を組み込まずに建てたとき
//
// ★ **同じ名前の関数をすべて定義します。** そうしないと lib/tls を
//   import しただけでリンクが通らなくなり、「TLS を使わない人も
//   OpenSSL を入れなければならない」ことになります。
//
// 注意: **静かに失敗しません。** どれも「組み込んでいません」と
//   はっきり言って断ります。黙って平文で繋ぐくらいなら止まるべきです。
// ══════════════════════════════════════════════════════════

static long long pl_tls_absent(void) {
    pl_tls_say("TLS を組み込まずに建てました（`make TLS=1` で建て直してください）");
    return -1;
}

long long pl_tls_available(void) { return 0; }
long long pl_tls_client_ctx(void) { return pl_tls_absent(); }
long long pl_tls_server_ctx(const char *cert, const char *key) {
    (void)cert; (void)key;
    return pl_tls_absent();
}
long long pl_tls_ctx_set_ca_file(long long ctx, const char *path) {
    (void)ctx; (void)path;
    return pl_tls_absent();
}
void pl_tls_ctx_close(long long ctx) { (void)ctx; }
long long pl_tls_connect(long long ctx, long long fd, const char *host) {
    (void)ctx; (void)fd; (void)host;
    return pl_tls_absent();
}
long long pl_tls_accept(long long ctx, long long fd) {
    (void)ctx; (void)fd;
    return pl_tls_absent();
}
long long pl_tls_send(long long h, const char *s) {
    (void)h; (void)s;
    return pl_tls_absent();
}
char *pl_tls_recv(long long h, long long max) {
    (void)h; (void)max;
    (void)pl_tls_absent();
    return NULL;
}
char *pl_tls_peer_name(long long h) {
    (void)h;
    return pl_str_from_cstr("");
}
char *pl_tls_version(long long h) {
    (void)h;
    return pl_str_from_cstr("");
}
void pl_tls_close(long long h) { (void)h; }

#else  // PL_TLS_OPENSSL

// ══════════════════════════════════════════════════════════
// OpenSSL 3.x に任せる本体
// ══════════════════════════════════════════════════════════

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#if OPENSSL_VERSION_NUMBER < 0x30000000L
// ★ **3.0 未満は断ります。** 1.1.1 以前は旧 OpenSSL/SSLeay ライセンス
//   （宣伝条項つき）で、このリポジトリの Apache-2.0 と衝突します。
//   技術的には動いてしまうので、ビルドの時点で止めます。
#error "OpenSSL 3.0 以上が要ります（1.1.1 以前はライセンスが両立しません）"
#endif

// 直前の失敗を消す（うまくいった、という印）。
//   注意: TLS を組み込まないビルドでは「うまくいく」道が無いので、
//     こちら側にだけ置いてあります。
static void pl_tls_ok(void) {
    g_tls_err[0] = '\0';
    g_tls_timeout = 0;
}


// ── 手（ハンドル）の表 ──────────────────────────────────
//
// ★ **この言語に生ポインタを渡しません。** SSL* をそのまま int に
//   入れて返すと、でたらめな数を渡されたときに落ちます。表に入れて
//   **番号**を渡し、使うたびに番号を確かめます。
//
// ★ 番号には**世代**を混ぜます。枠を使い回すので、世代が無いと
//   「閉じた接続の番号」が、あとから開いた別の接続に当たります
//   （閉じたはずの相手に書く、という一番まずい取り違えです）。
//
//     手 ＝ 枠の番号 * PL_TLS_GEN + 世代
#define PL_TLS_GEN 4096

typedef struct {
    void *p;          // SSL * か SSL_CTX *（used が 0 なら NULL）
    int used;
    int gen;
} PlTlsSlot;

typedef struct {
    PlTlsSlot *v;
    int n;
    int cap;
} PlTlsTable;

static PlTlsTable g_ssl;   // 接続
static PlTlsTable g_ctx;   // 文脈（証明書の設定などを持つ）

// 空いている枠に入れて、手を返す。足りなければ -1。
static long long tbl_put(PlTlsTable *t, void *p) {
    for (int i = 0; i < t->n; i++) {
        if (!t->v[i].used) {
            t->v[i].used = 1;
            t->v[i].p = p;
            return (long long)i * PL_TLS_GEN + t->v[i].gen;
        }
    }
    if (t->n == t->cap) {
        int cap = t->cap ? t->cap * 2 : 8;
        PlTlsSlot *v = (PlTlsSlot *)pl_hook_alloc((long long)sizeof(PlTlsSlot) * cap);
        if (!v) return -1;
        if (t->v) {
            memcpy(v, t->v, sizeof(PlTlsSlot) * (size_t)t->n);
            pl_hook_free(t->v);
        }
        t->v = v;
        t->cap = cap;
    }
    int i = t->n++;
    t->v[i].used = 1;
    t->v[i].gen = 0;
    t->v[i].p = p;
    return (long long)i * PL_TLS_GEN;
}

// 手から中身を引く。知らない手なら NULL。
static void *tbl_get(PlTlsTable *t, long long h) {
    if (h < 0) return NULL;
    long long i = h / PL_TLS_GEN;
    int gen = (int)(h % PL_TLS_GEN);
    if (i >= (long long)t->n) return NULL;
    if (!t->v[i].used || t->v[i].gen != gen) return NULL;
    return t->v[i].p;
}

// 枠を空ける（世代を 1 つ進めるので、同じ手は二度と当たりません）。
static void *tbl_take(PlTlsTable *t, long long h) {
    void *p = tbl_get(t, h);
    if (!p) return NULL;
    long long i = h / PL_TLS_GEN;
    t->v[i].used = 0;
    t->v[i].p = NULL;
    t->v[i].gen = (t->v[i].gen + 1) % PL_TLS_GEN;
    return p;
}

// ── 失敗の言い直し ──────────────────────────────────────

// OpenSSL の積んである失敗を 1 行にまとめる。
//
// 注意: **積んだまま残しません。** 残すと、次にどこか別の場所で失敗した
//   ときに、関係の無い古い行がくっついて出ます。
static void pl_tls_fail(const char *what) {
    unsigned long code = ERR_get_error();
    if (code == 0) {
        snprintf(g_tls_err, sizeof(g_tls_err), "%s: 理由が分かりません", what);
    } else {
        char buf[256];
        ERR_error_string_n(code, buf, sizeof(buf));
        snprintf(g_tls_err, sizeof(g_tls_err), "%s: %s", what, buf);
    }
    ERR_clear_error();
    g_tls_timeout = 0;
}

// その番号は「待ち時間切れ」か。
//   対になる定義: runtime/hosted.c :: pl_sock_is_timeout
static int pl_tls_is_timeout(int code) {
#ifdef _WIN32
    return code == WSAETIMEDOUT || code == WSAEWOULDBLOCK;
#else
    return code == ETIMEDOUT || code == EAGAIN || code == EWOULDBLOCK;
#endif
}

// 読み書き・ハンドシェイクの失敗を言い直す。
//
// ★ ここで**「待ち時間切れ」と「相手が閉じた」を他の失敗と分けます**。
//   どちらも「壊れた」ではないので、呼び出し側の判断が変わります。
//
//   戻り値:  0 … 相手がきちんと閉じた（EOF）
//           -1 … 失敗（理由は g_tls_err）
static int pl_tls_io_fail(SSL *ssl, int ret, const char *what) {
    int err = SSL_get_error(ssl, ret);
    switch (err) {
    case SSL_ERROR_ZERO_RETURN:
        // 相手が close_notify を送って閉じた。**ふつうの終わり方です。**
        pl_tls_ok();
        return 0;
    case SSL_ERROR_SYSCALL: {
        int code = PL_TLS_ERRNO;
        if (pl_tls_is_timeout(code)) {
            snprintf(g_tls_err, sizeof(g_tls_err), "%s: 待ち時間を過ぎました", what);
            g_tls_timeout = 1;
            ERR_clear_error();
            return -1;
        }
        // 注意: **close_notify 無しで切られたのは失敗として返します。**
        //   ここを EOF と同じ扱いにすると、通信の**途中で切る**だけで
        //   「全部受け取った」と思わせられます（切り詰め）。上の層が
        //   長さで区切っていない読み方をしているときに効いてきます。
        if (code == 0) {
            snprintf(g_tls_err, sizeof(g_tls_err),
                     "%s: 相手が閉じる合図なしに切りました（途中で切られた可能性があります）",
                     what);
        } else {
#ifdef _WIN32
            snprintf(g_tls_err, sizeof(g_tls_err), "%s: エラー番号 %d", what, code);
#else
            snprintf(g_tls_err, sizeof(g_tls_err), "%s: %s", what, strerror(code));
#endif
        }
        g_tls_timeout = 0;
        ERR_clear_error();
        return -1;
    }
    default:
        pl_tls_fail(what);
        return -1;
    }
}

// ── 文脈 ────────────────────────────────────────────────

// 両方に共通の、緩められない既定を入れる。
static void pl_tls_harden(SSL_CTX *ctx) {
    // ★ TLS 1.2 より古いものは繋ぎません（1.0 / 1.1 は既に破られています）。
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    // ★ 圧縮を切る（CRIME）。再ネゴシエーションも切る。
    //   注意: SSL_OP_NO_RENEGOTIATION は 1.2 までの話で、1.3 には
    //     再ネゴシエーション自体がありません。
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION |
                                 SSL_OP_NO_RENEGOTIATION |
                                 SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
                                 SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1);

    // ★ 1.3 の切符（session ticket）が読みの途中で来ても、利用者に
    //   「もう一度呼んでください」を見せずに済ませます。
    SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
}

// 取りに行く側の文脈を作る。失敗すれば -1。
//
// ★ **信頼する証明書は OS のものを使います**（`SSL_CTX_set_default_verify_paths`）。
//   環境変数 SSL_CERT_FILE / SSL_CERT_DIR も OpenSSL が見ます。
//
// 注意: 綴じ込みが 1 つも見つからない環境（最小の容れ物など）では、
//   ここでは失敗させず、繋ぎに行ったときに「証明書を確かめられません」
//   として出ます。ここで断ると、自分で set_ca_file する人まで
//   止めてしまうためです。
long long pl_tls_client_ctx(void) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        pl_tls_fail("TLS の文脈を作れません");
        return -1;
    }
    pl_tls_harden(ctx);

    // ★ **相手の証明書を必ず検証します。** 外す口はこのファイルにも
    //   lib/tls にもありません（自己署名を試したい人は set_ca_file を使います）。
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_verify_depth(ctx, 10);
    SSL_CTX_set_default_verify_paths(ctx);

    long long h = tbl_put(&g_ctx, ctx);
    if (h < 0) {
        SSL_CTX_free(ctx);
        pl_tls_say("TLS の文脈を覚えておく場所が足りません");
        return -1;
    }
    pl_tls_ok();
    return h;
}

// 待ち受ける側の文脈を作る（証明書と秘密鍵は PEM のファイル）。
long long pl_tls_server_ctx(const char *cert, const char *key) {
    if (!cert || !key || !cert[0] || !key[0]) {
        pl_tls_say("証明書と秘密鍵の道が要ります");
        return -1;
    }
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        pl_tls_fail("TLS の文脈を作れません");
        return -1;
    }
    pl_tls_harden(ctx);

    // 注意: **use_certificate_chain_file です**（_file ではなく）。
    //   中間証明書が要る本物の証明書は 1 つのファイルに連なって入っており、
    //   単体版で読むと先頭しか送らず、相手によっては検証に落ちます。
    if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1) {
        pl_tls_fail("証明書を読めません");
        SSL_CTX_free(ctx);
        return -1;
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1) {
        pl_tls_fail("秘密鍵を読めません");
        SSL_CTX_free(ctx);
        return -1;
    }
    // ★ 組み合わせが合っているかを**ここで**確かめます。合っていないと、
    //   気づくのは客が来たときになります。
    if (SSL_CTX_check_private_key(ctx) != 1) {
        pl_tls_fail("証明書と秘密鍵が対になっていません");
        SSL_CTX_free(ctx);
        return -1;
    }

    long long h = tbl_put(&g_ctx, ctx);
    if (h < 0) {
        SSL_CTX_free(ctx);
        pl_tls_say("TLS の文脈を覚えておく場所が足りません");
        return -1;
    }
    pl_tls_ok();
    return h;
}

// 信頼する証明書を**このファイルだけ**にする。
//
// ★ 自己署名の証明書を試すための道です。「検証を外す」の代わりに
//   「何を信じるかを指定する」形にしてあります。**名前の照合は
//   そのまま効きます**（自己署名でも、名前が違えば繋がりません）。
long long pl_tls_ctx_set_ca_file(long long h, const char *path) {
    SSL_CTX *ctx = (SSL_CTX *)tbl_get(&g_ctx, h);
    if (!ctx) {
        pl_tls_say("知らない TLS の文脈です");
        return -1;
    }
    if (!path || !path[0]) {
        pl_tls_say("証明書の道が空です");
        return -1;
    }
    if (SSL_CTX_load_verify_locations(ctx, path, NULL) != 1) {
        pl_tls_fail("信頼する証明書を読めません");
        return -1;
    }
    pl_tls_ok();
    return 0;
}

void pl_tls_ctx_close(long long h) {
    SSL_CTX *ctx = (SSL_CTX *)tbl_take(&g_ctx, h);
    if (ctx) SSL_CTX_free(ctx);
}

long long pl_tls_available(void) { return 1; }

// ── 繋ぐ ────────────────────────────────────────────────

// 名前が住所（IPv4 / IPv6 のリテラル）かどうか。
//
// ★ 住所で繋ぐときの扱いが 2 つ変わります。
//   ① SNI を送らない（RFC 6066 が禁じています）
//   ② 名前ではなく**住所として**証明書と照合する
static int pl_tls_is_ip(const char *host) {
    if (!host || !host[0]) return 0;
    if (strchr(host, ':')) return 1;             // IPv6 は必ずコロンを含む
    // IPv4 は「数と点だけ」。名前なら必ず数以外の字が入ります。
    for (const char *p = host; *p; p++)
        if ((*p < '0' || *p > '9') && *p != '.') return 0;
    return 1;
}

// 繋がっている fd を TLS に包む（取りに行く側）。成功すれば手、失敗すれば -1。
//
// 注意: **fd はこちらのものになりません。** 閉じるのは今までどおり
//   net.Conn の仕事です（pl_tls_close は fd を閉じません）。
long long pl_tls_connect(long long ctx_h, long long fd, const char *host) {
    SSL_CTX *ctx = (SSL_CTX *)tbl_get(&g_ctx, ctx_h);
    if (!ctx) {
        pl_tls_say("知らない TLS の文脈です");
        return -1;
    }
    if (fd < 0) {
        pl_tls_say("閉じたソケットを包もうとしました");
        return -1;
    }
    if (!host || !host[0]) {
        // ★ **名前が無ければ繋ぎません。** 名前が無いと証明書が誰のものか
        //   確かめようがなく、検証が形だけになります。
        pl_tls_say("相手の名前が要ります（証明書の照合に使います）");
        return -1;
    }

    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        pl_tls_fail("TLS の接続を作れません");
        return -1;
    }

    int ip = pl_tls_is_ip(host);

    // ★ **名前の照合を OpenSSL にさせます。** 自分で証明書を開いて
    //   比べる形にすると、ワイルドカードや NUL 入りの名前といった
    //   細かい穴を全部自分で塞ぐことになります。
    if (ip) {
        // ★ **住所には住所専用の口を使います。** SSL_set1_host に住所を
        //   渡しても OpenSSL 3 の新しめの版なら通りますが、そこに寄りかかると
        //   「3.0 では名前として照合され、どの証明書とも一致しない」という
        //   版差が出ます。set1_ip_asc は昔からあり、意味が 1 つに決まります。
        if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host) != 1) {
            pl_tls_fail("照合する住所を決められません");
            SSL_free(ssl);
            return -1;
        }
    } else {
        SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        if (SSL_set1_host(ssl, host) != 1) {
            pl_tls_fail("照合する名前を決められません");
            SSL_free(ssl);
            return -1;
        }
        // ★ SNI（どの名前で繋ぎに来たかを相手に伝える）。1 つの住所で
        //   何枚も証明書を出し分けている相手には、これが無いと
        //   別人の証明書が返ってきます。
        //   注意: **住所で繋ぐときは送りません**（RFC 6066）。
        if (SSL_set_tlsext_host_name(ssl, host) != 1) {
            pl_tls_fail("相手の名前を伝えられません");
            SSL_free(ssl);
            return -1;
        }
    }

    if (SSL_set_fd(ssl, (int)fd) != 1) {
        pl_tls_fail("ソケットを結び付けられません");
        SSL_free(ssl);
        return -1;
    }

    int rc = SSL_connect(ssl);
    if (rc != 1) {
        // ★ 証明書の検証で落ちたときは、OpenSSL の番号よりも
        //   **何が起きたか**を先に出します（「自己署名です」など）。
        long vr = SSL_get_verify_result(ssl);
        if (vr != X509_V_OK) {
            snprintf(g_tls_err, sizeof(g_tls_err),
                     "相手の証明書を信用できません: %s",
                     X509_verify_cert_error_string(vr));
            g_tls_timeout = 0;
            ERR_clear_error();
        } else {
            (void)pl_tls_io_fail(ssl, rc, "TLS のやり取りに失敗しました");
        }
        SSL_free(ssl);
        return -1;
    }

    // ★ 念のためもう一度確かめます。SSL_VERIFY_PEER を立ててあるので
    //   ここまで来た時点で通っているはずですが、**通っていないのに
    //   通ったことにする**のが一番まずい失敗なので、二重に見ます。
    if (SSL_get_verify_result(ssl) != X509_V_OK) {
        pl_tls_say("相手の証明書を確かめられませんでした");
        SSL_free(ssl);
        return -1;
    }

    long long h = tbl_put(&g_ssl, ssl);
    if (h < 0) {
        SSL_free(ssl);
        pl_tls_say("TLS の接続を覚えておく場所が足りません");
        return -1;
    }
    pl_tls_ok();
    return h;
}

// 受け付けた fd を TLS に包む（待ち受ける側）。
//
// 注意: **客の証明書は求めません。** 求める形（相互 TLS）はまだありません。
long long pl_tls_accept(long long ctx_h, long long fd) {
    SSL_CTX *ctx = (SSL_CTX *)tbl_get(&g_ctx, ctx_h);
    if (!ctx) {
        pl_tls_say("知らない TLS の文脈です");
        return -1;
    }
    if (fd < 0) {
        pl_tls_say("閉じたソケットを包もうとしました");
        return -1;
    }
    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        pl_tls_fail("TLS の接続を作れません");
        return -1;
    }
    if (SSL_set_fd(ssl, (int)fd) != 1) {
        pl_tls_fail("ソケットを結び付けられません");
        SSL_free(ssl);
        return -1;
    }
    int rc = SSL_accept(ssl);
    if (rc != 1) {
        (void)pl_tls_io_fail(ssl, rc, "TLS のやり取りに失敗しました");
        SSL_free(ssl);
        return -1;
    }
    long long h = tbl_put(&g_ssl, ssl);
    if (h < 0) {
        SSL_free(ssl);
        pl_tls_say("TLS の接続を覚えておく場所が足りません");
        return -1;
    }
    pl_tls_ok();
    return h;
}

// ── 読み書き ────────────────────────────────────────────

// 全部書く。書いたバイト数を返す。失敗すれば -1。
//
// 注意: pl_sock_send と同じく、**途中まで書けた状態で失敗しえます**
//   （待ち時間切れ）。そうなった接続は閉じてください。
long long pl_tls_send(long long h, const char *s) {
    SSL *ssl = (SSL *)tbl_get(&g_ssl, h);
    if (!ssl) {
        pl_tls_say("知らない TLS の接続です");
        return -1;
    }
    long long n = pl_str_len(s);
    long long sent = 0;
    while (sent < n) {
        // 注意: SSL_write は 0 バイトの書き込みを**未定義**としています。
        //   ここには n > sent のときしか来ないので、0 にはなりません。
        int chunk = (n - sent > 1 << 20) ? (1 << 20) : (int)(n - sent);
        int k = SSL_write(ssl, s + sent, chunk);
        if (k <= 0) {
            int eof = pl_tls_io_fail(ssl, k, "TLS の書き込みに失敗しました");
            if (eof == 0) pl_tls_say("TLS の書き込み先が閉じています");
            return -1;
        }
        sent += k;
    }
    pl_tls_ok();
    return sent;
}

// 最大 max バイト読む。
//
// 注意: **戻り値の意味を 3 つに分けます**（pl_sock_recv と同じ）。
//     文字列（長さ > 0） … 読めた
//     ""                  … 相手がきちんと閉じた（close_notify）
//     NULL                … 失敗（理由は pl_tls_error）
char *pl_tls_recv(long long h, long long max) {
    SSL *ssl = (SSL *)tbl_get(&g_ssl, h);
    if (!ssl) {
        pl_tls_say("知らない TLS の接続です");
        return NULL;
    }
    if (max <= 0) max = 4096;
    if (max > (1 << 20)) max = 1 << 20;

    char *buf = (char *)pl_hook_alloc(max);
    if (!buf) {
        pl_tls_say("読み取りの場所を確保できません");
        return NULL;
    }
    int k = SSL_read(ssl, buf, (int)max);
    if (k <= 0) {
        int eof = pl_tls_io_fail(ssl, k, "TLS の読み取りに失敗しました");
        pl_hook_free(buf);
        if (eof == 0) return pl_str_from_cstr("");   // 相手が閉じた
        return NULL;
    }
    char *out = pl_str_alloc(k);
    memcpy(out, buf, (size_t)k);
    out[k] = '\0';
    pl_hook_free(buf);
    pl_tls_ok();
    return out;
}

// 相手の証明書に書かれている名前（無ければ ""）。
//
// 注意: **検証の代わりに使うものではありません。** 検証はすでに
//   済んでいて、ここは記録や表示のためのものです。
char *pl_tls_peer_name(long long h) {
    SSL *ssl = (SSL *)tbl_get(&g_ssl, h);
    if (!ssl) return pl_str_from_cstr("");
    X509 *cert = SSL_get1_peer_certificate(ssl);
    if (!cert) return pl_str_from_cstr("");
    char name[256];
    name[0] = '\0';
    X509_NAME *sub = X509_get_subject_name(cert);
    if (sub) X509_NAME_oneline(sub, name, (int)sizeof(name));
    X509_free(cert);
    return pl_str_from_cstr(name);
}

// 実際に使っている版（"TLSv1.3" など）
char *pl_tls_version(long long h) {
    SSL *ssl = (SSL *)tbl_get(&g_ssl, h);
    if (!ssl) return pl_str_from_cstr("");
    const char *v = SSL_get_version(ssl);
    return pl_str_from_cstr(v ? v : "");
}

// 閉じる（2 回呼んでもよい）。
//
// ★ **close_notify を送ってから外します。** 送らずに切ると、相手からは
//   「途中で切られた」と見分けが付かず、相手が正しく作られているほど
//   エラーとして扱われます。
//
// 注意: **fd は閉じません。** 下のソケットは net.Conn のもので、
//   閉じるのはそちらの仕事です（二重に閉じると、その番号を
//   別の接続が取っているかもしれません）。
void pl_tls_close(long long h) {
    SSL *ssl = (SSL *)tbl_take(&g_ssl, h);
    if (!ssl) return;
    // 注意: 相手の close_notify は待ちません（0 が返っても構いません）。
    //   待つと、黙ったまま切った相手で待ち時間いっぱい止まります。
    (void)SSL_shutdown(ssl);
    ERR_clear_error();
    SSL_free(ssl);
}

#endif  // PL_TLS_OPENSSL
