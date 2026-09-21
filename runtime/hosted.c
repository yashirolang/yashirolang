// hosted.c — PC の上で動かすときのランタイム（分離）
//
// ★ 役割は 2 つです。
//   ① core.c が求める 4 つのフックを libc で実装する
//   ② ファイル入出力など、**OS があるからこそ使える機能**を提供する
//
// 注意: ベアメタルではこのファイルをリンクしません。
//    代わりにカーネルが同じ 4 つのフックを実装します。

// 注意: **どの #include よりも前に書くこと。**
//
//   glibc は `-std=c11`（＝ `__STRICT_ANSI__` が立つ）のとき、POSIX の
//   拡張を宣言しません。`popen` / `pclose` がまさにそれで、
//   **Linux の CI だけがビルドに失敗**しました:
//
//     error: call to undeclared function 'popen'
//
//   macOS の SDK は既定で見せるので、手元では気づけません。
//   `_DEFAULT_SOURCE` は glibc に「POSIX 2008 + BSD の拡張を見せる」と
//   伝える印で、macOS と MSYS2 では単に無視されます。
//
//   注意: 同じ理由で `sys/wait.h` も明示的に include しています（下）。
//     ランタイムに POSIX の関数を足すときは、ここを思い出してください。
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include <errno.h>
#include <signal.h>   // SIGPIPE を無視する（ソケット。A-22）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

// 注意: WIFEXITED / WEXITSTATUS は POSIX の <sys/wait.h> にあります。
//    macOS では <stdlib.h> が連れてきますが、Linux では明示しないと通りません
//    （CI の Linux ジョブが最初に見つけた移植性の穴です）。
//    Windows にはこのヘッダが無いので、system() の戻り値をそのまま使います。
#ifndef _WIN32
#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>
#else
// 注意: Windows の標準出力は既定で「テキストモード」で、\n を \r\n に書き換えます。
//    それでは **書いたバイトと出るバイトが違う**ことになり、
//    「どの OS でも同じ結果」という約束が崩れます（CI の Windows ジョブが
//    出力の不一致で見つけました）。binary モードに切り替えて、
//    print が書いた通りのバイトを出します。
// 注意: **winsock2.h は windows.h より前に include すること。**
//   windows.h は古い winsock.h を連れてくるので、あとから winsock2.h を
//   読むと「再定義」の山になります（Windows で最初に踏む穴です）。
//   WIN32_LEAN_AND_MEAN を立てて winsock.h を外し、winsock2.h を先に読みます。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
// 注意: MSVC には S_ISDIR がありません（MinGW にはあります）。
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#endif

#include "core.h"

// core.c が使う関数のうち、ここでも使うもの
char *pl_str_alloc(long long len);
char *pl_str_from_cstr(const char *s);
long long pl_str_len(const char *s);
_Noreturn void pl_panic(const char *msg);
void *pl_alloc(long long size);

// ★ リストの中身（PlList）は core.c の中だけの秘密です。
//   ここでは「ポインタとして受け渡す」だけなので、不完全型で足ります。
typedef struct PlList PlList;
PlList *pl_list_new(void);
void pl_list_push_ptr(PlList *l, void *v);

// ── ① フックの実装（libc で）────────────────────────────────

void *pl_hook_alloc(long long size) { return calloc(1, (size_t)size); }

void pl_hook_free(void *p) { free(p); }

void pl_hook_write(const char *s, long long len) {
#ifdef _WIN32
    static int mode_set = 0;
    if (!mode_set) {
        mode_set = 1;
        _setmode(_fileno(stdout), _O_BINARY);
        _setmode(_fileno(stderr), _O_BINARY);
    }
#endif
    fwrite(s, 1, (size_t)len, stdout);
}

void pl_hook_panic(const char *msg) {
    fprintf(stderr, "runtime error: %s\n", msg);
    exit(1);
}

// ── ② OS があるからこそ使える機能 ───────────────────────────

static long long g_argc;
static char **g_argv;

// stderr にそのまま書く（改行は付けない）。
//
// ★ コンパイラは診断を stderr に書きます。print は stdout なので、
//   セルフホストの診断にはこれが要ります（移植で見つかった穴）。
void pl_eprint(const char *s) {
#ifdef _WIN32
    _setmode(_fileno(stderr), _O_BINARY);  // 上と同じ理由
#endif
    fputs(s, stderr);
}

_Noreturn void pl_exit(long long code) { exit((int)code); }

char *pl_read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        char buf[512];
        snprintf(buf, sizeof(buf), "cannot open file: %s", path);
        pl_panic(buf);
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = pl_str_alloc((long long)size);
    size_t got = fread(buf, 1, (size_t)size, fp);
    buf[got] = '\0';
    // 注意: テキストモードの差などで読めたバイト数が減ることがあるので、
    //    実際に読めた長さで書き直します（長さは 8 バイト手前）。
    ((long long *)buf)[-1] = (long long)got;
    fclose(fp);
    return buf;
}

// ── 標準入力 ────────────────────────────────────────────
//
// 注意: **core.c ではなく、ここ（hosted.c）に置きます。**
//    ベアメタルには標準入力がありません。core.c に置くと、カーネル側に
//    「使わないのに実装しなければならないフック」を強いることになります
//    （docs/design/os-support.md の 4 フックを増やさない、という判断）。

// 1 行読む。改行は**含めません**。EOF で 1 行も読めなければ NULL
// （本言語側では str | None の None になります）。
char *pl_read_line(void) {
    long long cap = 128;
    long long n = 0;
    char *buf = (char *)malloc((size_t)cap);
    if (!buf) pl_panic("out of memory");

    int c = fgetc(stdin);
    if (c == EOF) {
        free(buf);
        return NULL;
    }
    // 注意: 改行の直前の '\r' を落とします。Windows で作ったファイルを
    //    読んだときに、末尾に見えない文字が残らないようにするためです。
    while (c != EOF && c != '\n') {
        if (n + 1 >= cap) {
            cap *= 2;
            char *g = (char *)realloc(buf, (size_t)cap);
            if (!g) pl_panic("out of memory");
            buf = g;
        }
        buf[n++] = (char)c;
        c = fgetc(stdin);
    }
    if (n > 0 && buf[n - 1] == '\r') n--;

    char *out = pl_str_alloc(n);
    memcpy(out, buf, (size_t)n);
    out[n] = '\0';
    free(buf);
    return out;
}

// input(prompt) — プロンプトを出して 1 行読む
//
// ★ Python の input() に合わせます。
//   注意: **EOF では panic します。** Python も EOFError を投げます。
//     「読めなかった」を静かに空文字列にすると、ループが止まらなくなります。
//     読めないかもしれない場面では io.read_line()（None が返る）を使ってください。
char *pl_input(const char *prompt) {
    if (prompt && prompt[0]) {
        fputs(prompt, stdout);
        fflush(stdout);
    }
    char *line = pl_read_line();
    if (!line) pl_panic("input: 入力がありません（EOF）");
    return line;
}

// 標準入力を最後まで読む。何も無ければ空文字列。
char *pl_read_all(void) {
    long long cap = 4096;
    long long n = 0;
    char *buf = (char *)malloc((size_t)cap);
    if (!buf) pl_panic("out of memory");

    while (1) {
        if (n == cap) {
            cap *= 2;
            char *g = (char *)realloc(buf, (size_t)cap);
            if (!g) pl_panic("out of memory");
            buf = g;
        }
        size_t got = fread(buf + n, 1, (size_t)(cap - n), stdin);
        if (got == 0) break;
        n += (long long)got;
    }

    char *out = pl_str_alloc(n);
    memcpy(out, buf, (size_t)n);
    out[n] = '\0';
    free(buf);
    return out;
}

void pl_write_file(const char *path, const char *text) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        char buf[512];
        snprintf(buf, sizeof(buf), "cannot write file: %s", path);
        pl_panic(buf);
    }
    fputs(text, fp);
    fclose(fp);
}

// 注意: bool ではなく int を返します。extern の境界を bool は越えられません
//    （14.2 節。C の _Bool と i1 の ABI が環境依存のため）。
long long pl_file_exists(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fclose(fp);
    return 1;
}

// 生成される C の main が、いちばん最初に呼びます。
void pl_set_args(long long argc, char **argv) {
    g_argc = argc;
    g_argv = argv;
}

// 注意: system() が返すのは「終了コード」ではなく wait(2) の状態値です。
//    そのまま返すと exit 3 が 768（3 << 8）に見えて驚くので、
//    ここで終了コードに直します。境界の食い違いはランタイムで吸収します。
// ファイルを削除する（失敗しても何もしない）。
// ★ コンパイラは中間ファイル（.ll）を片付ける必要があります。
void pl_remove(const char *path) { remove(path); }

// 環境変数を読む（無ければ空文字列）。
//
// ★ stage1 が標準ライブラリの場所を知るために使います。C 版はビルド時に
//   埋め込みますが（-DPLC_LIB_DIR）、本言語にプリプロセッサは無いので
//   実行時に読みます。C 版も同じ環境変数を見るようにして、挙動を揃えます。
char *pl_getenv(const char *name) {
    const char *v = getenv(name);
    if (!v) return pl_str_from_cstr("");
    return pl_str_from_cstr(v);
}

long long pl_system(const char *cmd) {
    int st = system(cmd);
    if (st == -1) return -1;
#ifdef _WIN32
    // Windows の system() は終了コードをそのまま返します
    return (long long)st;
#else
    if (WIFEXITED(st)) return (long long)WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return (long long)(128 + WTERMSIG(st));
    return (long long)st;
#endif
}

// argv を list[str] にして返す。
//
// ★ PlList を使うので、list の実装より後ろに置いています。
//   argv の文字列はプロセスの寿命のあいだ有効なので、複製せずそのまま指します。
PlList *pl_argv(void) {
    PlList *l = pl_list_new();
    // 注意: argv の文字列は C のものなので長さヘッダがありません。
    //    本言語の str として渡すには作り直す必要があります。
    for (long long i = 0; i < g_argc; i++)
        pl_list_push_ptr(l, pl_str_from_cstr(g_argv[i]));
    return l;
}

// ── 時刻 ────────────────────────────────────────────────
//
// ★ lib/time がこの 2 つだけを使います。単位は**ナノ秒**です。
//   秒やミリ秒への換算は本言語側でやります（境界は int だけで済ませる）。
//
// 注意: **ベアメタルにはありません。** 時計は OS（か割り込み）の持ち物なので、
//    core.c ではなくここに置いています。kernel/ からは import time できません。

// 単調時計。**起点に意味はありません**（差だけを使ってください）。
//
// 注意: 実時刻を使わないのは、NTP の補正で**時間が戻ることがある**ためです。
//    速さを測っている最中に戻ると、負の経過時間が出ます。
long long pl_time_ns(void) {
#if defined(CLOCK_MONOTONIC)
    // POSIX（macOS 10.12+ / Linux / MSYS2 の mingw-w64）
    {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
            return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
    }
#endif
    // 注意: 単調時計が無い環境への逃げ道。実時刻なので**戻ることがあります**。
    //
    // 注意: **timespec_get は「C11 だからどこでもある」ではありません。**
    //    MSYS2（mingw-w64）の clang は -std=c11 でも持っていません
    //    （CI の Windows ジョブが「undeclared function」で見つけました）。
    //    TIME_UTC が定義されているかで判断します。
#if defined(TIME_UTC)
    {
        struct timespec ts;
        if (timespec_get(&ts, TIME_UTC) == TIME_UTC)
            return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
    }
#endif
    // 最後の逃げ道（秒の精度しかありません）。time は C89 からあります。
    return (long long)time(NULL) * 1000000000LL;
}

// 実時刻（1970-01-01 00:00:00 UTC からのナノ秒）。
//
// 注意: こちらは**飛びます**（NTP・夏時間・利用者が時計を直す）。
//    経過時間を測るのには使わないでください。
long long pl_time_wall_ns(void) {
#if defined(TIME_UTC)
    {
        struct timespec ts;
        if (timespec_get(&ts, TIME_UTC) == TIME_UTC)
            return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
    }
#endif
#if defined(CLOCK_REALTIME)
    {
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
            return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
    }
#endif
    return (long long)time(NULL) * 1000000000LL;
}

// ── スレッド（A-18） ─────────────────────────────────
//
// ★ 置き場所が core.c ではなく hosted.c なのは、**ベアメタルに OS の
//   スレッドが無い**からです（time と同じ扱い。docs/design/concurrency.md 3）。
//
// ★ ABI は「i64 を 2 つ取って i64 を 1 つ返す」に統一してあります。
//   1 つ目が**呼ぶ相手の関数ポインタ**、2 つ目が**その引数**です。
//
//   本言語側の関数は型がまちまち（fn(Node) -> str、fn(float) -> float …）
//   ですが、codegen が **シグネチャごとに中継関数（thunk）** を 1 つ出して、
//   その thunk をここに渡します。
//
//   注意: 「全部 i64 に読み替えて直接呼ぶ」ではいけません。float は整数と
//      **別のレジスタ**で渡され、None を返す関数には戻り値レジスタが
//      ありません。型どおりに呼ぶのは thunk の仕事、待ち合わせるのが
//      このファイルの仕事、と分けてあります。
// ★ 引数は**何個でも**受け取れます（i64 の配列で渡します）。
//   クロージャが無いので、複数の値をスレッドに渡すには
//   「クラスにまとめる」しかありませんでした。ところが**クラスは借りを
//   保存できません**（E-BORROW-3）。つまり scope: を入れても、借りを
//   共有する形が書けないままでした。引数を可変長にして解きます。
typedef long long (*pl_thread_body)(long long fn, const long long *args);

// scope の枠（実体はこのファイルの下のほう）
static void pl_scope_register(void *h);

#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>   // sysconf（pl_cpu_count）

// スレッド 1 本ぶんの覚え書き。join で戻り値を受け取るために、
// 「何を呼ぶか」と「何が返ったか」を同じ箱に入れて持ち回します。
typedef struct {
    pthread_t      id;
    pl_thread_body thunk;
    long long      fn;
    long long     *args;    // 引数の写し（呼ぶ側のスタックに置けないため）
    long long      nargs;
    long long      ret;
    int            joined;  // もう待ち合わせたか（join は何度呼んでもよい）
    int            scoped;  // scope の枠に記録されているか
} PlThread;

static void *pl_thread_trampoline(void *p) {
    PlThread *t = (PlThread *)p;
    t->ret = t->thunk(t->fn, t->args);
    return NULL;
}

// 注意: args は**この呼び出しのあいだだけ**有効で構いません。ここで写しを
//    取ってからスレッドを作るので、呼ぶ側は alloca に置けます。
void *pl_thread_spawn(pl_thread_body thunk, long long fn, const long long *args,
                      long long nargs) {
    PlThread *t = (PlThread *)pl_hook_alloc((long long)sizeof(PlThread));
    if (!t) pl_hook_panic("spawn: メモリが足りません");
    t->thunk = thunk;
    t->fn = fn;
    t->nargs = nargs;
    t->args = NULL;
    if (nargs > 0) {
        t->args = (long long *)pl_hook_alloc(nargs * (long long)sizeof(long long));
        if (!t->args) pl_hook_panic("spawn: メモリが足りません");
        for (long long i = 0; i < nargs; i++) t->args[i] = args[i];
    }
    t->ret = 0;
    t->joined = 0;
    t->scoped = 0;
    pl_scope_register(t);   // scope: の中なら枠に記録する
    if (pthread_create(&t->id, NULL, pl_thread_trampoline, t) != 0)
        pl_hook_panic("spawn: スレッドを作れませんでした");
    return t;
}

// ★ join は**何度呼んでもよい**ようにしました。scope の出口が
//   「まだ待っていないものを待つ」ために呼ぶので、利用者が明示的に
//   join した後でも安全に通れる必要があります。
long long pl_thread_join(void *h) {
    PlThread *t = (PlThread *)h;
    if (!t->joined) {
        if (pthread_join(t->id, NULL) != 0)
            pl_hook_panic("join: スレッドの終了を待てませんでした");
        t->joined = 1;
    }
    long long r = t->ret;
    // 注意: 枠に記録されているものは、ここでは解放しません（枠の出口で
    //    もう一度たどるため）。記録されていないものは今までどおり。
    if (!t->scoped) {
        pl_hook_free(t->args);
        pl_hook_free(t);
    }
    return r;
}

// ── mutex[T] ──
//
// ★ 生の lock / unlock は**出しません**。「ロックを取り、渡された関数を呼び、
//   必ず解く」の 1 つだけにします（解き忘れが起きえない形。設計文書 §4）。
typedef struct {
    pthread_mutex_t m;
    long long       value;  // 中身（ポインタを i64 として持つ）
} PlMutex;

void *pl_mutex_new(long long value) {
    PlMutex *m = (PlMutex *)pl_hook_alloc((long long)sizeof(PlMutex));
    if (!m) pl_hook_panic("mutex: メモリが足りません");
    if (pthread_mutex_init(&m->m, NULL) != 0)
        pl_hook_panic("mutex: 初期化に失敗しました");
    m->value = value;
    return m;
}

long long pl_mutex_with(void *h, pl_thread_body thunk, long long fn) {
    PlMutex *m = (PlMutex *)h;
    pthread_mutex_lock(&m->m);
    long long a[1];
    a[0] = m->value;
    long long r = thunk(fn, a);
    pthread_mutex_unlock(&m->m);
    return r;
}

#else  // _WIN32
#include <windows.h>

typedef struct {
    HANDLE         id;
    pl_thread_body thunk;
    long long      fn;
    long long     *args;
    long long      nargs;
    long long      ret;
    int            joined;
    int            scoped;
} PlThread;

static DWORD WINAPI pl_thread_trampoline(LPVOID p) {
    PlThread *t = (PlThread *)p;
    t->ret = t->thunk(t->fn, t->args);
    return 0;
}

void *pl_thread_spawn(pl_thread_body thunk, long long fn, const long long *args,
                      long long nargs) {
    PlThread *t = (PlThread *)pl_hook_alloc((long long)sizeof(PlThread));
    if (!t) pl_hook_panic("spawn: メモリが足りません");
    t->thunk = thunk;
    t->fn = fn;
    t->nargs = nargs;
    t->args = NULL;
    if (nargs > 0) {
        t->args = (long long *)pl_hook_alloc(nargs * (long long)sizeof(long long));
        if (!t->args) pl_hook_panic("spawn: メモリが足りません");
        for (long long i = 0; i < nargs; i++) t->args[i] = args[i];
    }
    t->ret = 0;
    t->joined = 0;
    t->scoped = 0;
    pl_scope_register(t);
    t->id = CreateThread(NULL, 0, pl_thread_trampoline, t, 0, NULL);
    if (!t->id) pl_hook_panic("spawn: スレッドを作れませんでした");
    return t;
}

long long pl_thread_join(void *h) {
    PlThread *t = (PlThread *)h;
    if (!t->joined) {
        if (WaitForSingleObject(t->id, INFINITE) != WAIT_OBJECT_0)
            pl_hook_panic("join: スレッドの終了を待てませんでした");
        CloseHandle(t->id);
        t->joined = 1;
    }
    long long r = t->ret;
    if (!t->scoped) {
        pl_hook_free(t->args);
        pl_hook_free(t);
    }
    return r;
}

typedef struct {
    CRITICAL_SECTION m;
    long long        value;
} PlMutex;

void *pl_mutex_new(long long value) {
    PlMutex *m = (PlMutex *)pl_hook_alloc((long long)sizeof(PlMutex));
    if (!m) pl_hook_panic("mutex: メモリが足りません");
    InitializeCriticalSection(&m->m);
    m->value = value;
    return m;
}

long long pl_mutex_with(void *h, pl_thread_body thunk, long long fn) {
    PlMutex *m = (PlMutex *)h;
    EnterCriticalSection(&m->m);
    long long a[1];
    a[0] = m->value;
    long long r = thunk(fn, a);
    LeaveCriticalSection(&m->m);
    return r;
}
#endif

// ── 子プロセスを起こす／待つ ──────────────────────────
//
// ★ sys.run（system()）は**終わるまで戻ってきません**。コンパイラのドライバが
//   モジュールごとの clang を並べるには、「起こす」と「待つ」を分ける必要が
//   あります（C 版 src/main.c の run_jobs と対になる道具）。
//
// 注意: **スレッドから system() を呼ぶ形にしてはいけません。** macOS の system()
//    はシグナル処理を守るためにグローバルなロックを取るので、何本のスレッドから
//    呼んでも 1 本ずつしか走りません（C 版で実測して気づきました）。
#ifndef _WIN32
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

// /bin/sh -c cmd を起こして pid を返す。起こせなければ -1。
long long pl_proc_spawn(const char *cmd) {
    char *argv[] = {(char *)"/bin/sh", (char *)"-c", (char *)cmd, NULL};
    pid_t pid;
    if (posix_spawn(&pid, "/bin/sh", NULL, NULL, argv, environ) != 0) return -1;
    return (long long)pid;
}

// pid の終了を待って、終了コードを返す。
long long pl_proc_wait(long long pid) {
    int status = 0;
    if (waitpid((pid_t)pid, &status, 0) < 0) return 1;
    return WIFEXITED(status) ? (long long)WEXITSTATUS(status) : 1;
}
#else
// 注意: Windows では**使えません**（-1 を返します）。呼ぶ側は sys.run に落ちます。
//    cmd.exe には & による並行実行が無く、並べるなら Win32 の API を
//    別に書くことになります。「効かないだけで、壊れない」ほうを選びました。
long long pl_proc_spawn(const char *cmd) {
    (void)cmd;
    return -1;
}

long long pl_proc_wait(long long pid) {
    (void)pid;
    return 1;
}
#endif


// ── scope（スレッドの生存範囲） ─────────────────────────
//
// なぜ「枠」が要るのか
//   scope: ブロックの出口で、**そこで始めたスレッドを全部** join する必要が
//   あります。変数に入っているものだけ見ればよい、とは言えません
//   （list[Thread[R]] に貯めることがあるため）。だから spawn した時点で
//   枠に記録し、出口でその枠をたどります。
//
// 注意: 枠はスレッドごとに別々です（_Thread_local）。scope: の中で spawn した
//    スレッドが、さらにその中で scope: を開くことがあるためです。
//
// 注意: **この保証が「借りをスレッドに渡してよい」の根拠です。** 出口までに
//    必ず join されるので、「借りは呼び出しより長生きしない」という
//    借用検査の不変条件が**成り立ったまま**になります。
typedef struct PlScope PlScope;
struct PlScope {
    void      **items;
    long long   n;
    long long   cap;
    PlScope    *outer;
};

#if defined(_WIN32)
#define PL_TLS __declspec(thread)
#else
#define PL_TLS _Thread_local
#endif

static PL_TLS PlScope *g_pl_scope;

// spawn から呼ばれる。いまの枠があれば記録する。
static void pl_scope_register(void *h) {
    PlScope *sc = g_pl_scope;
    if (!sc) return;
    if (sc->n == sc->cap) {
        long long ncap = sc->cap ? sc->cap * 2 : 8;
        void **ni = (void **)pl_hook_alloc(ncap * (long long)sizeof(void *));
        if (!ni) pl_hook_panic("scope: メモリが足りません");
        for (long long i = 0; i < sc->n; i++) ni[i] = sc->items[i];
        pl_hook_free(sc->items);
        sc->items = ni;
        sc->cap = ncap;
    }
    sc->items[sc->n++] = h;
    ((PlThread *)h)->scoped = 1;   // join がここで解放しないようにする
}

void pl_scope_begin(void) {
    PlScope *sc = (PlScope *)pl_hook_alloc((long long)sizeof(PlScope));
    if (!sc) pl_hook_panic("scope: メモリが足りません");
    sc->items = NULL;
    sc->n = 0;
    sc->cap = 0;
    sc->outer = g_pl_scope;
    g_pl_scope = sc;
}

// 枠を閉じる。**まだ待っていないスレッドを全部 join** してから捨てます。
void pl_scope_end(void) {
    PlScope *sc = g_pl_scope;
    if (!sc) return;
    g_pl_scope = sc->outer;
    for (long long i = 0; i < sc->n; i++) {
        PlThread *t = (PlThread *)sc->items[i];
        t->scoped = 0;          // ここから先は join が解放してよい
        pl_thread_join(sc->items[i]);
    }
    pl_hook_free(sc->items);
    pl_hook_free(sc);
}

// 使える CPU コアの数（並列度を決めるのに使います）。
// 注意: 0 を返してはいけません（利用者が割り算に使うため）。
long long pl_cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    long long n = (long long)si.dwNumberOfProcessors;
#else
    long long n = (long long)sysconf(_SC_NPROCESSORS_ONLN);
#endif
    return n > 0 ? n : 1;
}

// ── ファイルシステム ───────────────────────────────────
//
// ★ ここまで io は「読む・書く・消す・あるか」の 4 つだけでした。
//   パッケージマネージャを書いたときに、ディレクトリを作るのも
//   一覧するのも `sys.run("mkdir -p …")` に頼るしかないことが分かったので、
//   境界をランタイムに引き直します（シェルを介さない＝引用の心配が無い）。
//
// 注意: 返り値の約束（本言語側で bool に直します）:
//     0  … できた
//     1  … 既にある（mkdir だけ）
//    -1  … 失敗した
long long pl_mkdir(const char *path) {
    if (!path || !path[0]) return -1;
#ifdef _WIN32
    if (_mkdir(path) == 0) return 0;
#else
    if (mkdir(path, 0777) == 0) return 0;
#endif
    if (errno == EEXIST) return 1;
    return -1;
}

long long pl_is_dir(const char *path) {
    if (!path || !path[0]) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

// 空のディレクトリを消す（中身が残っていれば失敗します）
long long pl_rmdir(const char *path) {
    if (!path || !path[0]) return -1;
#ifdef _WIN32
    return _rmdir(path) == 0 ? 0 : -1;
#else
    return rmdir(path) == 0 ? 0 : -1;
#endif
}

long long pl_rename(const char *from, const char *to) {
    if (!from || !to) return -1;
    return rename(from, to) == 0 ? 0 : -1;
}

// ファイルの大きさ（バイト）。無ければ -1。
long long pl_file_size(const char *path) {
    struct stat st;
    if (!path || stat(path, &st) != 0) return -1;
    return (long long)st.st_size;
}

// ディレクトリの中身を list[str] で返す（"." と ".." は除く）。
//
// 注意: **並びは OS 任せです。** 名前順が要るなら呼び出し側で整列してください
//    （そうしないと、同じプログラムが環境によって違う順で動きます）。
// 注意: 開けなかったときは**空のリスト**を返します。空のディレクトリと
//    区別したいときは先に pl_is_dir で確かめてください。
PlList *pl_listdir(const char *path) {
    PlList *l = pl_list_new();
    if (!path || !path[0]) return l;
#ifdef _WIN32
    // Windows は FindFirstFile（dirent.h が無い環境があるため）
    {
        char pat[4096];
        size_t n = strlen(path);
        if (n + 3 >= sizeof(pat)) return l;
        memcpy(pat, path, n);
        pat[n] = '\\';
        pat[n + 1] = '*';
        pat[n + 2] = '\0';
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) return l;
        do {
            const char *nm = fd.cFileName;
            if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
            pl_list_push_ptr(l, pl_str_from_cstr(nm));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR *d = opendir(path);
        if (!d) return l;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            pl_list_push_ptr(l, pl_str_from_cstr(e->d_name));
        }
        closedir(d);
    }
#endif
    return l;
}

// ── コマンドの出力を受け取る ───────────────────────────
//
// ★ これが無いと、外部コマンドの出力を読むのに**一時ファイルを経由**する
//   しかありません（パッケージマネージャの shell.capture が実際そうでした）。
//
// 注意: 受け取るのは標準出力だけです。標準エラーも欲しいときは
//   コマンド側に "2>&1" を書いてください（混ぜるかどうかは呼ぶ側の判断）。
// 注意: 終了コードは pl_capture_status() で受け取ります。戻り値を 2 つ返せない
//   ためで、**スレッドごとに別の値**を持ちます（同時に走らせても混ざりません）。
//
// 注意: **Windows では popen を使いません。**
//   `_popen` は `cmd.exe` を起動しますが、`system()`（＝ sys.run）は
//   MSYS2 では `sh` を使います。同じ文字列が run と capture で**別の
//   シェルに解釈される**のは危険なので（`echo a; echo b` が 1 行になって
//   CI が落ちました）、Windows だけは system() にリダイレクトさせて
//   結果のファイルを読み直します。**どちらの経路でもシェルは 1 つ**です。
static PL_TLS long long g_capture_status = -1;

long long pl_capture_status(void) { return g_capture_status; }

// 開いたストリームを最後まで読む（読めた分だけ返す）
static char *pl_slurp(FILE *fp, size_t *out_len) {
    size_t cap = 4096, len = 0;
    char *buf = (char *)pl_hook_alloc((long long)cap);
    if (!buf) {
        *out_len = 0;
        return NULL;
    }
    for (;;) {
        if (len + 1024 > cap) {
            size_t ncap = cap * 2;
            char *nb = (char *)pl_hook_alloc((long long)ncap);
            if (!nb) break;
            memcpy(nb, buf, len);
            pl_hook_free(buf);
            buf = nb;
            cap = ncap;
        }
        size_t got = fread(buf + len, 1, 1024, fp);
        len += got;
        if (got < 1024) break;
    }
    *out_len = len;
    return buf;
}

// 読んだバイト列を本言語の str（長さヘッダ付き）にして返す
static char *pl_take_str(char *buf, size_t len) {
    char *out = pl_str_alloc((long long)len);
    if (buf) memcpy(out, buf, len);
    out[len] = '\0';
    if (buf) pl_hook_free(buf);
    return out;
}

#ifdef _WIN32

char *pl_capture(const char *cmd) {
    g_capture_status = -1;
    if (!cmd) return pl_str_from_cstr("");

    char dir[MAX_PATH];
    char path[MAX_PATH];
    if (GetTempPathA((DWORD)sizeof(dir), dir) == 0) return pl_str_from_cstr("");
    if (GetTempFileNameA(dir, "plc", 0, path) == 0) return pl_str_from_cstr("");

    // 注意: 一時ファイルの名前は GetTempFileNameA が作るので空白は入りませんが、
    //    念のため引用符で囲みます（sh と cmd のどちらでも通る形）。
    size_t n = strlen(cmd) + strlen(path) + 8;
    char *full = (char *)pl_hook_alloc((long long)n);
    if (!full) return pl_str_from_cstr("");
    snprintf(full, n, "%s > \"%s\"", cmd, path);

    int st = system(full);
    pl_hook_free(full);
    g_capture_status = st == -1 ? -1 : (long long)st;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        remove(path);
        return pl_str_from_cstr("");
    }
    size_t len = 0;
    char *buf = pl_slurp(fp, &len);
    fclose(fp);
    remove(path);
    return pl_take_str(buf, len);
}

#else

char *pl_capture(const char *cmd) {
    g_capture_status = -1;
    if (!cmd) return pl_str_from_cstr("");

    // ★ POSIX では popen も system も /bin/sh を使うと決まっています。
    //   だから同じ文字列が両者で同じ意味になり、パイプで直に受け取れます。
    FILE *fp = popen(cmd, "r");
    if (!fp) return pl_str_from_cstr("");

    size_t len = 0;
    char *buf = pl_slurp(fp, &len);

    int st = pclose(fp);
    if (st == -1) {
        g_capture_status = -1;
    } else if (WIFEXITED(st)) {
        g_capture_status = (long long)WEXITSTATUS(st);
    } else if (WIFSIGNALED(st)) {
        g_capture_status = 128 + (long long)WTERMSIG(st);
    } else {
        g_capture_status = -1;
    }
    return pl_take_str(buf, len);
}

#endif

// ══════════════════════════════════════════════════════════════════
//  ソケット（A-22）
// ══════════════════════════════════════════════════════════════════
//
// ★ **ここ（hosted.c）にしか置きません。** ベアメタルにネットワークは
//   ありません。core.c の 4 フックを増やさない、という方針のとおりです。
//
// ★ 方針：**薄く、素直に。** 提供するのは BSD ソケットの 9 つだけです
//   （listen / accept / connect / send / recv / close / port と、
//   A-23 で足した set_timeout / timed_out）。多重化（select / epoll）は
//   まだありません。要るようになってから足します。
//
// 注意: **失敗は戻り値で返します。panic しません。**
//   「相手が切った」「ポートが使われている」は**ふつうに起きること**で、
//   プログラムが続けられなければ困ります。直前の失敗の理由は
//   pl_sock_error() が文字列で返します（errno を持ち回らせないため）。
//
// 注意: **fd は int です。** Windows の SOCKET は符号なし 64 ビットですが、
//   実際に返る値は小さく、INVALID_SOCKET だけが特別です。ここで
//   -1 に正規化して、言語側からは「負なら失敗」だけを見れば済むようにします。

#ifdef _WIN32
// ★ winsock2.h / ws2tcpip.h はファイルの先頭で読んであります
//   （windows.h より前でなければならないため。上の説明を参照）。
typedef int pl_socklen;
#define PL_SOCK_ERRNO WSAGetLastError()
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>         // accept の待ち時間（下の pl_sock_wait_accept）
#include <sys/socket.h>
#include <sys/time.h>     // struct timeval（SO_RCVTIMEO。A-23）
#include <sys/types.h>
typedef socklen_t pl_socklen;
#define PL_SOCK_ERRNO errno
#define closesocket close
#endif

// 直前の失敗の理由（言語側は pl_sock_error() で取り出す）
static char g_sock_err[256] = {0};

// 注意: **待ち時間切れは、他の失敗と分けられなければ役に立ちません**（A-23）。
//   「まだ来ないだけ」と「壊れた」を同じ NetError にしてしまうと、
//   呼び出し側が「もう一度待つ」のか「あきらめて閉じる」のかを選べません。
//   直前の失敗がそれだったかを覚えておき、pl_sock_timed_out() で渡します。
static int g_sock_timeout = 0;

// この失敗は「待ち時間を過ぎた」か。
//
// 注意: 待ち時間付きの recv が空振りしたときに返るのは ETIMEDOUT とは
//   限りません。**POSIX は EAGAIN / EWOULDBLOCK を返します**（Linux では
//   この 2 つは同じ値なので、|| で並べても 1 つぶんです）。
static int pl_sock_is_timeout(int code) {
#ifdef _WIN32
    return code == WSAETIMEDOUT || code == WSAEWOULDBLOCK;
#else
    return code == ETIMEDOUT || code == EAGAIN || code == EWOULDBLOCK;
#endif
}

static void pl_sock_fail(const char *what, int code) {
    g_sock_timeout = pl_sock_is_timeout(code);
    if (g_sock_timeout) {
        // ★ 待ち時間切れは OS の文言（"Resource temporarily unavailable"）では
        //   何が起きたのか伝わらないので、こちらで言い直します。
        snprintf(g_sock_err, sizeof(g_sock_err), "%s: 待ち時間を過ぎました", what);
        return;
    }
#ifdef _WIN32
    snprintf(g_sock_err, sizeof(g_sock_err), "%s: エラー番号 %d", what, code);
#else
    snprintf(g_sock_err, sizeof(g_sock_err), "%s: %s", what, strerror(code));
#endif
}

static void pl_sock_ok(void) { g_sock_err[0] = '\0'; g_sock_timeout = 0; }

char *pl_sock_error(void) { return pl_str_from_cstr(g_sock_err); }

// 直前の失敗は待ち時間切れだったか（1 なら yes）。
long long pl_sock_timed_out(void) { return g_sock_timeout ? 1 : 0; }

// 受け取り・送り出しの待ち時間を決める（ミリ秒。0 なら無期限）。
//
// ★ **これが無いと、サーバーは黙って止まります。** 繋いだまま何も
//   送ってこない相手が 1 つあるだけで、accept の輪がそこで止まります
//   （http.serve は 1 本ずつ順に捌くので、他の客も全員待たされます）。
//
// 注意: **繋ぎに行く（connect）の待ち時間はこれでは決まりません。**
//   SO_RCVTIMEO / SO_SNDTIMEO は繋がったあとの読み書きにだけ効きます。
//   connect を区切るには非同期の接続と select が要るので、まだありません。
long long pl_sock_set_timeout(long long fd, long long ms) {
    if (fd < 0) {
        snprintf(g_sock_err, sizeof(g_sock_err), "閉じたソケットです");
        g_sock_timeout = 0;
        return -1;
    }
    if (ms < 0) ms = 0;
#ifdef _WIN32
    // ★ Windows はミリ秒の DWORD をそのまま渡します（timeval ではありません）。
    DWORD v = (DWORD)ms;
    const char *pv = (const char *)&v;
    pl_socklen vn = (pl_socklen)sizeof(v);
#else
    struct timeval v;
    v.tv_sec = (time_t)(ms / 1000);
    v.tv_usec = (int)((ms % 1000) * 1000);
    const char *pv = (const char *)&v;
    pl_socklen vn = (pl_socklen)sizeof(v);
#endif
    if (setsockopt((int)fd, SOL_SOCKET, SO_RCVTIMEO, pv, vn) != 0 ||
        setsockopt((int)fd, SOL_SOCKET, SO_SNDTIMEO, pv, vn) != 0) {
        pl_sock_fail("setsockopt", PL_SOCK_ERRNO);
        return -1;
    }
    pl_sock_ok();
    return 0;
}

// 注意: **Windows は使う前に WSAStartup が要ります。**
//   利用者に「最初に init を呼んでください」とは言いたくないので、
//   ソケットを作る入口で 1 回だけ済ませます。POSIX では何もしません。
static int pl_sock_start(void) {
    static int done = 0;
    if (done) return 1;
#ifdef _WIN32
    WSADATA w;
    int rc = WSAStartup(MAKEWORD(2, 2), &w);
    if (rc != 0) {
        pl_sock_fail("WSAStartup", rc);
        return 0;
    }
#else
    // 注意: **書き込み側が閉じた相手へ送ると SIGPIPE で落ちます。**
    //   サーバーでは「相手が先に切る」のがふつうなので、無視して
    //   send の戻り値（EPIPE）として扱えるようにします。
    signal(SIGPIPE, SIG_IGN);
#endif
    done = 1;
    return 1;
}

// host:port を解決して addrinfo を返す（passive なら bind 用）。
//
// ★ **IPv4 と IPv6 の両方を返します**（A-23。A-22 では AF_INET だけでした）。
//   どちらで繋がるかは相手と機械の設定しだいなので、ここでは選ばず、
//   **呼び出し側が返ってきた順に試します**。
//
//   注意: v4 だけにしていた頃は、`localhost` が `::1` に解決される機械で
//     繋がりませんでした。名前が複数の住所を持つのがふつうで、
//     「1 つめが駄目なら次」を**呼び出し側が書かなければならない**のが
//     getaddrinfo の作法です。
static struct addrinfo *pl_sock_resolve(const char *host, long long port,
                                        int passive) {
    char service[16];
    snprintf(service, sizeof(service), "%lld", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;      // ★ IPv4 も IPv6 も
    hints.ai_socktype = SOCK_STREAM;
    if (passive) hints.ai_flags = AI_PASSIVE;

    struct addrinfo *res = NULL;
    const char *node = (host && host[0]) ? host : NULL;
    int rc = getaddrinfo(node, service, &hints, &res);
    if (rc != 0) {
        snprintf(g_sock_err, sizeof(g_sock_err), "名前を解決できません: %s",
                 gai_strerror(rc));
        return NULL;
    }
    return res;
}

// 待ち受ける。成功すれば fd、失敗すれば -1。
//
// ★ port に 0 を渡すと OS が空いているポートを選びます。選ばれた番号は
//   pl_sock_port(fd) で取れます。**テストがこれを使います**
//   （決め打ちのポートは、同時に走らせると衝突するため）。
long long pl_sock_listen(const char *host, long long port, long long backlog) {
    if (!pl_sock_start()) return -1;
    struct addrinfo *res = pl_sock_resolve(host, port, 1);
    if (!res) return -1;

    // ★ 返ってきた住所を**順に試します**（A-23）。IPv6 しか使えない機械も、
    //   IPv4 しか使えない機械もあるので、1 つめで決め打ちできません。
    int fd = -1;
    int last_code = 0;
    const char *last_what = "socket";

    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            last_what = "socket"; last_code = PL_SOCK_ERRNO;
            continue;
        }

        // 注意: **SO_REUSEADDR を既定で立てます。** これが無いと、落としたばかりの
        //   サーバーを立て直すときに「アドレスが使用中です」で数十秒待たされます
        //   （TIME_WAIT）。サーバーを書く人がまず引っかかるところです。
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));

#ifdef IPV6_V6ONLY
        // 注意: **v6 の待ち受け口は、既定では v4 の客を受けない機械があります**
        //   （Windows と多くの BSD。Linux は設定しだい）。0 を入れて
        //   **1 本で両方**受けられるようにします。断られたら（OpenBSD は
        //   これを許しません）v6 だけの待ち受け口として続けます。
        if (ai->ai_family == AF_INET6) {
            int off = 0;
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
                       (const char *)&off, sizeof(off));
        }
#endif

        if (bind(fd, ai->ai_addr, (pl_socklen)ai->ai_addrlen) != 0) {
            last_what = "bind"; last_code = PL_SOCK_ERRNO;
            closesocket(fd); fd = -1;
            continue;
        }
        if (listen(fd, (int)(backlog > 0 ? backlog : 16)) != 0) {
            last_what = "listen"; last_code = PL_SOCK_ERRNO;
            closesocket(fd); fd = -1;
            continue;
        }
        break;                        // ここまで来たら成功
    }
    freeaddrinfo(res);

    if (fd < 0) {
        pl_sock_fail(last_what, last_code);
        return -1;
    }
    pl_sock_ok();
    return fd;
}

// 実際に割り当てられたポート番号（listen(…, 0) のあとで使う）。
//
// 注意: **sockaddr_storage で受けます。** sockaddr_in（v4 ぶん）では IPv6 の
//   住所が入りきらず、切り詰められた中身からポートを読むことになります
//   （A-23 で v6 を受け付けるようになったので、v4 決め打ちは危険です）。
long long pl_sock_port(long long fd) {
    struct sockaddr_storage a;
    pl_socklen n = (pl_socklen)sizeof(a);
    memset(&a, 0, sizeof(a));
    if (getsockname((int)fd, (struct sockaddr *)&a, &n) != 0) {
        pl_sock_fail("getsockname", PL_SOCK_ERRNO);
        return -1;
    }
    pl_sock_ok();
    if (a.ss_family == AF_INET6)
        return (long long)ntohs(((struct sockaddr_in6 *)&a)->sin6_port);
    return (long long)ntohs(((struct sockaddr_in *)&a)->sin_port);
}

// 待ち時間が決まっていれば、その時間だけ「客が来るか」を待つ。
//
// ★ **なぜ accept の前に待つのか。**
//   待ち時間は SO_RCVTIMEO で入れていますが、**これを accept に効かせるかは
//   OS で違います**。Linux は効かせ、**Darwin（macOS）は見ません**。
//   そのため macOS では「客が来なければ戻ってくる」輪が書けず、
//   accept がそのまま待ち続けていました（2026-09-21 に実測で判明）。
//
//   ★ **どの OS でも同じ結果にするため、自分で待ちます。** 待ってから
//     accept を呼べば、SO_RCVTIMEO を accept に効かせるかどうかに
//     依存しなくなります。
//
// ★ **待ち時間は別に覚えません。** いま入っている SO_RCVTIMEO を
//   読み返します。覚えると「setsockopt で入れた値」と「こちらが覚えた値」の
//   2 つができ、必ずずれます。
//
// 戻り値:  1 … 客が来た（accept してよい）
//          0 … 待ち時間を過ぎた
//         -1 … 失敗
//
// 注意: POSIX では poll を使います。select の fd_set は FD_SETSIZE（多くは
//   1024）までしか入らず、**fd がそれを超えると書き潰します**。Windows の
//   fd_set は添字ではなく SOCKET の配列なので、そちらは select で構いません。
static int pl_sock_wait_accept(int fd) {
#ifdef _WIN32
    DWORD ms = 0;
    int n = (int)sizeof(ms);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&ms, &n) != 0) return 1;
    if (ms == 0) return 1;                    // 無期限
    struct timeval tv;
    tv.tv_sec = (long)(ms / 1000);
    tv.tv_usec = (long)((ms % 1000) * 1000);
    fd_set r;
    FD_ZERO(&r);
    FD_SET((SOCKET)fd, &r);
    int rc = select(0, &r, NULL, NULL, &tv);
    if (rc > 0) return 1;
    return rc == 0 ? 0 : -1;
#else
    struct timeval tv;
    pl_socklen n = (pl_socklen)sizeof(tv);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, &n) != 0) return 1;
    long long ms = (long long)tv.tv_sec * 1000 + (long long)tv.tv_usec / 1000;
    if (ms <= 0) return 1;                    // 無期限
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    int rc;
    // 注意: **合図（シグナル）で中断されたら待ち直します。** ここで
    //   EINTR をそのまま失敗にすると、関係の無い合図 1 つでサーバーが
    //   「時間切れ」でもないのに止まります。
    do { rc = poll(&p, 1, (int)ms); } while (rc < 0 && errno == EINTR);
    if (rc > 0) return 1;
    return rc == 0 ? 0 : -1;
#endif
}

// 1 本受け付ける（相手が来るまで待つ）。失敗すれば -1。
//
// ★ **待ち時間を決めてあれば、そこで戻ります**（Listener.set_timeout）。
//   時間切れは NetError の timed_out で見分けられます。
long long pl_sock_accept(long long fd) {
    int w = pl_sock_wait_accept((int)fd);
    if (w == 0) {
        // ★ 待ち時間切れ。**失敗と分けます** — 「まだ来ていない」だけで、
        //   待ち受け口は生きており、もう一度 accept できます。
        g_sock_timeout = 1;
        snprintf(g_sock_err, sizeof(g_sock_err), "accept: 待ち時間を過ぎました");
        return -1;
    }
    if (w < 0) {
        pl_sock_fail("accept", PL_SOCK_ERRNO);
        return -1;
    }
    int c = (int)accept((int)fd, NULL, NULL);
    if (c < 0) {
        pl_sock_fail("accept", PL_SOCK_ERRNO);
        return -1;
    }
    pl_sock_ok();
    return c;
}

// つなぎに行く。成功すれば fd、失敗すれば -1。
//
// ★ **繋がるまで住所を順に試します**（A-23）。`localhost` のように
//   v6 と v4 の両方を持つ名前は珍しくなく、1 つめで断られても
//   次で繋がることがあります。
long long pl_sock_connect(const char *host, long long port) {
    if (!pl_sock_start()) return -1;
    struct addrinfo *res = pl_sock_resolve(host, port, 0);
    if (!res) return -1;

    int fd = -1;
    int last_code = 0;
    const char *last_what = "socket";

    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            last_what = "socket"; last_code = PL_SOCK_ERRNO;
            continue;
        }
        if (connect(fd, ai->ai_addr, (pl_socklen)ai->ai_addrlen) != 0) {
            last_what = "connect"; last_code = PL_SOCK_ERRNO;
            closesocket(fd); fd = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        pl_sock_fail(last_what, last_code);
        return -1;
    }
    pl_sock_ok();
    return fd;
}

// 書けるだけ書く。**全部書けるまで繰り返します**（部分送信は上に見せない）。
// 書いたバイト数を返す。失敗すれば -1。
//
// 注意: **待ち時間を決めてあると、途中まで書けた状態で失敗しえます**（A-23）。
//   どこまで届いたかは分からないので、**送り出しが時間切れになった接続は
//   閉じてください**。続きを書いても相手には壊れた列が届きます。
long long pl_sock_send(long long fd, const char *s) {
    long long n = pl_str_len(s);
    long long sent = 0;
    while (sent < n) {
        // 注意: MSG_NOSIGNAL が無い環境（macOS / Windows）があるので 0 を渡し、
        //   代わりに SIGPIPE を無視します（下の pl_sock_init_once）。
        long long k = (long long)send((int)fd, s + sent, (size_t)(n - sent), 0);
        if (k <= 0) {
            pl_sock_fail("send", PL_SOCK_ERRNO);
            return -1;
        }
        sent += k;
    }
    pl_sock_ok();
    return sent;
}

// 最大 max バイト読む。
//
// 注意: **戻り値の意味を 3 つに分けます。**
//     文字列（長さ > 0） … 読めた
//     ""                  … 相手が閉じた（EOF）
//     None                … 失敗（理由は pl_sock_error）
//   EOF とエラーを混ぜると、サーバーの while ループが書けません。
char *pl_sock_recv(long long fd, long long max) {
    if (max <= 0) max = 4096;
    char *buf = (char *)pl_hook_alloc(max);
    if (!buf) return NULL;

    long long k = (long long)recv((int)fd, buf, (size_t)max, 0);
    if (k < 0) {
        pl_sock_fail("recv", PL_SOCK_ERRNO);
        pl_hook_free(buf);
        return NULL;
    }
    pl_sock_ok();
    return pl_take_str(buf, (size_t)k);   // k == 0 なら "" ＝ EOF
}

// 閉じる（閉じ済み・負の fd に渡してもよい）。
void pl_sock_close(long long fd) {
    if (fd < 0) return;
    closesocket((int)fd);
}
