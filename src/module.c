// module.c — import をたどってモジュールを読み込む
#include "module.h"

#include "langinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "langinfo.h"
#include "lexer.h"
#include "parser.h"
#include "util.h"

// ビルド時に Makefile が -DPLC_LIB_DIR=... で渡してきます。
// ⚠️ stage0 だけの割り切り（ビルドツリー内で完結すればよい）。いずれ見直します。
#ifndef PLC_LIB_DIR
#define PLC_LIB_DIR "lib"
#endif

// ── 追加の探索場所（-I <dir>） ─────────────────────────
//
// ★ ここだけ静的変数にしています。Loader に持たせても良いのですが、
//   sema.c の「import を書き忘れていませんか」も同じ場所を見る必要があり、
//   そこへ Loader は届かないからです。対になる定義: selfhost/module
static const char **g_search = NULL;
static int g_nsearch = 0;

void module_set_search_paths(const char **dirs, int ndirs) {
    g_search = dirs;
    g_nsearch = ndirs;
}

// ── 読み込みの状態 ──────────────────────────────────────────
typedef struct {
    char *dir;         // 入口ファイルのあるディレクトリ（探索場所。13.4 節）

    Module **loaded;   // 読み込み済み（名前で引く）
    int nloaded, lcap;

    Module *order;     // 依存順のリスト（依存が先。Module.next で繋ぐ）
    Module *order_tail;

    Module **stack;    // 今たどっている経路（循環したときの表示用）
    int depth, scap;
} Loader;

static void vec_push(Module ***vec, int *n, int *cap, Module *m) {
    if (*n == *cap) {
        int c = *cap ? *cap * 2 : 8;
        Module **p = xmalloc(sizeof(Module *) * (size_t)c);
        memcpy(p, *vec, sizeof(Module *) * (size_t)*n);
        *vec = p;
        *cap = c;
    }
    (*vec)[(*n)++] = m;
}

// ── パスの小道具 ────────────────────────────────────────────

// "a/b/lexer" → "a/b"（区切りが無ければ "."）
static char *dir_of(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return xstrndup(".", 1);
    if (slash == path) return xstrndup("/", 1);
    return xstrndup(path, (size_t)(slash - path));
}

// "a/b/lexer" → "lexer"
//
// ⚠️ 入口のファイル名は識別子とは限りません（future-fizzbuzz など）。
//    モジュール名は IR の名前修飾に使うので、識別子として使えない文字を
//    '_' に置き換えます。import される側は "import 名前" と書ける以上
//    必ず識別子なので、この置き換えが効くのは入口ファイルだけです。
static char *module_name_of(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    size_t len = strlen(base);
    if (len > PLC_LANG_EXT_LEN &&
        strcmp(base + len - PLC_LANG_EXT_LEN, PLC_LANG_EXT) == 0)
        len -= PLC_LANG_EXT_LEN;

    char *name = xstrndup(base, len);
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
                  (i > 0 && c >= '0' && c <= '9');
        if (!ok) name[i] = '_';
    }
    if (len == 0) name = xstrndup("m", 1);
    return name;
}

static bool file_exists(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    fclose(fp);
    return true;
}

// モジュール名をファイルの相対パスにする（"pkg.mod" → "pkg/mod"）。
//
// ★ パッケージ（A-32）。名前のドットが、そのままディレクトリの区切りです。
//   ⚠️ `..` のような名前は字句解析が作れません（識別子とドットの並びだけ）。
static char *name_to_relpath(const char *name) {
    char *out = xstrndup(name, strlen(name));
    for (char *p = out; *p; p++)
        if (*p == '.') *p = '/';
    return out;
}

static char *join_path(const char *dir, const char *name) {
    StrBuf sb;
    sb_init(&sb);
    // ⚠️ 入口が "main" のときディレクトリは "." になります。
    //    "./lexer" と表示されると診断が読みにくいので、そこだけ省きます。
    if (strcmp(dir, ".") == 0) sb_printf(&sb, "%s" PLC_LANG_EXT, name);
    else sb_printf(&sb, "%s/%s" PLC_LANG_EXT, dir, name);
    return sb_str(&sb);
}

// モジュールを探す（探索場所が 2 つになった）。
//
//   ① 入口ファイルのあるディレクトリ
//   ② lib/（標準ライブラリ。ビルド時に埋め込んだパス）
//
// ★ 優先順位は決めません。両方にあったらエラーにします。
//   どちらを先にしても「黙って隠れる」ものが出るからです（14.4 節）。
//   曖昧さは、解決規則を作るより起こせなくするほうが小さく済みます。
// 標準ライブラリの場所。
// ★ 環境変数 PLC_LIB_DIR があればそちらを使います。
//   stage1（セルフホスト版）にはプリプロセッサが無く、ビルド時に埋め込めないので、
//   **両方が同じ規則で探す**ようにするためです。
// ★ 配布物でも動くように、実行ファイルからの相対も見ます。
//   探す順番は runtime と同じ（環境変数 → 埋め込み → <exe>/../lib/plc/lib）。
const char *plc_installed_lib_dir(void);  // main.c が教えてくれる（無ければ NULL）

static const char *lib_dir(void) {
    const char *env = getenv("PLC_LIB_DIR");
    if (env && env[0]) return env;

    char *probe = join_path(PLC_LIB_DIR, "strings" PLC_LANG_EXT);
    bool baked_ok = file_exists(probe);
    if (baked_ok) return PLC_LIB_DIR;

    const char *installed = plc_installed_lib_dir();
    if (installed) return installed;
    return PLC_LIB_DIR;
}

// 探す場所を順に並べる（① 入口のディレクトリ ② -I ③ lib/）。
// ★ 並びは「表示する順」であって「優先順位」ではありません。
static int candidates(Loader *ld, const char *name, char **out) {
    // ★ パッケージ（A-32）：`pkg.mod` は `pkg/mod` を探します
    const char *rel = name_to_relpath(name);
    int n = 0;
    out[n++] = join_path(ld->dir, rel);
    for (int i = 0; i < g_nsearch; i++) out[n++] = join_path(g_search[i], rel);
    out[n++] = join_path(lib_dir(), rel);
    return n;
}

// 見つからなかったときに「どこを探したか」を並べる
static char *searched_list(Loader *ld, const char *name) {
    char **cand = xmalloc(sizeof(char *) * (size_t)(g_nsearch + 2));
    int ncand = candidates(ld, name, cand);
    StrBuf sb;
    sb_init(&sb);
    for (int i = 0; i < ncand; i++)
        sb_printf(&sb, i == 0 ? "%s" : "\n             %s", cand[i]);
    return sb_str(&sb);
}

const char *module_lib_dir(void) { return lib_dir(); }

static char *path_for(Loader *ld, const char *name, Token *from) {
    char **cand = xmalloc(sizeof(char *) * (size_t)(g_nsearch + 2));
    int ncand = candidates(ld, name, cand);
    int lib_at = ncand - 1;  // lib/ は最後（衝突の文言を変えるために覚えておく）

    // 実在するものだけを集めます。**同じパスは 1 つに畳みます**
    //（-I . のように入口のディレクトリと重なる指定を衝突にしないため）。
    char **hits = xmalloc(sizeof(char *) * (size_t)ncand);
    int nhits = 0;
    bool lib_hit = false;
    for (int i = 0; i < ncand; i++) {
        if (!file_exists(cand[i])) continue;
        bool dup = false;
        for (int k = 0; k < nhits; k++)
            if (strcmp(hits[k], cand[i]) == 0) dup = true;
        if (dup) continue;
        if (i == lib_at) lib_hit = true;
        hits[nhits++] = cand[i];
    }

    // ★ 優先順位は決めません。2 つ以上で見つかったらエラーにします。
    //   どちらを先にしても「黙って隠れる」ものが出るからです（14.4 節）。
    if (nhits > 1) {
        StrBuf sb;
        sb_init(&sb);
        sb_printf(&sb, "見つかった場所:");
        for (int i = 0; i < nhits; i++) sb_printf(&sb, "\n             %s", hits[i]);
        sb_printf(&sb, "\n             %s",
                  nhits == 2 ? "どちらかの名前を変えてください"
                             : "名前が重ならないようにしてください");
        Diag d = {0};
        d.message = lib_hit
                        ? diag_fmt("モジュール '%s' が標準ライブラリと衝突しています", name)
                        : diag_fmt("モジュール '%s' が複数の探索場所にあります", name);
        d.primary.tok = from;
        d.primary.label = "どちらを指しているか決められません";
        d.hint = sb_str(&sb);
        diag_fail(&d);
    }
    if (nhits == 1) return hits[0];
    return cand[0];  // 見つからない場合も「利用者側のパス」を返す（診断に出すため）
}

static Module *find_module(Loader *ld, const char *name) {
    for (int i = 0; i < ld->nloaded; i++)
        if (strcmp(ld->loaded[i]->name, name) == 0) return ld->loaded[i];
    return NULL;
}

// ── 循環の経路を "main → lexer → parser → lexer" の形にする ──
static char *cycle_path(Loader *ld, const char *name) {
    StrBuf sb;
    sb_init(&sb);
    bool started = false;
    for (int i = 0; i < ld->depth; i++) {
        if (!started && strcmp(ld->stack[i]->name, name) != 0) continue;
        started = true;
        sb_printf(&sb, "%s → ", ld->stack[i]->name);
    }
    sb_printf(&sb, "%s", name);
    return sb_str(&sb);
}

static void push_path(Loader *ld, Module *m) {
    vec_push(&ld->stack, &ld->depth, &ld->scap, m);
}

// ── 依存の登録 ──────────────────────────────────────────────
static void add_dep(Module *m, Module *dep) {
    Module **p = xmalloc(sizeof(Module *) * (size_t)(m->ndeps + 1));
    memcpy(p, m->deps, sizeof(Module *) * (size_t)m->ndeps);
    p[m->ndeps++] = dep;
    m->deps = p;
}

bool module_file_exists(const char *dir, const char *name) {
    const char *rel = name_to_relpath(name);   // A-32
    if (file_exists(join_path(dir, rel))) return true;
    // ★ 依存を置いた場所（-I）も見ます。lib/ は見ません
    //   （標準ライブラリまで「import を書き忘れていませんか」と言い出すと、
    //     既存の診断の意味が変わってしまうため）。
    for (int i = 0; i < g_nsearch; i++)
        if (file_exists(join_path(g_search[i], rel))) return true;
    return false;
}

// ── 本体：深さ優先で import をたどる ────────────────────────
//
// ★ 再帰から戻るときに order へ積むと、それだけでトポロジカル順になります。
//   整列アルゴリズムを書く必要はありません（13.4 節）。
static Module *load(Loader *ld, const char *name, const char *path, Token *from) {
    Module *m = find_module(ld, name);
    if (m) {
        if (m->state == 1) {  // 訪問中に再訪 → 循環
            Diag d = {0};
            d.message = diag_fmt("循環 import です: %s", cycle_path(ld, name));
            d.primary.tok = from;
            d.primary.label = "この import が循環を作っています";
            d.hint = "モジュールの依存関係は一方通行（DAG）にしてください。"
                     "共通部分を 3 つ目のモジュールに切り出すと解けます";
            diag_fail(&d);
        }
        return m;  // 読み込み済み
    }

    if (from && !file_exists(path)) {
        Diag d = {0};
        d.message = diag_fmt("モジュール '%s' が見つかりません", name);
        d.primary.tok = from;
        d.primary.label = "この import を解決できません";
        d.hint = diag_fmt("次のパスを探しました:\n             %s\n"
                          "             モジュール名はファイル名（" PLC_LANG_EXT " を除いたもの）です"
                          "\n             （'pkg.mod' は 'pkg/mod" PLC_LANG_EXT "' を指します）",
                          searched_list(ld, name));
        diag_fail(&d);
    }

    m = xmalloc(sizeof(Module));
    m->name = (char *)name;
    m->path = (char *)path;
    m->src = read_file(path);
    m->dir = ld->dir;  // import を探す場所（入口ファイルのディレクトリ）
    m->state = 1;  // 訪問中

    // ★ 構文解析より前に表へ載せます。そうしないと循環を検出できません。
    vec_push(&ld->loaded, &ld->nloaded, &ld->lcap, m);

    m->ast = parse(tokenize(m->path, m->src));

    push_path(ld, m);
    for (Node *d = m->ast->body; d; d = d->next) {
        if (d->kind != ND_IMPORT) continue;

        if (strcmp(d->name, m->name) == 0)
            error_at_hint(d->tok, "モジュールは自分自身を import できません",
                          "'%s' は自分自身です", d->name);

        for (int i = 0; i < m->ndeps; i++) {
            if (strcmp(m->deps[i]->name, d->name) != 0) continue;
            Diag e = {0};
            e.message = diag_fmt("'%s' は既に import されています", d->name);
            e.primary.tok = d->tok;
            e.primary.label = "重複した import です";
            e.hint = "同じモジュールを 2 回書く必要はありません";
            diag_fail(&e);
        }

        Module *dep = load(ld, d->name, path_for(ld, d->name, d->tok), d->tok);
        add_dep(m, dep);
    }
    ld->depth--;

    m->state = 2;  // 完了

    // ★ 再帰から戻るときに積むので、依存が必ず自分より前に並びます。
    if (ld->order_tail) ld->order_tail->next = m;
    else ld->order = m;
    ld->order_tail = m;
    return m;
}

Module *load_modules(const char *entry_path, Module **entry_out) {
    Loader ld = {0};
    ld.dir = dir_of(entry_path);

    Module *entry = load(&ld, module_name_of(entry_path), entry_path, NULL);
    if (entry_out) *entry_out = entry;
    return ld.order;
}
