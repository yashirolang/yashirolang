// main.c — コマンドライン処理と各パスの起動
//
//   <cc> [options] <input><ext>
//
// パイプライン：
//   load_modules（import をたどって読み込み・構文解析）
//     → sema_program（全モジォールをまとめて検査）
//     → ownck_program（所有権の検査）
//     → codegen（モジュールごとに .ll）
//     → clang（.ll を全部渡してリンク）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ★ モジュールごとの clang を並列に走らせるため（下の run_jobs）。
#ifndef _WIN32
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

#include "ast.h"
#include "codegen.h"
#include "lexer.h"
#include "module.h"
#include "ownck.h"
#include "prove.h"
#include "parser.h"
#include "langinfo.h"
#include "sema.h"
#include "types.h"
#include "util.h"

// ビルド時に Makefile が -DPLC_RUNTIME_O=... で渡してきます。
#ifndef PLC_RUNTIME_O
#define PLC_RUNTIME_O "build/runtime.a"
#endif

// 生成物をリンクするのに使う clang。
// ★ clang-18 のように名前が違う環境があるので、ビルド時に埋め込み、
//   環境変数 PLC_CLANG でも上書きできるようにします。
#ifndef PLC_CLANG
#define PLC_CLANG "clang"
#endif

// 実際に使う clang を返す（環境変数 → ビルド時の埋め込み）。
static const char *clang_cmd(void) {
    const char *env = getenv("PLC_CLANG");
    if (env && env[0]) return env;
    return PLC_CLANG;
}

// clang へ渡す道を確かめる（シェルに解釈される字が無いか）。
//
// 注意: **リンクは system() でシェルを通ります。** 道は "…" で囲んでいますが、
//   POSIX の sh は**二重引用符の中でも** `$` ・ ` ・ `\` を解釈します。
//   囲んであるから安全、ではありません。実際、
//
//       <コンパイラ> a<拡張子> -o 'out$(touch X)x'
//
//   は touch を実行しました。注意: `-o` は自分で打つものなので自分で自分を
//   撃つだけですが、**道を組み立てるのが人とは限りません**
//   （書き出し先を設定から作る作業手順書や、雛形から作る仕組み）。
//
// ★ **囲い方を賢くするのではなく、断ります。** 引用の規則は sh と
//   cmd.exe で違い、両方に効く「正しい囲み方」を書こうとすると、
//   そこが次の穴になります。使えなくなるのは、道に入れないほうがよい字
//   だけです。
//
//   注意: `\` は Windows の区切り文字なので、そちらでは通します
//     （cmd.exe は `\` を逃がし字として扱いません）。
//     `%` は逆に cmd.exe だけが展開します。
static void check_shell_safe(const char *path, const char *what) {
    for (const char *p = path; *p; p++) {
        unsigned char c = (unsigned char)*p;
        int bad = (c == '"' || c == '`' || c == '$' || c == '\n' || c == '\r');
#ifdef _WIN32
        if (c == '%') bad = 1;
#else
        if (c == '\\') bad = 1;
#endif
        if (bad)
            error("%s に使えない字 '%c' が入っています"
                  "（シェルが解釈してしまうため断ります）", what, c);
    }
}

// ★ --version が出す内容。
//
//   版番号だけでなく **stage と target triple** も出します。この処理系は
//   C 製（stage0）とセルフホスト製（stage1 以降）の 2 つがあり、
//   triple はビルド時に埋め込まれるので、「どれを使っているのか」が
//   不具合の報告で最初に要る情報になります。
//   対になる定義: selfhost/main の print_version
static void print_version(void) {
    printf("%s %s (stage0)\n", PLC_LANG_CC, PLC_LANG_VERSION);
    printf("target: %s\n", PLC_TARGET_TRIPLE);
}

static void usage(int status) {
    FILE *out = status == 0 ? stdout : stderr;
    fprintf(out,
            PLC_LANG_NAME " コンパイラ (stage0)\n"
            "\n"
            "使い方: " PLC_LANG_CC " [オプション] <入力" PLC_LANG_EXT ">\n"
            "\n"
            "オプション:\n"
            "  -o <file>       出力する実行ファイル名（既定: a.out）\n"
            "  -S              LLVM IR を標準出力に書いて終了\n"
            "  --dump-tokens   トークン列を表示して終了（字句解析のデバッグ用）\n"
            "  --dump-ast      AST を S 式で表示して終了（構文解析のデバッグ用）\n"
            "  --keep-ll       実行ファイル生成後も .ll を残す\n"
            "  --check         型検査までで止める（エラーが無ければ何も出さない）\n"
            "  --warn-own      所有権の指摘を警告に落とす（既定はエラー）\n"
            "                  注意: 0.17 以前の既定です。逃げ道であって、\n"
            "                  これを付けたコードは安全性を保証しません\n"
            "  --deny-move     移動済みの値の使用をエラーにする（既定）\n"
            "  --deny-borrow   借用した値の保存・返却をエラーにする（既定）\n"
            "  --deny-mut      読み取り専用の借用への書き換えをエラーにする（既定）\n"
            "  --deny-store-borrow\n"
            "                  借りものを所有スロットへ入れる箇所をエラーにする（既定）\n"
            "                  ★ --deny-* は --warn-own の後に書くと、\n"
            "                  その検査だけエラーに戻せます（後勝ち）\n"
            "  --explain-mut   呼び出しで変更される実引数を一覧表示して終了\n"
            "  --drop          スコープの出口に解放（drop）を挿入する（既定）\n"
            "  --no-drop       解放を挿入しない（--drop を打ち消す。後勝ち）\n"
            "  --no-overflow-check\n"
            "                  数の実行時検査を外す（既定は検査する）:\n"
            "                  整数の + - * の桁あふれ／float の 0 除算\n"
            "  -g              デバッグ情報を出す（デバッガ・perf が行を出せます）\n"
            "  --no-prove      証明で実行時検査を消さない（A-34）\n"
            "  --verify-prove  消せると判断した検査を**残す**（解析の誤りを捕まえる）\n"
            "  --prove-report  消えた検査の数を出す\n"
            "  -l<名前> / -L<dir> / -framework <名前>\n"
            "                  リンクのときに clang へそのまま渡す\n"
            "                  （C のライブラリを extern で呼ぶときに使います）\n"
            "  -I <dir>        import を探す場所を足す（何度でも書ける）\n"
            "                  パッケージマネージャ " PLC_LANG_PM " が使います\n"
            "  -c              リンクせずオブジェクト（.o）を出す\n"
            "  -j <N>          clang を同時に何本走らせるか（既定: コア数）\n"
            "                  注意: 出来上がる実行ファイルは並列度で変わりません\n"
            "  --target=<t>    生成する IR の target triple を指定する\n"
            "                  （例: --target=riscv64-unknown-elf）\n"
            "  -O0|-O1|-O2|-O3 clang に渡す最適化レベル（既定: -O0）\n"
            "  -h, --help      この使い方を表示\n"
            "  --version       版番号と target triple を表示\n"
            "  --print-lib-dir 標準ライブラリの場所を表示（" PLC_LANG_PM " が使います）\n");
    exit(status);
}

// 実行するステージ
typedef enum {
    STAGE_ALL,          // 実行ファイルまで作る
    STAGE_DUMP_TOKENS,  // 字句解析まで
    STAGE_DUMP_AST,     // 構文解析まで
    STAGE_EMIT_IR,      // コード生成まで（-S）
    STAGE_CHECK,        // 意味解析まで（--check）
    STAGE_EXPLAIN_MUT,  // 所有権検査まで。変更される実引数を並べる（--explain-mut）
} Stage;

typedef struct {
    const char *input;
    const char *output;
    const char *opt_level;
    Stage stage;
    int keep_ll;
    // ★ 所有権の検査は **既定でエラー**です（A-24）。--warn-own で警告に落とせます。
    int deny_move;    // --deny-move   （既定 1）
    int deny_borrow;  // --deny-borrow （既定 1）
    int deny_mut;     // --deny-mut    （既定 1）
    int deny_store_borrow;  // --deny-store-borrow（E-BORROW-7。既定 1）
    int drop;         // --drop（解放を挿入する）
    int no_ovf;       // --no-overflow-check（桁あふれの検査を出さない）
    int debug;        // -g（デバッグ情報を出す。A-30）
    // ── 証明（A-34）──
    int no_prove;      // --no-prove（検査を 1 つも消さない）
    int verify_prove;  // --verify-prove（消さずに残し、外れたら専用の診断で止める）
    int prove_report;  // --prove-report（消えた検査の数を出す）
    // ★ リンクするときに clang へそのまま渡すもの（-l / -L / -framework。A-33）
    const char **link;
    int nlink;
    const char *target;  // --target=<triple>（ベアメタル向け）
    int emit_obj;        // -c（リンクせずオブジェクトを出す）
    int jobs;            // -j N（clang を同時に何本走らせるか。0 = コア数）
    // -I <dir>（import を探す場所を足す。何度でも書ける）
    const char **inc;
    int ninc;
} Options;

// 文字列の配列を 1 つ伸ばす（数えるのは呼び出し側）
static const char **xrealloc_ptrs(const char **p, int n) {
    const char **q = xmalloc(sizeof(char *) * (size_t)(n + 1));
    for (int i = 0; i < n; i++) q[i] = p[i];
    return q;
}

static Options parse_args(int argc, char **argv) {
    Options o = {0};
    o.output = "a.out";
    o.opt_level = "-O0";
    o.stage = STAGE_ALL;
    // ★ 解放（drop）は**既定で入れます**（A-21 ⑬。決定 D16 を改めました）。
    //   逃げ道は --no-drop です。所有権検査（ownck）が「借りもの」「移動済み」に
    //   印を付けているので、それに従って安全な場所にだけ解放を挿します。
    o.drop = 1;
    // ★ 所有権の検査は**既定でエラー**です（A-24。決定 D12 を改めました）。
    //   二重解放・解放後の使用が「警告どまり」では、Rust と同じ強さだとは
    //   言えません。逃げ道は --warn-own です（§0 ③ の折衷はここで畳みました）。
    o.deny_move = 1;
    o.deny_borrow = 1;
    o.deny_mut = 1;
    o.deny_store_borrow = 1;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];

        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) usage(0);

        if (strcmp(a, "--version") == 0) {
            print_version();
            exit(0);
        }

        // ★ 標準ライブラリの場所を教える。
        //   パッケージマネージャが「入れようとしているモジュール名が
        //   標準ライブラリとぶつかっていないか」を先に確かめるのに使います。
        if (strcmp(a, "--print-lib-dir") == 0) {
            printf("%s\n", module_lib_dir());
            exit(0);
        }

        if (strcmp(a, "-o") == 0) {
            if (i + 1 >= argc) error("-o の後に出力ファイル名が必要です");
            // clang へはシェル経由で渡ります。中間の .ll の名前も
            //   ここから作るので、**入口で 1 回**確かめれば足ります。
            check_shell_safe(argv[i + 1], "出力ファイル名");
            o.output = argv[++i];
            continue;
        }
        if (strcmp(a, "-S") == 0) { o.stage = STAGE_EMIT_IR; continue; }
        if (strcmp(a, "--dump-tokens") == 0) { o.stage = STAGE_DUMP_TOKENS; continue; }
        if (strcmp(a, "--dump-ast") == 0) { o.stage = STAGE_DUMP_AST; continue; }
        if (strcmp(a, "--keep-ll") == 0) { o.keep_ll = 1; continue; }
        // ★ 型検査までで止める。stage1（セルフホスト版）と
        //   「エラーが出るか / 出ないか」を突き合わせるために使います。
        if (strcmp(a, "--check") == 0) { o.stage = STAGE_CHECK; continue; }
        // ★ 所有権の検査（ownck）を警告に落とす逃げ道。
        //
        // 注意: **後に書いたほうが勝ちます**（--drop / --no-drop と同じ規則）。
        //   --warn-own --deny-move なら「移動だけエラー」に戻せるので、
        //   古いコードを検査ごとに直していけます（決定 D12 の意図はこちら側へ）。
        if (strcmp(a, "--warn-own") == 0) {
            o.deny_move = 0;
            o.deny_borrow = 0;
            o.deny_mut = 0;
            o.deny_store_borrow = 0;
            continue;
        }
        if (strcmp(a, "--deny-move") == 0) { o.deny_move = 1; continue; }
        if (strcmp(a, "--deny-borrow") == 0) { o.deny_borrow = 1; continue; }
        if (strcmp(a, "--deny-mut") == 0) { o.deny_mut = 1; continue; }
        if (strcmp(a, "--deny-store-borrow") == 0) { o.deny_store_borrow = 1; continue; }
        // ★ 解放（drop）の挿入。**既定で入ります**（A-21 ⑬）。
        //
        // 注意: **後に書いたほうが勝ちます**（--drop --no-drop なら入れない）。
        //   既定を解放ありに変えるとき（A-21 ⑨）、逃げ道として --no-drop が要ります。
        //   診断を見せるためのテスト（warn_* など）は、危険な書き方をわざとして
        //   いるので解放すると壊れます。そこに付けるのが --no-drop です。
        if (strcmp(a, "--drop") == 0) { o.drop = 1; continue; }
        if (strcmp(a, "--no-drop") == 0) { o.drop = 0; continue; }
        // ★ 既定は検査あり。速さのために外したいときだけ付ける。
        if (strcmp(a, "--no-overflow-check") == 0) { o.no_ovf = 1; continue; }
        // ★ デバッグ情報（A-30）。デバッガ・perf・バックトレースが行を出せます。
        if (strcmp(a, "-g") == 0) { o.debug = 1; continue; }

        // ── 証明（A-34）──
        //
        // 注意: --verify-prove は「消せる」と判断した検査を**残したまま**、
        //   外れたら専用の診断で止めます。CI でこちらを回せば、解析の誤りが
        //   利用者ではなく私たちに返ってきます。
        if (strcmp(a, "--no-prove") == 0) { o.no_prove = 1; continue; }
        if (strcmp(a, "--verify-prove") == 0) { o.verify_prove = 1; continue; }
        if (strcmp(a, "--prove-report") == 0) { o.prove_report = 1; continue; }

        // ── C のライブラリを繋ぐ（A-33）──
        //
        // ★ `-l` / `-L` / `-framework` を **リンクのときだけ** clang へ
        //   そのまま渡します。extern で宣言した関数の実体が、標準ライブラリの
        //   外（BLAS や自前の .o）にあるときに要ります。
        //
        // シェル経由で渡るので、`-o` と同じように**入口で 1 回**確かめます。
        if ((strncmp(a, "-l", 2) == 0 || strncmp(a, "-L", 2) == 0) && a[2]) {
            check_shell_safe(a, "リンクの指定");
            o.link = xrealloc_ptrs(o.link, o.nlink);
            o.link[o.nlink++] = a;
            continue;
        }
        if (strcmp(a, "-framework") == 0) {
            if (i + 1 >= argc) error("-framework の後に名前が必要です");
            check_shell_safe(argv[i + 1], "framework の名前");
            o.link = xrealloc_ptrs(o.link, o.nlink);
            o.link[o.nlink++] = "-framework";
            o.link = xrealloc_ptrs(o.link, o.nlink);
            o.link[o.nlink++] = argv[++i];
            continue;
        }
        // ★ ベアメタル向け。リンクは自分でやるので -c で止める。
        if (strcmp(a, "-c") == 0) { o.emit_obj = 1; continue; }
        // ★ モジュールごとの clang を何本同時に走らせるか。
        //   注意: 出来上がる実行ファイルは並列度によって変わりません。
        if (strncmp(a, "-j", 2) == 0 && a[2] != '\0') {
            o.jobs = atoi(a + 2);
            if (o.jobs < 0) error("-j には 0 以上を指定してください: %s", a);
            continue;
        }
        if (strncmp(a, "--jobs=", 7) == 0) {
            o.jobs = atoi(a + 7);
            if (o.jobs < 0) error("--jobs には 0 以上を指定してください: %s", a);
            continue;
        }
        if (strncmp(a, "--target=", 9) == 0) { o.target = a + 9; continue; }
        // ★ import を探す場所を足す（-I deps / -Ideps のどちらでも）。
        //   パッケージマネージャが依存の置き場所を渡すのに使います。
        if (strcmp(a, "-I") == 0 || strncmp(a, "-I", 2) == 0) {
            const char *dir = NULL;
            if (strcmp(a, "-I") == 0) {
                if (i + 1 >= argc) error("-I の後にディレクトリ名が必要です");
                dir = argv[++i];
            } else {
                dir = a + 2;
            }
            if (!dir[0]) error("-I に空のディレクトリは指定できません");
            const char **p = xmalloc(sizeof(char *) * (size_t)(o.ninc + 1));
            for (int k = 0; k < o.ninc; k++) p[k] = o.inc[k];
            p[o.ninc++] = dir;
            o.inc = p;
            continue;
        }
        // ★ 呼び出し側に mut を書かせない代わりの道具（仕様 §5.3）。
        if (strcmp(a, "--explain-mut") == 0) { o.stage = STAGE_EXPLAIN_MUT; continue; }

        if (strcmp(a, "-O0") == 0 || strcmp(a, "-O1") == 0 ||
            strcmp(a, "-O2") == 0 || strcmp(a, "-O3") == 0) {
            o.opt_level = a;
            continue;
        }

        if (a[0] == '-' && a[1] != '\0') error("不明なオプション: %s", a);

        if (o.input) error("入力ファイルが複数指定されています: %s と %s", o.input, a);
        o.input = a;
    }

    if (!o.input) usage(1);
    return o;
}

// ── ランタイムと標準ライブラリの探し方 ─────────────────────
//
// ★ 探す順番は 3 つ（「配って使える」ようにするため足しました）。
//   ① 環境変数（PLC_RUNTIME_O / PLC_LIB_DIR）
//   ② ビルド時に埋め込んだ絶対パス（ソースの木の中で使うとき）
//   ③ **実行ファイルからの相対**（インストールしたとき / 配布物を展開したとき）
//
//      <prefix>/bin/<cc>
//      <prefix>/lib/plc/runtime.a
//      <prefix>/lib/plc/lib/*
//
// 注意: ③ が無いと、ビルドした場所を動かした瞬間に動かなくなります。
//    「ダウンロードして展開したら動く」ためには、この規則が要ります。
static bool file_exists(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    fclose(fp);
    return true;
}

// argv[0] から実行ファイルのあるディレクトリを求める（無理なら NULL）
static char *exe_dir(const char *argv0) {
    if (!argv0 || !argv0[0]) return NULL;
    const char *slash = NULL;
    for (const char *c = argv0; *c; c++)
        if (*c == '/' || *c == '\\') slash = c;
    if (!slash) return NULL;  // PATH 経由で起動された（相対を諦める）
    return xstrndup(argv0, (size_t)(slash - argv0));
}

static char *g_exe_dir;  // main が最初に埋める

// <exe>/../lib/plc/<name> を組み立てる
static char *installed_path(const char *name) {
    if (!g_exe_dir) return NULL;
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "%s/../lib/plc/%s", g_exe_dir, name);
    return sb_str(&sb);
}

// module.c が「標準ライブラリの場所」を聞きに来る
const char *plc_installed_lib_dir(void) {
    char *p = installed_path("lib");
    if (!p) return NULL;
    StrBuf probe;
    sb_init(&probe);
    sb_printf(&probe, "%s/strings" PLC_LANG_EXT, p);
    return file_exists(sb_str(&probe)) ? p : NULL;
}

static const char *runtime_o(void) {
    const char *env = getenv("PLC_RUNTIME_O");
    if (env && env[0]) return env;
    if (file_exists(PLC_RUNTIME_O)) return PLC_RUNTIME_O;
    char *p = installed_path("runtime.a");
    if (p && file_exists(p)) return p;
    return PLC_RUNTIME_O;  // 見つからないときは、埋め込んだ値でエラーを出させる
}

// 出力ファイル名とモジュール名から .ll のパスを作る。
//   a.out + main  → a.out.main.ll
//
// ★ モジュールごとに 1 本出すので、名前にモジュール名を挟みます。
static char *ll_path_for(const char *output, const char *mod_name) {
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "%s.%s.ll", output, mod_name);
    return sb_str(&sb);
}


// ── モジュールごとの clang を並列に走らせる ─────────────────
//
// なぜここが効くのか
//   このコンパイラの仕事は、実測で **前段（字句〜意味解析〜IR 生成）が 5%、
//   clang が 95%** です（セルフホスト版一式で 0.17s 対 2.9s）。
//   つまり**コンパイラ自身をスレッド化しても意味がなく**、
//   「clang の呼び出しを並べる」だけで頭打ちまで行きます。
//
//   注意: 0.13.0 までは **1 つの clang に .ll を全部渡していました**。clang は
//     入力を順番に処理するので、コアが 12 あっても 1 つしか回りません。
//
// ★ 上限はモジュール 1 本の時間です（selfhost なら sema の 0.95 秒）。
//   そこから先を詰めるには、大きいモジュールを分割することになります。
//
// 注意: **スレッド + system() では速くなりませんでした。** macOS の system() は
//    シグナル処理を守るためにグローバルなロックを取るので、何本のスレッドから
//    呼んでも 1 本ずつしか走りません（最初にそう書いて、実測で気づきました）。
//    だから**プロセスを直接起こします**（posix_spawn）。スレッドが要らなく
//    なるぶん、こちらのほうが単純でもあります。

// 走らせる仕事 1 つ。
typedef struct {
    const char *cmd;  // シェルに渡すコマンド
    int rc;           // その終了コード
} CcJob;

// 全部の仕事を走らせる。1 つでも失敗したら 0 以外を返す。
//
// 注意: **Windows では逐次のまま**です。cmd.exe には & による並行実行が無く、
//    並べるなら Win32 の API を別に書くことになります。
//    ここは「効かないだけで、壊れない」ほうを選びました。
static int run_jobs(CcJob *jobs, int njobs, int want) {
    int worst = 0;
#ifdef _WIN32
    (void)want;
    for (int i = 0; i < njobs; i++) {
        jobs[i].rc = system(jobs[i].cmd);
        if (jobs[i].rc != 0) worst = jobs[i].rc;
    }
#else
    int n = want > 0 ? want : (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > njobs) n = njobs;

    pid_t *pids = xmalloc(sizeof(pid_t) * (size_t)njobs);
    for (int i = 0; i < njobs; i++) pids[i] = -1;

    int started = 0, done = 0, running = 0;
    while (done < njobs) {
        // ① 空きがあるだけ起こす
        while (started < njobs && running < n) {
            char *argv[] = {(char *)"/bin/sh", (char *)"-c",
                            (char *)jobs[started].cmd, NULL};
            pid_t pid;
            if (posix_spawn(&pid, "/bin/sh", NULL, NULL, argv, environ) != 0) {
                // 注意: 起こせなかったら、その場で自分が走ります（落とさない）。
                jobs[started].rc = system(jobs[started].cmd);
                done++;
            } else {
                pids[started] = pid;
                running++;
            }
            started++;
        }
        if (running == 0) break;  // 起こせるものが無い（全部その場で走らせた）

        // ② 1 つ終わるのを待つ
        int status = 0;
        pid_t got = wait(&status);
        if (got < 0) break;
        running--;
        for (int i = 0; i < njobs; i++) {
            if (pids[i] != got) continue;
            jobs[i].rc = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            pids[i] = -1;
            done++;
            break;
        }
    }

    for (int i = 0; i < njobs; i++)
        if (jobs[i].rc != 0) worst = jobs[i].rc;
#endif
    return worst;
}

int main(int argc, char **argv) {
    plc_use_binary_streams();      // Windows の \r\n 変換を止める
    g_exe_dir = exe_dir(argv[0]);  // ★ 配布物でも動くように（上の runtime_o を参照）
    Options opt = parse_args(argc, argv);

    // プリミティブ型のシングルトンを用意する（types.h 参照）
    types_init();

    // ── ①② 字句解析・構文解析だけを見たいとき（入口ファイルのみ）──
    //
    // 注意: --dump-tokens / --dump-ast は import をたどりません。
    //    「1 ファイルの中身を確かめる」道具だからです。
    if (opt.stage == STAGE_DUMP_TOKENS || opt.stage == STAGE_DUMP_AST) {
        char *src = read_file(opt.input);
        TokenVec toks = tokenize(opt.input, src);
        if (opt.stage == STAGE_DUMP_TOKENS) {
            dump_tokens(toks);
            return 0;
        }
        // 注意: --dump-ast は sema の前に出します。
        //    構文解析だけを独立して確認したいためです（型エラーがあっても木は見たい）。
        dump_ast(parse(toks));
        return 0;
    }

    // ── ⓪ 読み込み：import をたどって依存順に並べる──
    // ★ -I で足した場所も探させます（load_modules より前に）。
    module_set_search_paths(opt.inc, opt.ninc);
    Module *entry = NULL;
    Module *mods = load_modules(opt.input, &entry);

    // ── ③ 意味解析・型検査（全モジュールまとめて）──
    sema_program(mods, entry);

    // ── target triple を決める ──
    //
    //   ① --target=... が最優先
    //   ② ソースの pragma target "..."
    //   ③ ビルド時に埋め込んだ、この機械のもの（codegen の既定）
    const char *triple = opt.target;
    bool no_runtime = false;
    for (Node *d = entry->ast->body; d; d = d->next) {
        if (d->kind != ND_PRAGMA) continue;
        if (!triple && strcmp(d->name, "target") == 0) triple = d->sval;
        // ★ ベアメタルでは C の main も argv も無い。
        //   「@main のラッパを出さない」がこの pragma の意味です。
        if (strcmp(d->name, "no_runtime") == 0) no_runtime = true;
    }

    // ── ④ 所有権の検査──
    //
    // ★ 既定は警告です。移動済みの値を使っていても、生成される IR は
    //   v1 のまま変わりません（解放の挿入は ownck）。
    OwnckOptions own = {0};
    own.deny_move = opt.deny_move;
    own.deny_borrow = opt.deny_borrow;
    own.deny_mut = opt.deny_mut;
    own.deny_store_borrow = opt.deny_store_borrow;
    own.explain_mut = opt.stage == STAGE_EXPLAIN_MUT;
    ownck_program(mods, &own);

    if (opt.stage == STAGE_EXPLAIN_MUT) return 0;

    // --check : ここで終わり（エラーがあれば sema / ownck が既に終了している）
    //
    // ★ ④ 所有権の検査まで通します。かつてここは ③ の直後にあり、
    //   「stage1（セルフホスト版）にはまだ ownck が無いので --check の出力を
    //   突き合わせられない」ことが理由でした。0.16.0 で ownck を
    //   セルフホスト版へ移したので、その理由は無くなっています。
    //   注意: 直すまで 2 実装の --check は食い違っていました
    //   （stage1 だけが E-MOVE-1 を出し、--check --deny-move で 1 を返す）。
    //   ★ --check は編集中のコードを見る入口（将来の LSP）でもあるので、
    //     ここで所有権の指摘が落ちると、その先で全部落ちます。
    if (opt.stage == STAGE_CHECK) return 0;

    // ── ⑤ 証明（A-34）──
    //
    // ★ 区間解析で「必ず成り立つ」と示せた実行時検査に印を立てます。
    //   codegen はその印を見て検査を出しません。
    //   注意: --no-prove なら 1 つも消しません（比べるための逃げ道）。
    //   注意: --verify-prove なら印は立てたまま検査を**残し**、外れたら
    //     専用の診断で止めます（解析の誤りを私たち側に返すため）。
    if (!opt.no_prove) {
        ProveStats ps = {0};
        prove_program(mods, &ps, opt.no_ovf != 0);
        if (opt.prove_report) {
            fprintf(stderr, "証明で消した実行時検査:\n");
            fprintf(stderr, "  桁あふれ  %6d 消 / %6d 残\n", ps.ovf, ps.ovf_left);
            fprintf(stderr, "  添字      %6d 消 / %6d 残\n", ps.bounds,
                    ps.bounds_left);
            fprintf(stderr, "  範囲型    %6d 消 / %6d 残\n", ps.range,
                    ps.range_left);
            fprintf(stderr, "  契約      %6d 消 / %6d 残\n", ps.contract,
                    ps.contract_left);
            fprintf(stderr, "  0 除算    %6d 消 / %6d 残\n", ps.div, ps.div_left);
        }
    }

    // 入口モジュールの main の IR 名（@main のラッパが呼ぶ相手）
    StrBuf main_ir;
    sb_init(&main_ir);
    sb_printf(&main_ir, "%s.main", entry->name);

    // ── ④ コード生成（モジュールごとに 1 本の .ll）──
    for (Module *m = mods; m; m = m->next) {
        const char *entry_main = (m == entry && !no_runtime) ? sb_str(&main_ir) : NULL;
        char *ir = codegen(m, entry_main, opt.drop != 0, opt.no_ovf != 0, triple,
                           opt.debug != 0, opt.verify_prove != 0);

        if (opt.stage == STAGE_EMIT_IR) {
            // -S : IR を出して終了。複数モジュールなら区切りを入れて並べる。
            if (mods->next) printf("; ── module: %s ──\n", m->name);
            fputs(ir, stdout);
            continue;
        }

        m->ll_path = ll_path_for(opt.output, m->name);
        write_file(m->ll_path, ir);
    }
    if (opt.stage == STAGE_EMIT_IR) return 0;

    // ── -c ならオブジェクトを出して終わり ──
    //
    // ★ ベアメタルでは、リンクはこちらの仕事ではありません。
    //   リンカスクリプトを渡すのも、起動アセンブリを混ぜるのも利用者側です。
    //
    // 注意: いまは 1 モジュールだけ対応します。import を含むカーネルは、
    //    モジュールごとに .o を作って自分でリンクしてください。
    if (opt.emit_obj) {
        if (mods->next)
            error("-c は 1 モジュールのファイルにだけ使えます"
                  "（import があるときは、モジュールごとに分けてください）");

        StrBuf oc;
        sb_init(&oc);
        // ★ RISC-V などは Apple の clang が対応していないことがあるので、
        //   PLC_CLANG で使う clang を差し替えられるようにします。
        const char *cc = clang_cmd();
        // 注意: 引用は "…" にします。Windows の system() は cmd.exe を通すので、
        //    '…' は引用符として扱われません（POSIX の sh は "…" も理解します）。
        sb_printf(&oc, "%s %s -Wno-override-module -c \"%s\" -o \"%s\"", cc,
                  opt.opt_level, entry->ll_path, opt.output);
        if (triple) sb_printf(&oc, " --target=%s", triple);
        const char *extra = getenv("PLC_CFLAGS");
        if (extra && extra[0]) sb_printf(&oc, " %s", extra);

        int orc = system(sb_str(&oc));
        if (orc != 0) {
            fprintf(stderr, "error: オブジェクトの生成に失敗しました\n  %s\n",
                    sb_str(&oc));
            return 1;
        }
        if (!opt.keep_ll) unlink(entry->ll_path);
        return 0;
    }

    // ── ⑤ clang に丸投げして実行ファイルを作る ──
    //
    // ★ Windows では拡張子が無いと実行できないので、`.` を含まない
    //   出力名には .exe を足します（gcc / clang と同じふるまい）。
    const char *out_path = opt.output;
#ifdef _WIN32
    if (!strchr(opt.output, '.')) {
        StrBuf w;
        sb_init(&w);
        sb_printf(&w, "%s.exe", opt.output);
        out_path = sb_str(&w);
    }
#endif
    //
    // ★ **モジュールごとに .o を作ってからリンク**します。
    //
    //   0.13.0 までは「1 つの clang に .ll を全部渡す」でした。
    //   clang は入力を順番に処理するので、**コアが 12 あっても 1 つしか
    //   回りません**。分ければそのまま並列になります（実測 4.21s → 1.41s）。
    //
    //   注意: 出来上がる実行ファイルは変わりません。clang は元々、複数の .ll を
    //     渡されても 1 本ずつ独立にコンパイルしていました（LTO ではありません）。
    int nmods = 0;
    for (Module *m = mods; m; m = m->next) nmods++;

    CcJob *jobs = xmalloc(sizeof(CcJob) * (size_t)nmods);
    char **objs = xmalloc(sizeof(char *) * (size_t)nmods);
    int k = 0;
    for (Module *m = mods; m; m = m->next, k++) {
        StrBuf op;
        sb_init(&op);
        sb_printf(&op, "%s.%s.o", opt.output, m->name);
        objs[k] = sb_str(&op);

        StrBuf c;
        sb_init(&c);
        // ★ -g のときは clang にも渡します（DWARF を実際に作らせるため。A-30）
        sb_printf(&c, "%s %s%s -c \"%s\" -o \"%s\"", clang_cmd(), opt.opt_level,
                  opt.debug ? " -g" : "", m->ll_path, objs[k]);
        jobs[k].cmd = sb_str(&c);
        jobs[k].rc = 0;
    }

    int rc = run_jobs(jobs, nmods, opt.jobs);
    if (rc == 0) {
        // ── リンク ──
        StrBuf cmd;
        sb_init(&cmd);
        sb_printf(&cmd, "%s %s%s", clang_cmd(), opt.opt_level,
                  opt.debug ? " -g" : "");
        for (int i = 0; i < nmods; i++) sb_printf(&cmd, " \"%s\"", objs[i]);
        // ★ ランタイム（runtime/runtime.c をコンパイルしたもの）をリンクする。
        sb_printf(&cmd, " \"%s\"", runtime_o());
        // ★ スレッド（A-18）。ランタイムが pthread を使うので、
        //   POSIX ではリンク時に -pthread が要ります（新しめの glibc は libc に
        //   入っていますが、古い環境と *BSD では明示しないと undefined になります）。
        //   注意: Windows のスレッドは kernel32 にあるので、何も足しません。
#ifndef _WIN32
        sb_printf(&cmd, " -pthread");
#else
        // ★ ソケット（A-22）。Windows の socket は ws2_32.dll にあります
        //   （POSIX では libc に入っているので何も足しません）。
        sb_printf(&cmd, " -lws2_32");
#endif
        // ★ 利用者が指定したリンクの指定（A-33）。ランタイムの後に置きます
        //   （後から来たものが先のものの未解決を埋める、という並びのため）。
        for (int i = 0; i < opt.nlink; i++)
            sb_printf(&cmd, " \"%s\"", opt.link[i]);
        sb_printf(&cmd, " -o \"%s\"", out_path);
        rc = system(sb_str(&cmd));

        // ★ macOS では DWARF が **.o の中に残り**、実行ファイルには
        //   「どの .o にあるか」の地図だけが入ります（デバッグマップ）。
        //   こちらは .o を片付けてしまうので、そのままではデバッガが
        //   行を出せません。dsymutil で <出力>.dSYM にまとめてから消します。
        //   注意: Linux / Windows は実行ファイルに直接入るので、何もしません。
        // 注意: triple は「指定が無ければ NULL」です。ここで既定を補わないと、
        //   ふつうに使ったとき（指定なし）に dsymutil が走りません。
        const char *eff_triple = triple ? triple : PLC_TARGET_TRIPLE;
        if (rc == 0 && opt.debug && strstr(eff_triple, "apple")) {
            StrBuf dsym;
            sb_init(&dsym);
            sb_printf(&dsym, "dsymutil \"%s\" 2>/dev/null", out_path);
            // 注意: 失敗しても止めません（dsymutil が無い環境でも実行ファイルは
            //   できています。デバッグ情報が無いだけです）。
            (void)system(sb_str(&dsym));
        }
    }

    // 注意: .o は成否によらず片付けます（.ll は失敗時だけ残します。下記）。
    for (int i = 0; i < nmods; i++) unlink(objs[i]);

    if (rc != 0) {
        // ここに来たら、生成した IR に問題があるということ。
        // .ll を残して調査できるようにする。
        fprintf(stderr,
                "error: clang の実行に失敗しました（生成した IR に問題があります）\n"
                "  生成された IR を残しました:\n");
        for (Module *m = mods; m; m = m->next)
            fprintf(stderr, "    %s\n", m->ll_path);
        return 1;
    }

    if (!opt.keep_ll)
        for (Module *m = mods; m; m = m->next) unlink(m->ll_path);
    return 0;
}
