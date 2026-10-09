#include "sema.h"

#include "module.h"

#include <string.h>

#include "diag.h"
#include "ownck.h"   // ty_is_rc（rc[T] / rc[T] | None は「共有」型）
#include "types.h"
#include "util.h"

// ── シンボルテーブルとスコープ ──────────────────────────────
//
// 「名前 → 型」の対応表です。
//
// なぜハッシュテーブルではなく線形リストなのか
//   1 つのスコープに宣言される変数は普通 10 個程度です。
//   線形探索で十分速く、コードは 5 行で済みます。
//   「まず動かす、測ってから直す」が原則（docs/ja/spec/type-system.md 7.2）。

typedef struct VarEntry VarEntry;
struct VarEntry {
    char *name;      // 本言語上の名前（エラーメッセージ用）
    char *ir_name;   // LLVM 上の名前。★ 記号（% / @）まで含めた完全な形
                     //   ローカル : %x, %x.1
                     //   グローバル: @g.x
    bool is_global;
    // ★ 型が 2 つになりました。
    //   declared … 宣言された型（Token | None）。代入できるかはこれで判定する
    //   type     … 今の型。絞り込まれていれば Token になっている
    Type *declared;
    Type *type;
    Token *decl_tok;  // 宣言された位置（再宣言エラーで「前の宣言はここ」を示す）
    // ★ 捕獲した lambda を入れたことがある変数なら、その捕獲の並び（A-51）。
    //   この変数を読んだ節点にも書き写すので、「変数に入れてから返す・しまう」
    //   も、lambda を直接書いたときと同じ検査で止まります。
    Node *caps;
    VarEntry *next;
};

typedef struct Scope Scope;
struct Scope {
    Scope *parent;   // 外側のスコープ（グローバルなら NULL）
    VarEntry *vars;  // このスコープで宣言された変数
};

// 意味解析の状態。
// のちに「今どの関数を検査中か」（戻り型の検査に必要）が加わります。
// これまでに使った IR 名の記録（衝突を避けるため）
typedef struct UsedName UsedName;
struct UsedName {
    char *name;
    UsedName *next;
};

// 関数のシグネチャ表。
//
// ★ 本体を見る前に、全部の関数をここに登録します（8.5 節）。
//   前方参照と再帰が自然に通るようになります。
typedef struct ModuleSyms ModuleSyms;

typedef struct FuncSig FuncSig;
struct FuncSig {
    char *name;      // 表を引く鍵（"add" / "Token.show"。★ モジュール内で一意）
    char *ir_name;   // IR 上の名前（"lexer.add" / "lexer.Token.show"）
    Type *ret;

    // ── raises 節 ──
    // ★ 失敗しうる関数は、IR 上でエラー出力用の引数を 1 本余分に取ります
    //   （docs/ja/design/error-handling.md §2）。
    Class **raises;  // 宣言されたエラー型（クラス）
    int nraises;
    Type **params;  // 引数の型
    char **pnames;  // 引数名（エラーメッセージ用）
    // ★ 引数の受け取り方（own か、借用か）。
    //   codegen が「実引数の一時値を呼び出し後に解放してよいか」を
    //   判断するのに使います（A-21e）。own なら所有権が移るので解放しません。
    ParamMode *pmodes;
    // ★ 既定値（A-38）。書かれていない引数は NULL。
    //   注意: **リテラルの木そのもの**を持ちます。sema が呼び出し側へ
    //     複製して入れるので、呼び出しごとに 1 つずつ作られます
    //     （Python のように「定義時に 1 つ作って共有」しません）。
    Node **defaults;
    // ★ lambda から作った実体か（A-42）。エラーの言葉を変えるために持ちます。
    bool is_lambda;
    int nparams;
    Token *tok;     // 定義位置（「この関数はここで定義されています」用）
    ModuleSyms *owner;  // どのモジュールのものか

    // ★ ジェネリック関数のテンプレート（実体化前）
    Node *tmpl;         // ND_FUNC（targs を持つ）。実体なら NULL
    // ★ 定義の木（A-43）。捕獲した lambda を渡してよいか——つまり
    //   「この関数は受け取った関数の値をしまうか」——を見るのに使います。
    Node *node;

    FuncSig *next;
};

// ── モジュールごとのシンボル表 ──────────────────────
//
// ★ 当初は「表が 1 本ずつ」でした。モジュールが増えると
//   「モジュールごとに 1 本ずつ」になります。名前空間とはこれのことです。
//   他のモジュールの中身は、必ず修飾（lexer.Token）を通してしか見えません。
// 範囲型（部分型。A-28）の表。
//
// ★ クラスと同じ「モジュールごとの名前の表」です。型そのものは
//   名前つきの int（types.c の type_range）なので、ここでは名前と
//   Type * の対応だけを持ちます。
typedef struct RangeTy RangeTy;
struct RangeTy {
    char *name;
    Type *type;
    Token *tok;     // 定義位置（「ここで定義されています」用）
    RangeTy *next;
};

struct ModuleSyms {
    Module *mod;
    Iface *ifaces;     // インタフェース
    FuncSig *funcs;    // トップレベル関数とメソッド
    Class *classes;
    RangeTy *ranges;   // 範囲型（type Percent = int range(0, 100)）
    EnumDef *enums;    // 列挙（enum Color: ...。A-37）
    Scope *globals;    // グローバル変数のスコープ
    ModuleSyms *next;
};

// エラー型の ID。
//
// ★ 0 は「エラー無し」に予約し、1 から連番を振ります。
//   注意: 割り当て規則は仕様として固定してあります（error-handling.md §4）：
//     「モジュールを依存順に、モジュール内は出現順に」。
//     stage0 と stage1 で番号が食い違うと IR が一致しなくなるためです。
typedef struct ErrTag ErrTag;
struct ErrTag {
    Class *cls;
    int tag;
    ErrTag *next;
};

typedef struct {
    Scope *scope;      // 現在のスコープ
    int loop_depth;    // 今いるループの深さ（break / continue の検査用）
    UsedName *used;    // 割り当て済みの IR 名（関数ごとにリセット）
    FuncSig *funcs;    // 関数表（メソッドも "Token.show" として入る）
                       // ★ これは「今検査中のモジュールの」表です
    FuncSig *cur_func; // 今どの関数を検査中か（return の検査に必要）
    Class *classes;    // クラス表（同上、モジュールごと）
    Iface *ifaces;     // インタフェース表（モジュールごと）
    RangeTy *ranges;   // 範囲型の表（同上、モジュールごと）
    EnumDef *enums;    // 列挙の表（同上、モジュールごと。A-37）
    int next_slot;     // 次に振る vtable のスロット番号

    // ── モジュール ──
    ModuleSyms *cur;   // 今検査中のモジュール
    ModuleSyms *mods;  // 全モジュール（依存順）

    // 今この式に期待されている型。
    //
    // ★ 空リスト [] だけは、それ自身から要素型が決まりません。
    //   本格的なやり方は双方向型検査（期待型を引数で渡す）ですが、
    //   v1 で期待型を必要とする式は [] だけなので、状態を 1 つ持たせて済ませます。
    // 注意: 期待型が要る式が増えたら、この手は破綻します。そのときは引数で渡す形に直します。
    Type *expected;

    // ── エラー処理 ──
    ErrTag *err_tags;   // エラー型 → ID
    int next_err_tag;   // 次に振る番号（1 から）
    struct TryCtx *cur_try;  // 今いる try 文（入れ子になるのでスタック）

    // ── 契約（A-29）──
    //
    // ★ 'result' が「戻り値そのもの」を指すのは、**ensures の式の中だけ**です。
    //   ここが 0 なら、result はただの変数名として扱われます
    //   （この処理系自身が result という局所変数を何か所も使っています）。
    int ensures_depth;

    // ── 低レベル ──
    int unsafe_depth;  // unsafe: の中にいる深さ（0 なら外）
    int scope_depth;   // scope: の中にいる深さ（0 なら外）

    // ── ジェネリクス（単相化）──
    //
    // ★ 型引数の束縛。K → str のような対応を、テンプレートを読む間だけ
    //   有効にします（「入る前に積んで、抜けたら降ろす」。絞り込みと同じ手）。
    struct TBind *tbind;

    // ★ 実体化したクラスの待ち行列。本体の検査は**あとでまとめて**行います。
    //   検査の途中で新しい実体が増えるので、その場でやると入れ子になります。
    struct Instance *pending;

    // ★ いま検査しているのが「どこで要求された実体か」。
    //   注意: テンプレートの中で出たエラーは、**ライブラリの中の行**を
    //     指してしまいます。使う側のどの行が発端かを添えます。
    Token *inst_site;
    const char *inst_name;

    // ★ sema が作る隠し変数の連番（A-41 の match）。
    //   注意: この言語は同名の変数を隠せない（シャドーイングなし）ので、
    //     名前は**必ず一意**にします。
    int hidden;
} Sema;

// 型引数の束縛（K → str）
typedef struct TBind TBind;
struct TBind {
    const char *name;
    Type *type;
    TBind *next;
};

// 実体化したクラス 1 つぶん（本体の検査を後回しにするための記録）
typedef struct Instance Instance;
struct Instance {
    Node *node;          // 複製した ND_CLASS
    ModuleSyms *owner;   // テンプレートが定義されているモジュール
    TBind *binds;        // そのときの型引数の束縛
    Token *site;         // ★ どこで要求されたか（エラーに添える）
    const char *iname;   // その実体の名前（Dict$Node$int）
    bool is_func;        // ★ 関数の実体か（クラスでなく）
    Instance *next;
};

// 入れ子の try（内側で捕まらなければ外側が受け止める）
typedef struct TryCtx TryCtx;
struct TryCtx {
    Node *node;
    TryCtx *outer;
};

// エラー型の ID を引く（無ければ割り当てる）
static int err_tag_of(Sema *s, Class *c) {
    for (ErrTag *e = s->err_tags; e; e = e->next)
        if (e->cls == c) return e->tag;
    ErrTag *e = xmalloc(sizeof(ErrTag));
    e->cls = c;
    e->tag = ++s->next_err_tag;
    // ★ 末尾に足す（登録順＝出現順を保つため）
    if (!s->err_tags) {
        s->err_tags = e;
    } else {
        ErrTag *t = s->err_tags;
        while (t->next) t = t->next;
        t->next = e;
    }
    return e->tag;
}

// いま入っている try のどれかが c を捕まえるか。
// ★ 捕まえた try には印を付けます（「意味のない try」の警告に使う）。
static bool try_catches(Sema *s, Class *c) {
    for (TryCtx *t = s->cur_try; t; t = t->outer)
        for (Node *ex = t->node->els; ex; ex = ex->next)
            if (ex->type && ex->type->cls == c) {
                t->node->can_fail = true;
                return true;
            }
    return false;
}

// 今検査中の関数が c を raises に宣言しているか
static bool func_declares(FuncSig *f, Class *c) {
    if (!f) return false;
    for (int i = 0; i < f->nraises; i++)
        if (f->raises[i] == c) return true;
    return false;
}

// ── モジュールの出入り ────────────────────────────────
//
// ★ 「今どのモジュールを検査中か」を切り替えるだけの関数です。
//   表そのものは Sema に置いたまま（それまでのコードが 1 行も変わらない）、
//   切り替えるときに ModuleSyms へ書き戻します。
static void enter_module(Sema *s, ModuleSyms *ms) {
    if (s->cur) {  // 今のモジュールの状態を保存する
        s->cur->funcs = s->funcs;
        s->cur->classes = s->classes;
        s->cur->ifaces = s->ifaces;
        s->cur->ranges = s->ranges;
        s->cur->globals = s->scope;
    }
    s->cur = ms;
    s->funcs = ms->funcs;
    s->classes = ms->classes;
    s->ifaces = ms->ifaces;
    s->ranges = ms->ranges;
    s->scope = ms->globals;
}

// import しているモジュールを名前で引く。
// 注意: import していないモジュールは、たとえ読み込まれていても見えません。
static ModuleSyms *lookup_import(Sema *s, const char *name) {
    Module *m = s->cur->mod;
    for (int i = 0; i < m->ndeps; i++)
        if (strcmp(m->deps[i]->name, name) == 0) return m->deps[i]->syms;
    return NULL;
}

// IR 上の修飾名を作る（mangle をモジュールにも使う）
static char *mangle(const char *prefix, const char *name);

static char *mod_mangle(Sema *s, const char *name) {
    return mangle(s->cur->mod->name, name);
}

// ── インタフェースを引く ────────────────────────────────
static Iface *lookup_iface_in(ModuleSyms *ms, const char *name) {
    for (Iface *i = ms->ifaces; i; i = i->next)
        if (strcmp(i->name, name) == 0) return i;
    return NULL;
}

static Iface *lookup_iface(Sema *s, const char *name) {
    for (Iface *i = s->ifaces; i; i = i->next)
        if (strcmp(i->name, name) == 0) return i;
    return NULL;
}

static FuncSig *lookup_func_in(ModuleSyms *ms, const char *name) {
    for (FuncSig *f = ms->funcs; f; f = f->next)
        if (strcmp(f->name, name) == 0) return f;
    return NULL;
}

static Class *lookup_class_in(ModuleSyms *ms, const char *name) {
    for (Class *c = ms->classes; c; c = c->next)
        if (strcmp(c->name, name) == 0) return c;
    return NULL;
}

static VarEntry *lookup_global_in(ModuleSyms *ms, const char *name) {
    for (VarEntry *v = ms->globals->vars; v; v = v->next)
        if (strcmp(v->name, name) == 0) return v;
    return NULL;
}

static FuncSig *lookup_func(Sema *s, const char *name) {
    for (FuncSig *f = s->funcs; f; f = f->next)
        if (strcmp(f->name, name) == 0) return f;
    return NULL;
}

// IR 名（モジュール修飾済み）で引く。
// ★ 表の鍵は「モジュール内で一意な名前」（add / Token.show）ですが、
//   検査中の関数を特定するときは、定義ノードに入っている IR 名から引くのが
//   確実です（関数もメソッドも同じ 1 行で済む）。
static FuncSig *lookup_func_by_ir(Sema *s, const char *ir_name) {
    for (FuncSig *f = s->funcs; f; f = f->next)
        if (strcmp(f->ir_name, ir_name) == 0) return f;
    return NULL;
}

// ── クラス表と名前修飾 ────────────────────────────────
//
// ★ メソッドは「名前を修飾しただけの、ただの関数」です。
//   名前を "Token.show" にしてしまえば、関数表にそのまま載ります。
//   '.' を含む名前は利用者が書ける識別子と絶対に衝突しません
//   （脱糖が作る隠し変数 for.ix.0 と同じ手口）。
static RangeTy *lookup_range_in(ModuleSyms *ms, const char *name) {
    for (RangeTy *r = ms->ranges; r; r = r->next)
        if (strcmp(r->name, name) == 0) return r;
    return NULL;
}

// ── 列挙の引き当て（A-37）──────────────────────────────
static EnumDef *lookup_enum_in(ModuleSyms *ms, const char *name) {
    for (EnumDef *e = ms->enums; e; e = e->next)
        if (strcmp(e->name, name) == 0) return e;
    return NULL;
}

static EnumDef *lookup_enum(Sema *s, const char *name) {
    for (EnumDef *e = s->enums; e; e = e->next)
        if (strcmp(e->name, name) == 0) return e;
    return NULL;
}

// 枝を名前で引く（無ければ NULL）
static EnumVal *lookup_enum_val(EnumDef *e, const char *name) {
    for (EnumVal *v = e->vals; v; v = v->next)
        if (strcmp(v->name, name) == 0) return v;
    return NULL;
}

static RangeTy *lookup_range(Sema *s, const char *name) {
    for (RangeTy *r = s->ranges; r; r = r->next)
        if (strcmp(r->name, name) == 0) return r;
    return NULL;
}

static Class *lookup_class(Sema *s, const char *name) {
    for (Class *c = s->classes; c; c = c->next)
        if (strcmp(c->name, name) == 0) return c;
    return NULL;
}

static char *mangle(const char *cls, const char *method) {
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "%s.%s", cls, method);
    return sb_str(&sb);
}

static Field *lookup_field(Class *c, const char *name) {
    for (Field *f = c->fields; f; f = f->next)
        if (strcmp(f->name, name) == 0) return f;
    return NULL;
}

static Scope *scope_push(Sema *s) {
    Scope *sc = xmalloc(sizeof(Scope));
    sc->parent = s->scope;
    s->scope = sc;
    return sc;
}

// if / while のブロックスコープで使います。
// 今はトップレベルの 1 段だけなので、対になる pop は最後の 1 回だけです。
static void scope_pop(Sema *s) { s->scope = s->scope->parent; }

// 現在のスコープだけを探す（再宣言の検査用）
static VarEntry *lookup_local(Sema *s, const char *name) {
    for (VarEntry *v = s->scope->vars; v; v = v->next)
        if (strcmp(v->name, name) == 0) return v;
    return NULL;
}

// 内側から外側へ順に探す（名前解決）
static VarEntry *lookup(Sema *s, const char *name) {
    for (Scope *sc = s->scope; sc; sc = sc->parent)
        for (VarEntry *v = sc->vars; v; v = v->next)
            if (strcmp(v->name, name) == 0) return v;
    return NULL;
}

// ── IR 名の割り当て ─────────────────────────────────
//
// ★ 「シャドーイング禁止なので変数名がそのまま一意」という前提は、
//   ブロックスコープが入ると崩れます。兄弟スコープが同じ名前を使えるからです。
//
//       if a:
//           x: int = 1     ← %x
//       if b:
//           x: int = 2     ← %x（衝突！どちらも相手を隠していない）
//
//   衝突したら連番を足します。これは名前修飾（mangling）の入口で、
//   メソッドとモジュールで本格的に必要になります。
//
// なぜ sema がやるのか
//   parser はスコープを知らず、codegen は宣言と参照を結びつける情報を
//   持っていません。シンボルテーブルを持つ sema だけが両方できます。
static bool name_used(Sema *s, const char *name) {
    // 注意: "entry" は予約する。
    //
    //   LLVM ではラベルとローカル値が同じ名前空間にいます。関数の先頭は
    //   慣習的に `entry:` なので、利用者が `entry` という変数を書くと
    //   `%entry = alloca` と衝突して IR が壊れます（stage1 の移植で踏んだ）。
    //
    //   ★ コンパイラが作る名前は '.' を含めて衝突を避ける約束ですが
    //     （%t.0 / if.then.0 / for.ix.0）、`entry` だけは慣習を優先して
    //     '.' を入れていません。その代わりここで予約します。
    if (strcmp(name, "entry") == 0) return true;

    for (UsedName *u = s->used; u; u = u->next)
        if (strcmp(u->name, name) == 0) return true;
    return false;
}

static void remember_name(Sema *s, char *name) {
    UsedName *u = xmalloc(sizeof(UsedName));
    u->name = name;
    u->next = s->used;
    s->used = u;
}

static char *unique_ir_name(Sema *s, char *name) {
    if (!name_used(s, name)) {
        remember_name(s, name);
        return name;
    }
    for (int i = 1;; i++) {
        StrBuf sb;
        sb_init(&sb);
        sb_printf(&sb, "%s.%d", name, i);
        char *cand = sb_str(&sb);
        if (!name_used(s, cand)) {
            remember_name(s, cand);
            return cand;
        }
    }
}

// ★ モジュール名と同じ名前は宣言できない（13.5 節）。
//
//   import lexer
//   lexer: int = 1      ← エラー
//
// 「lexer.x」の lexer が変数かモジュールか、読む人が迷うからです。
// コンパイラは「変数優先」と決めれば動きますが、それは規則を覚える負担を
// 利用者に押しつけることになります。シャドーイング禁止と同じ判断です。
static void reject_module_name(Sema *s, const char *name, Token *tok,
                               const char *what) {
    ModuleSyms *ms = lookup_import(s, name);
    if (!ms) return;

    Diag d = {0};
    d.message = MSG1("sema.001", "'{0}' は import したモジュールの名前です", name);
    d.primary.tok = tok;
    d.primary.label = MSG1("sema.002", "この名前の{0}は宣言できません", what);
    d.hint = MSG1("sema.003", "モジュール名と同じ名前を使うと '{0}.x' が曖昧になります", name);
    diag_fail(&d);
}

// ローカル変数として登録する（IR 名は %x 形式）
static VarEntry *declare(Sema *s, char *name, Type *type, Token *tok) {
    reject_module_name(s, name, tok, MSG0("sema.366", "変数"));

    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "%%%s", unique_ir_name(s, name));

    VarEntry *v = xmalloc(sizeof(VarEntry));
    v->name = name;
    v->ir_name = sb_str(&sb);
    v->declared = type;
    v->type = type;
    v->decl_tok = tok;
    v->next = s->scope->vars;
    s->scope->vars = v;
    return v;
}

// ── 式の検査 ────────────────────────────────────────────────
//
// check_expr の約束：
//   「式を検査し、n->type を埋めて、その型を返す」
//
// gen_expr（コード生成）と対になる構造です。
static Type *check_expr(Sema *s, Node *n);

static Type *check_call(Sema *s, Node *n);
static Type *check_list_lit(Sema *s, Node *n);
static Type *check_index_expr(Sema *s, Node *n);
static Type *check_listcomp(Sema *s, Node *n);
static void check_unpack(Sema *s, Node *n);
static void rewrite_old(Node *fn);
static Type *check_method(Sema *s, Node *n);
static Type *check_class_method(Sema *s, Node *n, Class *c);
static Type *check_field(Sema *s, Node *n);
// 既定値の並びと型を確かめる（A-38）
static void check_defaults(Sema *s, FuncSig *f, Node *fn);
// raises 節を解決する（A-40 でインタフェースの宣言からも使います）
static void resolve_raises(Sema *s, Node *fn, FuncSig *f);
// 中身を持つ枝の隠しクラスを引く / 生成に書き換える（A-41）
static Class *branch_class(Sema *s, EnumDef *e, EnumVal *ev);
// 捕獲した lambda を逃がさない（A-43）
static void reject_escaping_closure(Node *v, const char *what);
static bool capture_by_value(Type *t);   // A-51
static bool fn_param_escapes_at(Sema *s, Node *body, const char *pname,
                                int hops);
static Type *check_new(Sema *s, Node *n, Class *c);
// lambda を「使う側の型」から実体にする（A-42）
static FuncSig *instantiate_lambda(Sema *s, FuncSig *tmpl, Node *ref);
// 呼び出しの引数を並べ替えて、足りないぶんを既定値で埋める（A-38）
static void bind_args(Node *n, int nparams, char **pnames, Node **defaults,
                      Token *deftok, const char *subject, bool has_self);
static void bind_args_sig(Node *n, FuncSig *f, int skip, const char *subject);

// 型注釈（構文）を Type（意味）に変換する。
//
// ★ 「名前から型への解決は sema の仕事」（判断 #47）が、
//   複合型になっても同じ形で通用します。
static Type *resolve_base_type(Sema *s, Node *tr);

// ── 範囲型（部分型）の検査を挿す（A-28）────────────────
//
// ★ 「入れるとき」だけ確かめます（Ada と同じ）。途中の計算は基底型（int）で
//   行うので、`p * 2` のたびに止まることはありません。
//
// ★ 定数なら**コンパイル時に**断ります。実行時まで待つ理由がありません。
//   それ以外は ND_RANGECHK で包み、codegen が比較 2 つを出します。
//
// 注意: 仮引数は**ここでは包みません**。呼び出し側は 5 通りもあるので、
//   関数の入口で 1 回だけ確かめます（codegen の gen_func）。そのほうが
//   関数ポインタ越しの呼び出しや spawn も同じ 1 か所で守れます。
static Node *range_coerce(Sema *s, Node *val, Type *want) {
    (void)s;
    if (!val || !ty_is_range(want)) return val;
    if (val->type == want) return val;   // 同じ部分型なら確かめ済み

    // ★ 定数はコンパイル時に決まります
    if (val->kind == ND_INT) {
        if (val->ival < want->lo || val->ival > want->hi) {
            Diag d = {0};
            d.message = MSG4("sema.004", "{0} は '{1}' の範囲（{2}..{3}）の外です", diag_fmt("%lld", val->ival), want->name, diag_fmt("%lld", want->lo), diag_fmt("%lld", want->hi));
            d.primary.tok = val->tok;
            d.primary.label = MSG0("sema.367", "この値はこの型に入りません");
            d.hint = MSG3("sema.005", "'{0}' に入れられるのは {1} から {2} までです", want->name, diag_fmt("%lld", want->lo), diag_fmt("%lld", want->hi));
            diag_fail(&d);
        }
        return val;
    }

    Node *chk = new_node(ND_RANGECHK, val->tok);
    chk->lhs = val;
    chk->type = want;
    return chk;
}


// ★ T | None を包む層。
//   nullable にできるのは参照型（str / list / class）だけです。
static Type *resolve_type(Sema *s, Node *tr) {
    // ★ 関数型 fn(A, B) -> C
    if (tr->name && strcmp(tr->name, "fn") == 0 && !tr->mod_name) {
        Type *t = xmalloc(sizeof(Type));
        t->kind = TY_FN;
        int n = 0;
        for (Node *a = tr->body; a; a = a->next) n++;
        t->nparams = n;
        t->params = n ? xmalloc(sizeof(Type *) * (size_t)n) : NULL;
        int k = 0;
        for (Node *a = tr->body; a; a = a->next) t->params[k++] = resolve_type(s, a);
        t->elem = resolve_type(s, tr->rhs);
        // ★ 投げうるエラー（A-49）。関数定義の raises と同じ規則で解決します
        if (tr->raises) {
            FuncSig tmp = {0};
            resolve_raises(s, tr, &tmp);
            t->nraises = tmp.nraises;
            t->raises = tmp.raises;
        }
        return t;
    }

    Type *base = resolve_base_type(s, tr);
    if (!tr->nullable) return base;

    if (!type_can_be_opt(base)) {
        Diag d = {0};
        d.message = MSG1("sema.006", "'{0} | None' は書けません", type_name(base));
        d.primary.tok = tr->tok;
        d.primary.label = MSG0("sema.368", "この型は None になれません");
        d.hint = MSG0("sema.369", "None はヌルポインタとして表すので、int や bool には付けられません（nullable にできるのは str / list[T] / class です）");
        diag_fail(&d);
    }
    return type_opt(base);
}

// ── ジェネリクス（単相化） ─────────────────────────────
//
// ★ 方針は docs/ja/design/generics-and-interfaces.md §1 のとおり **単相化**です。
//   Dict[str, int] と Dict[str, Symbol] は、**別々のクラスを作ります**。
//   型消去（1 つの実体で済ませる）を採らないのは、int と str で値の大きさと
//   解放の要否が違い、箱詰めが要るためです（GC を持たない方針と噛み合わない）。

// 型引数の束縛を引く（K → str）
static Type *lookup_tbind(Sema *s, const char *name) {
    for (TBind *b = s->tbind; b; b = b->next)
        if (strcmp(b->name, name) == 0) return b->type;
    return NULL;
}

// 実体の名前を作る（Dict$str$int）。
//
// 注意: 型名をそのまま使うと '[' や ',' が混ざって IR の名前に使えません。
//   英数字と '.' 以外を '_' に潰します。
static char *mangle_inst(const char *base, Type **args, int n) {
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "%s", base);
    for (int i = 0; i < n; i++) {
        sb_printf(&sb, "$");
        for (const char *q = type_name(args[i]); *q; q++) {
            char ch = *q;
            bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || ch == '.' || ch == '_';
            sb_printf(&sb, "%c", ok ? ch : '_');
        }
    }
    return sb_str(&sb);
}

static void declare_class(Sema *s, Node *n);
static void declare_class_members(Sema *s, Node *n);

// テンプレートから実体を 1 つ作る（既にあれば作らない）
static Type *instantiate_class(Sema *s, Class *tmpl, Type **args, int nargs,
                               Token *at) {
    int want = 0;
    for (Node *tp = tmpl->node->targs; tp; tp = tp->next) want++;
    if (nargs != want) {
        Diag d = {0};
        d.message = MSG3("sema.007", "'{0}' は型引数を {1} 個取りますが、{2} 個書かれました", tmpl->name, diag_fmt("%d", want), diag_fmt("%d", nargs));
        d.primary.tok = at;
        d.primary.label = MSG0("sema.370", "型引数の個数が違います");
        d.related.tok = tmpl->tok;
        d.related.label = MSG0("sema.371", "このクラスの定義です");
        diag_fail(&d);
    }

    char *iname = mangle_inst(tmpl->name, args, nargs);

    // 既に作ってあれば、それを返す（同じ組み合わせは 1 個だけ）
    for (Class *c = tmpl->owner->classes; c; c = c->next)
        if (strcmp(c->name, iname) == 0) return c->type;

    // ★ テンプレートが定義されているモジュールに実体を作ります。
    //   メソッドの本体はそのモジュールの名前しか参照しないためです。
    //   注意: 型引数（Symbol など）は **Type として**渡すので、名前解決は要りません。
    //
    // 注意: モジュールの出入りは **enter_module に任せます**。
    //   自前で s->funcs / s->classes を退避すると、enter_module が行う
    //   「今のモジュールへの書き戻し」と二重になり、表が失われます
    //   （実際にそれで main が見つからなくなりました）。
    ModuleSyms *saved_mod = s->cur;
    Scope *saved_scope = s->scope;
    TBind *saved_tbind = s->tbind;
    FuncSig *saved_cur_func = s->cur_func;
    UsedName *saved_used = s->used;

    enter_module(s, tmpl->owner);

    // 型引数を束縛する
    TBind *binds = NULL;
    int i = 0;
    for (Node *tp = tmpl->node->targs; tp; tp = tp->next, i++) {
        TBind *b = xmalloc(sizeof(TBind));
        b->name = tp->name;
        b->type = args[i];
        b->next = binds;
        binds = b;
    }
    s->tbind = binds;

    // 木を複製して、名前を実体のものに差し替える
    Node *inst = ast_clone(tmpl->node);
    inst->name = iname;
    inst->targs = NULL;          // ★ 実体はもうテンプレートではありません
    inst->next = NULL;

    declare_class(s, inst);
    inst->cls->from_template = tmpl;      // ★ 生成のときに実体を選ぶ手がかり
    // ★ 渡された型引数を覚えます（unify_tparam が入れ子をたどるため）
    if (nargs > 0) {
        inst->cls->targs = xmalloc(sizeof(Type *) * (size_t)nargs);
        for (int t = 0; t < nargs; t++) inst->cls->targs[t] = args[t];
        inst->cls->ntargs = nargs;
    }
    declare_class_members(s, inst);
    Type *ty = inst->type;

    // ★ codegen が拾えるように、テンプレートのモジュールの AST に足します。
    Node *ast = tmpl->owner->mod->ast;
    Node *last = ast->body;
    while (last->next) last = last->next;
    last->next = inst;

    // 本体の検査は後回し（今は別のものを検査している最中かもしれない）
    Instance *q = xmalloc(sizeof(Instance));
    q->node = inst;
    q->owner = tmpl->owner;
    q->binds = binds;
    q->site = at;                 // ★ どこで要求されたか
    q->iname = iname;
    q->next = s->pending;
    s->pending = q;

    enter_module(s, saved_mod);
    s->scope = saved_scope;          // ★ 検査中の局所スコープに戻す
    s->cur_func = saved_cur_func;
    s->used = saved_used;
    s->tbind = saved_tbind;
    return ty;
}

// ── ジェネリック関数 ────────────────────────────────────
//
// ★ クラスと違い、**呼び出し側の実引数から型引数を決めます**。
//   左辺の型からは決められないためです（f(xs) の左辺は戻り値の型しかない）。
//
//   def first[T](xs: list[T]) -> T:
//       first([1, 2])        → T = int
//
// ★ 見るのは「T そのもの」「list[T] / rc[T] / ptr[T] の中身」
//   「fn(A) -> B の引数と戻り型」、そして **Box[T] のような
//   ジェネリッククラスの型引数**です。
//   決められなければ、その旨をエラーにします（黙って通しません）。
static bool unify_tparam(const char *name, Node *decl_tr, Type *actual,
                         Type **out) {
    if (!decl_tr) return false;

    // T そのもの
    if (decl_tr->name && !decl_tr->lhs && strcmp(decl_tr->name, name) == 0) {
        *out = actual;
        return true;
    }
    // ★ 関数型 fn(A) -> B。**引数と戻り型の両方**を見ます。
    //   これが無いと map_to[T, U](xs: list[T], f: fn(T) -> U) の U が
    //   決められません（U は f の戻り型にしか現れないため）。
    if (decl_tr->name && strcmp(decl_tr->name, "fn") == 0 &&
        actual->kind == TY_FN) {
        int i = 0;
        for (Node *a = decl_tr->body; a; a = a->next, i++) {
            if (i >= actual->nparams) break;
            if (unify_tparam(name, a, actual->params[i], out)) return true;
        }
        if (decl_tr->rhs && actual->elem)
            return unify_tparam(name, decl_tr->rhs, actual->elem, out);
        return false;
    }

    // ★ ジェネリックなクラス Box[T] → 実体に渡された型引数を見る。
    //   これが無いと、集合演算のような「自分で作った容器を受け取る関数」が
    //   1 つも書けません（def union[T](a: Set[T], b: Set[T])）。
    //
    // 注意: 突き合わせるのは**名前だけ**です（モジュールは見ません）。
    //   同じ名前のジェネリッククラスを 2 つのモジュールから 1 つの
    //   シグネチャに混ぜたときだけ取り違えますが、そのときは
    //   型検査が後で弾きます（決まった型で本体を検査するため）。
    if (decl_tr->targs && decl_tr->name && actual->kind == TY_CLASS &&
        actual->cls && actual->cls->from_template &&
        strcmp(actual->cls->from_template->name, decl_tr->name) == 0) {
        int i = 0;
        for (Node *a = decl_tr->targs; a; a = a->next, i++) {
            if (i >= actual->cls->ntargs) break;
            if (unify_tparam(name, a->lhs, actual->cls->targs[i], out)) return true;
        }
        return false;
    }

    // list[T] / rc[T] / ptr[T] → 中身を見る
    if (decl_tr->lhs && actual->elem)
        return unify_tparam(name, decl_tr->lhs, actual->elem, out);
    return false;
}

static FuncSig *instantiate_func(Sema *s, FuncSig *tmpl, Node *call);

// 型参照に書かれた型引数を並べる
static int collect_targs(Sema *s, Node *tr, Type **out, int max) {
    int n = 0;
    for (Node *a = tr->targs; a; a = a->next) {
        if (n >= max)
            error_at_m(tr->tok, MSG1("sema.008", "型引数が多すぎます（最大 {0} 個です）", diag_fmt("%d", max)));
        out[n++] = resolve_type(s, a->lhs);
    }
    return n;
}

#define MAX_TARGS 8

static Type *resolve_base_type(Sema *s, Node *tr) {
    // ★ lexer.Token のようにモジュール修飾された型注釈。
    //   引く表が「自分のモジュール」から「そのモジュール」に変わるだけです。
    if (tr->mod_name) {
        ModuleSyms *ms = lookup_import(s, tr->mod_name);
        if (!ms) {
            Diag d = {0};
            d.message = MSG1("sema.009", "モジュール '{0}' を import していません", tr->mod_name);
            d.primary.tok = tr->tok;
            d.primary.label = MSG0("sema.372", "この修飾を解決できません");
            d.hint = MSG1("sema.010", "ファイルの先頭に 'import {0}' を書いてください", tr->mod_name);
            diag_fail(&d);
        }
        // ★ 他のモジュールの範囲型（A-28）
        RangeTy *mr = lookup_range_in(ms, tr->name);
        if (mr) {
            if (tr->lhs)
                error_at_hint_m(tr->tok, MSG0("sema.012", "範囲型は要素型を取りません"), MSG2("sema.011", "型 '{0}.{1}' は要素型を取りません", tr->mod_name, tr->name));
            return mr->type;
        }

        // ★ 他のモジュールのインタフェース
        // ★ 他のモジュールの列挙（A-37）
        EnumDef *me = lookup_enum_in(ms, tr->name);
        if (me) {
            if (tr->lhs)
                error_at_hint_m(tr->tok, MSG0("sema.013", "列挙は要素型を取りません"), MSG2("sema.011", "型 '{0}.{1}' は要素型を取りません", tr->mod_name, tr->name));
            return me->type;
        }

        Iface *mi = lookup_iface_in(ms, tr->name);
        if (mi) return type_iface(mi->name, mi);

        Class *c = lookup_class_in(ms, tr->name);
        if (!c) {
            Diag d = {0};
            d.message = MSG2("sema.014", "モジュール '{0}' にクラス '{1}' はありません", tr->mod_name, tr->name);
            d.primary.tok = tr->tok;
            d.primary.label = MSG0("sema.373", "このクラスは定義されていません");
            d.hint = MSG0("sema.374", "他のモジュールから使えるのはクラスだけです（int や list はモジュール修飾なしで書きます）");
            diag_fail(&d);
        }
        // ★ 他のモジュールのジェネリッククラスも実体化できます
        if (c->node && c->node->targs) {
            Type *args[MAX_TARGS];
            int n = collect_targs(s, tr, args, MAX_TARGS);
            return instantiate_class(s, c, args, n, tr->tok);
        }
        if (tr->lhs)
            error_at_hint_m(tr->tok, MSG0("sema.015", "要素型を取るのは list / rc / mutex / Thread だけです"), MSG2("sema.011", "型 '{0}.{1}' は要素型を取りません", tr->mod_name, tr->name));
        return c->type;
    }

    if (strcmp(tr->name, "list") == 0) {
        if (!tr->lhs)
            error_at_hint_m(tr->tok, MSG0("sema.017", "要素型を書いてください（例: list[int]）"), MSG0("sema.016", "list には要素型が必要です"));
        Type *elem = resolve_type(s, tr->lhs);  // ★ 再帰
        if (elem->kind == TY_NONE)
            error_at_hint_m(tr->tok, MSG0("sema.019", "None 型の値は存在しないので要素にできません"), MSG0("sema.018", "list の要素型に None は使えません"));
        return type_list(elem);
    }

    // ── ptr[T]（生ポインタ）──
    //
    // ★ 中身に取れるのは int だけにしてあります。OS で触るのはメモリの番地で、
    //   そこに「本言語の型」は載っていないからです（仕様 §10.2）。
    if (strcmp(tr->name, "ptr") == 0) {
        if (!tr->lhs)
            error_at_hint_m(tr->tok, MSG0("sema.021", "中身の型を書いてください（例: ptr[int]）"), MSG0("sema.020", "ptr には中身の型が必要です"));
        Type *elem = resolve_type(s, tr->lhs);
        if (elem->kind != TY_INT)
            error_at_hint_m(tr->tok, MSG0("sema.023", "いま ptr に書けるのは int だけです（例: ptr[int]）"), MSG1("sema.022", "'{0}' は ptr に入れられません", type_name(elem)));
        return type_ptr(elem);
    }

    // ── rc[T]（共有所有）──
    //
    // ★ 中身に取れるのはクラスだけにしてあります。
    //   「所有者を 1 つに決められない」のは木やグラフの節点で、
    //   それはクラスとして書かれるからです（仕様 §7.1）。
    if (strcmp(tr->name, "rc") == 0) {
        if (!tr->lhs)
            error_at_hint_m(tr->tok, MSG0("sema.025", "中身の型を書いてください（例: rc[Node]）"), MSG0("sema.024", "rc には中身の型が必要です"));
        Type *elem = resolve_type(s, tr->lhs);
        if (elem->kind != TY_CLASS)
            error_at_hint_m(tr->tok, MSG0("sema.027", "rc に入れられるのはクラスだけです（例: rc[Node]）"), MSG1("sema.026", "'{0}' は rc に入れられません", type_name(elem)));
        return type_rc(elem);
    }

    // ── weak[T]（弱参照。A-50）── rc[T] と同じく中身はクラスだけです
    if (strcmp(tr->name, "weak") == 0) {
        if (!tr->lhs)
            error_at_hint_m(tr->tok, MSG0("sema.627", "中身の型を書いてください（例: weak[Node]）"), MSG0("sema.626", "weak には中身の型が必要です"));
        Type *elem = resolve_type(s, tr->lhs);
        if (elem->kind != TY_CLASS)
            error_at_hint_m(tr->tok, MSG0("sema.629", "weak に入れられるのはクラスだけです（例: weak[Node]）"), MSG1("sema.628", "'{0}' は weak にできません", type_name(elem)));
        return type_weak(elem);
    }

    // ── Thread[R] / mutex[T]（A-18）──
    //
    // ★ Thread[R] の R は**戻り型**です。spawn した関数が返すものを、
    //   join が返します。
    if (strcmp(tr->name, "Thread") == 0) {
        if (!tr->lhs)
            error_at_hint_m(tr->tok, MSG0("sema.029", "戻り型を書いてください（例: Thread[int]）"), MSG0("sema.028", "Thread には戻り型が必要です"));
        return type_thread(resolve_type(s, tr->lhs));
    }

    // ★ mutex に入れられるのは「共有したい実体」なので、rc[T] と同じく
    //   クラスとリストに限ります。int を包んでも取り出せないためです
    //   （lock に渡した関数が受け取るのは中身で、書き換えるには参照が要る）。
    if (strcmp(tr->name, "mutex") == 0) {
        if (!tr->lhs)
            error_at_hint_m(tr->tok, MSG0("sema.031", "中身の型を書いてください（例: mutex[Counter]）"), MSG0("sema.030", "mutex には中身の型が必要です"));
        Type *elem = resolve_type(s, tr->lhs);
        if (elem->kind != TY_CLASS && elem->kind != TY_LIST)
            error_at_hint_m(tr->tok, MSG0("sema.033", "mutex に入れられるのはクラスかリストだけです（例: mutex[Counter] / mutex[list[float]]）"), MSG1("sema.032", "'{0}' は mutex に入れられません", type_name(elem)));
        return type_mutex(elem);
    }

    // ★ 型引数の名前（class Dict[K, V] の K）。
    //   注意: **クラス名より先に**引きます。テンプレートを読む間だけ有効です。
    Type *tv = lookup_tbind(s, tr->name);
    if (tv) {
        if (tr->lhs)
            error_at_hint_m(tr->tok, MSG0("sema.035", "型引数そのものは型引数を取りません"), MSG1("sema.034", "型 '{0}' は型引数を取りません", tr->name));
        return tv;
    }

    Type *t = type_from_name(tr->name);
    if (t) {
        if (tr->lhs)
            error_at_hint_m(tr->tok, MSG0("sema.037", "要素型を取るのは list と rc だけです"), MSG1("sema.036", "型 '{0}' は要素型を取りません", tr->name));
        return t;
    }

    // ★ 範囲型（部分型。A-28）。クラス名より先に引きます。
    RangeTy *rt = lookup_range(s, tr->name);
    if (rt) {
        if (tr->lhs)
            error_at_hint_m(tr->tok, MSG0("sema.012", "範囲型は要素型を取りません"), MSG1("sema.036", "型 '{0}' は要素型を取りません", tr->name));
        return rt->type;
    }

    // ★ 列挙（A-37）。クラス名より先に引きます。
    EnumDef *et0 = lookup_enum(s, tr->name);
    if (et0) {
        if (tr->lhs)
            error_at_hint_m(tr->tok, MSG0("sema.013", "列挙は要素型を取りません"), MSG1("sema.036", "型 '{0}' は要素型を取りません", tr->name));
        return et0->type;
    }

    // ★ タプル型 (A, B)
    if (strcmp(tr->name, "(tuple)") == 0) {
        Type *t = xmalloc(sizeof(Type));
        t->kind = TY_TUPLE;
        int n = 0;
        for (Node *a = tr->targs; a; a = a->next) n++;
        t->nparams = n;
        t->params = xmalloc(sizeof(Type *) * (size_t)n);
        int k = 0;
        for (Node *a = tr->targs; a; a = a->next) {
            Type *et = resolve_type(s, a->lhs);
            if (et->kind == TY_NONE)
                error_at_hint_m(a->tok, MSG0("sema.019", "None 型の値は存在しないので要素にできません"), MSG0("sema.038", "タプルの要素に None は使えません"));
            t->params[k++] = et;
        }
        return t;
    }

    // ★ インタフェース名も型として書けます（list[Show] など）
    Iface *ifc0 = lookup_iface(s, tr->name);
    if (ifc0) {
        if (tr->lhs)
            error_at_hint_m(tr->tok, MSG0("sema.039", "インタフェースは型引数を取りません"), MSG1("sema.034", "型 '{0}' は型引数を取りません", tr->name));
        return type_iface(ifc0->name, ifc0);
    }

    // ★ 組み込みの型名で無ければ、クラス名として引きます。
    //   「型の一覧がソースコードによって増える」のは、この章が初めてです。
    Class *c = lookup_class(s, tr->name);
    if (c) {
        // ★ ジェネリッククラスなら、ここで実体を作ります。
        if (c->node && c->node->targs) {
            Type *args[MAX_TARGS];
            int n = collect_targs(s, tr, args, MAX_TARGS);
            return instantiate_class(s, c, args, n, tr->tok);
        }
        if (tr->lhs)
            error_at_hint_m(tr->tok, MSG0("sema.037", "要素型を取るのは list と rc だけです"), MSG1("sema.036", "型 '{0}' は要素型を取りません", tr->name));
        return c->type;
    }

    Diag d = {0};
    d.message = MSG1("sema.040", "未知の型名 '{0}' です", tr->name);
    d.primary.tok = tr->tok;
    d.primary.label = MSG0("sema.375", "この型は存在しません");
    d.hint = MSG1("sema.041", "現在使える型: {0}、および定義したクラス名", type_name_list());
    diag_fail(&d);
}

// ★ 同名の別クラスだったときは、モジュール修飾つきで説明する。
//
//   x: a.Box = b.Box()     →  「型 'Box' の式」だけでは何が起きたか分からない
//
// 型が同じかどうかは名前ではなく定義で決まります。
// その判断の結果を、利用者に読める言葉で見せるための一言です。
static const char *no_implicit_hint(Type *got, Type *want) {
    if (got->kind == TY_CLASS && want->kind == TY_CLASS &&
        strcmp(got->cls->name, want->cls->name) == 0)
        return MSG2("sema.042", "'{0}' と '{1}' は名前が同じだけの別のクラスです（同じ型かどうかは名前ではなく定義で決まります）", got->cls->ir_name, want->cls->ir_name);
    // ★ 関数型どうしなら、**どこが違うのか**を言います。
    //   「暗黙の型変換がありません」では、何を直せばよいか分かりません。
    if (got->kind == TY_FN && want->kind == TY_FN) {
        if (got->nparams != want->nparams)
            return MSG2("sema.043", "引数の数が違います（{0} 個と {1} 個）", diag_fmt("%d", got->nparams), diag_fmt("%d", want->nparams));
        for (int i = 0; i < got->nparams; i++)
            if (!type_equal(got->params[i], want->params[i]))
                return MSG3("sema.044", "{0} 番目の引数の型が違います（'{1}' と '{2}'）", diag_fmt("%d", i + 1), type_name(got->params[i]), type_name(want->params[i]));
        // ★ 投げうるエラーが違う（A-49）。集合が一致しなければ別の型です
        if (type_equal(got->elem, want->elem))
            return want->nraises == 0
                ? MSG1("sema.624", "この関数は失敗しえます（'{0}'）。失敗しない関数の型には入りません", type_name(got))
                : MSG1("sema.625", "投げうるエラーが違います。ここには '{0}' が必要です", type_name(want));
        return MSG2("sema.045", "戻り型が違います（'{0}' と '{1}'）", type_name(got->elem), type_name(want->elem));
    }

    // ★ クラス → インタフェースなら、実装宣言の書き忘れを疑います。
    if (want->kind == TY_IFACE && got->kind == TY_CLASS)
        return MSG4("sema.046", "'{0}' が '{1}' を実装すると宣言していません（'class {2}({3}):' と書きます）", got->cls->name, want->iface->name, got->cls->name, want->iface->name);

    return MSG0("sema.376", "本言語には暗黙の型変換がありません（言語仕様 3.5）");
}

// ── None リテラルと is / is not ──────────────────────

// x is None / x is not None の検査。
//
// ★ 右辺は None リテラルだけを許します。一般の同一性比較にしないのは、
//   クラスの == が既に参照比較だからです（区別を説明できない記号は増やさない）。
static Type *check_is(Sema *s, Node *n) {
    Type *l = check_expr(s, n->lhs);

    if (n->rhs->kind != ND_NONE) {
        Type *r = check_expr(s, n->rhs);
        Diag d = {0};
        d.message = MSG1("sema.047", "{0} は None との比較にだけ使えます", op_symbol(n->op));
        d.primary.tok = n->rhs->tok;
        d.primary.label = MSG1("sema.048", "ここには None を書いてください（型 '{0}' の式です）", type_name(r));
        d.hint = MSG0("sema.377", "値が等しいかを調べるには == を使ってください");
        diag_fail(&d);
    }
    n->rhs->type = ty_null;

    if (l->kind != TY_OPT) {
        Diag d = {0};
        d.message = MSG1("sema.049", "型 '{0}' の値が None になることはありません", type_name(l));
        d.primary.tok = n->lhs->tok;
        d.primary.label = MSG0("sema.378", "この式は必ず値を持ちます");
        d.hint = type_can_be_opt(l)
                     ? MSG1("sema.050", "None を入れたいなら、型注釈を '{0} | None' にしてください", type_name(l))
                     : MSG0("sema.379", "None になれるのは str / list[T] / class だけです");
        diag_fail(&d);
    }
    return ty_bool;
}

// 二項演算子が、その型に適用できるか
static bool op_supports(OpKind op, Type *t) {
    // ★ クラスと list は「参照」なので、比べられるのは
    //   同一性（== / !=）だけです。大小関係には意味がありません
    //   （言語仕様 4.3 / docs/ja/spec/type-system.md 5.6）。
    if (t->kind == TY_CLASS)
        return op == OP_EQ || op == OP_NE;

    // ★ list は連結（+）と繰り返し（*）ができます。
    //   注意: どちらも **新しい list を作ります**（元は変わりません）。
    if (t->kind == TY_LIST)
        return op == OP_EQ || op == OP_NE || op == OP_ADD || op == OP_MUL;

    // ★ T | None には何も適用できません。
    //   == で比べたいなら、先に絞り込んでもらいます（None かどうかは is で調べる）。
    if (t->kind == TY_OPT || t->kind == TY_NULL) return false;

    // 比較は int どうし・bool どうしのどちらでも使える。
    // （両辺の型が等しいことは呼び出し側で検査済み）
    // 言語仕様 4.3 / docs/ja/spec/type-system.md 5.5
    if (is_compare(op)) return true;

    if (t->kind == TY_INT) {
        // 言語仕様 4.2：int に '/' は使えない（'//' を使う）
        return op != OP_TRUEDIV;
    }
    // ★ float は int のちょうど裏返しです。
    //   '/' が使えて、'//' '%' とビット演算が使えません。
    //   注意: 切り捨てが要るなら int に変換してから（暗黙変換はしない）。
    if (t->kind == TY_FLOAT) {
        switch (op) {
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_TRUEDIV:
            // ★ '**' を float でも使えるようにしました。
            //   ランタイムに pl_fpow を自前で置いています（libc の pow は
            //   ベアメタルで使えないため）。
            case OP_POW:
                return true;

            default:
                return false;
        }
    }
    // ★ str は連結（+）と繰り返し（*）。
    //   v1 では * を採用していませんでしたが、実用上よく使うので入れました。
    if (t->kind == TY_STR) return op == OP_ADD || op == OP_MUL;
    return false;  // ★ bool に算術・ビット演算は使えない
}

// 「ここには bool が必要」というエラー。
// and の左辺・and の右辺・not の 3 か所で同じ形になるので関数にまとめます
// （span_token や advance_newline と同じ「3 回目でまとめる」判断）。
// ★ のちに一般化：if / while の条件からも呼ばれるようになったので、
//   「どこで bool が必要なのか」を文字列で受け取る形に変えました。
//   ND_IF には op が無いため、op_symbol() を使う形のままでは書けません。
//   最初から汎用に作らず、2 つ目の利用者が現れてから一般化する。
static Type *bool_required(const char *message, const char *where_label,
                           Token *where_tok, Node *operand, Type *actual) {
    Diag d = {0};
    d.message = message;
    d.primary.tok = operand->tok;
    d.primary.label = MSG1("sema.051", "これは '{0}' 型です", type_name(actual));
    d.related.tok = where_tok;
    d.related.label = where_label;
    d.hint = MSG0("sema.380", "本言語は int を真偽値として扱いません（言語仕様 4.4）。比較を書いてください（例: x != 0）");
    diag_fail(&d);
}

static Type *check_tuple(Sema *s, Node *n);     // タプル
static Type *check_slice(Sema *s, Node *n);     // スライス
static Type *check_in(Sema *s, Node *n);        // in / not in

// ── 演算子の多重定義 ──────────────────────────────────
//
// ★ `a + b` の左辺がクラスで、そのクラスに `__add__` があれば
//   **`a.__add__(b)` に読み替えます。** 読み替えるのは意味解析だけで、
//   codegen と IR は 1 行も変えていません（ふつうのメソッド呼び出しに
//   なるためです）。
//
// なぜ Python と同じ `__add__` という名前か
//   ふつうのメソッドと**必ず区別できる**名前が要ります。`add` にすると
//   `linalg.Matrix.add` のような既存のメソッドが、書いた覚えのないところで
//   演算子として呼ばれてしまいます。`__…__` は Python の利用者にそのまま
//   通じる、いちばん驚きの少ない選び方です。
//
// 注意: **反転（`__radd__`）はありません。** `2.0 * m` は書けません
//   （`m * 2.0` と書いてください）。左辺の型だけで決まるほうが、
//   どのメソッドが呼ばれるか読んで分かります。
static const char *op_method_name(OpKind op) {
    switch (op) {
        case OP_ADD: return "__add__";
        case OP_SUB: return "__sub__";
        case OP_MUL: return "__mul__";
        case OP_TRUEDIV: return "__truediv__";
        case OP_FLOORDIV: return "__floordiv__";
        case OP_MOD: return "__mod__";
        case OP_POW: return "__pow__";
        case OP_EQ: return "__eq__";
        case OP_NE: return "__ne__";
        case OP_LT: return "__lt__";
        case OP_LE: return "__le__";
        case OP_GT: return "__gt__";
        case OP_GE: return "__ge__";
        default: return NULL;
    }
}

// そのクラスにそのメソッドがあるか
static bool class_has_method(Class *c, const char *name) {
    return c && lookup_func_in(c->owner, mangle(c->name, name)) != NULL;
}

// n（ND_BINOP）を n->lhs.name(n->rhs) の呼び出しに作り替える
static Type *rewrite_to_method(Sema *s, Node *n, Class *c, const char *name) {
    n->kind = ND_METHOD;
    n->name = (char *)name;
    n->args = n->rhs;
    n->rhs = NULL;
    n->args->next = NULL;
    return check_class_method(s, n, c);
}

static Type *check_binop(Sema *s, Node *n) {
    // ★ in / not in は「両辺が同じ型」ではないので先に分岐します
    if (n->op == OP_IN || n->op == OP_NOTIN) return check_in(s, n);

    // ★ is / is not は型の合わせ方がまったく違うので、先に分岐します
    if (n->op == OP_IS || n->op == OP_ISNOT) return check_is(s, n);

    Type *l = check_expr(s, n->lhs);

    // ★ 左辺がクラスなら、演算子は多重定義を探します。
    //   注意: 右辺はここでは検査しません。**メソッドの引数として**
    //     check_class_method が検査します（2 回検査しないため）。
    if (l->kind == TY_CLASS && l->cls) {
        const char *mn = op_method_name(n->op);
        if (mn && class_has_method(l->cls, mn))
            return rewrite_to_method(s, n, l->cls, mn);

        // ★ `!=` は `__ne__` が無ければ `not (a == b)` に読み替えます
        //   （Python と同じ。`__eq__` だけ書けば両方使えます）。
        if (n->op == OP_NE && class_has_method(l->cls, "__eq__")) {
            Node *call = new_node(ND_METHOD, n->tok);
            call->lhs = n->lhs;
            call->name = "__eq__";
            call->args = n->rhs;
            call->args->next = NULL;
            Type *rt = check_class_method(s, call, l->cls);
            if (rt->kind != TY_BOOL)
                error_at_m(n->tok, MSG1("sema.052", "'!=' に使うには __eq__ が bool を返す必要があります（いまは '{0}' を返しています）", type_name(rt)));
            call->type = rt;
            n->kind = ND_UNARY;
            n->op = OP_NOT;
            n->lhs = call;
            n->rhs = NULL;
            return ty_bool;
        }
    }

    Type *r = check_expr(s, n->rhs);

    // ★ 繰り返し（"ab" * 3 / [0] * 3）だけは **両辺の型が違います**。
    //   左が str か list、右が int という組み合わせだけを認めます。
    //   注意: 3 * "ab"（左右が逆）は認めません。「何を何回」の順を固定して、
    //     読むときに迷わないようにします。
    if (n->op == OP_MUL && (l->kind == TY_STR || l->kind == TY_LIST) &&
        r->kind == TY_INT)
        return l;

    // ★ 検査は 2 段構え（docs/ja/spec/type-system.md 5.3）
    //   ① 両辺の型が等しいか
    //   ② その型がその演算子を支持するか
    //   この順にするとコードが短くなり、エラーメッセージも的確になります。
    if (!type_equal(l, r)) {
        Diag d = {0};
        d.message = MSG3("sema.053", "型 '{0}' と '{1}' に演算子 '{2}' は適用できません", type_name(l), type_name(r), op_symbol(n->op));
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.381", "この演算子の両辺の型が違います");
        d.hint = no_implicit_hint(l, r);
        diag_fail(&d);
    }

    if (!op_supports(n->op, l)) {
        if (n->op == OP_TRUEDIV) {
            // 当初は codegen で弾いていた検査を、本来の担当である
            // 意味解析パスに移しました。
            error_at_hint_m(n->tok, MSG0("sema.055", "切り捨て除算の '//' を使ってください（本言語には暗黙の型変換がないため、'/' は float 専用です）"), MSG0("sema.054", "整数の除算に '/' は使えません"));
        }
        if (l->kind == TY_CLASS && op_method_name(n->op))
            error_at_hint_m(n->tok, MSG2("sema.057", "クラス '{0}' に '{1}(self, other) -> …' を定義すると、この演算子が使えます", l->cls->name, op_method_name(n->op)), MSG2("sema.056", "型 '{0}' に演算子 '{1}' は適用できません", type_name(l), op_symbol(n->op)));
        error_at_m(n->tok, MSG2("sema.056", "型 '{0}' に演算子 '{1}' は適用できません", type_name(l), op_symbol(n->op)));
    }

    // 0 除算のうち、右辺がリテラル 0 の場合はここで弾く。
    // 右辺が式の場合は実行時 SIGFPE。実行時チェックを入れます
    if ((n->op == OP_FLOORDIV || n->op == OP_MOD) && n->rhs->kind == ND_INT &&
        n->rhs->ival == 0) {
        Diag d = {0};
        d.message = MSG0("sema.382", "0 で除算しています");
        d.primary.tok = n->rhs->tok;
        d.primary.label = MSG0("sema.383", "この 0 で割ろうとしています");
        d.related.tok = n->tok;
        d.related.label = MSG1("sema.058", "演算子 '{0}' はここです", op_symbol(n->op));
        diag_fail(&d);
    }

    // ★ 比較は bool を返す。算術は両辺と同じ型を返す。
    return is_compare(n->op) ? ty_bool : l;
}

// ── 絞り込みの道具（実体は 15.5 節のところ）─────────────────
//
// ★ 短絡評価する条件式の「途中」でも絞り込みを効かせたいので、
//    型と関数だけ先に見えるようにしておきます。
#define NARROW_MAX 16

typedef struct {
    VarEntry *vars[NARROW_MAX];
    Type *saved[NARROW_MAX];
    int n;
} NarrowSet;

static void narrow_apply(Sema *s, Node *cond, bool positive, NarrowSet *ns);
static void narrow_restore(NarrowSet *ns);

// and / or は両辺が bool のみ（言語仕様 4.4）。
// Python と違い int を真偽値として扱いません（truthiness を採用しない）。
//
// なぜ「最後に評価した値」を返さないのか
//   1 and "hello" のような式の型が一意に決まらなくなるからです。
//   bool に固定すれば and / or の型は常に bool です。
static Type *check_logical(Sema *s, Node *n) {
    Type *l = check_expr(s, n->lhs);

    // ★ 短絡評価するので、rhs は「lhs がある側に転んだとき」しか
    //   評価されません。その側の絞り込みを rhs の検査中だけ効かせます。
    //
    //     a is not None and a.v == 0   ← and の rhs は lhs が真のときだけ見る
    //     a is None     or  a.v == 0   ← or  の rhs は lhs が偽のときだけ見る
    //
    // 注意: 抜けたら必ず戻します（15.5 節と同じ「入る前に変えて、抜けたら戻す」）。
    NarrowSet sc = {0};
    narrow_apply(s, n->lhs, n->op == OP_AND, &sc);
    Type *r = check_expr(s, n->rhs);
    narrow_restore(&sc);

    char *msg = MSG1("sema.059", "演算子 '{0}' には bool が必要です", op_symbol(n->op));
    char *lbl = MSG1("sema.058", "演算子 '{0}' はここです", op_symbol(n->op));
    if (l->kind != TY_BOOL) return bool_required(msg, lbl, n->tok, n->lhs, l);
    if (r->kind != TY_BOOL) return bool_required(msg, lbl, n->tok, n->rhs, r);
    return ty_bool;
}

// タプル式 a, b
//
// ★ 期待型（左辺や戻り型）が分かっていれば、それに合わせて要素を検査します。
//   分からなければ、そのまま要素の型を並べたタプルになります。
static Type *check_tuple(Sema *s, Node *n) {
    Type *want = s->expected;
    int n_elems = 0;
    for (Node *x = n->body; x; x = x->next) n_elems++;

    if (want && want->kind == TY_TUPLE && want->nparams != n_elems) {
        Diag d = {0};
        d.message = MSG2("sema.060", "タプルの要素の数が違います（{0} 個と {1} 個）", diag_fmt("%d", n_elems), diag_fmt("%d", want->nparams));
        d.primary.tok = n->tok;
        d.primary.label = MSG1("sema.061", "ここは {0} 個です", diag_fmt("%d", n_elems));
        d.hint = MSG1("sema.062", "'{0}' が必要です", type_name(want));
        diag_fail(&d);
    }

    Type *t = xmalloc(sizeof(Type));
    t->kind = TY_TUPLE;
    t->nparams = n_elems;
    t->params = xmalloc(sizeof(Type *) * (size_t)n_elems);
    int k = 0;
    for (Node *x = n->body; x; x = x->next, k++) {
        s->expected = (want && want->kind == TY_TUPLE) ? want->params[k] : NULL;
        Type *et = check_expr(s, x);
        s->expected = NULL;
        if (want && want->kind == TY_TUPLE &&
            !type_assignable(et, want->params[k])) {
            Diag d = {0};
            d.message = MSG1("sema.063", "タプルの {0} 番目の型が合いません", diag_fmt("%d", k + 1));
            d.primary.tok = x->tok;
            d.primary.label = MSG1("sema.051", "これは '{0}' 型です", type_name(et));
            d.hint = MSG1("sema.064", "ここには '{0}' が必要です", type_name(want->params[k]));
            diag_fail(&d);
        }
        t->params[k] = (want && want->kind == TY_TUPLE) ? want->params[k] : et;
    }
    return t;
}

// xs[a:b] / s[a:b]
//
// 注意: **新しい値を作ります**（借用ではありません）。借用のスライスは
//   「元より長生きしないこと」の検査が要るためで、仕様 §6 の方針に従い
//   まず複製する形だけを入れました。
static Type *check_slice(Sema *s, Node *n) {
    Type *t = check_expr(s, n->lhs);
    if (t->kind != TY_STR && t->kind != TY_LIST) {
        Diag d = {0};
        d.message = MSG1("sema.065", "'{0}' 型はスライスできません", type_name(t));
        d.primary.tok = n->lhs->tok;
        d.primary.label = MSG0("sema.384", "ここには str か list[T] が必要です");
        diag_fail(&d);
    }
    if (n->rhs) {
        Type *a = check_expr(s, n->rhs);
        if (a->kind != TY_INT)
            error_at_m(n->rhs->tok, MSG1("sema.066", "スライスの開始は int です（'{0}' 型でした）", type_name(a)));
    }
    if (n->els) {
        Type *b = check_expr(s, n->els);
        if (b->kind != TY_INT)
            error_at_m(n->els->tok, MSG1("sema.067", "スライスの終端は int です（'{0}' 型でした）", type_name(b)));
    }
    return t;
}

// x in xs / sub in s
//
// ★ 右辺の型で意味が変わります。
//     list[T] … 要素に等しいものがあるか（== と同じ比べ方）
//     str     … 部分文字列として含まれるか
//   注意: dict には使えません（d.has(k) を使ってください）。鍵と値のどちらを
//     見るのかが記号から読み取れないためです。
static Type *check_in(Sema *s, Node *n) {
    Type *l = check_expr(s, n->lhs);
    Type *r = check_expr(s, n->rhs);

    // ★ `k in d` は `d.__contains__(k)` に読み替えます。
    //   注意: ここだけは **右辺の型**で決まります（入れ物のほうが決めます）。
    //     Python の `__contains__` と同じで、`in` は「入れ物に聞く」演算子です。
    //   注意: `not in` は結果を反転させます（ND_UNARY の not で包みます）。
    if (r->kind == TY_CLASS && r->cls && class_has_method(r->cls, "__contains__")) {
        Node *obj = n->rhs;   // 入れ物
        Node *arg = n->lhs;   // 探すもの
        arg->next = NULL;

        // 注意: ノードの中身を丸ごと写すと、式の並び（実引数の next）が
        //   壊れます。**その場で作り替えます。**
        if (n->op == OP_IN) {
            n->kind = ND_METHOD;
            n->name = "__contains__";
            n->lhs = obj;
            n->args = arg;
            n->rhs = NULL;
            Type *rt = check_class_method(s, n, r->cls);
            if (rt->kind != TY_BOOL)
                error_at_m(n->tok, MSG1("sema.068", "'in' に使うには __contains__ が bool を返す必要があります（いまは '{0}' を返しています）", type_name(rt)));
            return ty_bool;
        }

        // not in は結果を反転させます
        Node *call = new_node(ND_METHOD, n->tok);
        call->lhs = obj;
        call->name = "__contains__";
        call->args = arg;
        Type *rt = check_class_method(s, call, r->cls);
        if (rt->kind != TY_BOOL)
            error_at_m(n->tok, MSG1("sema.069", "'not in' に使うには __contains__ が bool を返す必要があります（いまは '{0}' を返しています）", type_name(rt)));
        call->type = rt;
        n->kind = ND_UNARY;
        n->op = OP_NOT;
        n->lhs = call;
        n->rhs = NULL;
        return ty_bool;
    }

    if (r->kind == TY_STR) {
        if (l->kind != TY_STR)
            error_at_m(n->tok, MSG1("sema.070", "str の 'in' には str が必要です（左辺は '{0}' 型です）", type_name(l)));
        return ty_bool;
    }
    if (r->kind == TY_LIST) {
        if (!type_assignable(l, r->elem)) {
            Diag d = {0};
            d.message = MSG0("sema.385", "'in' の左辺が要素の型と合いません");
            d.primary.tok = n->lhs->tok;
            d.primary.label = MSG1("sema.051", "これは '{0}' 型です", type_name(l));
            d.related.tok = n->rhs->tok;
            d.related.label = MSG1("sema.071", "こちらの要素は '{0}' 型です", type_name(r->elem));
            diag_fail(&d);
        }
        return ty_bool;
    }

    Diag d = {0};
    d.message = MSG1("sema.072", "'{0}' 型に 'in' は使えません", type_name(r));
    d.primary.tok = n->rhs->tok;
    d.primary.label = MSG0("sema.386", "ここには list[T] か str が必要です");
    d.hint = r->kind == TY_CLASS
                 ? MSG1("sema.073", "クラス '{0}' に '__contains__(self, x) -> bool' を定義すると、'in' が使えます", r->cls->name)
                 : MSG0("sema.387", "dict の鍵を調べるには d.has(k) を使ってください");
    diag_fail(&d);
    return ty_bool;
}

// 三項演算子 a if c else b
// 注意: 名前は check_ternary。check_cond は「文の条件式」用に既にあります。
static Type *check_ternary(Sema *s, Node *n) {
    Type *c = check_expr(s, n->lhs);
    if (c->kind != TY_BOOL)
        bool_required(MSG0("sema.388", "三項演算子の条件には bool が必要です"),
                      MSG0("sema.389", "この 'if' の条件です"), n->tok, n->lhs, c);

    Type *a = check_expr(s, n->rhs);
    Type *b = check_expr(s, n->els);

    // ★ どちらかが None リテラルなら、もう一方に合わせます
    //   x if c else None が書けるように。代入互換と同じ扱い
    if (type_assignable(b, a)) return a;
    if (type_assignable(a, b)) return b;

    Diag d = {0};
    d.message = MSG0("sema.390", "三項演算子の両側で型が違います");
    d.primary.tok = n->els->tok;
    d.primary.label = MSG1("sema.074", "こちらは '{0}' 型です", type_name(b));
    d.related.tok = n->rhs->tok;
    d.related.label = MSG1("sema.074", "こちらは '{0}' 型です", type_name(a));
    d.hint = MSG0("sema.391", "式の型は 1 つに決まらなければなりません（どちらかを合わせてください）");
    diag_fail(&d);
    return a;
}

static Type *check_unary(Sema *s, Node *n) {
    Type *t = check_expr(s, n->lhs);

    // not は bool を取り bool を返す（言語仕様 4.4）
    if (n->op == OP_NOT) {
        if (t->kind != TY_BOOL)
            return bool_required(
                MSG1("sema.059", "演算子 '{0}' には bool が必要です", op_symbol(n->op)),
                MSG1("sema.058", "演算子 '{0}' はここです", op_symbol(n->op)), n->tok, n->lhs,
                t);
        return ty_bool;
    }

    // ★ クラスの単項マイナスは __neg__ に読み替えます。
    if (t->kind == TY_CLASS && n->op == OP_NEG && class_has_method(t->cls, "__neg__")) {
        n->kind = ND_METHOD;
        n->name = "__neg__";
        n->args = NULL;
        return check_class_method(s, n, t->cls);
    }

    // ★ float には - と + が使えます（~ はビット演算なので int だけ）。
    if (t->kind == TY_FLOAT) {
        if (n->op == OP_NEG || n->op == OP_POS) return t;
        error_at_m(n->tok, MSG2("sema.075", "型 '{0}' に単項演算子 '{1}' は適用できません", type_name(t), op_symbol(n->op)));
    }

    // - + ~ は int のみ
    if (t->kind != TY_INT)
        error_at_m(n->tok, MSG2("sema.075", "型 '{0}' に単項演算子 '{1}' は適用できません", type_name(t), op_symbol(n->op)));
    return t;
}

// FuncSig から関数型を作る
static Type *fn_type_of(FuncSig *f) {
    Type *t = xmalloc(sizeof(Type));
    t->kind = TY_FN;
    t->nparams = f->nparams;
    t->params = f->params;
    t->elem = f->ret;
    // ★ raises する関数も値にできます（A-49）。投げうるエラーは型に載ります
    t->nraises = f->nraises;
    t->raises = f->raises;
    return t;
}

static Type *check_var(Sema *s, Node *n) {
    VarEntry *v = lookup(s, n->name);

    // ★ 契約（A-29）：ensures の式の中の 'result' は**戻り値そのもの**です。
    //
    // 注意: 局所変数のほうが先です。`result` という名前の変数を持っている
    //   コードを壊さないためです（この処理系自身がそうしています）。
    if (!v && s->ensures_depth > 0 && strcmp(n->name, "result") == 0) {
        Type *ret = s->cur_func ? s->cur_func->ret : NULL;
        if (!ret || ret->kind == TY_NONE) {
            Diag d = {0};
            d.message = MSG0("sema.392", "戻り値の無い関数では 'result' を書けません");
            d.primary.tok = n->tok;
            d.primary.label = MSG0("sema.393", "この関数は値を返しません");
            d.hint = MSG0("sema.394", "戻り値を見ない ensures（グローバルの条件など）にするか、戻り型を付けてください");
            diag_fail(&d);
        }
        n->kind = ND_RESULT;   // ★ 箱は作りません（codegen が返す値をそのまま使う）
        n->type = ret;
        return ret;
    }

    // ★ 変数に無ければ **関数を探します**。関数の名前を
    //   そのまま値として書けるようにするためです（f を渡す）。
    //   注意: 変数が先です。同名の局所変数があればそちらが勝ちます。
    if (!v) {
        FuncSig *f = lookup_func(s, n->name);
        // ★ lambda は**使う側の型**で実体になります（A-42）。
        if (f && f->tmpl && f->tmpl->is_lambda) {
            Type *shown = s->expected;   // 捕獲は隠すので、外から見える型はこれ
            f = instantiate_lambda(s, f, n);
            if (n->caps) {
                // ★ 捕まえた値は**末尾の引数**として渡しますが、
                //   **外から見える型には出しません**（A-43）。
                //   呼ぶ側は `fn(int) -> bool` としてしか触りません。
                n->ir_name = f->ir_name;
                n->is_func_ref = true;
                n->type = shown;
                return shown;
            }
        }
        if (f && f->tmpl) {
            // ジェネリック関数そのものは値にできません（型引数が決まらない）。
            Diag d = {0};
            d.message = MSG1("sema.076", "'{0}' は型引数を取る関数なので、値にできません", n->name);
            d.primary.tok = n->tok;
            d.primary.label = MSG0("sema.395", "ここでは値として使えません");
            d.hint = MSG0("sema.396", "値にするには型が 1 つに決まっている必要があります（呼び出しなら実引数から決まります）");
            diag_fail(&d);
        }
        if (f) {
            n->ir_name = f->ir_name;
            n->is_func_ref = true;
            return fn_type_of(f);
        }
    }

    if (!v) {
        // ★ モジュール名そのものは値ではありません。
        if (lookup_import(s, n->name)) {
            Diag d = {0};
            d.message = MSG1("sema.078", "モジュール '{0}' は値として使えません", n->name);
            d.primary.tok = n->tok;
            d.primary.label = MSG0("sema.398", "ここにはモジュール名を書けません");
            d.hint = MSG1("sema.079", "モジュールの中身は '{0}.名前' の形で使います", n->name);
            diag_fail(&d);
        }

        // 同名のモジュールが存在するのに import していない場合。
        // ★ 「未定義の名前です」で突き放さず、書き忘れを指摘します。
        if (module_file_exists(s->cur->mod->dir, n->name)) {
            Diag d = {0};
            d.message = MSG1("sema.009", "モジュール '{0}' を import していません", n->name);
            d.primary.tok = n->tok;
            d.primary.label = MSG0("sema.399", "このモジュールはここからは見えません");
            d.hint = MSG1("sema.010", "ファイルの先頭に 'import {0}' を書いてください", n->name);
            diag_fail(&d);
        }

        // ★ lambda の中から外の変数を使おうとした場合（A-42）。
        //   持ち上げた先はトップレベルなので、外の名前はそこから見えません。
        //   「未定義の名前です」で突き放すと、何が起きたのか分かりません。
        if (s->cur_func && s->cur_func->is_lambda) {
            Diag d = {0};
            d.message = MSG1("sema.080", "lambda の中から外の変数 '{0}' は使えません", n->name);
            d.primary.tok = n->tok;
            d.primary.label = MSG0("sema.400", "この名前は lambda の外のものです");
            d.hint = MSG0("sema.401", "捕獲（クロージャ）はまだありません。使う値は引数で受け取るか、def で書いた関数にしてください（グローバルなら使えます）");
            diag_fail(&d);
        }

        Diag d = {0};
        d.message = MSG1("sema.081", "未定義の名前 '{0}' です", n->name);
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.402", "この名前は宣言されていません");
        d.hint = MSG1("sema.082", "使う前に宣言してください（例: {0}: int = 0）", n->name);
        diag_fail(&d);
    }
    n->ir_name = v->ir_name;  // ★ codegen はこれを使う
    // ★ 捕獲した lambda を入れた変数（A-51）。読んだ節点に捕獲を書き写すので、
    //   返す・しまう・別スレッドへ渡す検査が、変数越しにも効きます。
    //   注意: codegen は is_func_ref のときだけ caps を見るので、IR は変わりません。
    if (v->caps) n->caps = v->caps;
    return v->type;
}

static Type *check_expr(Sema *s, Node *n) {
    Type *t;
    switch (n->kind) {
        // ★ 畳んだ列挙の枝（A-37）はここに来ます。**もう一度検査しても
        //   int に戻らない**ようにします（既定値は宣言の登録と本体の検査で
        //   2 度通ることがあり、2 度目に型が変わると「'int' を 'Level' に
        //   渡せません」という説明のつかないエラーになります）。
        case ND_INT: t = n->en ? n->en->type : ty_int; break;
        // ★ 中身を持つ枝のタグ（A-41）。先頭の i64 を読むだけです。
        case ND_ENUMTAG:
            check_expr(s, n->lhs);
            t = ty_int;
            break;
        // ★ 三項演算子。**両側の型が一致していること**を要求します。
        //   条件は bool。片方だけ絞り込む、といった細工はしません
        //   （式の型が一意に決まる、という言語全体の方針を守ります）。
        case ND_COND: t = check_ternary(s, n); break;
        // ★ スライス。**同じ型を返します**（str→str, list[T]→list[T]）
        case ND_SLICE: t = check_slice(s, n); break;
        // ★ タプル式 (a, b)
        case ND_TUPLE: t = check_tuple(s, n); break;
        case ND_FLOAT: t = ty_float; break;
        case ND_BOOL: t = ty_bool; break;
        case ND_STR: t = ty_str; break;
        case ND_NONE: t = ty_null; break;  // ヌルポインタという「値」
        case ND_VAR: t = check_var(s, n); break;
        case ND_BINOP: t = check_binop(s, n); break;
        case ND_LOGICAL: t = check_logical(s, n); break;
        case ND_CALL: t = check_call(s, n); break;
        case ND_LIST: t = check_list_lit(s, n); break;
        case ND_INDEX: t = check_index_expr(s, n); break;
        case ND_METHOD: t = check_method(s, n); break;
        case ND_FIELD: t = check_field(s, n); break;
        case ND_UNARY: t = check_unary(s, n); break;
        case ND_LISTCOMP: t = check_listcomp(s, n); break;
        default: UNREACHABLE();
    }
    n->type = t;  // ★ コード生成器はこれを見る
    return t;
}

// ── 文の検査 ────────────────────────────────────────────────

static void check_stmt(Sema *s, Node *n);

static void check_vardecl(Sema *s, Node *n) {
    // ① 型注釈を解決する（木になった）
    //
    // ★ type_ref が NULL なら「コンパイラが作った宣言」（for の脱糖）。
    //   初期化式の型をそのまま使います。
    // 注意: 利用者が書く宣言では parser が必ず type_ref を作るので、
    //    「型注釈は必須」（言語仕様 3.3）は破られません。
    //    言語仕様 5.5 も「ループ変数は型注釈不要（要素型から決まる）」としています。
    Type *declared = NULL;
    if (n->type_ref) {
        declared = resolve_type(s, n->type_ref);
        if (declared->kind == TY_NONE)
            error_at_hint_m(n->tok, MSG0("sema.084", "None 型の値は存在しないので変数にできません"), MSG0("sema.083", "変数の型に None は使えません"));
    }

    // ② 同じスコープでの再宣言を禁止（言語仕様 5.1）
    VarEntry *prev = lookup_local(s, n->name);
    if (prev) {
        Diag d = {0};
        d.message = MSG1("sema.085", "変数 '{0}' は既に宣言されています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.403", "ここで再宣言されています");
        d.related.tok = prev->decl_tok;
        d.related.label = MSG0("sema.404", "最初の宣言はここです");
        d.hint = MSG0("sema.405", "既存の変数に代入するなら型注釈を外してください（例: x = 1）");
        diag_fail(&d);
    }

    // ②' 外側のスコープの変数を隠していないか（シャドーイング禁止：言語仕様 5.1）
    //
    // ★ lookup と lookup_local を分けておいた判断が、ここで報われます。
    //   同じスコープの再宣言（上）と、外側を隠す宣言（ここ）とで
    //   別々の診断を出せます。1 つの関数で済ませていたら同じ文言でした。
    VarEntry *outer = lookup(s, n->name);
    if (outer) {
        Diag d = {0};
        d.message = MSG1("sema.086", "変数 '{0}' は外側のスコープの変数を隠しています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.406", "シャドーイングは禁止されています（言語仕様 5.1）");
        d.related.tok = outer->decl_tok;
        d.related.label = MSG0("sema.407", "外側の宣言はここです");
        d.hint = MSG0("sema.408", "別の名前にするか、型注釈を外して既存の変数に代入してください（例: x = 1）");
        diag_fail(&d);
    }

    // ③ 初期化式の型が宣言した型に代入できるか。
    //    ★ 空リスト [] の要素型を決めるため、期待型を渡す
    s->expected = declared;
    Type *actual = check_expr(s, n->rhs);
    s->expected = NULL;

    // 型注釈が無ければ、初期化式の型がそのまま変数の型になる
    if (!declared) {
        if (actual->kind == TY_NONE)
            error_at_hint_m(n->rhs->tok, MSG0("sema.088", "値を返さない式は変数に入れられません"), MSG0("sema.087", "None 型の値は変数にできません"));
        declared = actual;
    }

    // ★ 見立て（A-41）。「enum の値を、当たった枝のクラスとして見る」宣言です。
    //   **ここだけ型検査を通します。** match が枝を確かめた直後にしか作らない
    //   （sema が組み立てる）ので、利用者がこの抜け道に触ることはありません。
    if (n->is_enum_view && actual->kind == TY_ENUM && declared->kind == TY_CLASS) {
        VarEntry *vv = declare(s, n->name, declared, n->tok);
        n->ir_name = vv->ir_name;
        n->type = declared;     // ★ codegen が箱の型に使います
        return;
    }

    if (!type_assignable(actual, declared)) {
        Diag d = {0};
        d.message = MSG0("sema.409", "型が一致しません");
        d.primary.tok = n->rhs->tok;
        d.primary.label = MSG1("sema.089", "型 '{0}' の式", type_name(actual));
        d.related.tok = n->tok;
        d.related.label =
            MSG2("sema.090", "変数 '{0}' は '{1}' 型として宣言されています", n->name, type_name(declared));
        d.hint = no_implicit_hint(actual, declared);
        diag_fail(&d);
    }

    // ★ 範囲型なら、入れる前に確かめます（A-28）
    n->rhs = range_coerce(s, n->rhs, declared);

    // ④ スコープに登録する。
    //    ★ 順序が重要：初期化式を検査した「後」に登録します。
    //      そうしないと `x: int = x` が自分自身を参照できてしまいます。
    VarEntry *v = declare(s, n->name, declared, n->tok);
    n->ir_name = v->ir_name;  // ★ codegen が alloca / store に使う名前
    n->type = declared;
    if (n->rhs && n->rhs->caps) v->caps = n->rhs->caps;   // A-51
}

static void check_assign(Sema *s, Node *n) {
    Node *target = n->lhs;

    // 添字への代入 xs[i] = v
    if (target->kind == ND_INDEX) {
        // ★ クラスへの代入は __setitem__ に読み替えます。
        //   m[i, j] = v  →  m.__setitem__(i, j, v)
        //   注意: 読み替えるのは**代入文そのもの**です（値まで実引数にするため）。
        Type *ot = check_expr(s, target->lhs);
        if (ot->kind == TY_CLASS && ot->cls) {
            if (!class_has_method(ot->cls, "__setitem__"))
                error_at_hint_m(target->tok, MSG1("sema.092", "クラス '{0}' に '__setitem__(self, …, 値) -> None' を定義すると、添字への代入が使えます", ot->cls->name), MSG1("sema.091", "型 '{0}' の添字には代入できません", type_name(ot)));

            Node *args = target->rhs;
            Node *last = args;
            while (last->next) last = last->next;
            n->rhs->next = NULL;
            last->next = n->rhs;      // 最後の実引数は「代入する値」

            // 注意: **代入文のノードそのもの**を呼び出しにします。
            //   （文の並び next を壊さないよう、入れ替えではなく上書きです。）
            n->kind = ND_METHOD;
            n->tok = target->tok;
            n->name = "__setitem__";
            n->lhs = target->lhs;
            n->args = args;
            n->rhs = NULL;
            n->type = check_class_method(s, n, ot->cls);
            return;
        }

        Type *et = check_index_expr(s, target);
        target->type = et;

        // 注意: タプルの要素には代入できません
        if (ot->kind == TY_TUPLE)
            error_at_hint_m(target->tok, MSG0("sema.094", "タプルは作ったら変わりません（新しいタプルを作ってください）"), MSG0("sema.093", "タプルの要素には代入できません"));

        // 注意: str は不変（immutable）なので s[0] = "x" は書けません（言語仕様 3.1）
        if (target->lhs->type->kind == TY_STR)
            error_at_hint_m(target->tok, MSG0("sema.096", "str は不変（immutable）です。新しい文字列を作ってください"), MSG0("sema.095", "文字列の要素には代入できません"));

        s->expected = et;
        Type *actual = check_expr(s, n->rhs);
        s->expected = NULL;
        reject_escaping_closure(n->rhs, MSG0("sema.410", "しまうことが"));   // A-43

        if (!type_assignable(actual, et)) {
            Diag d = {0};
            d.message = MSG0("sema.409", "型が一致しません");
            d.primary.tok = n->rhs->tok;
            d.primary.label = MSG1("sema.089", "型 '{0}' の式", type_name(actual));
            d.related.tok = target->tok;
            d.related.label = MSG1("sema.097", "この要素は '{0}' 型です", type_name(et));
            d.hint = no_implicit_hint(actual, et);
            diag_fail(&d);
        }
        n->rhs = range_coerce(s, n->rhs, et);   // A-28
        n->type = et;
        return;
    }

    // フィールドへの代入 t.kind = v。
    // ★ 添字への代入とまったく同じ形です（型を引く関数が違うだけ）。
    if (target->kind == ND_FIELD) {
        Type *ft = check_field(s, target);
        target->type = ft;

        s->expected = ft;
        Type *actual = check_expr(s, n->rhs);
        reject_escaping_closure(n->rhs, MSG0("sema.410", "しまうことが"));   // A-43
        s->expected = NULL;

        if (!type_assignable(actual, ft)) {
            Diag d = {0};
            d.message = MSG0("sema.409", "型が一致しません");
            d.primary.tok = n->rhs->tok;
            d.primary.label = MSG1("sema.089", "型 '{0}' の式", type_name(actual));
            d.related.tok = target->field->tok;
            d.related.label = MSG2("sema.098", "フィールド '{0}' は '{1}' 型として宣言されています", target->field->name, type_name(ft));
            d.hint = no_implicit_hint(actual, ft);
            diag_fail(&d);
        }
        n->rhs = range_coerce(s, n->rhs, ft);   // A-28
        n->type = ft;
        return;
    }

    if (target->kind != ND_VAR) UNREACHABLE();  // parser が保証している

    VarEntry *v = lookup(s, target->name);
    if (!v) {
        Diag d = {0};
        d.message = MSG1("sema.099", "未定義の名前 '{0}' に代入しています", target->name);
        d.primary.tok = target->tok;
        d.primary.label = MSG0("sema.402", "この名前は宣言されていません");
        d.hint = MSG1("sema.100", "初めて使うときは型注釈が必要です（例: {0}: int = 0）", target->name);
        diag_fail(&d);
    }
    target->type = v->type;
    target->ir_name = v->ir_name;

    // ★ 代入できるかは「宣言された型」で判定します。
    //   絞り込みで一時的に狭くなっていても、代入できる範囲は変わりません。
    s->expected = v->declared;  // ★ xs = [] のため
    Type *actual = check_expr(s, n->rhs);
    // ★ グローバルは枠より長生きするので、捕獲した lambda は入れられません（A-43）
    if (v->is_global) reject_escaping_closure(n->rhs, MSG0("sema.410", "しまうことが"));
    // ★ 借りて捕まえた lambda は**宣言でだけ**受け取れます（A-51）。
    //   代入だと、内側のスコープの変数を捕まえた lambda を外側の変数へ
    //   運べてしまい、捕まえた変数が先に解放されます。
    if (n->rhs->caps) {
        for (Node *c = n->rhs->caps; c; c = c->next)
            if (!capture_by_value(c->type)) {
                Diag d = {0};
                d.message = MSG1("sema.639", "'{0}' を借りて捕まえた lambda は、代入では受け取れません", c->name);
                d.primary.tok = n->rhs->tok;
                d.primary.label = MSG0("sema.640", "ここで既にある変数へ入れています");
                d.related.tok = c->tok;
                d.related.label = MSG1("sema.251", "'{0}' を捕まえています", c->name);
                d.hint = MSG0("sema.641", "新しい変数の宣言で受け取ってください（例: f2: fn(int) -> int = lambda x: …）");
                diag_fail(&d);
            }
        v->caps = n->rhs->caps;
    }
    s->expected = NULL;
    if (!type_assignable(actual, v->declared)) {
        Diag d = {0};
        d.message = MSG0("sema.409", "型が一致しません");
        d.primary.tok = n->rhs->tok;
        d.primary.label = MSG1("sema.089", "型 '{0}' の式", type_name(actual));
        d.related.tok = v->decl_tok;
        d.related.label = MSG2("sema.090", "変数 '{0}' は '{1}' 型として宣言されています", v->name, type_name(v->declared));
        d.hint = no_implicit_hint(actual, v->declared);
        diag_fail(&d);
    }

    n->rhs = range_coerce(s, n->rhs, v->declared);   // A-28

    // ★ 代入したら絞り込みは解除する（15.5 節）。
    //   cur = cur.next のあと、cur はまた None かもしれないからです。
    //   本格的なフロー解析の代わりに「代入したら忘れる」という
    //   保守的な近似で済ませています。
    v->type = v->declared;
    target->type = v->declared;
    n->type = v->declared;
}

// 条件式は bool でなければならない（言語仕様 5.3 / 5.4）
static void check_cond(Sema *s, const char *where, Node *stmt_node, Node *cond) {
    Type *t = check_expr(s, cond);
    if (t->kind != TY_BOOL)
        bool_required(MSG1("sema.101", "{0}には bool が必要です", where),
                      MSG1("sema.102", "{0}はここです", where), stmt_node->tok, cond, t);
}

// ブロックは新しいスコープを作る。
// ★ scope_push / scope_pop が、ここで初めて入れ子で対になります。
// ── 絞り込み（narrowing） ─────────────────────────────
//
// ★ 「変数の型は 1 つ」という前提を、ここだけ崩します。
//
//     t: Token | None = find()
//     if t is not None:
//         print(t.kind)     ← この中でだけ t は Token
//
// 実装は「入る前に変えて、抜けたら戻す」だけです。スコープと同じ形で、
// C の呼び出しスタックがそのまま絞り込みのスタックになります。
//
// 注意: 絞れるのはローカル変数だけです（15.5 節）。
//   ・グローバル変数 … 呼んだ関数の中で書き換えられるかもしれない
//   ・フィールド     … node.next を絞ると「その間 node.next が変わらないこと」を
//                      保証しなければならない。メソッド呼び出し 1 つで壊れる

static void narrow_one(Sema *s, Node *var_node, NarrowSet *ns) {
    if (var_node->kind != ND_VAR) return;

    VarEntry *v = lookup(s, var_node->name);
    if (!v || v->is_global) return;   // グローバルは絞らない
    if (v->type->kind != TY_OPT) return;
    if (ns->n >= NARROW_MAX) return;  // 深すぎる条件は諦める（保守的でよい）

    ns->vars[ns->n] = v;
    ns->saved[ns->n] = v->type;
    ns->n++;
    v->type = v->type->elem;  // ★ ここだけ Token になる
}

// 条件式から「絞り込める変数」を集めて適用する。
//   positive = true  … 条件が成り立つ側（then / while の本体）
//   positive = false … 成り立たない側（else）
static void narrow_apply(Sema *s, Node *cond, bool positive, NarrowSet *ns) {
    if (!cond) return;

    if (cond->kind == ND_BINOP && cond->op == OP_ISNOT && positive)
        narrow_one(s, cond->lhs, ns);
    else if (cond->kind == ND_BINOP && cond->op == OP_IS && !positive)
        narrow_one(s, cond->lhs, ns);
    else if (cond->kind == ND_LOGICAL && cond->op == OP_AND && positive) {
        // a is not None and b is not None → 両方絞れる
        narrow_apply(s, cond->lhs, true, ns);
        narrow_apply(s, cond->rhs, true, ns);
    }
    else if (cond->kind == ND_LOGICAL && cond->op == OP_OR && !positive) {
        // ★ ド・モルガン。
        //   not(a or b) = (not a) and (not b) なので、成り立たない側では両方絞れます。
        //
        //     if b is None or c is None:
        //         return 0
        //     # ← ここでは b も c も None ではない
        //
        // 注意: 「成り立つ側」の or は相変わらず絞れません（どちらか一方しか保証されない）。
        narrow_apply(s, cond->lhs, false, ns);
        narrow_apply(s, cond->rhs, false, ns);
    }
}

static void narrow_restore(NarrowSet *ns) {
    // 注意: 必ず戻します。戻し忘れると、if の外でも絞られたままになります。
    for (int i = 0; i < ns->n; i++) ns->vars[i]->type = ns->saved[i];
    ns->n = 0;
}

static bool always_returns(Node *n);

// 文の並びを順に検査する（スコープは呼び出し側が用意する）。
//
// ★ ガード節による絞り込みをここに入れます。
//
//     if b is None:
//         return 0          ← ここで必ず抜ける
//     return b.v            ← だから、この先の b は None ではない
//
// 「その if の中で必ず return するなら、その後ろでは条件の反対側が
//   成り立っている」だけの判断です。到達可能性の検査（
//   always_returns）を、そのまま絞り込みに再利用しています。
//
// 注意: 関数本体もブロックも同じ関数を通します。片方だけに入れると、
//    「関数の直下では効くのに if の中では効かない」という説明できない差が出ます。
static void check_stmt_list(Sema *s, Node *first) {
    NarrowSet guard = {0};

    for (Node *st = first; st; st = st->next) {
        check_stmt(s, st);

        if (st->kind == ND_IF && !st->els && always_returns(st->body))
            narrow_apply(s, st->lhs, false, &guard);
    }

    narrow_restore(&guard);
}

static void check_block(Sema *s, Node *n) {
    scope_push(s);
    check_stmt_list(s, n->body);
    scope_pop(s);
}

// ── 組み込み関数の表 ─────────────────────────────────
//
// ★ 名前 + 引数型 で 1 つの候補を表します。
//   sema は「型が合う候補があるか」を、codegen は「どの C 関数を呼ぶか」を
//   同じ表から引きます。
//
// なぜ print だけオーバーロードを許すのか（言語仕様 7 節）
//   ユーザー定義関数のオーバーロードは許しません（名前解決が複雑になる）。
//   組み込みは表を引くだけで解決できるので、「言語機能」ではなく
//   「表のエントリ」として扱えます。実装が増えません。
const Builtin BUILTINS[] = {
    // 名前     引数型     戻り型    呼び出す C 関数
    {"print", TY_INT, TY_NONE, "pl_print_int"},
    {"print", TY_STR, TY_NONE, "pl_print_str"},
    {"print", TY_BOOL, TY_NONE, "pl_print_bool"},
    {"print", TY_FLOAT, TY_NONE, "pl_print_float"},
    {"len", TY_STR, TY_INT, "pl_str_len"},
    {"len", TY_LIST, TY_INT, "pl_list_len"},  // 要素型は見ない
    {"str", TY_INT, TY_STR, "pl_str_from_int"},
    {"str", TY_BOOL, TY_STR, "pl_str_from_bool"},
    {"str", TY_FLOAT, TY_STR, "pl_str_from_float"},
    // ★ str(str) は複製を返します。f-string が中身の型を
    //   知らずに str(...) で包めるようにするためです。
    //   注意: 同じポインタを返すと、--drop のときに二重解放になります。
    {"str", TY_STR, TY_STR, "pl_str_copy"},
    {"int", TY_STR, TY_INT, "pl_str_to_int"},
    // ★ float(str)。CSV を読むのに要ります（正しく丸めます）
    {"float", TY_STR, TY_FLOAT, "pl_str_to_float"},
    // int ↔ float。注意: 暗黙変換はしないので、必ずここを通します。
    {"int", TY_FLOAT, TY_INT, "pl_int_from_float"},
    {"float", TY_INT, TY_FLOAT, "pl_float_from_int"},
    {"ord", TY_STR, TY_INT, "pl_ord"},
    // ★ range の増分が変数のとき、for の脱糖が 1 回だけ呼びます（A-46）。
    //   注意: 名前に '.' があるので、利用者のコードからは書けません。
    {"range.step", TY_INT, TY_INT, "pl_range_step"},
    {"chr", TY_INT, TY_STR, "pl_chr"},
    {"exit", TY_INT, TY_NONE, "pl_exit"},
    {"panic", TY_STR, TY_NONE, "pl_panic"},
    // ★ 借りたものを保存したいときの逃げ道（決定 D8）。
    //   注意: いまは str だけです。list[T] の複製は要素の所有まで考える必要が
    //      あるので、`rc[T]`と一緒に見直します。
    // ★ Python で最も使う組み込みを足しました。
    //   注意: min / max は **2 引数**の表では表せない（引数 2 個）ので、
    //     ここではなく check_call で特別扱いします。
    {"abs", TY_INT, TY_INT, "pl_iabs"},
    {"abs", TY_FLOAT, TY_FLOAT, "pl_fabs"},
    {"sum", TY_LIST, TY_INT, "pl_list_sum"},
    // ★ ハッシュ。**単相化のおかげで**、ジェネリックなコードの中で
    //   hash(k) と書けます（K が確定してから型検査されるため）。
    //   注意: クラスを鍵にすると、ここで「使えません」と言われます。
    {"hash", TY_STR, TY_INT, "pl_hash_str"},
    {"hash", TY_INT, TY_INT, "pl_hash_i64"},
    {"hash", TY_FLOAT, TY_INT, "pl_hash_f64"},
    {"hash", TY_BOOL, TY_INT, "pl_hash_i64"},
    // ★ input(プロンプト) — Python と同じ形。
    //   注意: EOF では panic します。読めないかもしれない場面では
    //     io.read_line()（None が返る）を使ってください。
    {"input", TY_STR, TY_STR, "pl_input"},
    // ★ copy は**型ごとに出すものが変わる**ので、この表では扱いません
    //   （A-44。check_call の中で 1 か所にまとめてあります）。
    {NULL, 0, 0, NULL},
};

// ★ 折り返す算術の名前か（wrap_add / wrap_sub / wrap_mul）
static bool is_wrap_name(const char *name) {
    return strcmp(name, "wrap_add") == 0 || strcmp(name, "wrap_sub") == 0 ||
           strcmp(name, "wrap_mul") == 0;
}

// その名前の組み込みが 1 つでもあるか
bool is_builtin_name(const char *name) {
    // ★ move_out は BUILTINS の表に載せられません（戻り型が引数と同じ型そのもので、
    //   表は TypeKind しか持てないため）。名前だけここで数えます。
    if (strcmp(name, "move_out") == 0) return true;
    for (int i = 0; BUILTINS[i].name; i++)
        if (strcmp(BUILTINS[i].name, name) == 0) return true;
    return false;
}

// 受け取れる型の一覧（エラーメッセージ用）。type_name_list と同じ発想。
static const char *builtin_arg_types(const char *name) {
    StrBuf sb;
    sb_init(&sb);
    bool first = true;
    for (int i = 0; BUILTINS[i].name; i++) {
        if (strcmp(BUILTINS[i].name, name) != 0) continue;
        // list[T] にはシングルトンが無いので、表示用の名前を直接書く
        const char *nm = BUILTINS[i].arg == TY_LIST
                             ? "list[T]"
                             : type_name(type_from_kind(BUILTINS[i].arg));
        sb_printf(&sb, "%s%s", first ? "" : ", ", nm);
        first = false;
    }
    return sb_str(&sb);
}

// 組み込み関数の呼び出しを検査し、使う候補を n->builtin に記録する。
static Type *check_builtin_call(Sema *s, Node *n) {
    int nargs = 0;
    for (Node *a = n->args; a; a = a->next) nargs++;

    // ★ input() は引数なしでも書けます（Python と同じ）。
    //   注意: 引数を省いたときは、空のプロンプトを渡したことにします。
    if (strcmp(n->name, "input") == 0 && nargs == 0) {
        n->args = new_str_node(n->tok, "", 0);
        n->args->type = ty_str;
        nargs = 1;
    }

    if (nargs != 1) {
        Diag d = {0};
        d.message = MSG2("sema.103", "{0} は 1 個の引数を取りますが、{1} 個渡されました", n->name, diag_fmt("%d", nargs));
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.197", "引数の個数が違います");
        d.hint = MSG1("sema.104", "{0}(値) の形で使ってください", n->name);
        diag_fail(&d);
    }

    Type *at = check_expr(s, n->args);

    // ★ print(xs) / str(xs) — list をそのまま表示できるようにします。
    //   注意: 要素の型ごとにランタイム関数を呼び分けるので、**要素が
    //     int / float / str / bool のときだけ**です。入れ子（list[list[int]]）は
    //     要素をさらに文字列にする手立てが要るので、ここで弾きます。
    if (at->kind == TY_LIST &&
        (strcmp(n->name, "print") == 0 || strcmp(n->name, "str") == 0)) {
        TypeKind ek = at->elem->kind;
        if (ek != TY_INT && ek != TY_FLOAT && ek != TY_STR && ek != TY_BOOL) {
            Diag d = {0};
            d.message = MSG2("sema.105", "'{0}' はそのまま {1} できません", type_name(at), n->name);
            d.primary.tok = n->args->tok;
            d.primary.label = MSG1("sema.106", "要素が '{0}' 型です", type_name(at->elem));
            d.hint = MSG0("sema.411", "そのまま出せるのは list[int] / list[float] / list[str] / list[bool] だけです。ほかは for でまわしてください");
            diag_fail(&d);
        }
        n->is_list_str = true;
        return strcmp(n->name, "print") == 0 ? ty_none : ty_str;
    }

    // ── move_out(場所) — 所有権を取り出し、その場所は空にする ──────
    //
    // ★ なぜ要るか（docs/ja/roadmap.md A-21d）
    //   `return self.out` は仕様 §4.5 が許しますが、**戻り値の型に
    //   「借用だ」と書く手段がありません**。呼ぶ側は所有として受け取り、
    //   自分でも解放するので、--drop すると二重解放になります。
    //   take は「持っていく」と書けるようにして、この形を無くします。
    //
    //       return move_out(self.out)   # self.out は空のリストになる
    //
    // 注意: 戻り型は**引数と同じ型そのもの**です（list[rc[Token]] なら
    //   list[rc[Token]]）。BUILTINS の表は TypeKind しか持てないので、
    //   表引きの前にここで返します。
    if (strcmp(n->name, "move_out") == 0) {
        // ① 引数は「場所」でなければならない。
        //    値を取り出したあと**空を書き戻す**ので、書ける場所が要ります。
        Node *a = n->args;
        bool is_place = a->kind == ND_FIELD || a->kind == ND_INDEX ||
                        (a->kind == ND_VAR && a->is_global);
        if (!is_place) {
            Diag d = {0};
            d.message = MSG0("sema.412", "move_out は「場所」からしか取り出せません");
            d.primary.tok = a->tok;
            d.primary.label = MSG0("sema.413", "ここは書き戻せる場所ではありません");
            d.hint = MSG0("sema.414", "move_out(self.xs) / move_out(obj.f) / move_out(xs[i]) の形で使ってください（局所変数は、そのまま返せば所有権ごと動きます）");
            diag_fail(&d);
        }
        // ② 空の値を作れる型だけ。str は ""、list[T] は [] を書き戻します。
        if (at->kind != TY_STR && at->kind != TY_LIST) {
            Diag d = {0};
            d.message = MSG1("sema.107", "move_out は '{0}' 型を取り出せません", type_name(at));
            d.primary.tok = a->tok;
            d.primary.label = MSG1("sema.051", "これは '{0}' 型です", type_name(at));
            d.hint = MSG0("sema.415", "move_out が受け取れるのは str と list[T] です（取り出したあとに書き戻す「空の値」が要るためです）");
            diag_fail(&d);
        }
        n->is_move_out = true;  // ★ codegen と ownck はこれを見る
        return at;
    }

    for (int i = 0; BUILTINS[i].name; i++) {
        if (strcmp(BUILTINS[i].name, n->name) != 0) continue;
        if (BUILTINS[i].arg != (int)at->kind) continue;
        n->builtin = &BUILTINS[i];  // ★ codegen はこれを見る
        return type_from_kind(BUILTINS[i].ret);
    }

    Diag d = {0};
    d.message = MSG2("sema.108", "{0} は '{1}' 型を受け取れません", n->name, type_name(at));
    d.primary.tok = n->args->tok;
    d.primary.label = MSG1("sema.051", "これは '{0}' 型です", type_name(at));
    if (s->inst_site)
        d.related = (DiagLabel){s->inst_site,
                                MSG1("sema.109", "この実体化（{0}）で使われました", s->inst_name)};
    d.hint = MSG2("sema.110", "{0} が受け取れるのは {1} です", n->name, builtin_arg_types(n->name));
    diag_fail(&d);
}

// リストリテラルの検査
static Type *check_list_lit(Sema *s, Node *n) {
    Type *want = s->expected;  // ★ 使う前に控える（下で check_expr が上書きするため）

    if (!n->body) {
        // 空リストは、それ自身から要素型が決まらない
        if (!want || want->kind != TY_LIST) {
            Diag d = {0};
            d.message = MSG0("sema.416", "空のリストの要素型が決まりません");
            d.primary.tok = n->tok;
            d.primary.label = MSG0("sema.417", "この [] がどんなリストなのか分かりません");
            d.hint = MSG0("sema.418", "型注釈を書いてください（例: xs: list[int] = []）。関数の引数に直接渡す場合は、いったん変数に入れてください");
            diag_fail(&d);
        }
        return want;
    }

    // 要素があるなら、最初の要素の型を要素型にする（推論はしない）。
    //
    // ★ 期待型があるなら、そちらを要素型に使います。
    //   [Box(1), None] は最初の要素だけ見ると list[Box] になってしまい、
    //   2 つ目の None が入りません。宣言に list[Box | None] と書いてあるなら
    //   それに従うのが素直です（「空リストの期待型」の延長）。
    s->expected = want && want->kind == TY_LIST ? want->elem : NULL;
    Type *first = check_expr(s, n->body);
    reject_escaping_closure(n->body, MSG0("sema.410", "しまうことが"));   // A-43
    Type *et = first;
    if (want && want->kind == TY_LIST && type_assignable(first, want->elem))
        et = want->elem;

    // ★ 範囲型なら、要素も入れる前に確かめます（A-28）。
    //   注意: 並びの途中を差し替えるので、1 つ前を覚えながら進みます。
    n->body = range_coerce(s, n->body, et);

    int i = 2;
    Node *prev = n->body;
    for (Node *el = n->body->next; el; el = el->next, i++) {
        s->expected = et;
        Type *t = check_expr(s, el);
        reject_escaping_closure(el, MSG0("sema.410", "しまうことが"));   // A-43
        if (!type_assignable(t, et)) {
            Diag d = {0};
            d.message = MSG1("sema.111", "リストの要素の型がそろっていません（第 {0} 要素）", diag_fmt("%d", i));
            d.primary.tok = el->tok;
            d.primary.label = MSG1("sema.051", "これは '{0}' 型です", type_name(t));
            d.related.tok = n->body->tok;
            d.related.label = MSG1("sema.112", "最初の要素は '{0}' 型です", type_name(et));
            d.hint = MSG0("sema.419", "リストの要素はすべて同じ型でなければなりません");
            diag_fail(&d);
        }
        // ★ A-28：差し替えたら、次の周回のために el を進め直します
        Node *co = range_coerce(s, el, et);
        if (co != el) {
            co->next = el->next;
            prev->next = co;
            el = co;
        }
        prev = el;
    }
    s->expected = NULL;
    return type_list(et);
}

// 添字アクセスの検査（型システム 5.8）

// ── 内包表記 ──────────────────────────────────────────
//
//   [ E for x in xs if C ]
//
// ★ **式の位置に現れるので、構文解析での脱糖にできません**（for やカンマ代入
//   との違い）。ここで型を決め、codegen がその場でループを組み立てます。
//
// 注意: 隠し宣言 3 つ（ループ変数 / 結果の list / 添字）は構文解析器が並べています。
//   ここで型を入れて宣言し、**alloca の名前**を用意します。
// ── 旧値 old(式)（ensures の中だけ）──────────────────────────
//
// ★ ensures の中の old(e) は「関数の入口での e の値」です（Ada の 'Old）。
//   **意味解析の前に書き換えます**：
//
//     def push(xs: mut list[int], v: int) -> None:      def push(...):
//         ensures len(xs) == old(len(xs)) + 1     →        requires …（あれば）
//         xs.append(v)                                     old.0 = len(xs)        ← 入口で控える
//                                                          ensures len(xs) == old.0 + 1
//                                                          xs.append(v)
//
//   先頭の契約は「requires → 隠し変数 → ensures」の順に並べ直します。requires を
//   確かめてから控えるので、old(xs[i]) を requires i < len(xs) で守れます。
//   注意: requires と ensures の相対順は実行に影響しません（requires は入口、
//     ensures は出口で確かめるため）。codegen は先頭の契約を集めるときに
//     隠し変数 old.N を読み飛ばします。
//   注意: 型は右辺から推論します（for の隠し変数と同じ）。値型だけに限る検査は
//     ND_ENSURES の検査でします（list を控えると所有権が移ってしまうため）。
//   対になる定義: selfhost/sema の rewrite_old
#define OLD_MAX 256

// 式をたどり、old(e) を見つけるたびに隠し変数の宣言を作って decls に足す。
// 順序は「lhs → rhs → els → args」の前順です（セルフホスト版と同じ番号にするため）。
static void old_walk(Node *n, Node **decls, int *nd, bool inside) {
    for (; n; n = n->next) {
        if (n->kind == ND_CALL && n->name && strcmp(n->name, "old") == 0 && !n->mod_name) {
            if (inside)
                error_at_hint_m(n->tok, MSG0("sema.114", "old(...) の中に old は書けません"), MSG0("sema.113", "old の入れ子です"));
            if (!n->args || n->args->next)
                error_at_hint_m(n->tok, MSG0("sema.116", "old には式を 1 つだけ渡します（例: old(len(xs))）"), MSG0("sema.115", "old の引数の数が違います"));
            if (*nd >= OLD_MAX)
                error_at_hint_m(n->tok, MSG0("sema.118", "old は 1 つの関数に 256 個までです"), MSG0("sema.117", "old が多すぎます"));
            Node *arg = n->args;
            old_walk(arg, decls, nd, true);   // 入れ子を見つけるため
            StrBuf sb;
            sb_init(&sb);
            sb_printf(&sb, "old.%d", *nd);
            Node *d = new_node(ND_VARDECL, n->tok);
            d->name = sb_str(&sb);
            d->rhs = arg;
            decls[(*nd)++] = d;
            // 呼び出しの節点を、その場で隠し変数の参照に変えます
            n->kind = ND_VAR;
            n->name = d->name;
            n->args = NULL;
            continue;
        }
        old_walk(n->lhs, decls, nd, inside);
        old_walk(n->rhs, decls, nd, inside);
        old_walk(n->els, decls, nd, inside);
        old_walk(n->args, decls, nd, inside);
    }
}

// old(e) の e は値型（int / bool / float）に限ります。list や str を控えると、
// 隠し変数へ所有権が移ってしまうためです（len(xs) のように数にして書きます）。
static void check_old_types(Node *n) {
    for (; n; n = n->next) {
        if (n->kind == ND_VAR && n->name && strncmp(n->name, "old.", 4) == 0 && n->type) {
            TypeKind k = n->type->kind;
            if (k != TY_INT && k != TY_BOOL && k != TY_FLOAT) {
                Diag d = {0};
                d.message = MSG0("sema.119", "old(...) に書けるのは int / bool / float の式です");
                d.primary.tok = n->tok;
                d.primary.label = MSG1("sema.051", "これは '{0}' 型です", type_name(n->type));
                d.hint = MSG0("sema.420", "list なら old(len(xs)) のように、比べたい数にして書いてください");
                diag_fail(&d);
            }
        }
        check_old_types(n->lhs);
        check_old_types(n->rhs);
        check_old_types(n->els);
        check_old_types(n->args);
    }
}

static void rewrite_old(Node *fn) {
    Node *decls[OLD_MAX];
    int nd = 0;
    for (Node *st = fn->body->body; st && (st->kind == ND_REQUIRES || st->kind == ND_ENSURES);
         st = st->next)
        if (st->kind == ND_ENSURES) old_walk(st->lhs, decls, &nd, false);
    if (nd == 0) return;

    // 先頭の契約を「requires → 隠し変数 → ensures」に並べ直す
    Node rq = {0}, en = {0};
    Node *rt = &rq, *et = &en;
    Node *st = fn->body->body;
    while (st && (st->kind == ND_REQUIRES || st->kind == ND_ENSURES)) {
        Node *nx = st->next;
        st->next = NULL;
        if (st->kind == ND_REQUIRES) { rt->next = st; rt = st; }
        else { et->next = st; et = st; }
        st = nx;
    }
    for (int i = 0; i < nd; i++) {
        rt->next = decls[i];
        rt = decls[i];
    }
    rt->next = en.next;
    if (en.next) et->next = st;
    else rt->next = st;
    fn->body->body = rq.next;
}

static Type *check_listcomp(Sema *s, Node *n) {
    Node *lv = n->body;          // ループ変数
    Node *res = lv->next;        // 結果の list
    Node *ix = res->next;        // 添字（range のときは数える変数）

    // ── 対象の型を決める ──
    Type *elem_t;
    if (n->args) {
        // range：構文解析器が「開始・終端」に正規化し、増分は n->ival に入れています
        for (Node *a = n->args; a; a = a->next) {
            Type *at = check_expr(s, a);
            if (at->kind != TY_INT)
                error_at_hint_m(a->tok, MSG0("sema.120", "range の引数は int です"), MSG1("sema.051", "これは '{0}' 型です", type_name(at)));
        }
        elem_t = ty_int;
    } else {
        Type *it = check_expr(s, n->rhs);
        if (it->kind != TY_LIST) {
            Diag d = {0};
            d.message = MSG1("sema.121", "'{0}' 型は内包表記の対象にできません", type_name(it));
            d.primary.tok = n->rhs->tok;
            d.primary.label = MSG1("sema.051", "これは '{0}' 型です", type_name(it));
            d.hint = MSG0("sema.421", "対象にできるのは list[T] と range(...) です");
            diag_fail(&d);
        }
        elem_t = it->elem;
    }

    // ── ループ変数の見えるところで、要素の式と条件を検査する ──
    scope_push(s);
    lv->type = elem_t;
    VarEntry *vlv = declare(s, lv->name, elem_t, lv->tok);
    lv->ir_name = vlv->ir_name;

    // ★ 受け取る名前の分解（A-47）。ループ変数の見えるところで宣言します
    for (Node *u = n->incr; u; u = u->next) check_unpack(s, u);

    Type *et = check_expr(s, n->lhs);
    if (et->kind == TY_NONE) {
        Diag d = {0};
        d.message = MSG0("sema.422", "内包表記の要素が値を持ちません");
        d.primary.tok = n->lhs->tok;
        d.primary.label = MSG0("sema.423", "この式は None 型です");
        d.hint = MSG0("sema.424", "値を返す式を書いてください");
        diag_fail(&d);
    }

    if (n->els) {
        Type *ct = check_expr(s, n->els);
        if (ct->kind != TY_BOOL)
            error_at_hint_m(n->els->tok, MSG0("sema.122", "内包表記の 'if' には bool が必要です"), MSG1("sema.051", "これは '{0}' 型です", type_name(ct)));
    }
    scope_pop(s);

    // ★ 量化子 all(E for …) / any(E for …)。E は bool で、結果も bool です
    //   注意: 隠し変数の宣言は内包表記と同じにしておきます（箱の並びを揃えるため）。
    if (n->name) {
        if (et->kind != TY_BOOL) {
            Diag d = {0};
            d.message = MSG1("sema.123", "{0}(...) の中の式は bool でなければなりません", n->name);
            d.primary.tok = n->lhs->tok;
            d.primary.label = MSG1("sema.051", "これは '{0}' 型です", type_name(et));
            d.hint = MSG1("sema.124", "例: {0}(xs[i] >= 0 for i in range(len(xs)))", n->name);
            diag_fail(&d);
        }
        res->type = ty_bool;
        VarEntry *vq = declare(s, res->name, ty_bool, res->tok);
        res->ir_name = vq->ir_name;
        ix->type = ty_int;
        VarEntry *vqi = declare(s, ix->name, ty_int, ix->tok);
        ix->ir_name = vqi->ir_name;
        return ty_bool;
    }

    // ── 隠し変数（結果の list と添字）──
    Type *lt = type_list(et);
    res->type = lt;
    VarEntry *vres = declare(s, res->name, lt, res->tok);
    res->ir_name = vres->ir_name;
    ix->type = ty_int;
    VarEntry *vix = declare(s, ix->name, ty_int, ix->tok);
    ix->ir_name = vix->ir_name;

    return lt;
}

static Type *check_index_expr(Sema *s, Node *n) {
    Type *ot = check_expr(s, n->lhs);

    // ★ クラスの添字は __getitem__ に読み替えます（m[i, j] を含む）。
    //   注意: 添字は **並び**なので、そのまま実引数のリストになります。
    if (ot->kind == TY_CLASS && ot->cls) {
        if (!class_has_method(ot->cls, "__getitem__"))
            error_at_hint_m(n->tok, MSG1("sema.126", "クラス '{0}' に '__getitem__(self, …) -> …' を定義すると、添字が使えます", ot->cls->name), MSG1("sema.125", "型 '{0}' は添字を取れません", type_name(ot)));
        n->kind = ND_METHOD;
        n->name = "__getitem__";
        n->args = n->rhs;
        n->rhs = NULL;
        return check_class_method(s, n, ot->cls);
    }

    // ★ タプルの添字 t[0]
    //
    //   注意: **添字は定数（整数リテラル）だけ**です。`(int, str)` の要素は
    //     位置ごとに型が違うので、添字が実行時に決まると**式の型が決まりません**。
    if (ot->kind == TY_TUPLE) {
        if (n->rhs->next)
            error_at_hint_m(n->rhs->next->tok, MSG0("sema.128", "タプルの添字は 1 つだけです"), MSG0("sema.127", "添字が 2 つ以上あります"));
        if (n->rhs->kind != ND_INT) {
            Diag d = {0};
            d.message = MSG0("sema.425", "タプルの添字は整数リテラルでなければなりません");
            d.primary.tok = n->rhs->tok;
            d.primary.label = MSG0("sema.426", "ここは定数である必要があります");
            d.hint = MSG0("sema.427", "要素ごとに型が違うので、添字が実行時に決まると式の型が決められません（分解代入 a, b = t も使えます）");
            diag_fail(&d);
        }
        long long k = n->rhs->ival;
        if (k < 0 || k >= ot->nparams) {
            Diag d = {0};
            d.message = MSG1("sema.129", "タプルの添字が範囲外です（{0}）", diag_fmt("%lld", k));
            d.primary.tok = n->rhs->tok;
            d.primary.label = MSG2("sema.130", "要素は {0} 個です（0 〜 {1}）", diag_fmt("%d", ot->nparams), diag_fmt("%d", ot->nparams - 1));
            diag_fail(&d);
        }
        n->rhs->type = ty_int;
        return ot->params[k];
    }

    // list[T] と str の添字は 1 つだけです
    if (n->rhs->next) {
        Diag d = {0};
        d.message = MSG1("sema.131", "型 '{0}' に添字を 2 つ以上は書けません", type_name(ot));
        d.primary.tok = n->rhs->next->tok;
        d.primary.label = MSG0("sema.428", "2 つめの添字はここです");
        d.hint = MSG0("sema.429", "2 次元の添字（m[i, j]）が使えるのは __getitem__ を定義したクラスだけです");
        diag_fail(&d);
    }

    Type *it = check_expr(s, n->rhs);

    if (it->kind != TY_INT) {
        Diag d = {0};
        d.message = MSG0("sema.430", "添字は int でなければなりません");
        d.primary.tok = n->rhs->tok;
        d.primary.label = MSG1("sema.051", "これは '{0}' 型です", type_name(it));
        diag_fail(&d);
    }

    if (ot->kind == TY_LIST) return ot->elem;
    // ★ str の添字は 1 文字の str を返す（char 型は作らない。型システム 5.8）
    if (ot->kind == TY_STR) return ty_str;

    Diag d = {0};
    d.message = MSG1("sema.125", "型 '{0}' は添字を取れません", type_name(ot));
    d.primary.tok = n->lhs->tok;
    d.primary.label = MSG1("sema.051", "これは '{0}' 型です", type_name(ot));
    d.hint = MSG0("sema.431", "添字が使えるのは list[T] と str です");
    diag_fail(&d);
}

// フィールドアクセスの検査（型システム 5.9）
// 「'.' の左がモジュールか」を判定する（13.5 節の名前解決の順序）。
//
// ★ 変数が先、モジュールは最後。ただしモジュール名と同じ名前の変数は
//   宣言できない（declare_* で弾く）ので、実際には競合しません。
//   それでも順序を実装の順序としてそのまま書いておきます。
static ModuleSyms *dot_module(Sema *s, Node *n) {
    if (n->lhs->kind == ND_VAR) {
        if (lookup(s, n->lhs->name)) return NULL;
        return lookup_import(s, n->lhs->name);
    }

    // ★ パッケージ（A-32）：`pkg.mod.f()` の左側は `pkg.mod` という
    //   **ドットを含む 1 つのモジュール名**です。
    //
    // 注意: 変数のフィールド（`obj.field.f()`）と見分けが要ります。見分け方は
    //   「いちばん左が変数として宣言されていないこと」＋「繋げた名前が
    //   import されていること」の 2 つです。import は明示的に書くものなので、
    //   両方を満たす形は 1 つしかありません。
    if (n->lhs->kind != ND_FIELD) return NULL;
    StrBuf sb;
    sb_init(&sb);
    Node *segs[8];
    int nseg = 0;
    for (Node *q = n->lhs; q; q = q->lhs) {
        if (nseg >= 8) return NULL;   // 深すぎる修飾は扱いません
        segs[nseg++] = q;
        if (q->kind == ND_VAR) break;
        if (q->kind != ND_FIELD) return NULL;
    }
    Node *root = segs[nseg - 1];
    if (root->kind != ND_VAR) return NULL;
    if (lookup(s, root->name)) return NULL;   // 変数が先（13.5 節）
    for (int i = nseg - 1; i >= 0; i--)
        sb_printf(&sb, i == nseg - 1 ? "%s" : ".%s", segs[i]->name);
    return lookup_import(s, sb_str(&sb));
}

// lexer.MAX_KIND — 他のモジュールのグローバル変数
static Type *check_module_global(Sema *s, Node *n, ModuleSyms *ms) {
    VarEntry *v = lookup_global_in(ms, n->name);

    // ★ グローバル変数に無ければ **関数を探します**。
    //   math.sin のように、他のモジュールの関数も値として渡せます。
    if (!v) {
        FuncSig *f = lookup_func_in(ms, n->name);
        if (f && f->nraises == 0) {
            n->ir_name = f->ir_name;
            n->is_func_ref = true;
            n->is_extern = true;    // 他モジュールなので declare が要る
            return fn_type_of(f);
        }
    }

    if (!v) {
        Diag d = {0};
        d.message = MSG2("sema.132", "モジュール '{0}' に '{1}' はありません", ms->mod->name, n->name);
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.432", "この名前は定義されていません");
        if (lookup_func_in(ms, n->name))
            d.hint = MSG3("sema.133", "'{0}' は関数です。'{1}.{2}(...)' と呼んでください", n->name, ms->mod->name, n->name);
        else if (lookup_class_in(ms, n->name))
            d.hint = MSG3("sema.134", "'{0}' はクラスです。生成するには '{1}.{2}(...)' と書いてください", n->name, ms->mod->name, n->name);
        else
            d.hint = MSG0("sema.433", "モジュールから使えるのは、そのファイルのトップレベルの関数・クラス・グローバル変数です");
        diag_fail(&d);
    }

    // ★ codegen への記録。ND_FIELD のままだが「グローバル変数の読み書き」になる。
    n->mod_name = ms->mod->name;
    n->ir_name = v->ir_name;
    n->is_global = true;
    n->is_extern = ms != s->cur;
    return v->type;
}

// ★ T | None に '.' で触ろうとしたときの案内。
//   ここが narrowing の入口になる、いちばんよく出るエラーです。
static _Noreturn void reject_opt_access(Node *obj, Node *at, const char *what,
                                        Type *ot) {
    Diag d = {0};
    d.message = MSG2("sema.135", "型 '{0}' の値には{1}がありません", type_name(ot), what);
    d.primary.tok = at->tok;
    d.primary.label = MSG0("sema.434", "None かもしれない値です");
    d.related.tok = obj->tok;
    d.related.label = MSG1("sema.136", "この式は '{0}' 型です", type_name(ot));
    if (obj->kind == ND_VAR)
        d.hint = MSG1("sema.137", "先に None を除いてください:\n             if {0} is not None:\n                 ...", obj->name);
    else
        d.hint = MSG0("sema.435", "一度ローカル変数に入れてから絞り込んでください:\n             x: T | None = ...\n             if x is not None:\n                 ...");
    diag_fail(&d);
}

// rc[T] は「中身のように」使える。
//
// ★ Rust の Deref と同じ考えです。`with ... borrow()` を毎回書かせると、
//   木やグラフを扱うコードが読めなくなります（仕様 §7 の目的が達成できません）。
static Type *auto_deref(Type *t) {
    return t && t->kind == TY_RC ? t->elem : t;
}

// `Color.Red` を、型のついた定数に畳む（A-37）。
//
// ★ **ND_INT に書き換えます。** 値はただの i64 なので、codegen に
//   新しい形を増やす必要がありません。型だけ TY_ENUM にしておけば、
//   `Color.Red + 1` は「int と Color は混ざりません」で止まります。
//
// 戻り値: 畳めたら型、そうでなければ NULL（ふつうのフィールドとして続ける）
// ── 中身を持つ枝を作る（A-41）────────────────────────────
//
// ★ `Shape.Circle(1.5)` を `Shape$Circle(0, 1.5)` に書き換えます
//   （第 1 引数がタグ）。作るのは**ふつうのクラスの生成**なので、
//   確保も解放も所有権の検査も、クラスのための仕組みがそのまま効きます。
//
//   nargs は「利用者が書いた中身の数」。ND_FIELD（中身なしの枝）から
//   呼ぶときは 0 です。
static Type *make_enum_value(Sema *s, Node *n, EnumDef *e, EnumVal *v,
                             Node *args, int nargs) {
    if (nargs != v->nfields) {
        Diag d = {0};
        d.message = MSG4("sema.138", "枝 '{0}.{1}' は {2} 個の中身を取りますが、{3} 個渡されました", e->name, v->name, diag_fmt("%d", v->nfields), diag_fmt("%d", nargs));
        d.primary.tok = n->tok;
        d.primary.label = v->nfields == 0 ? MSG0("sema.436", "この枝は中身を持ちません")
                                          : MSG0("sema.437", "中身の数が違います");
        d.related.tok = v->tok;
        d.related.label = MSG0("sema.438", "枝の定義はここです");
        d.hint = v->nfields == 0
                     ? MSG2("sema.139", "'{0}.{1}' とだけ書きます", e->name, v->name)
                     : MSG3("sema.140", "'{0}.{1}(…)' に {2} 個書きます", e->name, v->name, diag_fmt("%d", v->nfields));
        diag_fail(&d);
    }

    Class *bc = branch_class(s, e, v);

    // タグを先頭に足して、隠しクラスの生成に書き換えます
    Node *tag = new_int_node(n->tok, v->val);
    tag->next = args;

    n->kind = ND_CALL;
    n->name = bc->name;
    n->lhs = NULL;
    n->args = tag;
    n->en = e;
    check_new(s, n, bc);

    // ★ 静的な型は**列挙**です（枝のクラスではありません）。
    //   利用者から見える型は 1 つ、という約束を守ります。
    n->type = e->type;
    return e->type;
}

static Type *fold_enum_value(Sema *s, Node *n) {
    EnumDef *e = NULL;

    // `Color.Red` … 左が変数として宣言されていない名前で、列挙なら
    if (n->lhs->kind == ND_VAR && !lookup(s, n->lhs->name))
        e = lookup_enum(s, n->lhs->name);

    // `mod.Color.Red` … 左がモジュール修飾の列挙なら
    if (!e && n->lhs->kind == ND_FIELD) {
        ModuleSyms *lm = dot_module(s, n->lhs);
        if (lm) e = lookup_enum_in(lm, n->lhs->name);
    }
    if (!e) return NULL;

    EnumVal *v = lookup_enum_val(e, n->name);
    if (!v) {
        Diag d = {0};
        d.message = MSG2("sema.141", "列挙 '{0}' に枝 '{1}' はありません", e->name, n->name);
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.439", "この枝は定義されていません");
        d.related.tok = e->tok;
        d.related.label = MSG0("sema.440", "列挙の定義はここです");
        StrBuf sb;
        sb_init(&sb);
        sb_printf(&sb, "%s", MSG0("sema.142", "書ける枝は "));
        int k = 0;
        for (EnumVal *q = e->vals; q; q = q->next)
            sb_printf(&sb, "%s%s", k++ ? " / " : "", q->name);
        sb_printf(&sb, "%s", MSG0("sema.143", " です"));
        d.hint = sb_str(&sb);
        diag_fail(&d);
    }

    // ★ 中身を持つ列挙なら、枝も**物体**です（A-41）。
    //   表現を枝ごとに変えないので、中身なしの枝も同じ形で作ります。
    if (e->has_payload) return make_enum_value(s, n, e, v, NULL, 0);

    // ★ ここで木を書き換えます（畳み込み）。
    n->kind = ND_INT;
    n->ival = v->val;
    n->lhs = NULL;
    n->type = e->type;
    n->en = e;
    return e->type;
}

static Type *check_field(Sema *s, Node *n) {
    // ★ 列挙の枝はフィールドではありません（A-37）。モジュール解決より
    //   先に見ます——`Color.Red` の `Color` は変数でもモジュールでもない
    //   からです。
    Type *et = fold_enum_value(s, n);
    if (et) return et;

    ModuleSyms *ms = dot_module(s, n);
    if (ms) return check_module_global(s, n, ms);

    Type *ot = auto_deref(check_expr(s, n->lhs));  // rc[T] は中身のように使える

    if (ot->kind == TY_OPT) reject_opt_access(n->lhs, n, MSG0("sema.441", "フィールド"), ot);

    if (ot->kind != TY_CLASS) {
        Diag d = {0};
        d.message = MSG1("sema.144", "型 '{0}' にフィールドはありません", type_name(ot));
        d.primary.tok = n->lhs->tok;
        d.primary.label = MSG1("sema.051", "これは '{0}' 型です", type_name(ot));
        d.hint = MSG0("sema.442", "'.' でフィールドを読めるのは class のインスタンスだけです");
        diag_fail(&d);
    }

    Field *f = lookup_field(ot->cls, n->name);
    if (!f) {
        Diag d = {0};
        d.message = MSG2("sema.145", "クラス '{0}' にフィールド '{1}' はありません", ot->cls->name, n->name);
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.443", "このフィールドは宣言されていません");
        d.related.tok = ot->cls->tok;
        d.related.label = MSG0("sema.444", "クラスの定義はここです");
        d.hint = MSG0("sema.445", "クラス本体の先頭に「名前: 型」の形で宣言してください");
        diag_fail(&d);
    }

    n->field = f;  // ★ codegen はこれ（の index）を getelementptr に渡す
    return f->type;
}

// クラスのメソッド呼び出しの検査（型システム 5.10）。
//
// ★ 「関数呼び出しの検査に self を 1 個足すだけ」です。
//   名前を修飾して関数表に載せておいたので、引ける表はそのまま。
static void check_can_fail(Sema *s, Node *n, FuncSig *f, const char *shown);
static void reject_raising_fn(Node *at, Type *ft, const char *where);

// 実引数に「own の仮引数へ渡す rc[T] か」を書き写す（A-21 ⑬）。
//
// ★ pi は仮引数の番号です。メソッド・生成は self があるので 1 から始まります。
//
// 注意: **rc[T] を own の仮引数へ渡すときは、呼び出し側が参照を 1 つ増やします**
//   （ast.h の arg_own_rc）。rc[T] は移動しないので、増やさないと
//   受け取った側の「出口で手放す」だけが残り、二重解放になります。
//
// 注意: ここで arg_is_borrowed は**触りません**。あちらは「呼び出しのあとで
//   一時値を解放してよいか」の旗で、既定（false＝解放しない）が安全側です。
//   メソッドと生成では立てないままにしてあります（漏れるが、壊れない）。
static void mark_arg_own_rc(Node *a, FuncSig *f, int pi) {
    if (!f->pmodes) return;
    a->arg_own_rc = f->pmodes[pi] == PM_OWN && ty_is_rc(a->type);
}

static Type *check_class_method(Sema *s, Node *n, Class *c) {
    // ★ メソッドは「クラスが定義されたモジュール」の表にいます。
    //   自分のモジュールの表を引くと、import したクラスのメソッドが見つかりません。
    char *mname = mangle(c->name, n->name);
    FuncSig *f = lookup_func_in(c->owner, mname);
    if (!f) {
        Diag d = {0};
        d.message = MSG2("sema.146", "クラス '{0}' にメソッド '{1}' はありません", c->name, n->name);
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.446", "このメソッドは定義されていません");
        d.related.tok = c->tok;
        d.related.label = MSG0("sema.444", "クラスの定義はここです");
        if (lookup_field(c, n->name))
            d.hint = MSG1("sema.147", "'{0}' はフィールドです。'()' を外してください", n->name);
        diag_fail(&d);
    }

    // ★ drop は自分で呼べません（仕様 §6.2）。呼べると、解放のときにもう一度
    //   呼ばれます（E-DROP-2。0.45.0 までは呼べていました）。
    if (strcmp(n->name, "drop") == 0) {
        Diag d = {0};
        d.code = "E-DROP-2";
        d.message = MSG0("sema.447", "drop は自分で呼べません（解放のときに自動で呼ばれます）");
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.448", "ここで呼ぶと、解放のときにもう一度呼ばれます");
        d.hint = MSG0("sema.449", "早く後始末をしたいときは、別の名前のメソッド（close など）に分けてください（仕様 §6.2）");
        diag_fail(&d);
    }

    // ★ 並べ替えと既定値の穴埋め（A-38）。self のぶん 1 つ飛ばします。
    bind_args_sig(n, f, 1, MSG1("sema.148", "メソッド '{0}'", mname));

    // 引数の個数（self は数えない）
    int nargs = 0;
    for (Node *a = n->args; a; a = a->next) nargs++;
    if (nargs != f->nparams - 1) {
        Diag d = {0};
        d.message = MSG3("sema.149", "メソッド '{0}' は {1} 個の引数を取りますが、{2} 個渡されました", mname, diag_fmt("%d", f->nparams - 1), diag_fmt("%d", nargs));
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.450", "呼び出しの引数の個数が違います");
        d.related.tok = f->tok;
        d.related.label = MSG0("sema.451", "このメソッドはここで定義されています");
        d.hint = MSG0("sema.452", "self は自動的に渡されるので、書く必要はありません");
        diag_fail(&d);
    }

    // ★ 第 1 引数は self なので、実引数は params[i + 1] と比べます
    int i = 0;
    for (Node *a = n->args; a; a = a->next, i++) {
        s->expected = f->params[i + 1];
        Type *at = check_expr(s, a);
        s->expected = NULL;
        // ★ 呼び先がしまうなら、捕獲した lambda は渡せません（A-43）
        // 注意: 定義の木が無いときは「しまう」と答えます（安全側）
        if (a->caps && (!f->node ||
                        fn_param_escapes_at(s, f->node->body, f->pnames[i + 1], 0)))
            reject_escaping_closure(a, MSG0("sema.410", "しまうことが"));
        mark_arg_own_rc(a, f, i + 1);
        if (!type_assignable(at, f->params[i + 1])) {
            Diag d = {0};
            d.message = MSG4("sema.150", "メソッド '{0}' の第 {1} 引数: 型 '{2}' を '{3}' に渡せません", mname, diag_fmt("%d", i + 1), type_name(at), type_name(f->params[i + 1]));
            d.primary.tok = a->tok;
            d.primary.label = MSG1("sema.051", "これは '{0}' 型です", type_name(at));
            d.related.tok = f->tok;
            d.related.label = MSG2("sema.151", "引数 '{0}' は '{1}' 型です", f->pnames[i + 1], type_name(f->params[i + 1]));
            d.hint = no_implicit_hint(at, f->params[i + 1]);
            diag_fail(&d);
        }
    }

    // ★ codegen が呼ぶ関数名（@lexer.Token.show）。
    //   import したクラスのメソッドなら declare も要る。
    n->ir_name = f->ir_name;
    n->is_extern = f->owner != s->cur;
    check_can_fail(s, n, f, mname);
    return f->ret;
}

// メソッド呼び出しの検査（list.append と、クラスのメソッド）
// lexer.make(1, "x") / lexer.Token(1, "x") — 他のモジュールの関数・クラス
static Type *check_new(Sema *s, Node *n, Class *c);
static Type *check_call_sig(Sema *s, Node *n, FuncSig *f, const char *what);

static Type *check_module_call(Sema *s, Node *n, ModuleSyms *ms) {
    n->mod_name = ms->mod->name;

    // ★ 名前がクラスなら、これはインスタンス生成（その分岐がそのまま）
    Class *c = lookup_class_in(ms, n->name);
    if (c) {
        n->is_extern = ms != s->cur;  // codegen が init を declare する判断
        return check_new(s, n, c);
    }

    FuncSig *f = lookup_func_in(ms, n->name);

    // ★ 他のモジュールのジェネリック関数も実体化します。
    //   注意: 実体はテンプレートのモジュールに作られるので、呼ぶ側から見ると
    //     ふつうの「他モジュールの関数」になります。
    if (f && f->tmpl) f = instantiate_func(s, f, n);

    if (!f) {
        Diag d = {0};
        d.message = MSG2("sema.152", "モジュール '{0}' に関数 '{1}' はありません", ms->mod->name, n->name);
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.453", "この関数は定義されていません");
        d.hint = lookup_global_in(ms, n->name)
                     ? MSG1("sema.153", "'{0}' はグローバル変数です。'()' を外してください", n->name)
                     : MSG0("sema.454", "そのモジュールのトップレベルに def があるか確認してください");
        diag_fail(&d);
    }

    n->ir_name = f->ir_name;
    n->is_extern = f->owner != s->cur;
    return check_call_sig(s, n, f, MSG0("sema.455", "関数"));
}


// ── scope: から途中で抜けることを禁じる ─────────────────
//
// なぜ禁じるのか
//   scope: の出口には「まだ待っていないスレッドを全部 join する」という
//   仕事があります。**途中から抜ける道があると、その仕事が飛びます。**
//   飛ぶと、まだ走っているスレッドが借りている値の寿命が切れます
//   — つまり scoped spawn の根拠そのものが崩れます。
//
// 注意: ループの中の break / continue は、そのループが scope: の**中**に
//    あるなら問題ありません（外へは出ないため）。
static void scope_escape(Sema *s, Node *n, int loop_depth, int try_depth) {
    for (; n; n = n->next) {
        const char *what = NULL;
        switch (n->kind) {
            case ND_RETURN: what = "return"; break;
            case ND_RAISE:  what = "raise"; break;
            case ND_BREAK:
                if (loop_depth == 0) what = "break";
                break;
            case ND_CONTINUE:
                if (loop_depth == 0) what = "continue";
                break;
            case ND_CALL:
            case ND_METHOD:
                // 注意: 失敗しうる呼び出しは、捕まえないと外へ飛びます。
                if (n->can_fail && try_depth == 0) what = MSG0("sema.456", "失敗しうる呼び出し");
                break;
            default: break;
        }
        if (what) {
            Diag d = {0};
            d.code = "E-SCOPE-1";
            d.message = MSG1("sema.154", "scope: の中から '{0}' で抜けることはできません", what);
            d.primary.tok = n->tok;
            d.primary.label = MSG0("sema.457", "ここで scope: の外へ出ようとしています");
            d.hint = MSG0("sema.458", "scope: の出口では、始めたスレッドを全部 join します。途中で抜けるとそれが飛ぶので、結果を変数に受けてからブロックを出てください");
            diag_fail(&d);
        }

        int ld = loop_depth + (n->kind == ND_WHILE ? 1 : 0);
        int td = try_depth + (n->kind == ND_TRY ? 1 : 0);
        // 注意: except 節の中は try の外です（そこで失敗すればさらに外へ飛ぶ）。
        scope_escape(s, n->lhs, ld, td);
        scope_escape(s, n->rhs, ld, td);
        scope_escape(s, n->incr, ld, td);
        scope_escape(s, n->body, ld, td);
        scope_escape(s, n->args, ld, td);
        scope_escape(s, n->els, ld, n->kind == ND_TRY ? try_depth : td);
    }
}

// ── 複製できる型か（A-44）────────────────────────────────
//
// ★ 「複製できる」は型ごとに意味が違います。
//     値型（int / float / bool / 列挙）… そのまま（写すものがありません）
//     str                              … 新しい文字列
//     list[T]                          … 新しい list（要素も T の複製）
//     クラス                            … `__copy__` を書いたものだけ
//     T | None                         … None はそのまま、中身は複製
//     中身を持つ列挙                     … 枝ごとに中身を複製（自動）
//
// 注意: **rc[T] は複製しません。** あれは「1 つの値を 2 か所から持つ」ための
//   型で、複製したい相手ではありません（複製したいなら中身を copy します）。
static void check_copyable(Sema *s, Type *t, Token *at) {
    switch (t->kind) {
        case TY_INT:
        case TY_FLOAT:
        case TY_BOOL:
        case TY_STR: return;
        case TY_ENUM: return;                    // 枝ごとの複製は codegen が出します
        case TY_LIST: check_copyable(s, t->elem, at); return;
        case TY_OPT:  check_copyable(s, t->elem, at); return;
        case TY_CLASS: {
            Class *c = t->cls;
            FuncSig *f = lookup_func_in(c->owner, mangle(c->name, "__copy__"));
            if (f && f->nparams == 1 && type_equal(f->ret, t)) return;
            Diag d = {0};
            d.message = MSG1("sema.155", "クラス '{0}' は copy できません", c->name);
            d.primary.tok = at;
            d.primary.label = MSG1("sema.156", "'{0}' 型です", type_name(t));
            d.related.tok = c->tok;
            d.related.label = MSG0("sema.444", "クラスの定義はここです");
            d.hint = f ? MSG1("sema.157", "'__copy__' は 'def __copy__(self) -> {0}:' の形で書きます", c->name)
                       : MSG2("sema.158", "複製できるようにするには __copy__ を書きます:\n             def __copy__(self) -> {0}:\n                 return {1}(…)", c->name, c->name);
            diag_fail(&d);
        }
        default: break;
    }
    Diag d = {0};
    d.message = MSG1("sema.159", "'{0}' 型は copy できません", type_name(t));
    d.primary.tok = at;
    d.primary.label = MSG0("sema.459", "ここは複製できる型ではありません");
    d.hint = t->kind == TY_RC
                 ? MSG0("sema.460", "rc[T] は「1 つの値を 2 か所から持つ」ための型です（複製ではありません）。中身を複製するなら copy(r.get()) のように中身を渡してください")
                 : MSG0("sema.461", "複製できるのは 値型 / str / list / __copy__ を持つクラス / 列挙 と、それらの T | None です");
    diag_fail(&d);
}

// ── 中身を持つ枝の隠しクラスを引く（A-41）────────────────
//
// ★ **遅れて引きます。** 列挙はクラスより先に登録する（クラスの
//   フィールドに列挙を書けるようにするため）ので、列挙を登録する時点では
//   枝のクラスがまだありません。最初に要ったときに引いて覚えます。
static Class *branch_class(Sema *s, EnumDef *e, EnumVal *ev) {
    if (ev->cls) return ev->cls;
    ev->cls = lookup_class_in(e->owner, diag_fmt("%s$%s", e->name, ev->name));
    if (!ev->cls) UNREACHABLE();   // parser が必ず作ります
    return ev->cls;
}

// ── 名前で引数を渡せない呼び出しを断る（A-38）──
//
// ★ 名前で渡せるのは、**呼び先がソースから 1 つに決まる**ときだけです
//   （この言語で定義した関数・メソッド・生成）。組み込み・関数の値・
//   インタフェース越しの呼び出しには、引数の名前がありません。
//
// 注意: **黙って無視してはいけません。** 名前を書いた人は「その引数に
//   渡した」と思っているので、位置で渡ったまま通すと、静かに違う引数へ
//   入ります。
static void reject_kwargs(Node *args, const char *message, const char *why) {
    for (Node *a = args; a; a = a->next) {
        if (!a->arg_name) continue;
        Diag d = {0};
        d.message = message;
        d.primary.tok = a->arg_name_tok;
        d.primary.label = MSG1("sema.160", "'{0} = …' と書いています", a->arg_name);
        d.hint = why;
        diag_fail(&d);
    }
}

static Type *check_method(Sema *s, Node *n) {
    // ★ 中身を持つ枝の生成（A-41）。`Shape.Circle(1.5)` はメソッド呼び出しに
    //   見えますが、**枝を作る式**です。左が変数でない名前で、それが列挙なら
    //   こちらに来ます（モジュール修飾は dot_module の後ろで見ます）。
    if (n->lhs && n->lhs->kind == ND_VAR && !lookup(s, n->lhs->name)) {
        EnumDef *e = lookup_enum(s, n->lhs->name);
        if (e) {
            EnumVal *v = lookup_enum_val(e, n->name);
            if (!v) {
                Diag d = {0};
                d.message = MSG2("sema.141", "列挙 '{0}' に枝 '{1}' はありません", e->name, n->name);
                d.primary.tok = n->tok;
                d.primary.label = MSG0("sema.439", "この枝は定義されていません");
                d.related.tok = e->tok;
                d.related.label = MSG0("sema.440", "列挙の定義はここです");
                diag_fail(&d);
            }
            if (!e->has_payload) {
                Diag d = {0};
                d.message = MSG2("sema.161", "枝 '{0}.{1}' は中身を持ちません", e->name, v->name);
                d.primary.tok = n->tok;
                d.primary.label = MSG0("sema.462", "ここに '(' は書けません");
                d.related.tok = v->tok;
                d.related.label = MSG0("sema.438", "枝の定義はここです");
                d.hint = MSG2("sema.139", "'{0}.{1}' とだけ書きます", e->name, v->name);
                diag_fail(&d);
            }
            int nargs = 0;
            for (Node *a = n->args; a; a = a->next) nargs++;
            return make_enum_value(s, n, e, v, n->args, nargs);
        }
    }

    ModuleSyms *ms = dot_module(s, n);
    if (ms) return check_module_call(s, n, ms);

    Type *ot = auto_deref(check_expr(s, n->lhs));
    if (ot->kind == TY_OPT) reject_opt_access(n->lhs, n, MSG0("sema.463", "メソッド"), ot);

    if (ot->kind == TY_CLASS) return check_class_method(s, n, ot->cls);

    // ★ ここから先は、引数の名前を持たない呼び先です（A-38）。
    //   インタフェース越しは**実行時に実装が決まる**ので、名前も既定値も
    //   使えません（どの実装の名前を見ればよいか決まらないため）。
    reject_kwargs(n->args,
                  ot->kind == TY_IFACE
                      ? MSG0("sema.464", "インタフェース越しの呼び出しでは、名前で引数を渡せません")
                      : MSG1("sema.162", "'{0}' は名前で引数を受け取りません", n->name),
                  ot->kind == TY_IFACE
                      ? MSG0("sema.465", "どの実装が呼ばれるかは実行時に決まります。名前と既定値が使えるのは、クラスの型が分かっているときだけです")
                      : MSG0("sema.466", "名前で渡せるのは、この言語で定義した関数・メソッド・生成だけです"));

    // ★ インタフェース越しの呼び出し。
    //   どの実装が呼ばれるかは **実行時に**決まります（vtable を引く）。
    if (ot->kind == TY_IFACE) {
        Iface *ifc = ot->iface;
        IMethod *im = NULL;
        for (IMethod *q = ifc->methods; q; q = q->next)
            if (strcmp(q->name, n->name) == 0) { im = q; break; }
        if (!im) {
            Diag d = {0};
            d.message = MSG2("sema.163", "インタフェース '{0}' に '{1}' はありません", ifc->name, n->name);
            d.primary.tok = n->tok;
            d.primary.label = MSG0("sema.467", "このメソッドは宣言されていません");
            d.related.tok = ifc->tok;
            d.related.label = MSG0("sema.468", "インタフェースの定義はここです");
            d.hint = MSG0("sema.469", "インタフェース越しに呼べるのは、そこに宣言したものだけです");
            diag_fail(&d);
        }

        Node *sig = im->sig;
        int want = 0;
        for (Node *pm = sig->params; pm; pm = pm->next) want++;
        want--;                       // self は数えない
        int nargs = 0;
        for (Node *a = n->args; a; a = a->next) nargs++;
        if (nargs != want)
            error_at_hint_m(n->tok, MSG3("sema.165", "'{0}.{1}' は {2} 個の引数を取ります", ifc->name, n->name, diag_fmt("%d", want)), MSG1("sema.164", "引数の個数が違います（{0} 個渡されました）", diag_fmt("%d", nargs)));

        int k = 1;
        Node *pm = sig->params->next;
        for (Node *a = n->args; a; a = a->next, pm = pm->next, k++) {
            Type *wt = resolve_type(s, pm->type_ref);
            s->expected = wt;
            Type *at = check_expr(s, a);
            s->expected = NULL;
            if (!type_assignable(at, wt))
                error_at_hint_m(a->tok, MSG1("sema.064", "ここには '{0}' が必要です", type_name(wt)), MSG2("sema.166", "{0} 番目の引数が '{1}' 型です", diag_fmt("%d", k), type_name(at)));
        }

        n->iface_slot = im->slot;
        n->is_iface_call = true;

        // ★ 失敗しうるメソッド（A-40）。**宣言に書いてある raises** で見ます。
        //   どの実装が呼ばれるかは実行時に決まりますが、**投げうるエラーの
        //   集合は宣言で固定**してあるので（check_implements が実装に同じ
        //   raises を求めます）、呼ぶ側はここで決められます。
        if (sig->raises) {
            FuncSig tmp = {0};
            tmp.name = n->name;
            tmp.tok = sig->tok;
            resolve_raises(s, sig, &tmp);
            check_can_fail(s, n, &tmp,
                           diag_fmt("%s.%s", ifc->name, n->name));
        }

        return resolve_type(s, sig->type_ref);
    }

    // ── weak[T].upgrade() — 中身が生きていれば rc[T]、無ければ None（A-50）──
    if (ot->kind == TY_WEAK) {
        if (strcmp(n->name, "upgrade") != 0)
            error_at_hint_m(n->tok, MSG0("sema.636", "weak にあるのは upgrade() だけです（中身を使うには upgrade() で rc[T] に戻します）"), MSG1("sema.635", "'weak' に '{0}' はありません", n->name));
        if (n->args)
            error_at_hint_m(n->tok, MSG0("sema.638", "w.upgrade() の形で使ってください"), MSG0("sema.637", "upgrade は引数を取りません"));
        return type_opt(type_rc(ot->elem));
    }

    // ── Thread[R].join() — 終わるまで待って戻り値を受け取る ──
    //
    // 注意: join は所有を消費します（2 回 join できません）。それを保証するのは
    //    ownck 側です（Thread[R] は移動する型として扱われます）。
    if (ot->kind == TY_THREAD) {
        if (strcmp(n->name, "join") != 0)
            error_at_hint_m(n->tok, MSG0("sema.168", "Thread にあるのは join() だけです"), MSG1("sema.167", "'Thread' に '{0}' はありません", n->name));
        if (n->args)
            error_at_hint_m(n->tok, MSG0("sema.170", "t.join() の形で使ってください"), MSG0("sema.169", "join は引数を取りません"));
        return ot->elem;
    }

    // ── mutex[T].lock(f) — ロックを取り、f(中身) を呼び、必ず解く ──
    //
    // ★ 生の lock / unlock は**出しません**。解き忘れが起きえない形に
    //   閉じ込めます（設計文書 §4）。
    if (ot->kind == TY_MUTEX) {
        if (strcmp(n->name, "lock") != 0)
            error_at_hint_m(n->tok, MSG0("sema.172", "mutex にあるのは lock(関数) だけです"), MSG1("sema.171", "'mutex' に '{0}' はありません", n->name));
        int nargs = 0;
        for (Node *a = n->args; a; a = a->next) nargs++;
        if (nargs != 1)
            error_at_hint_m(n->tok, MSG0("sema.174", "m.lock(関数) の形で使ってください"), MSG1("sema.173", "lock は 1 個の引数（関数）を取ります（{0} 個渡されました）", diag_fmt("%d", nargs)));
        Type *ft = check_expr(s, n->args);
        if (ft->kind != TY_FN || ft->nparams != 1)
            error_at_hint_m(n->args->tok, MSG1("sema.176", "lock には 'fn({0}) -> R' の関数を渡してください", type_name(ot->elem)), MSG1("sema.175", "'{0}' はその形の関数ではありません", type_name(ft)));
        reject_raising_fn(n->args, ft, "lock");
        if (!type_assignable(ot->elem, ft->params[0]))
            error_at_hint_m(n->args->tok, MSG1("sema.178", "中身は '{0}' です", type_name(ot->elem)), MSG1("sema.177", "この関数は '{0}' を受け取ります", type_name(ft->params[0])));
        return ft->elem;
    }

    // ★ list のメソッドを増やしました。
    //   引数と戻り型は「表」で持ちます。1 つずつ if を書くと、増やすたびに
    //   同じ形のコードが並ぶためです。
    //     argk: 'e'=要素型 / 'i'=int / 'l'=同じ list / '-'=引数なし
    //     retk: 'e'=要素型 / 'i'=int / 'n'=None / 'l'=同じ list
    if (ot->kind == TY_LIST) {
        static const struct { const char *name; char argk; char retk;
                              const char *ukey; const char *usage; } LM[] = {
            {"pop",     '-', 'e', NULL, "xs.pop()"},
            {"insert",  'i', 'n', MSGK("sema.470", "xs.insert(位置, 値)")},   // 引数 2 個（下で特別扱い）
            {"remove",  'i', 'e', MSGK("sema.471", "xs.remove(位置)")},
            {"index",   'e', 'i', MSGK("sema.472", "xs.index(値)")},
            {"reverse", '-', 'n', NULL, "xs.reverse()"},
            {"clear",   '-', 'n', NULL, "xs.clear()"},
            {"copy",    '-', 'l', NULL, "xs.copy()"},
            {"extend",  'l', 'n', MSGK("sema.473", "xs.extend(別のリスト)")},
            {NULL, 0, 0, NULL, NULL},
        };
        for (int i = 0; LM[i].name; i++) {
            if (strcmp(n->name, LM[i].name) != 0) continue;

            int want = LM[i].argk == '-' ? 0 : 1;
            if (strcmp(n->name, "insert") == 0) want = 2;
            int nargs = 0;
            for (Node *a = n->args; a; a = a->next) nargs++;
            if (nargs != want) {
                Diag d = {0};
                d.message = MSG3("sema.179", "{0} は {1} 個の引数を取りますが、{2} 個渡されました", n->name, diag_fmt("%d", want), diag_fmt("%d", nargs));
                d.primary.tok = n->tok;
                d.primary.label = MSG0("sema.197", "引数の個数が違います");
                d.hint = MSG1("sema.180", "{0} の形で使ってください", msgv(LM[i].ukey, LM[i].usage, NULL, 0));
                diag_fail(&d);
            }

            // 引数の型検査
            if (strcmp(n->name, "insert") == 0) {
                Type *a0 = check_expr(s, n->args);
                if (a0->kind != TY_INT)
                    error_at_m(n->args->tok, MSG1("sema.181", "insert の位置は int です（'{0}' 型でした）", type_name(a0)));
                s->expected = ot->elem;
                Type *a1 = check_expr(s, n->args->next);
                s->expected = NULL;
                if (!type_assignable(a1, ot->elem))
                    error_at_m(n->args->next->tok, MSG2("sema.182", "'{0}' のリストに '{1}' は入れられません", type_name(ot->elem), type_name(a1)));
            } else if (LM[i].argk == 'i') {
                Type *a0 = check_expr(s, n->args);
                if (a0->kind != TY_INT)
                    error_at_m(n->args->tok, MSG2("sema.183", "{0} の引数は int です（'{1}' 型でした）", n->name, type_name(a0)));
            } else if (LM[i].argk == 'e') {
                s->expected = ot->elem;
                Type *a0 = check_expr(s, n->args);
                s->expected = NULL;
                if (!type_assignable(a0, ot->elem))
                    error_at_m(n->args->tok, MSG2("sema.184", "'{0}' のリストから '{1}' は探せません", type_name(ot->elem), type_name(a0)));
            } else if (LM[i].argk == 'l') {
                Type *a0 = check_expr(s, n->args);
                if (!type_assignable(a0, ot))
                    error_at_m(n->args->tok, MSG1("sema.185", "extend には同じ型のリストが必要です（'{0}' でした）", type_name(a0)));
            }

            if (LM[i].retk == 'e') return ot->elem;
            if (LM[i].retk == 'i') return ty_int;
            if (LM[i].retk == 'l') return ot;
            return ty_none;
        }
    }

    if (ot->kind == TY_LIST && strcmp(n->name, "append") == 0) {
        int nargs = 0;
        for (Node *a = n->args; a; a = a->next) nargs++;
        if (nargs != 1) {
            Diag d = {0};
            d.message = MSG1("sema.186", "append は 1 個の引数を取りますが、{0} 個渡されました", diag_fmt("%d", nargs));
            d.primary.tok = n->tok;
            d.primary.label = MSG0("sema.197", "引数の個数が違います");
            d.hint = MSG0("sema.474", "xs.append(値) の形で使ってください");
            diag_fail(&d);
        }

        s->expected = ot->elem;
        Type *at = check_expr(s, n->args);
        s->expected = NULL;

        // 注意: ここでも代入互換の検査。list[list[int]] に list[str] を
        //    append するのを弾くには、要素型の再帰比較が要ります。
        if (!type_assignable(at, ot->elem)) {
            Diag d = {0};
            d.message = MSG2("sema.187", "'{0}' のリストに '{1}' を追加できません", type_name(ot->elem), type_name(at));
            d.primary.tok = n->args->tok;
            d.primary.label = MSG1("sema.051", "これは '{0}' 型です", type_name(at));
            d.hint = no_implicit_hint(at, ot->elem);
            diag_fail(&d);
        }

        n->args = range_coerce(s, n->args, ot->elem);   // A-28
        return ty_none;
    }

    Diag d = {0};
    d.message = MSG2("sema.188", "型 '{0}' にメソッド '{1}' はありません", type_name(ot), n->name);
    d.primary.tok = n->tok;
    d.primary.label = MSG0("sema.475", "このメソッドは存在しません");
    d.hint = MSG0("sema.476", "list[T] で使えるのは append / pop / insert / remove / index / reverse / clear / copy / extend です（class のメソッドは自分で定義できます）");
    diag_fail(&d);
}

// インスタンス生成 Token(1, "x") の検査。
//
// ★ 構文上はただの関数呼び出し（ND_CALL）です。名前解決の段階で分岐します。
//   「どう扱うか」の判断をここで終わらせ、codegen には n->cls という
//   記録を残すだけ。n->builtin とまったく同じ形です。
static Type *check_new(Sema *s, Node *n, Class *c) {
    // ★ ジェネリッククラスの生成。**どの実体を作るのかは
    //   「代入される先の型」から決めます**（設計 §1：推論は左辺からだけ）。
    //
    //     d: Dict[str, int] = Dict()
    //                         ^^^^^^ ここには型引数を書きません
    //
    // 注意: 左辺が無い場所（式の途中など）では決められないので、その旨を伝えます。
    if (c->node && c->node->targs) {
        Type *want = s->expected;
        if (!want || want->kind != TY_CLASS ||
            !want->cls->from_template ||
            want->cls->from_template != c) {
            Diag d = {0};
            d.message = MSG1("sema.189", "'{0}' のどの実体を作るのか決められません", c->name);
            d.primary.tok = n->tok;
            d.primary.label = MSG0("sema.477", "型引数が決まりません");
            d.related.tok = c->tok;
            d.related.label = MSG0("sema.478", "このクラスは型引数を取ります");
            d.hint = MSG2("sema.190", "変数の型から決めます。'x: {0}[型, ...] = {1}(...)' の形で書いてください", c->name, c->name);
            diag_fail(&d);
        }
        c = want->cls;   // ★ 以降は実体を相手にします
    }

    n->cls = c;  // ★ codegen はこれを見て「生成」だと分かる

    int nargs = 0;
    for (Node *a = n->args; a; a = a->next) nargs++;

    // init が無いクラスは、引数なしでしか作れない
    if (!c->has_init) {
        if (nargs != 0) {
            Diag d = {0};
            d.message = MSG1("sema.191", "クラス '{0}' には init が無いので引数を渡せません", c->name);
            d.primary.tok = n->tok;
            d.primary.label = MSG1("sema.192", "{0} 個の引数が渡されています", diag_fmt("%d", nargs));
            d.related.tok = c->tok;
            d.related.label = MSG0("sema.444", "クラスの定義はここです");
            d.hint = MSG0("sema.479", "引数を受け取るには init メソッドを定義してください:\n             def init(self, ...) -> None:");
            diag_fail(&d);
        }
        return c->type;
    }

    // init があるなら、その引数と突き合わせる（self は飛ばす）
    FuncSig *f = lookup_func_in(c->owner, mangle(c->name, "init"));

    // ★ 並べ替えと既定値の穴埋め（A-38）。self のぶん 1 つ飛ばします。
    bind_args_sig(n, f, 1, MSG1("sema.193", "クラス '{0}'", c->name));
    nargs = 0;
    for (Node *a = n->args; a; a = a->next) nargs++;

    if (nargs != f->nparams - 1) {
        Diag d = {0};
        d.message = MSG3("sema.194", "'{0}' の生成には {1} 個の引数が必要ですが、{2} 個渡されました", c->name, diag_fmt("%d", f->nparams - 1), diag_fmt("%d", nargs));
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.197", "引数の個数が違います");
        d.related.tok = f->tok;
        d.related.label = MSG0("sema.480", "init はここで定義されています");
        d.hint = MSG0("sema.452", "self は自動的に渡されるので、書く必要はありません");
        diag_fail(&d);
    }

    int i = 0;
    for (Node *a = n->args; a; a = a->next, i++) {
        s->expected = f->params[i + 1];
        Type *at = check_expr(s, a);
        s->expected = NULL;
        // ★ 生成はふつう「しまう」ので、捕獲した lambda は渡せません（A-43）
        // 注意: 定義の木が無いときは「しまう」と答えます（安全側）
        if (a->caps && (!f->node ||
                        fn_param_escapes_at(s, f->node->body, f->pnames[i + 1], 0)))
            reject_escaping_closure(a, MSG0("sema.410", "しまうことが"));
        mark_arg_own_rc(a, f, i + 1);
        if (!type_assignable(at, f->params[i + 1])) {
            Diag d = {0};
            d.message = MSG4("sema.195", "'{0}' の生成の第 {1} 引数: 型 '{2}' を '{3}' に渡せません", c->name, diag_fmt("%d", i + 1), type_name(at), type_name(f->params[i + 1]));
            d.primary.tok = a->tok;
            d.primary.label = MSG1("sema.051", "これは '{0}' 型です", type_name(at));
            d.related.tok = f->tok;
            d.related.label = MSG2("sema.151", "引数 '{0}' は '{1}' 型です", f->pnames[i + 1], type_name(f->params[i + 1]));
            d.hint = no_implicit_hint(at, f->params[i + 1]);
            diag_fail(&d);
        }
    }
    return c->type;
}

// 関数呼び出しの検査（docs/ja/spec/type-system.md 5.7 の順序に従う）
// ── 低レベルの組み込み ──────────────────────────────────
//
// ★ 引数の型が「表」で書けない（ptr[int] を取る／返す）ので、
//   rc(x) と同じくここで特別扱いします。
//
//   ptr_at(addr)        番地からポインタを作る
//   addr_of(p)          ポインタを番地に戻す
//   peek8/16/32/64(p, i)   読む（volatile。符号なしで読んで int にする）
//   poke8/16/32/64(p,i,v)  書く（volatile。下位のビットだけを書く）
//   ★ i は「その幅の何個目か」です（peek32(p, 1) は p + 4 バイト目）。
typedef struct {
    const char *name;
    int nargs;   // ポインタを除く引数の数（ptr_at は 0 で特別）
    bool ret_ptr;
    bool takes_ptr;
} LowLevel;

static const LowLevel LOWLEVEL[] = {
    // ── インラインアセンブリ ──
    //   asm(text)          … 命令を並べるだけ（wfi など）
    //   asm_in(text, v)     … 値を 1 つ渡す（%0 に入る。csrw など）
    //   asm_out(text)       … 値を 1 つ受け取る（%0 に入る。csrr など）
    {"asm", 1, false, false},
    {"asm_in", 2, false, false},
    {"asm_out", 1, false, false},

    {"ptr_at", 1, true, false},
    {"addr_of", 1, false, true},
    {"peek8", 2, false, true},
    {"peek16", 2, false, true},
    {"peek32", 2, false, true},
    {"peek64", 2, false, true},
    {"poke8", 3, false, true},
    {"poke16", 3, false, true},
    {"poke32", 3, false, true},
    {"poke64", 3, false, true},
    {NULL, 0, false, false},
};

static const LowLevel *lowlevel_of(const char *name) {
    for (int i = 0; LOWLEVEL[i].name; i++)
        if (strcmp(LOWLEVEL[i].name, name) == 0) return &LOWLEVEL[i];
    return NULL;
}

bool is_lowlevel_name(const char *name) { return lowlevel_of(name) != NULL; }

static Type *check_lowlevel_call(Sema *s, Node *n, const LowLevel *ll) {
    // ★ unsafe: の外では触れません（仕様 §10.1）
    if (s->unsafe_depth == 0) {
        Diag d = {0};
        d.code = "E-UNSAFE-1";
        d.message = MSG1("sema.196", "'{0}' は unsafe: の中でしか使えません", n->name);
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.481", "生ポインタを触っています");
        d.hint = MSG0("sema.482", "unsafe: ブロックで囲んでください:\n             unsafe:\n                 poke8(p, 0, 65)");
        diag_fail(&d);
    }

    int nargs = 0;
    for (Node *a = n->args; a; a = a->next) nargs++;
    if (nargs != ll->nargs)
        error_at_hint_m(n->tok, MSG2("sema.198", "{0} は {1} 個の引数を取ります", n->name, diag_fmt("%d", ll->nargs)), MSG0("sema.197", "引数の個数が違います"));

    // ★ asm 系は第 1 引数が「文字列リテラル」（実行時に組み立てられては困る）
    bool is_asm = strncmp(n->name, "asm", 3) == 0;
    if (is_asm && (!n->args || n->args->kind != ND_STR))
        error_at_hint_m(n->tok, MSG0("sema.200", "命令はリテラルで書いてください（例: asm(\"wfi\")）"), MSG0("sema.199", "asm の第 1 引数は文字列リテラルです"));

    int i = 0;
    for (Node *a = n->args; a; a = a->next, i++) {
        if (is_asm && i == 0) {
            check_expr(s, a);
            continue;
        }
        Type *at = check_expr(s, a);
        bool want_ptr = ll->takes_ptr && i == 0;
        if (want_ptr && at->kind != TY_PTR)
            error_at_hint_m(a->tok, MSG0("sema.202", "第 1 引数には ptr[int] を渡してください"), MSG1("sema.201", "'{0}' はポインタではありません", type_name(at)));
        if (!want_ptr && at->kind != TY_INT)
            error_at_hint_m(a->tok, MSG0("sema.204", "低レベルの操作が扱うのは int だけです"), MSG1("sema.203", "'{0}' はここに渡せません", type_name(at)));
    }

    n->builtin = NULL;
    if (ll->ret_ptr) return type_ptr(ty_int);
    if (strncmp(n->name, "poke", 4) == 0) return ty_none;
    if (strcmp(n->name, "asm") == 0 || strcmp(n->name, "asm_in") == 0) return ty_none;
    return ty_int;
}

// ★ 失敗しうる関数の値は、別の場所で呼ばれる先（spawn / lock）へ渡せません（A-49）。
//   そこにはエラーを受け止める相手がいないためです。
static void reject_raising_fn(Node *at, Type *ft, const char *where) {
    if (ft->kind != TY_FN || ft->nraises == 0) return;
    Diag d = {0};
    d.message = MSG2("sema.621", "'{0}' は失敗しうる関数なので、{1} に渡せません", type_name(ft), where);
    d.primary.tok = at->tok;
    d.primary.label = MSG0("sema.622", "渡した先には、エラーを受け止める相手がいません");
    d.hint = MSG0("sema.623", "関数の中で try を使ってエラーを受け止めてください");
    diag_fail(&d);
}

static Type *check_call(Sema *s, Node *n) {
    // ★ 名前が **関数型の変数**なら間接呼び出しです。
    //   注意: 変数を先に見ます。同名の関数があっても変数が勝ちます。
    VarEntry *fv = lookup(s, n->name);
    if (fv && fv->type && fv->type->kind == TY_FN) {
        Type *ft = fv->type;
        // ★ 関数型には引数の**名前が入っていません**（A-38）。
        reject_kwargs(n->args,
                      MSG1("sema.205", "関数の値 '{0}' は名前で引数を受け取りません", n->name),
                      MSG1("sema.206", "この変数の型は '{0}' です。関数の型には引数の名前が入らないので、位置で渡してください", type_name(ft)));
        int nargs = 0;
        for (Node *a = n->args; a; a = a->next) nargs++;
        if (nargs != ft->nparams) {
            Diag d = {0};
            d.message = MSG3("sema.207", "'{0}' は {1} 個の引数を取りますが、{2} 個渡されました", n->name, diag_fmt("%d", ft->nparams), diag_fmt("%d", nargs));
            d.primary.tok = n->tok;
            d.primary.label = MSG0("sema.197", "引数の個数が違います");
            d.hint = MSG1("sema.208", "この変数の型は '{0}' です", type_name(ft));
            diag_fail(&d);
        }
        int i = 0;
        for (Node *a = n->args; a; a = a->next, i++) {
            s->expected = ft->params[i];
            Type *at = check_expr(s, a);
            s->expected = NULL;
            if (!type_assignable(at, ft->params[i])) {
                Diag d = {0};
                d.message = MSG1("sema.209", "{0} 番目の引数の型が合いません", diag_fmt("%d", i + 1));
                d.primary.tok = a->tok;
                d.primary.label = MSG1("sema.051", "これは '{0}' 型です", type_name(at));
                d.hint = MSG1("sema.064", "ここには '{0}' が必要です", type_name(ft->params[i]));
                diag_fail(&d);
            }
        }
        n->ir_name = fv->ir_name;
        n->is_indirect = true;
        n->type = ft->elem;
        // ★ 失敗しうる関数の値（A-49）。**型に書いてある raises** で見ます
        //   （インタフェース越しの呼び出し A-40 と同じ考え方です）。
        if (ft->nraises > 0) {
            FuncSig tmp = {0};
            tmp.name = n->name;
            tmp.tok = fv->decl_tok ? fv->decl_tok : n->tok;
            tmp.nraises = ft->nraises;
            tmp.raises = ft->raises;
            check_can_fail(s, n, &tmp, n->name);
        }
        return ft->elem;
    }

    // ★ 組み込み（print / len / append …）には引数の名前がありません（A-38）。
    //   注意: クラス名なら生成なので、ここでは断りません（init の名前が使えます）。
    if (!lookup_func(s, n->name) && !lookup_class(s, n->name))
        reject_kwargs(n->args,
                      MSG1("sema.162", "'{0}' は名前で引数を受け取りません", n->name),
                      MSG0("sema.466", "名前で渡せるのは、この言語で定義した関数・メソッド・生成だけです"));

    // ── 低レベルの組み込み ──
    const LowLevel *ll = lowlevel_of(n->name);
    if (ll && !lookup_func(s, n->name)) return check_lowlevel_call(s, n, ll);

    // ★ min / max。**2 引数**なので組み込みの表（1 引数）では
    //   表せません。ここで特別扱いします。
    //   注意: Python の min([1,2,3])（リストを渡す形）は入れていません。
    //     リストの最小は linalg.vmin を使ってください。
    if ((strcmp(n->name, "min") == 0 || strcmp(n->name, "max") == 0) &&
        !lookup_func(s, n->name)) {
        int nargs = 0;
        for (Node *a = n->args; a; a = a->next) nargs++;
        if (nargs != 2)
            error_at_hint(n->tok, MSG1("sema.210", "{0}(a, b) の形で使ってください", n->name),
                          MSG1("sema.211", "{0} は 2 個の引数を取ります", n->name));
        Type *a0 = check_expr(s, n->args);
        Type *a1 = check_expr(s, n->args->next);
        if (!type_equal(a0, a1))
            error_at_m(n->args->next->tok, MSG3("sema.212", "{0} の 2 つの引数は同じ型である必要があります（'{1}' と '{2}'）", n->name, type_name(a0), type_name(a1)));
        if (a0->kind != TY_INT && a0->kind != TY_FLOAT)
            error_at_hint_m(n->args->tok, MSG0("sema.214", "min / max が使えるのは int と float です"), MSG1("sema.213", "'{0}' 型には使えません", type_name(a0)));
        n->builtin = NULL;
        n->ir_name = NULL;
        n->is_minmax = true;
        n->type = a0;
        return a0;
    }

    // ★ wrap_add / wrap_sub / wrap_mul。
    //   **桁あふれを検査せず 2 の補数で折り返す**算術です。
    //   注意: 既定の + - * は桁あふれで panic します。折り返しを
    //     「そういう計算だ」として使いたいところ（法 2⁶⁴ の線形合同法など）
    //     のための逃げ道で、**書いた人の意図が演算ごとに見えます。**
    //   注意: min / max と同じく 2 引数なので、組み込みの表では表せません。
    if (is_wrap_name(n->name) && !lookup_func(s, n->name)) {
        int nargs = 0;
        for (Node *a = n->args; a; a = a->next) nargs++;
        if (nargs != 2)
            error_at_hint(n->tok, MSG1("sema.210", "{0}(a, b) の形で使ってください", n->name),
                          MSG1("sema.211", "{0} は 2 個の引数を取ります", n->name));
        Type *a0 = check_expr(s, n->args);
        Type *a1 = check_expr(s, n->args->next);
        // 注意: **問題のある側の引数**を指します（2 つとも int でないときは左から）
        if (a0->kind != TY_INT)
            error_at_hint_m(n->args->tok, MSG0("sema.215", "折り返す算術が使えるのは int だけです"), MSG1("sema.213", "'{0}' 型には使えません", type_name(a0)));
        if (a1->kind != TY_INT)
            error_at_hint_m(n->args->next->tok, MSG0("sema.215", "折り返す算術が使えるのは int だけです"), MSG1("sema.213", "'{0}' 型には使えません", type_name(a1)));
        n->builtin = NULL;
        n->ir_name = NULL;
        n->is_wrap = true;
        n->type = ty_int;
        return ty_int;
    }

    // ── rc(x) — 共有所有にくるむ ──
    //
    // ★ 構文上はただの呼び出しですが、型が「引数の型から作られる」ので
    //   組み込み関数の表（名前 → 固定の型）では表せません。ここで分岐します。
    if (strcmp(n->name, "rc") == 0 && !lookup_func(s, "rc")) {
        int nargs = 0;
        for (Node *a = n->args; a; a = a->next) nargs++;
        if (nargs != 1)
            error_at_hint_m(n->tok, MSG0("sema.217", "rc(値) の形で使ってください"), MSG0("sema.216", "rc は 1 個の引数を取ります"));
        Type *at = check_expr(s, n->args);
        if (at->kind != TY_CLASS)
            error_at_hint_m(n->args->tok, MSG0("sema.218", "rc に入れられるのはクラスのインスタンスだけです"), MSG1("sema.026", "'{0}' は rc に入れられません", type_name(at)));
        n->is_extern = false;
        n->name = "rc";
        return type_rc(at);
    }

    // ── weak(r) — 弱参照を作る（A-50）──
    //
    // ★ 受け取るのは rc[T] だけです（借りるだけで、r はそのまま使えます）。
    //   中身を生かしておく力は持たないので、使うときは upgrade() で戻します。
    if (strcmp(n->name, "weak") == 0 && !lookup_func(s, "weak")) {
        int nargs = 0;
        for (Node *a = n->args; a; a = a->next) nargs++;
        if (nargs != 1)
            error_at_hint_m(n->tok, MSG0("sema.631", "weak(r) の形で使ってください（r は rc[T]）"), MSG0("sema.630", "weak は 1 個の引数を取ります"));
        Type *at = check_expr(s, n->args);
        if (at->kind != TY_RC) {
            Diag d = {0};
            d.message = MSG1("sema.632", "'{0}' から弱参照は作れません", type_name(at));
            d.primary.tok = n->args->tok;
            d.primary.label = MSG0("sema.633", "ここには rc[T] が必要です");
            d.hint = MSG0("sema.634", "弱参照は共有している値（rc[T]）を指します。先に rc(...) で包んでください");
            diag_fail(&d);
        }
        n->is_extern = false;
        n->name = "weak";
        n->type = type_weak(at->elem);
        return n->type;
    }

    // ── spawn(f, a…) — 別スレッドで f(a…) を始める（A-18）──
    //
    // ★ 新しい構文は作りません。**呼び出しの形のまま**です
    //   （docs/ja/design/concurrency.md 3）。rc(x) と同じ理由でここに置きます:
    //   戻り型が引数の型から決まるので、組み込み関数の表では表せません。
    //
    // ★ 引数は**何個でも**渡せるようになりました。クロージャが無いので
    //   もとは「関数 + 引数 1 つ」でしたが、その形だと複数の値を渡すのに
    //   クラスへまとめるしかなく、**クラスは借りを保存できません**
    //   （E-BORROW-3）。scope: を入れても借りを共有する形が書けないままだった、
    //   というのが可変長にした理由です。
    if (strcmp(n->name, "spawn") == 0 && !lookup_func(s, "spawn")) {
        int nargs = 0;
        for (Node *a = n->args; a; a = a->next) nargs++;
        if (nargs < 1)
            error_at_hint_m(n->tok, MSG0("sema.220", "spawn(関数, 引数…) の形で使ってください"), MSG0("sema.219", "spawn には少なくとも関数が要ります"));
        Type *ft = check_expr(s, n->args);
        if (ft->kind != TY_FN)
            error_at_hint_m(n->args->tok, MSG0("sema.222", "spawn の 1 つ目は関数です（例: spawn(work, job)）"), MSG1("sema.221", "'{0}' は関数ではありません", type_name(ft)));
        reject_raising_fn(n->args, ft, "spawn");
        // ★ 別のスレッドは、作った枠より長生きしえます（A-43）
        reject_escaping_closure(n->args, MSG0("sema.483", "別のスレッドへ渡すことが"));
        if (ft->nparams != nargs - 1)
            error_at_hint_m(n->args->tok, MSG2("sema.224", "'{0}' は {1} 個の引数を取ります", type_name(ft), diag_fmt("%d", ft->nparams)), MSG1("sema.223", "spawn に渡した引数が {0} 個です", diag_fmt("%d", nargs - 1)));
        int k = 0;
        for (Node *a = n->args->next; a; a = a->next, k++) {
            s->expected = ft->params[k];
            Type *at = check_expr(s, a);
            s->expected = NULL;
            if (!type_assignable(at, ft->params[k]))
                error_at_hint_m(a->tok, MSG1("sema.064", "ここには '{0}' が必要です", type_name(ft->params[k])), MSG2("sema.166", "{0} 番目の引数が '{1}' 型です", diag_fmt("%d", k + 1), type_name(at)));
        }
        n->is_extern = false;
        return type_thread(ft->elem);
    }

    // ── mutex(x) — 中身をロックの内側に閉じる ──
    //
    // ★ rc(x) と同じ形（構文はただの呼び出し、型は引数から作る）。
    if (strcmp(n->name, "mutex") == 0 && !lookup_func(s, "mutex")) {
        int nargs = 0;
        for (Node *a = n->args; a; a = a->next) nargs++;
        if (nargs != 1)
            error_at_hint_m(n->tok, MSG0("sema.226", "mutex(値) の形で使ってください"), MSG0("sema.225", "mutex は 1 個の引数を取ります"));
        Type *at = check_expr(s, n->args);
        if (at->kind != TY_CLASS && at->kind != TY_LIST)
            error_at_hint_m(n->args->tok, MSG0("sema.227", "mutex に入れられるのはクラスかリストだけです"), MSG1("sema.032", "'{0}' は mutex に入れられません", type_name(at)));
        n->is_extern = false;
        n->name = "mutex";
        return type_mutex(at);
    }

    // ── f-string の書式指定が作る呼び出し（A-45）──────────────
    //
    // ★ 利用者は書けない名前です（'.' が入っているので識別子になりません）。
    //   パーサが f"{x:>8}" を脱糖して作ります。
    if (strcmp(n->name, "fmt.pad") == 0 || strcmp(n->name, "fmt.f64") == 0) {
        bool is_pad = strcmp(n->name, "fmt.pad") == 0;
        int want = is_pad ? 4 : 2;
        int nargs = 0;
        for (Node *a = n->args; a; a = a->next) nargs++;
        if (nargs != want) UNREACHABLE();   // パーサが作る形です
        int i = 0;
        for (Node *a = n->args; a; a = a->next, i++) {
            Type *at = check_expr(s, a);
            Type *need = (i == 0) ? (is_pad ? ty_str : ty_float) : ty_int;
            if (!type_assignable(at, need)) {
                Diag d = {0};
                d.message = is_pad
                    ? MSG1("sema.228", "書式（桁揃え）は '{0}' 型には使えません", type_name(at))
                    : MSG1("sema.229", "小数の書式は '{0}' 型には使えません", type_name(at));
                d.primary.tok = a->tok;
                d.primary.label = MSG1("sema.051", "これは '{0}' 型です", type_name(at));
                d.hint = is_pad ? MSG0("sema.484", "桁揃えは文字列にしてから行います")
                                : MSG0("sema.485", "'.2f' のような書式は float にだけ使えます（int なら float(x) にしてください）");
                diag_fail(&d);
            }
        }
        n->builtin = NULL;
        n->ir_name = is_pad ? "pl_str_pad" : "pl_fmt_f64";
        n->is_fmt = true;
        n->type = ty_str;
        return ty_str;
    }

    // ── copy(v) — 複製の抽象（A-44）────────────────────────
    //
    // ★ **どの型でも書けること**が値打ちです。ジェネリックなコードの中では
    //   `T` が `int` かもしれず、`str` のときだけ複製が要る——という書き分けが
    //   できませんでした。`copy` を 1 つの入口にして、型ごとに出すものを
    //   変えます（値型はそのまま、str は新しい文字列、list は要素ごと…）。
    //
    // 注意: **クラスは `__copy__` を書いたものだけ**です。黙って浅く写すと、
    //   所有しているフィールド（str / list / 別のクラス）を 2 つの実体が
    //   指すことになり、解放が二重になります。
    if (strcmp(n->name, "copy") == 0 && !lookup_func(s, "copy")) {
        int nargs = 0;
        for (Node *a = n->args; a; a = a->next) nargs++;
        if (nargs != 1)
            error_at_hint_m(n->tok, MSG0("sema.231", "copy(値) の形で使ってください"), MSG1("sema.230", "copy は 1 個の引数を取りますが、{0} 個渡されました", diag_fmt("%d", nargs)));
        Type *t = auto_deref(check_expr(s, n->args));
        check_copyable(s, t, n->args->tok);
        n->builtin = NULL;
        n->ir_name = NULL;
        n->is_copy = true;
        n->type = t;
        return t;
    }

    if (is_builtin_name(n->name)) return check_builtin_call(s, n);

    // ★ 名前がクラスなら、これは呼び出しではなくインスタンス生成
    Class *cls = lookup_class(s, n->name);
    if (cls) return check_new(s, n, cls);

    // ① 定義されているか
    FuncSig *f = lookup_func(s, n->name);

    // ★ ジェネリック関数なら、実引数から型引数を決めて実体を作る
    if (f && f->tmpl) f = instantiate_func(s, f, n);

    if (!f) {
        Diag d = {0};
        d.message = MSG1("sema.232", "未定義の関数 '{0}' です", n->name);
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.453", "この関数は定義されていません");
        d.hint = MSG0("sema.486", "関数名の綴りを確認してください（定義の順序は問いません。後ろで定義した関数も呼べます）");
        diag_fail(&d);
    }

    // ★ 呼ぶ相手の IR 名（モジュール修飾済み）を codegen に渡す
    n->ir_name = f->ir_name;
    n->is_extern = f->owner != s->cur;

    // ③④ 引数の個数と型
    return check_call_sig(s, n, f, MSG0("sema.455", "関数"));
}

// 失敗しうる呼び出しを、誰が受け止めるかを決める（R1 / R2）。
//
// ★ 受け止め方は 2 つだけです。**try で捕まえる**か、**自分も raises を宣言する**か。
//   どちらでもなければ、そこで握りつぶされてしまうのでエラーにします（仕様 §8.2）。
static void check_can_fail(Sema *s, Node *n, FuncSig *f, const char *shown) {
    if (f->nraises == 0) return;
    n->can_fail = true;  // ★ codegen はこれを見て分岐を挿す

    for (int i = 0; i < f->nraises; i++) {
        Class *ec = f->raises[i];
        if (try_catches(s, ec)) continue;
        if (func_declares(s->cur_func, ec)) continue;

        Diag d = {0};
        d.primary.tok = n->tok;
        d.related.tok = f->tok;
        d.related.label = MSG1("sema.233", "'{0}' はここで宣言されています", shown);
        if (s->cur_func && s->cur_func->nraises == 0) {
            d.code = "E-RAISE-1";
            d.message = MSG1("sema.234", "失敗しうる呼び出し '{0}' を処理していません", shown);
            d.primary.label = MSG1("sema.235", "この呼び出しは '{0}' を返すことがあります", ec->name);
            d.hint = MSG1("sema.236", "try で捕まえるか、この関数に 'raises {0}' を足してください", ec->name);
        } else {
            d.code = "E-RAISE-2";
            d.message = MSG1("sema.237", "エラー '{0}' が宣言されていません", ec->name);
            d.primary.label = MSG2("sema.238", "'{0}' は '{1}' を返すことがあります", shown, ec->name);
            d.hint = MSG1("sema.239", "この関数の raises に '{0}' を足すか、try で捕まえてください", ec->name);
        }
        diag_fail(&d);
    }
}

// ── 呼び出しの引数を並べ替えて、足りないぶんを既定値で埋める（A-38）──
//
// ★ **ここで n->args を「ちょうど引数の数だけ・定義の順」に書き換えます。**
//   そうすれば、この先（型検査・codegen・所有権検査・証明）は今までどおり
//   「前から順に当てる」だけで済みます。**呼び出しの形を知るのは
//   この関数だけ**です。
//
// ★ 既定値は**呼び出しごとに複製**します。Python のように定義時に 1 つ
//   作って共有すると、`def f(xs=[])` が呼び出しをまたいで同じ list を
//   指します。もっとも、既定値に書けるのはリテラルだけなので、この言語では
//   そもそもその形が書けません（parser の default_value）。
//
// ★ 引数の名前と既定値を**配列で**受け取ります。FuncSig を持たない
//   ジェネリック関数（テンプレート）からも同じ検査を使うためです。
//   pnames / defaults は self を飛ばした先頭を指します。
//
//   deftok … 「この関数はここで定義されています」に使う位置
//   subject … エラーに出す呼び名（「関数 'add'」「クラス 'P'」など）
//   has_self … self を飛ばしたか（案内の言葉を変えるだけ）
//
// ★ **名前も既定値も出てこない呼び出しには手を触れません。** その形の
//   個数違いは、呼び出し側の検査が今までどおりの言葉で断ります
//   （「関数 'add' は 2 個の引数を取りますが、3 個渡されました」）。
//   ここで断ると、既定値のある関数のためだけの言い回しが、既定値と
//   関係のない呼び出しにまで出てしまいます。
static void bind_args(Node *n, int nparams, char **pnames, Node **defaults,
                      Token *deftok, const char *subject, bool has_self) {
    bool any_kw = false;
    int nargs = 0;
    for (Node *a = n->args; a; a = a->next) {
        nargs++;
        if (a->arg_name) any_kw = true;
    }

    bool has_default = false;
    for (int i = 0; i < nparams && defaults; i++)
        if (defaults[i]) { has_default = true; break; }

    if (!any_kw && !has_default) return;
    if (!any_kw && nargs == nparams) return;

    Node **slot = xmalloc(sizeof(Node *) * (size_t)(nparams ? nparams : 1));
    for (int i = 0; i < nparams; i++) slot[i] = NULL;

    // ── ① 位置引数を前から当てる ──
    int pos = 0;
    bool seen_kw = false;
    Node *kw_first = NULL;
    for (Node *a = n->args; a; a = a->next) {
        if (!a->arg_name) {
            // ★ **位置引数はキーワード引数より前だけ**です。混ぜられると、
            //   読む人が「この値は何番目の引数か」を数え直すことになります。
            if (seen_kw) {
                Diag d = {0};
                d.message = MSG0("sema.487", "位置で渡す引数は、名前で渡す引数より前に書きます");
                d.primary.tok = a->tok;
                d.primary.label = MSG0("sema.488", "ここは名前つきの引数より後ろです");
                d.related.tok = kw_first->arg_name_tok;
                d.related.label = MSG0("sema.489", "名前で渡し始めたのはここです");
                d.hint = MSG0("sema.490", "名前を付けるか、この引数を前へ動かしてください");
                diag_fail(&d);
            }
            if (pos >= nparams) {
                Diag d = {0};
                d.message = MSG3("sema.240", "{0} に渡せる引数は多くとも {1} 個です（{2} 個渡されました）", subject, diag_fmt("%d", nparams), diag_fmt("%d", nargs));
                d.primary.tok = a->tok;
                d.primary.label = MSG0("sema.491", "この引数に当たるものがありません");
                d.related.tok = deftok;
                d.related.label = MSG0("sema.492", "この関数はここで定義されています");
                if (has_self)
                    d.hint = MSG0("sema.452", "self は自動的に渡されるので、書く必要はありません");
                diag_fail(&d);
            }
            slot[pos++] = a;
            continue;
        }

        seen_kw = true;
        if (!kw_first) kw_first = a;

        int at = -1;
        for (int i = 0; i < nparams; i++)
            if (strcmp(pnames[i], a->arg_name) == 0) { at = i; break; }
        if (at < 0) {
            StrBuf sb;
            sb_init(&sb);
            sb_printf(&sb, "%s", MSG0("sema.241", "書ける名前は "));
            for (int i = 0; i < nparams; i++)
                sb_printf(&sb, "%s%s", i ? " / " : "", pnames[i]);
            sb_printf(&sb, "%s", MSG0("sema.143", " です"));
            Diag d = {0};
            d.message = MSG2("sema.242", "{0} に引数 '{1}' はありません", subject, a->arg_name);
            d.primary.tok = a->arg_name_tok;
            d.primary.label = MSG0("sema.493", "この名前の引数は定義されていません");
            d.related.tok = deftok;
            d.related.label = MSG0("sema.492", "この関数はここで定義されています");
            d.hint = sb_str(&sb);
            diag_fail(&d);
        }
        if (slot[at]) {
            Diag d = {0};
            d.message = MSG1("sema.243", "引数 '{0}' に 2 回渡しています", a->arg_name);
            d.primary.tok = a->arg_name_tok;
            d.primary.label = MSG0("sema.494", "2 回目です");
            d.related.tok = slot[at]->tok;
            d.related.label = MSG0("sema.495", "1 回目はここです");
            d.hint = at < pos ? MSG0("sema.496", "位置で渡したものに、名前でもう一度渡しています")
                              : MSG0("sema.497", "同じ名前を 2 回書いています");
            diag_fail(&d);
        }
        slot[at] = a;
    }

    // ── ② 空いているところを既定値で埋める ──
    for (int i = 0; i < nparams; i++) {
        if (slot[i]) continue;
        Node *dflt = defaults ? defaults[i] : NULL;
        if (!dflt) {
            Diag d = {0};
            d.message = MSG2("sema.244", "{0} の引数 '{1}' が渡されていません", subject, pnames[i]);
            d.primary.tok = n->tok;
            d.primary.label = MSG1("sema.245", "引数 '{0}' に当たるものがありません", pnames[i]);
            d.related.tok = deftok;
            d.related.label = MSG0("sema.492", "この関数はここで定義されています");
            d.hint = MSG0("sema.498", "この引数には既定値がないので、呼ぶときに必ず書きます");
            diag_fail(&d);
        }
        // ★ **呼び出しごとに複製**します（共有しません）。
        slot[i] = ast_clone(dflt);
    }

    // ── ③ 定義の順に繋ぎ直す ──
    Node head = {0};
    Node *cur = &head;
    for (int i = 0; i < nparams; i++) {
        slot[i]->next = NULL;
        // ★ 印はもう要りません（並びが答えになったので）。
        slot[i]->arg_name = NULL;
        cur->next = slot[i];
        cur = slot[i];
    }
    n->args = head.next;
}

// FuncSig を持つ呼び先（関数・メソッド・生成）はこちらを通します。
static void bind_args_sig(Node *n, FuncSig *f, int skip, const char *subject) {
    bind_args(n, f->nparams - skip, f->pnames ? f->pnames + skip : NULL,
              f->defaults ? f->defaults + skip : NULL, f->tok, subject,
              skip > 0);
}

// 呼び出しの引数を FuncSig と突き合わせる（③④）。
//
// ★ モジュール修飾の呼び出し（lexer.make(1)）でも同じ検査が要るので、
//   関数に切り出しました。呼ぶ側が変わっても、検査は 1 か所のままです。
static Type *check_call_sig(Sema *s, Node *n, FuncSig *f, const char *what) {
    const char *shown = n->mod_name ? diag_fmt("%s.%s", n->mod_name, n->name)
                                    : f->name;

    check_can_fail(s, n, f, shown);

    // ★ 並べ替えと既定値の穴埋め（A-38）。ここを通ると n->args は
    //   「ちょうど引数の数だけ・定義の順」になります。
    bind_args_sig(n, f, 0, diag_fmt("%s '%s'", what, shown));

    int nargs = 0;
    for (Node *a = n->args; a; a = a->next) nargs++;
    if (nargs != f->nparams) {
        Diag d = {0};
        d.message = MSG4("sema.246", "{0} '{1}' は {2} 個の引数を取りますが、{3} 個渡されました", what, shown, diag_fmt("%d", f->nparams), diag_fmt("%d", nargs));
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.450", "呼び出しの引数の個数が違います");
        d.related.tok = f->tok;
        d.related.label = MSG0("sema.492", "この関数はここで定義されています");
        diag_fail(&d);
    }

    int i = 0;
    for (Node *a = n->args; a; a = a->next, i++) {
        // 注意: 引数には期待型を渡しません。
        //    move_out([]) の [] は「型注釈を書いてください」というエラーになります。
        // ★ 例外は**関数型**です（A-42）。lambda はここから型をもらいます。
        //   注意: 関数型に限るのは、[] のような「型注釈を書いてください」と
        //     案内したい形まで通してしまわないためです。
        s->expected = f->params[i]->kind == TY_FN ? f->params[i] : NULL;
        Type *at = check_expr(s, a);
        s->expected = NULL;
        // ★ 捕獲した lambda は、**しまわない**相手にだけ渡せます（A-43）。
        //   呼ぶだけ・局所に置くだけなら、呼び出しは作った枠より短いので安全です。
        if (a->caps && f->node && i < f->nparams &&
            fn_param_escapes_at(s, f->node->body, f->pnames[i], 0)) {
            Diag d = {0};
            d.message = MSG1("sema.247", "捕獲した lambda を '{0}' に渡せません", shown);
            d.primary.tok = a->tok;
            d.primary.label = MSG0("sema.499", "この lambda は外の変数を捕まえています");
            d.related.tok = f->tok;
            d.related.label = MSG1("sema.248", "'{0}' は受け取った関数をしまいます", f->pnames[i]);
            d.hint = MSG0("sema.500", "捕まえた値は作った関数の枠の上にあるので、しまわれると読めなくなります。捕獲しない lambda か、def で書いた関数を渡してください");
            diag_fail(&d);
        }
        // ★ codegen へ「この実引数は借用で渡す」と**分かっている**ことを伝えます。
        //   借用なら相手は所有権を受け取らないので、呼び出し後に一時値を
        //   解放できます（A-21e）。
        // 注意: ここは check_call_sig＝**通常の関数**だけを通ります。メソッドは
        //   別経路なので旗が立たず、codegen は解放しません（安全側）。
        a->arg_is_borrowed = f->pmodes && f->pmodes[i] != PM_OWN;
        mark_arg_own_rc(a, f, i);
        if (!type_assignable(at, f->params[i])) {
            Diag d = {0};
            d.message = MSG5("sema.249", "{0} '{1}' の第 {2} 引数: 型 '{3}' を '{4}' に渡せません", what, shown, diag_fmt("%d", i + 1), type_name(at), type_name(f->params[i]));
            d.primary.tok = a->tok;
            d.primary.label = MSG1("sema.051", "これは '{0}' 型です", type_name(at));
            d.related.tok = f->tok;
            d.related.label = MSG2("sema.151", "引数 '{0}' は '{1}' 型です", f->pnames[i], type_name(f->params[i]));
            d.hint = no_implicit_hint(at, f->params[i]);
            diag_fail(&d);
        }
    }
    return f->ret;
}

// return の検査
// ── 捕獲した lambda を逃がさない（A-43）──────────────────
//
// ★ 捕獲した lambda は**作った関数の枠の上**にあります。枠が畳まれたあとに
//   呼ばれたら、読むのは他人の場所です。だから**逃がす形を断ります**——
//   返す・しまう・別スレッドへ渡す。
//
// 注意: 素の関数の値（`f` をそのまま渡す）は静的な記録を指すので、
//   ここには引っかかりません。制限がかかるのは**捕まえたときだけ**です。
static void reject_escaping_closure(Node *v, const char *what) {
    if (!v || !v->caps) return;
    Diag d = {0};
    d.message = MSG1("sema.250", "捕獲した lambda は{0}できません", what);
    d.primary.tok = v->tok;
    d.primary.label = MSG0("sema.499", "この lambda は外の変数を捕まえています");
    d.related.tok = v->caps->tok;
    d.related.label = MSG1("sema.251", "'{0}' を捕まえています", v->caps->name);
    d.hint = MSG0("sema.501", "捕まえた値は**作った関数の枠の上**にあるので、その関数より長生きできません。引数として渡す（呼んでもらう）のは できます");
    diag_fail(&d);
}

// この関数は、受け取った関数の値を**しまう**か（A-43）。
//
// ★ しまうのは「返す」「フィールド・グローバル・list に入れる」の 3 つです。
//   自分の枠の中で使うだけ（呼ぶ・局所変数に置く）なら、捕獲した lambda を
//   渡しても安全です——呼び出しは、作った枠より短いからです。
//
// 注意: **分からないときは「しまう」と答えます**（安全側）。
//   hops は**関数を何段たどったか**です（木の深さではありません）。
//   たどりすぎたら「しまう」と答えて打ち切ります。
static bool fn_param_escapes_at(Sema *s, Node *body, const char *pname, int hops) {
    for (Node *n = body; n; n = n->next) {
        if (n->kind == ND_RETURN && n->lhs && n->lhs->kind == ND_VAR &&
            strcmp(n->lhs->name, pname) == 0)
            return true;
        if (n->kind == ND_ASSIGN && n->rhs && n->rhs->kind == ND_VAR &&
            strcmp(n->rhs->name, pname) == 0)
            return true;   // フィールド・グローバル・添字のどれでも断ります
        if (n->kind == ND_LIST)
            for (Node *el = n->body; el; el = el->next)
                if (el->kind == ND_VAR && strcmp(el->name, pname) == 0)
                    return true;

        // ★ 別の関数へ渡している場合は、**その先も見ます**。
        //   `def a(p): return b(p)` の b が しまうなら、a も しまいます。
        if (n->kind == ND_CALL || n->kind == ND_METHOD) {
            int ai = 0;
            for (Node *a = n->args; a; a = a->next, ai++) {
                if (a->kind != ND_VAR || strcmp(a->name, pname) != 0) continue;
                if (hops >= 4) return true;          // たどりすぎ（安全側）
                FuncSig *g = n->kind == ND_CALL ? lookup_func(s, n->name) : NULL;
                if (!g || !g->node || ai >= g->nparams) return true;  // 分からない
                if (fn_param_escapes_at(s, g->node->body, g->pnames[ai], hops + 1))
                    return true;
            }
        }

        if (fn_param_escapes_at(s, n->lhs, pname, hops)) return true;
        if (fn_param_escapes_at(s, n->rhs, pname, hops)) return true;
        if (fn_param_escapes_at(s, n->els, pname, hops)) return true;
        if (fn_param_escapes_at(s, n->body, pname, hops)) return true;
        if (fn_param_escapes_at(s, n->incr, pname, hops)) return true;
        if (fn_param_escapes_at(s, n->args, pname, hops)) return true;
    }
    return false;
}

static void check_return(Sema *s, Node *n) {
    Type *want = s->cur_func->ret;

    if (!n->lhs) {  // return（値なし）
        if (want->kind != TY_NONE) {
            Diag d = {0};
            d.message = MSG2("sema.252", "関数 '{0}' は '{1}' を返さなければなりません", s->cur_func->name, type_name(want));
            d.primary.tok = n->tok;
            d.primary.label = MSG0("sema.502", "この return には値がありません");
            d.related.tok = s->cur_func->tok;
            d.related.label = MSG0("sema.503", "戻り型はここで宣言されています");
            diag_fail(&d);
        }
        return;
    }

    s->expected = want;  // ★ return [] のため
    Type *got = check_expr(s, n->lhs);
    s->expected = NULL;
    reject_escaping_closure(n->lhs, MSG0("sema.504", "返すことが"));   // A-43
    if (want->kind == TY_NONE) {
        Diag d = {0};
        d.message = MSG1("sema.253", "戻り型が None の関数 '{0}' は値を返せません", s->cur_func->name);
        d.primary.tok = n->lhs->tok;
        d.primary.label = MSG1("sema.089", "型 '{0}' の式", type_name(got));
        d.related.tok = s->cur_func->tok;
        d.related.label = MSG0("sema.503", "戻り型はここで宣言されています");
        diag_fail(&d);
    }
    if (!type_assignable(got, want)) {
        Diag d = {0};
        d.message = MSG0("sema.505", "return の型が戻り型と一致しません");
        d.primary.tok = n->lhs->tok;
        d.primary.label = MSG1("sema.089", "型 '{0}' の式", type_name(got));
        d.related.tok = s->cur_func->tok;
        d.related.label = MSG2("sema.254", "関数 '{0}' の戻り型は '{1}' です", s->cur_func->name, type_name(want));
        d.hint = no_implicit_hint(got, want);
        diag_fail(&d);
    }
    n->lhs = range_coerce(s, n->lhs, want);   // A-28
}

// 分解代入 q, r = f()
//
// ★ **宣言も兼ねます。** 型は右辺のタプルから決まるので書かせません
//   （roadmap.md §0 の ③：右辺から決まるものは書かせない）。
static void check_unpack(Sema *s, Node *n) {
    Type *rt = check_expr(s, n->rhs);
    if (rt->kind != TY_TUPLE) {
        Diag d = {0};
        d.message = MSG0("sema.506", "分解代入の右辺がタプルではありません");
        d.primary.tok = n->rhs->tok;
        d.primary.label = MSG1("sema.051", "これは '{0}' 型です", type_name(rt));
        d.hint = MSG0("sema.507", "分解できるのは (A, B) を返すものだけです（複数の値を返す関数を書いてください）");
        diag_fail(&d);
    }

    int nvars = 0;
    for (Node *v = n->params; v; v = v->next) nvars++;
    if (nvars != rt->nparams) {
        Diag d = {0};
        d.message = MSG2("sema.255", "受け取る数が合いません（{0} 個と {1} 個）", diag_fmt("%d", nvars), diag_fmt("%d", rt->nparams));
        d.primary.tok = n->tok;
        d.primary.label = MSG1("sema.061", "ここは {0} 個です", diag_fmt("%d", nvars));
        d.hint = MSG1("sema.256", "右辺は '{0}' です", type_name(rt));
        diag_fail(&d);
    }

    int k = 0;
    for (Node *v = n->params; v; v = v->next, k++) {
        VarEntry *e = declare(s, v->name, rt->params[k], v->tok);
        v->ir_name = e->ir_name;
        v->type = rt->params[k];
    }
    n->type = rt;
}

// ── 中身を持つ枝の match（A-41）──────────────────────────
//
// ★ **ここで形を書き換えます。** 調べるのは「先頭のタグ（i64）」で、
//   当たった枝の中身は**フィールドの読み出し**に落とします。
//   そうすれば、網羅の検査（A-37）も codegen も今までどおりです。
//
//   match s:                    __m.0: Shape = s           ← 1 回だけ評価
//       case Shape.Circle(r):   match tag(__m.0):
//           BODY            →       case 0:
//                                       __v.1: Shape$Circle = __m.0  ← 見立て
//                                       r: float = __v.1.r
//                                       BODY
//
// ★ 見立て（`__v`）に命令は要りません。LLVM のポインタは型を持たないので、
//   同じ値を別のクラスとして読むだけです。タグが先頭にあるので、どの枝でも
//   同じ場所にあります。
static char *sema_hidden(Sema *s, const char *tag) {
    return diag_fmt("%s.%d", tag, s->hidden++);
}

// パターン（`Shape.Circle(r)` / `Shape.Empty`）から枝を取り出す
static EnumVal *pattern_branch(Sema *s, Node *pat, EnumDef *e) {
    Node *owner = pat->lhs;
    const char *bname = pat->name;
    if (!owner || (pat->kind != ND_FIELD && pat->kind != ND_METHOD)) {
        Diag d = {0};
        d.message = MSG0("sema.508", "case には枝を書きます");
        d.primary.tok = pat->tok;
        d.primary.label = MSG0("sema.509", "ここは枝ではありません");
        d.hint = MSG2("sema.257", "'{0}.枝名' か '{1}.枝名(中身…)' の形で書きます", e->name, e->name);
        diag_fail(&d);
    }
    // 列挙の名前が合っているか（`Other.Branch` を断ります）
    bool same = owner->kind == ND_VAR && strcmp(owner->name, e->name) == 0;
    if (!same && owner->kind == ND_FIELD) same = strcmp(owner->name, e->name) == 0;
    if (!same) {
        Diag d = {0};
        d.message = MSG1("sema.258", "'{0}' の match に、別の列挙の枝は書けません", e->name);
        d.primary.tok = pat->tok;
        d.primary.label = MSG0("sema.510", "ここは違う列挙です");
        d.related.tok = e->tok;
        d.related.label = MSG0("sema.511", "調べている列挙はこれです");
        diag_fail(&d);
    }
    EnumVal *v = lookup_enum_val(e, bname);
    if (!v) {
        Diag d = {0};
        d.message = MSG2("sema.141", "列挙 '{0}' に枝 '{1}' はありません", e->name, bname);
        d.primary.tok = pat->tok;
        d.primary.label = MSG0("sema.439", "この枝は定義されていません");
        d.related.tok = e->tok;
        d.related.label = MSG0("sema.440", "列挙の定義はここです");
        diag_fail(&d);
    }
    return v;
}

// case の本体の先頭に「見立て」と「中身の束縛」を差し込む
static void bind_pattern(Sema *s, Node *c, Node *pat, EnumDef *e, EnumVal *v,
                         char *mname) {
    int nargs = 0;
    if (pat->kind == ND_METHOD)
        for (Node *a = pat->args; a; a = a->next) nargs++;

    if (nargs != v->nfields) {
        Diag d = {0};
        d.message = MSG4("sema.259", "枝 '{0}.{1}' の中身は {2} 個です（{3} 個書かれています）", e->name, v->name, diag_fmt("%d", v->nfields), diag_fmt("%d", nargs));
        d.primary.tok = pat->tok;
        d.primary.label = v->nfields == 0 ? MSG0("sema.436", "この枝は中身を持ちません")
                                          : MSG0("sema.512", "中身の数が合っていません");
        d.related.tok = v->tok;
        d.related.label = MSG0("sema.438", "枝の定義はここです");
        d.hint = v->nfields == 0
                     ? MSG2("sema.260", "'case {0}.{1}:' と書きます", e->name, v->name)
                     : MSG3("sema.261", "'case {0}.{1}(…)' に {2} 個の名前を書きます", e->name, v->name, diag_fmt("%d", v->nfields));
        diag_fail(&d);
    }
    if (nargs == 0) return;

    // 束縛の名前は「ただの名前」だけです（式は書けません）
    for (Node *a = pat->args; a; a = a->next) {
        if (a->kind != ND_VAR) {
            Diag d = {0};
            d.message = MSG0("sema.513", "case の中身には名前を書きます");
            d.primary.tok = a->tok;
            d.primary.label = MSG0("sema.514", "ここは名前ではありません");
            d.hint = MSG0("sema.515", "束縛する名前を書きます（例: case Shape.Circle(r):）。値で絞りたいときはガードを使ってください（例: case Shape.Circle(r) if r > 0:）");
            diag_fail(&d);
        }
        for (Node *q = pat->args; q != a; q = q->next)
            if (strcmp(q->name, a->name) == 0) {
                Diag d = {0};
                d.message = MSG1("sema.262", "束縛する名前 '{0}' が 2 回あります", a->name);
                d.primary.tok = a->tok;
                d.primary.label = MSG0("sema.516", "2 つめです");
                d.related.tok = q->tok;
                d.related.label = MSG0("sema.517", "最初はここです");
                diag_fail(&d);
            }
    }

    Class *bc = branch_class(s, e, v);
    Token *t = pat->tok;

    // __v.N: <枝のクラス> = __m.N（見立て。IR では何も起きません）
    char *vname = sema_hidden(s, "match.b");
    Node *view = new_node(ND_VARDECL, t);
    view->name = vname;
    Node *vtr = new_node(ND_TYPEREF, t);
    vtr->name = bc->name;
    view->type_ref = vtr;
    view->rhs = new_var_node(t, mname);
    view->is_enum_view = true;

    Node head = {0};
    Node *cur = &head;
    cur->next = view;
    cur = cur->next;

    // 中身を順に束縛（__tag は飛ばします）
    Field *f = bc->fields;
    if (f) f = f->next;              // ★ 先頭は __tag
    for (Node *a = pat->args; a; a = a->next, f = f->next) {
        Node *fld = new_node(ND_FIELD, a->tok);
        fld->lhs = new_var_node(a->tok, vname);
        fld->name = f->name;
        Node *bind = new_node(ND_VARDECL, a->tok);
        bind->name = a->name;
        bind->rhs = fld;             // 型は右辺から決まります
        cur->next = bind;
        cur = cur->next;
    }

    cur->next = c->body->body;
    c->body->body = head.next;
}

// match 全体を「タグで調べる形」に書き換える
// ── match のガードと入れ子のパターン（A-48）──────────────────
//
//   case Shape.Circle(r) if r > 10:   ← ガード
//   case Outer.Wrap(Inner.A(v)):       ← 入れ子のパターン
//
// ★ **同じ枝（同じ値）の case を 1 つにまとめ、中身を if と match で組み立てます。**
//   まとめた後は「同じ値を 2 回書かない match」に戻るので、網羅の検査も
//   中身を持つ枝の書き換え（lower_match_payload）も codegen も今までどおりです。
//
//   match s:                                    match s:
//       case Circle(r) if r > 10: A                 case Circle(match.h.0):
//       case Circle(r): B              →                if match.h.0 > 10:
//       case Dot: C                                         r = match.h.0
//                                                           A
//                                                       else:
//                                                           r = match.h.0
//                                                           B
//                                                   case Dot: C
//
// ★ ガードが外れた・入れ子が外れたときは、**同じ枝の次の case**（無ければ
//   case _ の中身）へ落ちます。落ち先は if の else と内側の match の case _ に
//   置きます。2 回目からは複製です（return の解析が if / else のまま働くように）。
//
// ★ **判定のあいだは隠し変数だけを使います。** 利用者の名前は、当たったときの
//   本体の先頭でだけ宣言します。そうしないと、後ろの case が同じ名前を
//   束縛したとき、外側の名前を隠すことになります（シャドーイングは禁止）。
//   ガードの式の中の名前は、隠し変数に読み替えます。
//
// 注意: 隠し変数の番号は「この case の名前を読み替える → 後ろの case を
//   組み立てる → 自分を組み立てる」の順に振ります（2 実装で同じ IR）。
//   対になる定義: selfhost/sema の lower_match_refine

static Node *hidden_var(Token *tok, char *name, Node *init);

// 読み替え表（利用者の名前 → 隠し変数）
typedef struct Rename {
    char *from;
    char *to;
    Token *tok;
    struct Rename *next;
} Rename;

static bool case_refutable(Node *c) {
    if (c->rhs) return true;
    if (c->lhs && c->lhs->kind == ND_METHOD)
        for (Node *a = c->lhs->args; a; a = a->next)
            if (a->kind != ND_VAR) return true;
    return false;
}

static bool same_case_key(Node *a, Node *b) {
    bool ea = a->kind == ND_FIELD || a->kind == ND_METHOD;
    bool eb = b->kind == ND_FIELD || b->kind == ND_METHOD;
    if (ea || eb) return ea && eb && a->name && b->name && strcmp(a->name, b->name) == 0;
    if (a->kind == ND_INT && b->kind == ND_INT) return a->ival == b->ival;
    if (a->kind == ND_STR && b->kind == ND_STR)
        return a->slen == b->slen && memcmp(a->sval, b->sval, (size_t)a->slen) == 0;
    return false;
}

static int count_args(Node *pat) {
    int k = 0;
    if (pat && pat->kind == ND_METHOD)
        for (Node *a = pat->args; a; a = a->next) k++;
    return k;
}

static Node *new_block_of(Token *t, Node *body) {
    Node *b = new_node(ND_BLOCK, t);
    b->body = body;
    return b;
}

// 入れ子のパターンの中の名前を、隠し変数に置き換える（深さ優先・左から）
static void rename_pattern(Sema *s, Node *pat, Rename **rn) {
    if (!pat || pat->kind != ND_METHOD) return;
    for (Node *a = pat->args; a; a = a->next) {
        if (a->kind == ND_VAR) {
            Rename *r = xmalloc(sizeof(Rename));
            r->from = a->name;
            r->to = sema_hidden(s, "match.n");
            r->tok = a->tok;
            r->next = *rn;
            *rn = r;
            a->name = r->to;
        } else {
            rename_pattern(s, a, rn);
        }
    }
}

// ガードの中の名前を読み替える
static void rename_expr(Node *n, Rename *rn) {
    for (; n; n = n->next) {
        if (n->kind == ND_VAR && !n->mod_name)
            for (Rename *r = rn; r; r = r->next)
                if (strcmp(n->name, r->from) == 0) {
                    n->name = r->to;
                    break;
                }
        rename_expr(n->lhs, rn);
        rename_expr(n->rhs, rn);
        rename_expr(n->els, rn);
        rename_expr(n->args, rn);
        rename_expr(n->body, rn);
    }
}

typedef struct {
    Node *rest;   // 落ち先（ND_BLOCK）。無ければ NULL
    int used;
} Fallback;

static Node *fb_take(Fallback *fb) {
    if (!fb->rest) return NULL;
    return fb->used++ ? ast_clone(fb->rest) : fb->rest;
}

// mem[i..] を順に試す文（ND_BLOCK）。どれにも当たらなければ case _ の中身（複製）。
static Node *refine_chain(Sema *s, Node **mem, int nmem, int i, Node *dflt,
                          char **hid, bool *used_default) {
    if (i == nmem) {
        if (!dflt) return NULL;
        *used_default = true;
        return ast_clone(dflt->body);
    }
    Node *c = mem[i];
    Token *t = c->tok;

    // ① この case の名前を読み替える
    Rename *rn = NULL;
    int k = 0;
    Node *pat = c->lhs;
    int nargs = count_args(pat);
    Node **argv = nargs ? xmalloc(sizeof(Node *) * (size_t)nargs) : NULL;
    if (pat && pat->kind == ND_METHOD)
        for (Node *a = pat->args; a; a = a->next, k++) {
            argv[k] = a;
            if (a->kind == ND_VAR) {
                Rename *r = xmalloc(sizeof(Rename));
                r->from = a->name;
                r->to = hid[k];
                r->tok = a->tok;
                r->next = rn;
                rn = r;
            } else {
                rename_pattern(s, a, &rn);
            }
        }

    // ② 後ろの case（落ち先）
    Fallback fb = {refine_chain(s, mem, nmem, i + 1, dflt, hid, used_default), 0};

    // ③ 当たったときの本体：利用者の名前を宣言してから本体
    //   注意: 表は先頭に積んだので、書いた順に戻して並べます。
    Node head = {0};
    Node *cur = &head;
    Rename *rev = NULL;
    for (Rename *r = rn; r;) {
        Rename *nx = r->next;
        r->next = rev;
        rev = r;
        r = nx;
    }
    rn = rev;
    for (Rename *r = rn; r; r = r->next) {
        cur->next = hidden_var(r->tok, r->from, new_var_node(r->tok, r->to));
        cur = cur->next;
    }
    cur->next = c->body;
    Node *inner = new_block_of(t, head.next);

    // ④ ガード（いちばん内側）
    if (c->rhs) {
        rename_expr(c->rhs, rn);
        Node *iff = new_node(ND_IF, c->rhs->tok);
        iff->lhs = c->rhs;
        iff->body = inner;
        iff->els = fb_take(&fb);
        if (!iff->els) {
            Diag d = {0};
            d.message = MSG0("sema.617", "ガードが外れたときに当たる case がありません");
            d.primary.tok = c->rhs->tok;
            d.primary.label = MSG0("sema.618", "この条件が偽のとき、どこへも行けません");
            d.hint = MSG0("sema.619", "後ろに同じ枝の case か case _: を書いてください");
            diag_fail(&d);
        }
        inner = new_block_of(t, iff);
    }

    // ⑤ 入れ子のパターン（後ろから包む）
    for (int j = nargs - 1; j >= 0; j--) {
        Node *a = argv[j];
        if (a->kind == ND_VAR) continue;
        a->next = NULL;
        Node *m = new_node(ND_MATCH, a->tok);
        m->lhs = new_var_node(a->tok, hid[j]);
        Node *cp = new_node(ND_CASE, a->tok);
        cp->lhs = a;
        cp->body = inner;
        Node *rest = fb_take(&fb);
        if (rest) {
            Node *cd = new_node(ND_CASE, a->tok);
            cd->body = rest;
            cd->is_fallback = true;
            cp->next = cd;
        }
        m->body = cp;
        inner = new_block_of(t, m);
    }
    return inner;
}

static void lower_match_refine(Sema *s, Node *n) {
    bool any = false;
    Node *dflt = NULL;
    int ncase = 0;
    for (Node *c = n->body; c; c = c->next) {
        if (!c->lhs) {
            // 注意: case _ が最後でないときは、何もしないで今までの検査に断らせます
            if (c->next) return;
            dflt = c;
            continue;
        }
        ncase++;
        if (case_refutable(c)) any = true;
    }
    if (!any) return;

    Node **cs = xmalloc(sizeof(Node *) * (size_t)ncase);
    int k = 0;
    for (Node *c = n->body; c; c = c->next)
        if (c->lhs) cs[k++] = c;
    bool *done = xmalloc(sizeof(bool) * (size_t)ncase);
    for (int i = 0; i < ncase; i++) done[i] = false;

    bool used_default = false;
    Node head = {0};
    Node *cur = &head;
    for (int i = 0; i < ncase; i++) {
        if (done[i]) continue;
        // 同じ枝（同じ値）の case を、書いた順に集める
        Node **mem = xmalloc(sizeof(Node *) * (size_t)ncase);
        int nmem = 0;
        for (int j = i; j < ncase; j++) {
            if (done[j] || !same_case_key(cs[i]->lhs, cs[j]->lhs)) continue;
            // ★ 外れない case の後ろの同じ枝は、決して選ばれません
            if (nmem > 0 && !case_refutable(mem[nmem - 1])) {
                Diag d = {0};
                d.message = MSG0("sema.533", "同じ値の case が 2 つあります");
                d.primary.tok = cs[j]->lhs->tok;
                d.primary.label = MSG0("sema.534", "2 つめは決して選ばれません");
                d.related.tok = mem[nmem - 1]->lhs->tok;
                d.related.label = MSG0("sema.535", "最初の case はここです");
                diag_fail(&d);
            }
            // 中身の数は同じでなければなりません（枝の定義との照合は後で行います）
            if (nmem > 0 && count_args(cs[j]->lhs) != count_args(mem[0]->lhs)) {
                Diag d = {0};
                d.message = MSG0("sema.620", "同じ枝の case で、中身の数が違います");
                d.primary.tok = cs[j]->lhs->tok;
                d.primary.label = MSG0("sema.512", "中身の数が合っていません");
                d.related.tok = mem[0]->lhs->tok;
                d.related.label = MSG0("sema.535", "最初の case はここです");
                diag_fail(&d);
            }
            mem[nmem++] = cs[j];
            done[j] = true;
        }

        // まとめた case：中身は隠し変数で受け取る
        Node *first = mem[0];
        int nargs = count_args(first->lhs);
        char **hid = nargs ? xmalloc(sizeof(char *) * (size_t)nargs) : NULL;
        for (int j = 0; j < nargs; j++) hid[j] = sema_hidden(s, "match.h");
        Node *mc = new_node(ND_CASE, first->tok);
        if (first->lhs->kind == ND_METHOD) {
            Node *pat = new_node(ND_METHOD, first->lhs->tok);
            pat->lhs = first->lhs->lhs;
            pat->name = first->lhs->name;
            pat->mod_name = first->lhs->mod_name;
            Node ah = {0};
            Node *at = &ah;
            int j = 0;
            for (Node *a = first->lhs->args; a; a = a->next, j++) {
                at->next = new_var_node(a->tok, hid[j]);
                at = at->next;
            }
            pat->args = ah.next;
            mc->lhs = pat;
        } else {
            mc->lhs = first->lhs;
        }
        mc->body = refine_chain(s, mem, nmem, 0, dflt, hid, &used_default);
        cur->next = mc;
        cur = mc;
    }
    // ★ 落ち先に使った case _ は届きます（枝を全部書いてあっても断りません）
    if (dflt) {
        if (used_default) dflt->is_fallback = true;
        cur->next = dflt;
    }
    n->body = head.next;
}

static void lower_match_payload(Sema *s, Node *n, EnumDef *e) {
    Token *t = n->tok;

    // ① 調べる式を 1 回だけ評価して、隠し変数に入れる
    char *mname = sema_hidden(s, "match.v");
    Node *decl = new_node(ND_VARDECL, t);
    decl->name = mname;
    decl->rhs = n->lhs;

    // ② それぞれの case を「タグの比較 ＋ 中身の束縛」にする
    for (Node *c = n->body; c; c = c->next) {
        if (!c->lhs) continue;                  // case _
        Node *pat = c->lhs;
        EnumVal *v = pattern_branch(s, pat, e);
        bind_pattern(s, c, pat, e, v, mname);
        c->lhs = new_int_node(pat->tok, v->val);   // 比べるのはタグだけ
    }

    // ③ match を「タグを調べる match」にして、宣言と並べる
    Node *m = new_node(ND_MATCH, t);
    Node *tagn = new_node(ND_ENUMTAG, t);
    tagn->lhs = new_var_node(t, mname);
    m->lhs = tagn;
    m->body = n->body;
    m->en = e;                                  // 網羅の検査に使います

    decl->next = m;

    n->kind = ND_BLOCK;
    n->lhs = NULL;
    n->body = decl;
    n->en = NULL;
}

// ── for の脱糖（A-39）───────────────────────────────────
//
// ★ **脱糖はここでします。** 0.1.0 からずっとパーサでやっていましたが、
//   利用者のクラスを回せるようにすると、**型が決まるまで形が決まりません**。
//
//     対象が list / str  → 添字で回す（今までと同じ形。IR は変わりません）
//     規約を持つクラス    → カーソルで回す
//
//   for x in xs:          for.it.N = xs            ← 対象は 1 回だけ評価
//       BODY        →     for.ix.N: int = 0
//                         while for.ix.N < len(for.it.N):
//                             x = for.it.N[for.ix.N]
//                             BODY
//                           incr: for.ix.N += 1    ← continue の飛び先
//
//   for x in c:           for.it.N = c
//       BODY        →     for.ix.N: int = for.it.N.__first__()
//                         while for.ix.N >= 0:
//                             x = for.it.N.__get__(for.ix.N)
//                             BODY
//                           incr: for.ix.N = for.it.N.__next__(for.ix.N)
//
// ★ カーソルは **int だけ**です。「イテレータ物体」を作る形にしません。
//   この言語は**借用を構造体に保存できない**ので（寿命注釈を入れないと
//   決めたため）、容器を指すイテレータを作るには容器を rc[T] にするか、
//   寿命を書かせることになります。カーソルなら、容器は借りたままで、
//   ループが持つのは int 1 つです。
//
// ★ 規約は 3 つのメソッドです（どれか 1 つでも欠けたら断ります）。
//     def __first__(self) -> int          最初のカーソル（無ければ -1）
//     def __next__(self, cur: int) -> int 次のカーソル（無ければ -1）
//     def __get__(self, cur: int) -> T    そのカーソルの要素
//   mut self で書けば「流れてくるもの」（行・受信）も表せます。

// 規約のメソッドを 1 つ引く（無ければ NULL）
static FuncSig *iter_method(Sema *s, Class *c, const char *name) {
    return lookup_func_in(c->owner, mangle(c->name, name));
}

// 規約どおりの形か確かめる
static void check_iter_sig(FuncSig *f, const char *name, int nparams,
                           Type *ret, Token *at, Class *c) {
    bool ok = f->nparams == nparams && (!ret || type_equal(f->ret, ret));
    // 第 2 引数（カーソル）は int
    if (ok && nparams == 2) ok = f->params[1]->kind == TY_INT;
    if (ok) return;

    Diag d = {0};
    d.message = MSG2("sema.263", "'{0}.{1}' の形が for の規約と違います", c->name, name);
    d.primary.tok = at;
    d.primary.label = MSG0("sema.518", "この for がその規約を使います");
    d.related.tok = f->tok;
    d.related.label = MSG0("sema.519", "このメソッドです");
    d.hint = strcmp(name, "__get__") == 0
                 ? MSG0("sema.520", "def __get__(self, cur: int) -> T の形で書いてください")
                 : MSG2("sema.264", "def {0}({1}) -> int の形で書いてください", name, nparams == 2 ? "self, cur: int" : "self");
    diag_fail(&d);
}

// 隠し変数の宣言（型注釈なし。型は右辺から決まります）
static Node *hidden_var(Token *tok, char *name, Node *init) {
    Node *n = new_node(ND_VARDECL, tok);
    n->name = name;
    n->rhs = init;
    return n;
}

// 対象.名前(引数…) のメソッド呼び出しを組み立てる
static Node *iter_call(Token *tok, char *obj, const char *name, Node *arg) {
    Node *m = new_node(ND_METHOD, tok);
    m->lhs = new_var_node(tok, obj);
    m->name = (char *)name;
    m->args = arg;
    return m;
}

static void check_foreach(Sema *s, Node *n) {
    Token *t = n->tok;
    Node *iter = n->lhs;
    Node *body = n->body;

    // ★ 対象の型を先に決めます（形がこれで決まります）。
    //   注意: この式はこのあと組み立てる木の中でもう一度検査されます。
    //     検査は同じ結果になる（べき冪等な）ものだけなので、問題ありません。
    Type *ct = auto_deref(check_expr(s, iter));

    Node head = {0};
    Node *cur = &head;

    // for.it.N = <対象>（★ 1 回だけ評価する）
    cur->next = hidden_var(t, n->hid_obj, iter);
    cur = cur->next;

    Node *cond = NULL;
    Node *bind = NULL;
    Node *inc = NULL;

    if (ct->kind == TY_CLASS) {
        Class *c = ct->cls;
        FuncSig *first = iter_method(s, c, "__first__");
        FuncSig *next = iter_method(s, c, "__next__");
        FuncSig *get = iter_method(s, c, "__get__");
        if (!first || !next || !get) {
            Diag d = {0};
            d.message = MSG1("sema.265", "クラス '{0}' は for で回せません", c->name);
            d.primary.tok = iter->tok;
            d.primary.label = MSG1("sema.156", "'{0}' 型です", type_name(ct));
            d.related.tok = c->tok;
            d.related.label = MSG0("sema.444", "クラスの定義はここです");
            d.hint = MSG0("sema.521", "for で回すには 3 つのメソッドが要ります:\n             def __first__(self) -> int          最初のカーソル（無ければ -1）\n             def __next__(self, cur: int) -> int 次のカーソル（無ければ -1）\n             def __get__(self, cur: int) -> T    そのカーソルの要素");
            diag_fail(&d);
        }
        check_iter_sig(first, "__first__", 1, ty_int, t, c);
        check_iter_sig(next, "__next__", 2, ty_int, t, c);
        check_iter_sig(get, "__get__", 2, NULL, t, c);

        // for.ix.N: int = for.it.N.__first__()
        cur->next = hidden_var(t, n->hid_cur,
                               iter_call(t, n->hid_obj, "__first__", NULL));
        cur = cur->next;

        // for.ix.N >= 0
        cond = new_binop_node(t, OP_GE, new_var_node(t, n->hid_cur),
                              new_int_node(t, 0));

        // x = for.it.N.__get__(for.ix.N)
        bind = hidden_var(t, n->name,
                          iter_call(t, n->hid_obj, "__get__",
                                    new_var_node(t, n->hid_cur)));

        // for.ix.N = for.it.N.__next__(for.ix.N)  ← continue の飛び先
        inc = new_node(ND_ASSIGN, t);
        inc->lhs = new_var_node(t, n->hid_cur);
        inc->rhs = iter_call(t, n->hid_obj, "__next__",
                             new_var_node(t, n->hid_cur));
    } else if (ct->kind == TY_LIST || ct->kind == TY_STR) {
        // for.ix.N: int = 0
        cur->next = hidden_var(t, n->hid_cur, new_int_node(t, 0));
        cur = cur->next;

        // for.ix.N < len(for.it.N)
        Node *lencall = new_node(ND_CALL, t);
        lencall->name = "len";
        lencall->args = new_var_node(t, n->hid_obj);
        cond = new_binop_node(t, OP_LT, new_var_node(t, n->hid_cur), lencall);

        // x = for.it.N[for.ix.N]
        Node *idx = new_node(ND_INDEX, t);
        idx->lhs = new_var_node(t, n->hid_obj);
        idx->rhs = new_var_node(t, n->hid_cur);
        bind = hidden_var(t, n->name, idx);

        // for.ix.N += 1  ← continue の飛び先
        inc = new_node(ND_ASSIGN, t);
        inc->lhs = new_var_node(t, n->hid_cur);
        inc->rhs = new_binop_node(t, OP_ADD, new_var_node(t, n->hid_cur),
                                  new_int_node(t, 1));
    } else {
        Diag d = {0};
        d.message = MSG1("sema.266", "'{0}' 型は for で回せません", type_name(ct));
        d.primary.tok = iter->tok;
        d.primary.label = MSG0("sema.522", "ここは回せる形ではありません");
        d.hint = MSG0("sema.523", "回せるのは list / str / range(...) と、__first__ / __next__ / __get__ を持つクラスです");
        diag_fail(&d);
    }

    // 本体の先頭にループ変数の束縛を差し込む
    bind->next = body->body;
    body->body = bind;

    Node *wh = new_node(ND_WHILE, t);
    wh->lhs = cond;
    wh->body = body;
    wh->incr = inc;
    cur->next = wh;

    // ★ 隠し変数を for のスコープに閉じ込めるため、ブロックにします。
    //   **このノード自身を書き換えます**（親から見た並びを崩さないため）。
    n->kind = ND_BLOCK;
    n->name = NULL;
    n->lhs = NULL;
    n->body = head.next;
}

static void check_stmt(Sema *s, Node *n) {
    switch (n->kind) {
        case ND_VARDECL: check_vardecl(s, n); break;
        case ND_UNPACK: check_unpack(s, n); break;
        case ND_ASSIGN: check_assign(s, n); break;
        case ND_BLOCK: check_block(s, n); break;
        case ND_RETURN: check_return(s, n); break;
        case ND_PASS: break;  // 何もしない

        case ND_IF: {
            check_cond(s, MSG0("parse.171", "if の条件"), n, n->lhs);

            // ★ then 節では条件が成り立っている
            NarrowSet ns = {0};
            narrow_apply(s, n->lhs, true, &ns);
            check_block(s, n->body);
            narrow_restore(&ns);

            // els は ND_BLOCK（else）か ND_IF（elif の脱糖結果）。
            // else 節では条件が成り立っていない（if t is None: の反対側）。
            if (n->els) {
                NarrowSet es = {0};
                narrow_apply(s, n->lhs, false, &es);
                check_stmt(s, n->els);
                narrow_restore(&es);
            }
            break;
        }

        // ── 場合分け（A-37）──────────────────────────────
        //
        // ★ 見るのは 4 つです。
        //     ① 調べる式の型（列挙 / int / str だけ）
        //     ② どの case の値も同じ型か
        //     ③ 同じ値を 2 回書いていないか
        //     ④ **枝が全部あるか**（列挙）／ case _ があるか（int / str）
        //
        // ★ ④ が本体です。枝を足したとき「直す場所をコンパイラに
        //   挙げさせる」ためにこの機能を入れました。網羅を確かめないなら
        //   if の連なりと同じで、入れる意味がありません。
        case ND_MATCH: {
            Type *st = auto_deref(check_expr(s, n->lhs));

            // ★ ガードと入れ子のパターン（A-48）。同じ枝の case をまとめます
            lower_match_refine(s, n);

            // ★ 中身を持つ列挙（A-41）。**タグで調べる形に書き換えて**から
            //   検査し直します。書き換えたあとは今までの match と同じ形です。
            if (st->kind == TY_ENUM && st->en->has_payload) {
                lower_match_payload(s, n, st->en);
                check_stmt(s, n);     // 書き換えた自分（ND_BLOCK）を検査する
                break;
            }

            // ★ タグを調べる match（A-41 が作った形）は、列挙として扱います。
            EnumDef *men = st->kind == TY_ENUM ? st->en : n->en;
            bool is_enum = men != NULL;
            if (!is_enum && st->kind != TY_INT && st->kind != TY_STR) {
                Diag d = {0};
                d.message = MSG1("sema.267", "'{0}' は match で調べられません", type_name(st));
                d.primary.tok = n->lhs->tok;
                d.primary.label = MSG0("sema.524", "ここに書けるのは 列挙 / int / str です");
                d.hint = MSG0("sema.525", "クラスの場合分けはインタフェースで書きます（どの枝かを型が持ちます）");
                diag_fail(&d);
            }
            if (is_enum) n->en = men;

            // 注意: **枝を数えるのに固定長の配列を使いません。** 枝の数に
            //   上限を設ける理由がありません。
            bool has_default = false;
            Node *default_at = NULL;

            for (Node *c = n->body; c; c = c->next) {
                if (!c->lhs) {
                    // ★ `case _`。**2 つ書けません**し、**最後でなければ
                    //   なりません**（後ろの case は決して選ばれないため）。
                    if (has_default) {
                        Diag d = {0};
                        d.message = MSG0("sema.526", "case _ が 2 つあります");
                        d.primary.tok = c->tok;
                        d.primary.label = MSG0("sema.516", "2 つめです");
                        d.related.tok = default_at->tok;
                        d.related.label = MSG0("sema.527", "最初の case _ はここです");
                        diag_fail(&d);
                    }
                    has_default = true;
                    default_at = c;
                    if (c->next)
                        error_at_hint_m(c->next->tok, MSG0("sema.269", "case _ より後ろの case は決して選ばれません"), MSG0("sema.268", "この case は届きません"));
                    check_block(s, c->body);
                    continue;
                }
                if (has_default) UNREACHABLE();  // 上で断っている

                Type *ct = check_expr(s, c->lhs);
                if (!type_equal(ct, st)) {
                    Diag d = {0};
                    d.message = MSG2("sema.270", "case の値の型が違います（'{0}' を調べているのに '{1}' です）", type_name(st), type_name(ct));
                    d.primary.tok = c->lhs->tok;
                    d.primary.label = MSG1("sema.271", "ここは '{0}' です", type_name(ct));
                    d.hint = MSG0("sema.528", "match は暗黙の変換をしません（言語全体と同じです）");
                    diag_fail(&d);
                }

                // ★ **値がコンパイル時に決まらないものは断ります。**
                //   変数を書けるようにすると、上から順に比べるだけの
                //   「if の連なり」と同じになり、網羅を確かめられません。
                if (c->lhs->kind != ND_INT && c->lhs->kind != ND_STR) {
                    Diag d = {0};
                    d.message = MSG0("sema.529", "case には決まった値を書きます");
                    d.primary.tok = c->lhs->tok;
                    d.primary.label = MSG0("sema.530", "ここはコンパイル時に決まりません");
                    d.hint = is_enum
                        ? MSG0("sema.531", "列挙の枝（例: Color.Red）を書いてください")
                        : MSG0("sema.532", "リテラルを書いてください（変えられる値は if で比べます）");
                    diag_fail(&d);
                }

                // ★ 同じ値を 2 回書いていないか。通すと、2 つめは決して
                //   選ばれないのに黙って通ります。
                for (Node *q = n->body; q != c; q = q->next) {
                    if (!q->lhs) continue;
                    bool same = c->lhs->kind == ND_INT
                        ? q->lhs->kind == ND_INT && q->lhs->ival == c->lhs->ival
                        : q->lhs->kind == ND_STR &&
                          q->lhs->slen == c->lhs->slen &&
                          memcmp(q->lhs->sval, c->lhs->sval,
                                 (size_t)c->lhs->slen) == 0;
                    if (same) {
                        Diag d = {0};
                        d.message = MSG0("sema.533", "同じ値の case が 2 つあります");
                        d.primary.tok = c->lhs->tok;
                        d.primary.label = MSG0("sema.534", "2 つめは決して選ばれません");
                        d.related.tok = q->lhs->tok;
                        d.related.label = MSG0("sema.535", "最初の case はここです");
                        diag_fail(&d);
                    }
                }

                check_block(s, c->body);
            }

            // ── ④ 網羅 ──
            if (is_enum) {
                if (!has_default) {
                    StrBuf missing;
                    sb_init(&missing);
                    int nmiss = 0;
                    for (EnumVal *v = men->vals; v; v = v->next) {
                        bool found = false;
                        for (Node *c = n->body; c && !found; c = c->next)
                            if (c->lhs && c->lhs->ival == v->val) found = true;
                        if (!found)
                            sb_printf(&missing, "%s%s.%s", nmiss++ ? " / " : "",
                                      men->name, v->name);
                    }
                    if (nmiss) {
                        Diag d = {0};
                        d.message = MSG1("sema.272", "match に書いていない枝があります: {0}", sb_str(&missing));
                        d.primary.tok = n->tok;
                        d.primary.label = MSG0("sema.536", "ここで全部の枝を扱ってください");
                        d.related.tok = men->tok;
                        d.related.label = MSG0("sema.440", "列挙の定義はここです");
                        d.hint = MSG0("sema.537", "どれにも当たらないときの動きが要るなら case _: を書いてください");
                        diag_fail(&d);
                    }
                    // ★ 全部書いてあるなら case _ は要りません（書くと
                    //   「決して選ばれない case」になるので、上で断ります）。
                } else {
                    // 枝を全部書いたうえでの case _ は届きません。
                    int ncase = 0;
                    for (Node *c = n->body; c; c = c->next)
                        if (c->lhs) ncase++;
                    if (ncase == men->nvals && !default_at->is_fallback)
                        error_at_hint_m(default_at->tok, MSG0("sema.274", "枝を全部書いてあるので case _ は届きません"), MSG0("sema.273", "この case は選ばれません"));
                }
            } else if (!has_default) {
                // ★ int / str は値が無限にあるので、網羅を静的に示せません。
                //   **case _ を必須にします** — 無いと「どれにも当たらない」
                //   ときの動きが書かれていないことになります。
                Diag d = {0};
                d.message = MSG1("sema.275", "'{0}' の match には case _ が要ります", type_name(st));
                d.primary.tok = n->tok;
                d.primary.label = MSG0("sema.538", "どれにも当たらないときの動きがありません");
                d.hint = MSG0("sema.539", "最後に case _: を書いてください（値が無限にあるので、全部を書き尽くせません）");
                diag_fail(&d);
            }
            break;
        }

        // ★ for は while へ書き換えてから検査します（A-39）
        case ND_FOREACH:
            check_foreach(s, n);
            check_stmt(s, n);     // 書き換えた自分（ND_BLOCK）を検査する
            break;

        case ND_WHILE: {
            check_cond(s, MSG0("parse.178", "while の条件"), n, n->lhs);
            s->loop_depth++;

            // ★ 本体に入れたということは条件が成り立っている。
            //   while cur is not None: … 連結リストの走査に必須です。
            NarrowSet ns = {0};
            narrow_apply(s, n->lhs, true, &ns);
            check_block(s, n->body);
            if (n->incr) check_stmt(s, n->incr);  // for の増分
            narrow_restore(&ns);

            s->loop_depth--;
            break;
        }

        // ── pragma（設定。検査するのは名前だけ）──
        case ND_PRAGMA: {
            bool is_target = strcmp(n->name, "target") == 0;
            bool is_no_rt = strcmp(n->name, "no_runtime") == 0;
            if (!is_target && !is_no_rt)
                error_at_hint_m(n->tok, MSG0("sema.277", "いま使える pragma は target と no_runtime です"), MSG1("sema.276", "未知の pragma '{0}' です", n->name));
            if (is_target && !n->sval)
                error_at_hint_m(n->tok, MSG0("sema.279", "pragma target \"riscv64-unknown-elf\" の形で書きます"), MSG0("sema.278", "pragma target には文字列が必要です"));
            break;
        }

        // ── 契約（事前条件・事後条件。A-29）──
        //
        // ★ 置ける場所は**関数の本体の先頭**だけです（check_func が確かめます）。
        //   ここでは「bool か」と、ensures の中の 'result' を見ます。
        case ND_REQUIRES:
        case ND_ENSURES: {
            if (n->kind == ND_ENSURES) s->ensures_depth++;
            Type *t = check_expr(s, n->lhs);
            if (n->kind == ND_ENSURES) s->ensures_depth--;
            if (n->kind == ND_ENSURES) check_old_types(n->lhs);
            if (t->kind != TY_BOOL) {
                Diag d = {0};
                d.message = MSG1("sema.280", "{0} には bool の式を書きます", n->kind == ND_REQUIRES ? "requires" : "ensures");
                d.primary.tok = n->lhs->tok;
                d.primary.label = MSG1("sema.051", "これは '{0}' 型です", type_name(t));
                d.hint = MSG0("sema.540", "比べる式を書いてください（例: requires b != 0）");
                diag_fail(&d);
            }
            break;
        }

        // ── scope: ブロック（A-18 の scoped spawn）──
        //
        // ★ 型としては何もしません。意味を与えるのは ownck（借りを渡せる）と
        //   codegen（出口で join する）です。
        case ND_SCOPE:
            s->scope_depth++;
            check_block(s, n->body);
            s->scope_depth--;
            // 注意: 検査は**型検査のあと**です。can_fail は検査中に付くためです。
            scope_escape(s, n->body, 0, 0);
            break;

        // ── unsafe: ブロック ──
        case ND_UNSAFE:
            s->unsafe_depth++;
            check_block(s, n->body);
            s->unsafe_depth--;
            break;

        // ── try / except ──
        case ND_TRY: {
            // ① except の型を先に解決する（本体を見るときに「捕まえられるか」が要る）
            for (Node *ex = n->els; ex; ex = ex->next) {
                Type *t = resolve_type(s, ex->type_ref);
                if (t->kind != TY_CLASS) {
                    Diag d = {0};
                    d.message = MSG1("sema.281", "'{0}' はエラー型として使えません", type_name(t));
                    d.primary.tok = ex->tok;
                    d.primary.label = MSG0("sema.541", "except に書けるのはクラスだけです");
                    d.hint = MSG0("sema.542", "エラーはふつうのクラスとして定義してください");
                    diag_fail(&d);
                }
                ex->type = t;
                ex->err_tag = err_tag_of(s, t->cls);
            }

            // ② try の本体（この間に起きた失敗は、この try が受け止める）
            TryCtx ctx = {n, s->cur_try};
            s->cur_try = &ctx;
            check_block(s, n->body);
            s->cur_try = ctx.outer;

            // ③ except の本体（as で束縛する変数は、その節の中だけ）
            for (Node *ex = n->els; ex; ex = ex->next) {
                scope_push(s);
                if (ex->name) {
                    VarEntry *v = declare(s, ex->name, ex->type, ex->tok);
                    ex->ir_name = v->ir_name;
                }
                check_stmt_list(s, ex->body->body);
                scope_pop(s);
            }

            // ④ 何も捕まえていない try は、書いた人の勘違い（仕様 §8 の R5）
            if (!n->can_fail) {
                Diag d = {0};
                d.severity = "warning";
                d.code = "E-RAISE-5";
                d.message = MSG0("sema.543", "この try の中に、失敗しうる呼び出しがありません");
                d.primary.tok = n->tok;
                d.primary.label = MSG0("sema.544", "except は決して実行されません");
                d.hint = MSG0("sema.545", "raises を宣言した関数を呼んでいるか確かめてください");
                diag_emit(&d);
            }
            break;
        }

        case ND_RAISE: {
            Type *t = check_expr(s, n->lhs);
            if (t->kind != TY_CLASS) {
                Diag d = {0};
                d.message = MSG1("sema.282", "'{0}' は raise できません", type_name(t));
                d.primary.tok = n->lhs->tok;
                d.primary.label = MSG0("sema.546", "raise にはエラーオブジェクトを渡します");
                d.hint = MSG0("sema.547", "エラーはふつうのクラスです（例: raise IOError(\"見つかりません\")）");
                diag_fail(&d);
            }

            // ★ 呼び出しと同じ判定：try で捕まえるか、自分の raises に書いてあるか
            if (!try_catches(s, t->cls) && !func_declares(s->cur_func, t->cls)) {
                Diag d = {0};
                d.primary.tok = n->tok;
                if (s->cur_func && s->cur_func->nraises == 0) {
                    d.code = "E-RAISE-1";
                    d.message = MSG1("sema.283", "'{0}' を raise していますが、宣言がありません", t->cls->name);
                    d.primary.label = MSG0("sema.548", "この関数は失敗しないと宣言されています");
                    d.hint = MSG1("sema.284", "関数の宣言に 'raises {0}' を足してください", t->cls->name);
                } else {
                    d.code = "E-RAISE-2";
                    d.message = MSG1("sema.237", "エラー '{0}' が宣言されていません", t->cls->name);
                    d.primary.label = MSG0("sema.549", "raises に含まれていません");
                    d.hint = MSG1("sema.285", "この関数の raises に '{0}' を足してください", t->cls->name);
                }
                d.related.tok = s->cur_func ? s->cur_func->tok : NULL;
                d.related.label = MSG0("sema.550", "関数の宣言はここです");
                diag_fail(&d);
            }
            n->err_tag = err_tag_of(s, t->cls);
            n->type = t;
            break;
        }

        case ND_BREAK:
        case ND_CONTINUE: {
            // ★ ここで弾いておけば、codegen は「飛び先が必ずある」と仮定できます。
            //   確立した「codegen は検査済みの AST だけを受け取る」
            if (s->loop_depth > 0) break;
            const char *kw = n->kind == ND_BREAK ? "break" : "continue";
            Diag d = {0};
            d.message = MSG1("sema.286", "'{0}' はループの外では使えません", kw);
            d.primary.tok = n->tok;
            d.primary.label = MSG1("sema.287", "この '{0}' を囲む while がありません", kw);
            d.hint = MSG1("sema.288", "'{0}' は while の中でだけ使えます", kw);
            diag_fail(&d);
            break;
        }

        default: check_expr(s, n); break;  // 式文
    }
}

// ── 入口 ───────────────────────────────────────────────────

// この while から抜ける break があるか。
//
// 注意: 入れ子のループの中には降りません。そこの break は内側のループのものです。
//    if の中には降ります（break は条件付きで書くのが普通なので）。
static bool has_break(Node *n) {
    if (!n) return false;

    switch (n->kind) {
        case ND_BREAK:
            return true;

        case ND_WHILE:
            return false;  // ★ 内側のループの break は、こちらには効かない

        case ND_IF:
            return has_break(n->body) || has_break(n->els);

        case ND_BLOCK:
            for (Node *st = n->body; st; st = st->next)
                if (has_break(st)) return true;
            return false;

        default:
            return false;
    }
}

// 戻らない組み込み（panic / exit）の呼び出しか。
//
// ★ ランタイム側で _Noreturn が付いている 2 つと、ここの判定は対になっています。
//   片方だけ変えると「sema は通すのに実行時には戻ってくる」ことになります。
static bool never_returns_call(Node *n) {
    if (n->kind != ND_CALL || !n->builtin) return false;
    return strcmp(n->builtin->impl, "pl_panic") == 0 ||
           strcmp(n->builtin->impl, "pl_exit") == 0;
}

// この文を実行したら、必ず関数から抜けるか（型システム 6.1）。
//
// 注意: 保守的に判定します。「実際には到達しない」経路でも return を要求します。
//    コンパイラが人間より賢くなろうとすると必ず破綻します。
//
// codegen の e->terminated と同じことを、別の場所でやっています。
//    こちらは AST の上（構造を見る／ユーザーに教えるため）、
//    あちらは命令列の上（出力を見る／正しい IR を出すため）。
static bool always_returns(Node *n) {
    // ★ raise はその経路を終わらせます（呼び出し元へ戻る）。
    if (n && n->kind == ND_RAISE) return true;
    if (!n) return false;

    switch (n->kind) {
        case ND_RETURN:
            return true;

        case ND_IF:
            // else が無ければ、条件が偽のときに素通りする
            return n->els && always_returns(n->body) && always_returns(n->els);

        case ND_BLOCK:
            // 1 つでも「必ず抜ける」文があればよい（その後ろは到達不能）
            for (Node *st = n->body; st; st = st->next)
                if (always_returns(st)) return true;
            return false;

        case ND_WHILE:
            // ★ while True: は break が無ければ抜けない。
            //   条件が「True というリテラルそのもの」のときだけ見ます。
            //   変数や式は追いません（保守的でよい）。
            return n->lhs && n->lhs->kind == ND_BOOL && n->lhs->ival != 0 &&
                   !has_break(n->body);

        // ★ match は「どの case も抜けるなら」抜けます（A-37）。
        //
        //   注意: **これが無いと、全部の case で return しているのに
        //     「値を返さない経路があります」と言われます**（try で同じ穴を
        //     踏みました。0.17.0 の記録を参照）。
        //
        //   注意: 素通りする経路が残っていないと言えるのは、
        //     **列挙で網羅されている**か **case _ がある**ときだけです。
        //     どちらでもなければ（＝ sema が通していない形）保守的に false。
        case ND_MATCH: {
            bool has_default = false;
            for (Node *c = n->body; c; c = c->next) {
                if (!always_returns(c->body)) return false;
                if (!c->lhs) has_default = true;
            }
            // 列挙は網羅を強制済み（check_stmt）。int / str は case _ 必須。
            return has_default || (n->en != NULL);
        }

        case ND_TRY:
            // ★ try の中身と、**すべての** except が抜けるなら、この try は抜けます。
            //
            //   注意: else が無い if と同じ話です。1 つでも素通りする except が
            //     あれば、そこから下へ落ちます。
            //
            //   注意: どの except にも当たらないエラーは呼び出し元へ伝播する
            //     ので、**これも「抜ける」ほうに数えます**（伝播を except の
            //     漏れと混同しないこと）。
            //
            //   except の並びは els に next で繋がっています（if の else と
            //     違って複数あるので、リストとしてたどります）。
            if (!always_returns(n->body)) return false;
            for (Node *ex = n->els; ex; ex = ex->next)
                if (!always_returns(ex->body)) return false;
            return true;

        case ND_CALL:
            // ★ panic() / exit() を呼んだら、その先へは進まない。
            return never_returns_call(n);

        default:
            return false;
    }
}

// ── パス 1：宣言の登録 ─────────────────────────────────────

// ★ 登録が 3 段に分かれます。
//
//     1a  クラス名と Type だけ登録する      ← クラスどうしの相互参照のため
//     1b  フィールドとメソッドを解決する      ← 型注釈に他のクラスを書ける
//     1c  トップレベルの関数・グローバル変数   ← 引数の型にクラスを書ける
//
//   関数の前方参照と同じ問題を、同じ手（先に名前だけ登録）で解いています。

// 1a：クラス名と Type を作る。中身はまだ見ない。
static void declare_class(Sema *s, Node *n) {
    reject_module_name(s, n->name, n->tok, MSG0("sema.551", "クラス"));
    if (type_from_name(n->name))
        error_at_hint_m(n->tok, MSG1("sema.290", "'{0}' は組み込みの型名です", n->name), MSG1("sema.289", "クラス名 '{0}' は使えません", n->name));
    if (is_builtin_name(n->name))
        error_at_hint_m(n->tok, MSG1("sema.291", "'{0}' は組み込み関数の名前です", n->name), MSG1("sema.289", "クラス名 '{0}' は使えません", n->name));

    Class *prev = lookup_class(s, n->name);
    if (prev) {
        Diag d = {0};
        d.message = MSG1("sema.292", "クラス '{0}' は既に定義されています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.552", "ここで再定義されています");
        d.related.tok = prev->tok;
        d.related.label = MSG0("parse.224", "最初の定義はここです");
        diag_fail(&d);
    }

    Class *c = xmalloc(sizeof(Class));
    c->name = n->name;
    c->ir_name = mod_mangle(s, n->name);  // ★ %lexer.Token.type になる
    c->owner = s->cur;                    // メソッドはこのモジュールの表にいる
    c->tok = n->tok;
    c->node = n;
    c->type = type_class(n->name, c);  // ★ クラスにつき Type は 1 個だけ
    c->next = s->classes;
    s->classes = c;

    n->cls = c;
    n->type = c->type;
}

// フィールドを並べて、オフセットとサイズを決める。
//
// ★ docs/ja/design/memory-model.md 5 節の表がそのまま実装になっています。
//   注意: 読み書きに offset は使いません（getelementptr に渡すのは index）。
//      offset は「自分の計算が合っているか」を確かめるための値です。
static int align_up(int offset, int align) {
    return (offset + align - 1) / align * align;
}

static void layout_class(Class *c) {
    // ★ インタフェースを 1 つでも実装するなら、**先頭に隠しフィールド**
    //   （vtable へのポインタ）を 1 つ置きます。これがあるおかげで、
    //   クラス → インタフェースの変換が「何もしない」で済みます。
    //   注意: 利用者から見える名前は付けません（フィールドの並びには入れない）。
    int offset = c->impls ? 8 : 0;
    int max_align = c->impls ? 8 : 1;
    int index = c->impls ? 1 : 0;
    for (Field *f = c->fields; f; f = f->next) {
        int a = type_align(f->type);
        offset = align_up(offset, a);  // ★ パディングはここで入る
        f->offset = offset;
        f->index = index++;
        offset += type_size(f->type);
        if (a > max_align) max_align = a;
    }
    c->nfields = index;
    c->align = max_align;
    c->size = align_up(offset, max_align);  // 全体もアラインメントに切り上げる
}

// メソッドを FuncSig として登録する。名前は "Token.show"（名前修飾）。
static void resolve_raises(Sema *s, Node *fn, FuncSig *f);

static void declare_method(Sema *s, Class *c, Node *fn) {
    char *mname = mangle(c->name, fn->name);

    FuncSig *prev = lookup_func(s, mname);
    if (prev) {
        Diag d = {0};
        d.message = MSG1("sema.293", "メソッド '{0}' は既に定義されています", mname);
        d.primary.tok = fn->tok;
        d.primary.label = MSG0("sema.552", "ここで再定義されています");
        d.related.tok = prev->tok;
        d.related.label = MSG0("parse.224", "最初の定義はここです");
        diag_fail(&d);
    }

    Field *clash = lookup_field(c, fn->name);
    if (clash) {
        Diag d = {0};
        d.message = MSG1("sema.294", "'{0}' はフィールドと同じ名前です", fn->name);
        d.primary.tok = fn->tok;
        d.primary.label = MSG0("sema.553", "メソッド名がフィールド名と衝突しています");
        d.related.tok = clash->tok;
        d.related.label = MSG0("sema.554", "同名のフィールドはここです");
        d.hint = MSG0("sema.555", "t.f が「フィールド」か「メソッド」か決められなくなるため禁止です");
        diag_fail(&d);
    }

    Type *ret = resolve_type(s, fn->type_ref);

    int nparams = 0;
    for (Node *pm = fn->params; pm; pm = pm->next) nparams++;

    FuncSig *f = xmalloc(sizeof(FuncSig));
    f->name = mname;
    f->ret = ret;
    f->nparams = nparams;
    f->params = xmalloc(sizeof(Type *) * (size_t)nparams);
    f->pnames = xmalloc(sizeof(char *) * (size_t)nparams);
    f->pmodes = xmalloc(sizeof(ParamMode) * (size_t)nparams);
    f->defaults = xmalloc(sizeof(Node *) * (size_t)(nparams ? nparams : 1));
    f->node = fn;                  // A-43
    f->tok = fn->tok;

    int i = 0;
    for (Node *pm = fn->params; pm; pm = pm->next, i++) {
        // ★ 第 1 引数 self には型注釈がありません（parser が保証している）。
        //   そのクラスの型をここで入れます。これが「self の暗黙の型」です。
        Type *pt = pm->type_ref ? resolve_type(s, pm->type_ref) : c->type;
        if (pt->kind == TY_NONE)
            error_at_hint_m(pm->tok, MSG0("sema.296", "None 型の値は存在しないので引数にできません"), MSG0("sema.295", "引数の型に None は使えません"));
        f->params[i] = pt;
        f->pnames[i] = pm->name;
        f->pmodes[i] = pm->mode;   // A-21e
        f->defaults[i] = pm->rhs;  // A-38（無ければ NULL）
        pm->type = pt;
    }
    check_defaults(s, f, fn);

    // コンストラクタ init は値を返せない（生成した自分自身が返るため）
    if (strcmp(fn->name, "init") == 0) {
        if (ret->kind != TY_NONE)
            error_at_hint_m(fn->tok, MSG0("sema.298", "init は戻り値を持てません（-> None と書いてください）"), MSG0("sema.297", "init の戻り型は None でなければなりません"));
        c->has_init = true;
    }

    resolve_raises(s, fn, f);
    if (strcmp(fn->name, "init") == 0 && f->nraises)
        error_at_hint_m(fn->tok, MSG0("sema.300", "init は失敗できません（生成に失敗した値は誰も受け取れません）"), MSG0("sema.299", "init に raises は書けません"));

    // ★ drop はデストラクタです（仕様 §6.2）。解放のときに codegen が
    //   **self だけを渡して**呼ぶので、形が違うと引数の数が合わないまま呼ばれ、
    //   メモリを壊します（0.45.0 までは形を確かめていませんでした。E-DROP-1）。
    if (strcmp(fn->name, "drop") == 0 &&
        (nparams != 1 || ret->kind != TY_NONE || f->nraises)) {
        Diag d = {0};
        d.code = "E-DROP-1";
        d.message = MSG0("sema.556", "drop はデストラクタです。形は 'def drop(self) -> None' か 'def drop(mut self) -> None' だけです");
        d.primary.tok = fn->tok;
        d.primary.label = nparams != 1        ? MSG0("sema.557", "引数を取れません（解放のときは self だけで呼ばれます）")
                          : ret->kind != TY_NONE ? MSG0("sema.558", "値を返せません（解放のときに受け取る相手がいません）")
                                                 : MSG0("sema.559", "失敗できません（解放の途中の失敗は誰も受け取れません）");
        d.hint = MSG0("sema.560", "解放とは別の処理なら、別の名前にしてください（仕様 §6.2）");
        diag_fail(&d);
    }

    f->ir_name = mangle(c->ir_name, fn->name);  // "lexer.Token.show"
    f->owner = s->cur;
    f->next = s->funcs;
    s->funcs = f;

    fn->ir_name = f->ir_name;  // ★ codegen が define する関数名（@lexer.Token.show）
    fn->type = ret;
}

// 1b：フィールドとメソッドを解決する。
// ── 未初期化フィールドの検査 ─────────────────────
//
// クラス型のフィールドは既定値を作れないので NULL から始まります。
// 当初はランタイムで検査していましたが、型の側から塞ぎます。
//
// ★ **どの経路でも代入するか**を見ます（A-27。0.18.0）。
//   それまでは「init のどこかに self.f = ... と書いてあるか」を見るだけで、
//   **`if` の中にしか代入が無い形がすり抜けていました**（仕様 15.6 の
//   「ほぼ」の中身）。Ada / SPARK の definite assignment にあたる検査です。
//
// 注意: 保守的に見ます — ループの中の代入は「0 回かもしれない」ので数えません。
//    注意: ランタイム検査（pl_check_not_none）は**残します**。
//    ここで見られるのは init の中だけで、`unsafe:` や `T | None` の
//    絞り込み漏れまでは面倒を見られないためです（多層で受けます）。
// この文（部分木）のどこかに return があるか。
//
// ★ 「代入せずに抜ける経路」を見つけるために使います。
//   注意: panic() / exit() は数えません。**戻らない**ので、そこから
//     オブジェクトが観測されることがないためです。
static bool contains_return(Node *n) {
    if (!n) return false;
    if (n->kind == ND_RETURN) return true;
    if (contains_return(n->lhs) || contains_return(n->rhs) ||
        contains_return(n->els) || contains_return(n->incr))
        return true;
    for (Node *st = n->body; st; st = st->next)
        if (contains_return(st)) return true;
    return false;
}

static bool definitely_assigns_stmt(Node *n, const char *fname);

// 文の並びが「**どの経路でも**必ず self.<fname> に代入する」か。
//
// 注意: 途中に return がありうる文を見つけたら、そこで打ち切ります
//   （代入せずに出ていく経路があるということなので）。
static bool definitely_assigns(Node *first, const char *fname) {
    for (Node *st = first; st; st = st->next) {
        if (definitely_assigns_stmt(st, fname)) return true;
        if (contains_return(st)) return false;
    }
    return false;
}

static bool definitely_assigns_stmt(Node *n, const char *fname) {
    if (!n) return false;

    switch (n->kind) {
        case ND_ASSIGN:
            return n->lhs && n->lhs->kind == ND_FIELD && n->lhs->lhs &&
                   n->lhs->lhs->kind == ND_VAR &&
                   strcmp(n->lhs->lhs->name, "self") == 0 &&
                   strcmp(n->lhs->name, fname) == 0;

        case ND_BLOCK:
            return definitely_assigns(n->body, fname);

        // ★ else が無ければ「条件が偽のときに素通りする」経路が残ります。
        //   always_returns と同じ形の判断です。
        case ND_IF:
            return n->els && definitely_assigns_stmt(n->body, fname) &&
                   definitely_assigns_stmt(n->els, fname);

        // 注意: ループは 0 回かもしれません（保守的に「代入しない」と見ます）。
        case ND_WHILE:
            return false;

        case ND_TRY:
            if (!definitely_assigns_stmt(n->body, fname)) return false;
            for (Node *ex = n->els; ex; ex = ex->next)
                if (!definitely_assigns_stmt(ex->body, fname)) return false;
            return true;

        default:
            return false;
    }
}

static Node *find_init(Class *c) {
    for (Node *m = c->node->body; m; m = m->next)
        if (m->kind == ND_FUNC && strcmp(m->name, "init") == 0) return m;
    return NULL;
}

static void check_fields_initialized(Sema *s, Class *c) {
    Node *init = find_init(c);

    for (Field *f = c->fields; f; f = f->next) {
        // ★ 既定値を作れる型は対象外です（int → 0 / str → "" / list → 空 /
        //   T | None → None。どれも「有効な値」から始まります）。
        if (f->type->kind != TY_CLASS) continue;
        if (init && definitely_assigns_stmt(init->body, f->name)) continue;

        Diag d = {0};
        d.message = MSG1("sema.301", "フィールド '{0}' は init で代入されていません", f->name);
        d.primary.tok = f->tok;
        d.primary.label = MSG0("sema.561", "このフィールドは None から始まってしまいます");
        if (init) {
            d.related.tok = init->tok;
            d.related.label = MSG0("sema.562", "init はここです");
        } else {
            d.related.tok = c->tok;
            d.related.label = MSG0("sema.563", "このクラスには init がありません");
        }
        d.hint = MSG2("sema.302", "次のどちらかにしてください:\n             ・型を '{0} | None' にする\n             ・init の中で self.{1} = ... と代入する", type_name(f->type), f->name);
        diag_fail(&d);
    }
}

// ── インタフェース ──────────────────────────────────────
//
// ★ 表現の決め方（design/generics-and-interfaces.md §2 から変更しました）
//
//   設計書ではファットポインタ（実体 + vtable の 2 語）を想定していましたが、
//   この処理系は **「値はどれも 8 バイト」** という前提で組まれています
//   （list の要素も、フィールドも、引数も）。2 語にすると全部に手が要ります。
//
//   そこで **vtable へのポインタをオブジェクトの先頭に隠しフィールドとして
//   持たせる**方式にしました。こうすると:
//     - インタフェースの値は「ただのポインタ」＝ 8 バイトのまま
//     - クラス → インタフェースの変換が **何もしないで済む**（同じポインタ）
//   代わりに、インタフェースを実装するクラスは 8 バイト大きくなります。
//
// 注意: メソッドのスロット番号は **プログラム全体で一意**にします。
//   1 つのクラスが複数のインタフェースを実装できるようにするためです
//   （クラスごとの vtable は「全スロットぶんの配列」になります）。
static void declare_iface(Sema *s, Node *n) {
    if (lookup_iface(s, n->name) || lookup_class(s, n->name)) {
        Diag d = {0};
        d.message = MSG1("sema.303", "'{0}' は既に定義されています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.552", "ここで再定義されています");
        diag_fail(&d);
    }

    Iface *ifc = xmalloc(sizeof(Iface));
    ifc->name = n->name;
    ifc->ir_name = mod_mangle(s, n->name);
    ifc->tok = n->tok;
    ifc->owner = s->cur;

    IMethod tail = {0};
    IMethod *cur = &tail;
    int nm = 0;
    for (Node *m = n->body; m; m = m->next) {
        for (IMethod *q = tail.next; q; q = q->next)
            if (strcmp(q->name, m->name) == 0)
                error_at_hint_m(m->tok, MSG0("sema.305", "同じ名前のメソッドを 2 度書くことはできません"), MSG1("sema.304", "メソッド '{0}' が重複しています", m->name));
        // ★ インタフェースの宣言に既定値は書けません（A-38）。
        //   呼ぶ側はどの実装が入るか知らないので、既定値を埋める人が
        //   決まりません（実装ごとに違う既定値を書けてしまいます）。
        for (Node *pm = m->params; pm; pm = pm->next) {
            if (!pm->rhs) continue;
            Diag d = {0};
            d.message = MSG0("sema.564", "インタフェースの宣言に既定値は書けません");
            d.primary.tok = pm->rhs->tok;
            d.primary.label = MSG0("sema.565", "ここには既定値を書けません");
            d.hint = MSG0("sema.566", "どの実装が呼ばれるかは実行時に決まるので、既定値を埋める人が決まりません。既定値はクラス側のメソッドに書いてください（インタフェース越しには使えません）");
            diag_fail(&d);
        }

        // ★ インタフェースに drop は置けません（E-DROP-2）。置けると、実装の
        //   デストラクタをインタフェース越しに呼べてしまいます。
        if (strcmp(m->name, "drop") == 0) {
            Diag d = {0};
            d.code = "E-DROP-2";
            d.message = MSG0("sema.567", "インタフェースに drop は宣言できません（drop はデストラクタの名前です）");
            d.primary.tok = m->tok;
            d.primary.label = MSG0("sema.568", "インタフェース越しに呼べると、解放のときにもう一度呼ばれます");
            d.hint = MSG0("sema.449", "早く後始末をしたいときは、別の名前のメソッド（close など）に分けてください（仕様 §6.2）");
            diag_fail(&d);
        }

        IMethod *im = xmalloc(sizeof(IMethod));
        im->name = m->name;
        im->sig = m;
        im->slot = s->next_slot++;   // ★ プログラム全体で一意
        cur->next = im;
        cur = im;
        nm++;
    }
    ifc->methods = tail.next;
    ifc->nmethods = nm;

    ifc->next = s->ifaces;
    s->ifaces = ifc;
    n->type = type_iface(n->name, ifc);
}

// 型注釈に書かれたインタフェース名を引く
static Iface *resolve_iface_ref(Sema *s, Node *tr) {
    if (tr->mod_name) {
        ModuleSyms *ms = lookup_import(s, tr->mod_name);
        if (!ms)
            error_at_hint_m(tr->tok, MSG1("sema.010", "ファイルの先頭に 'import {0}' を書いてください", tr->mod_name), MSG1("sema.009", "モジュール '{0}' を import していません", tr->mod_name));
        Iface *i = lookup_iface_in(ms, tr->name);
        if (!i)
            error_at_hint_m(tr->tok, MSG0("sema.307", "インタフェース名を確認してください"), MSG2("sema.306", "モジュール '{0}' にインタフェース '{1}' はありません", tr->mod_name, tr->name));
        return i;
    }
    Iface *i = lookup_iface(s, tr->name);
    if (!i) {
        Diag d = {0};
        d.message = MSG1("sema.308", "インタフェース '{0}' が見つかりません", tr->name);
        d.primary.tok = tr->tok;
        d.primary.label = MSG0("sema.569", "ここに書けるのはインタフェース名だけです");
        d.hint = lookup_class(s, tr->name)
                     ? MSG1("sema.309", "'{0}' はクラスです。継承はありません", tr->name)
                     : MSG0("sema.570", "interface で宣言してから使ってください");
        diag_fail(&d);
    }
    return i;
}

// クラスが宣言どおりのメソッドを持っているかを確かめる
static void check_implements(Sema *s, Class *c, Iface *ifc, Token *at) {
    for (IMethod *im = ifc->methods; im; im = im->next) {
        StrBuf key;
        sb_init(&key);
        sb_printf(&key, "%s.%s", c->name, im->name);
        FuncSig *f = lookup_func(s, sb_str(&key));
        if (!f) {
            Diag d = {0};
            d.message = MSG2("sema.310", "クラス '{0}' に '{1}' がありません", c->name, im->name);
            d.primary.tok = at;
            d.primary.label = MSG1("sema.311", "'{0}' を実装すると宣言しています", ifc->name);
            d.related.tok = im->sig->tok;
            d.related.label = MSG0("sema.571", "このメソッドが必要です");
            diag_fail(&d);
        }

        // 引数の型と戻り型が一致するか（self は数えない）
        Node *sig = im->sig;
        int want = 0;
        for (Node *pm = sig->params; pm; pm = pm->next) want++;
        if (f->nparams != want) {
            Diag d = {0};
            d.message = MSG2("sema.312", "'{0}.{1}' の引数の数が宣言と違います", c->name, im->name);
            d.primary.tok = f->tok;
            d.primary.label = MSG1("sema.313", "{0} 個です", diag_fmt("%d", f->nparams));
            d.related.tok = sig->tok;
            d.related.label = MSG1("sema.314", "宣言では {0} 個です", diag_fmt("%d", want));
            diag_fail(&d);
        }
        int k = 0;
        for (Node *pm = sig->params; pm; pm = pm->next, k++) {
            if (k == 0) continue;   // self
            // ★ 引数の**名前**も宣言と同じであること（A-38 の続き）。
            //   名前は呼び出し側から見える約束です（キーワード引数）。
            //   ここを見ないと、宣言と実装で名前が違っても通ってしまい、
            //   「インタフェース越しにも名前で渡せる」ようにした日に、
            //   どちらの名前が正しいのか決められなくなります。
            if (strcmp(f->pnames[k], pm->name) != 0) {
                Diag d = {0};
                d.message = MSG3("sema.315", "'{0}.{1}' の {2} 番目の引数の名前が宣言と違います", c->name, im->name, diag_fmt("%d", k));
                d.primary.tok = f->tok;
                d.primary.label = MSG1("sema.316", "実装では '{0}' です", f->pnames[k]);
                d.related.tok = pm->tok;
                d.related.label = MSG1("sema.317", "宣言では '{0}' です", pm->name);
                d.hint = MSG0("sema.572", "引数の名前は呼び出し側から見える約束です（名前で渡すときに使います）。宣言に合わせてください");
                diag_fail(&d);
            }
            Type *wt = resolve_type(s, pm->type_ref);
            if (!type_equal(f->params[k], wt)) {
                Diag d = {0};
                d.message = MSG3("sema.318", "'{0}.{1}' の {2} 番目の引数の型が宣言と違います", c->name, im->name, diag_fmt("%d", k));
                d.primary.tok = f->tok;
                d.primary.label = MSG1("sema.156", "'{0}' 型です", type_name(f->params[k]));
                d.related.tok = pm->tok;
                d.related.label = MSG1("sema.319", "宣言では '{0}' 型です", type_name(wt));
                diag_fail(&d);
            }
        }
        Type *wr = resolve_type(s, sig->type_ref);
        if (!type_equal(f->ret, wr)) {
            Diag d = {0};
            d.message = MSG2("sema.320", "'{0}.{1}' の戻り型が宣言と違います", c->name, im->name);
            d.primary.tok = f->tok;
            d.primary.label = MSG1("sema.321", "'{0}' を返します", type_name(f->ret));
            d.related.tok = sig->tok;
            d.related.label = MSG1("sema.317", "宣言では '{0}' です", type_name(wr));
            diag_fail(&d);
        }
        // ★ raises も宣言の一部です（A-40）。
        //   実装だけが失敗しうる、という形は通せません——呼ぶ側は
        //   インタフェースの宣言しか見ないので、try を書く手がかりが
        //   無くなります。逆に、宣言にあって実装に無いのも断ります
        //   （呼ぶ側に要らない try を書かせることになります）。
        FuncSig decl = {0};
        decl.name = im->name;
        decl.tok = sig->tok;
        resolve_raises(s, sig, &decl);
        if (f->nraises != decl.nraises) {
            Diag d = {0};
            d.message = MSG2("sema.322", "'{0}.{1}' の raises が宣言と違います", c->name, im->name);
            d.primary.tok = f->tok;
            d.primary.label = MSG1("sema.323", "実装は {0} 個のエラーを宣言しています", diag_fmt("%d", f->nraises));
            d.related.tok = sig->tok;
            d.related.label = MSG1("sema.314", "宣言では {0} 個です", diag_fmt("%d", decl.nraises));
            d.hint = MSG0("sema.573", "インタフェース越しに呼ぶ側は宣言しか見ません。同じ raises を書いてください");
            diag_fail(&d);
        }
        for (int i = 0; i < decl.nraises; i++) {
            bool found = false;
            for (int j = 0; j < f->nraises; j++)
                if (f->raises[j] == decl.raises[i]) { found = true; break; }
            if (found) continue;
            Diag d = {0};
            d.message = MSG3("sema.324", "'{0}.{1}' が '{2}' を宣言していません", c->name, im->name, decl.raises[i]->name);
            d.primary.tok = f->tok;
            d.primary.label = MSG0("sema.574", "このエラーが raises にありません");
            d.related.tok = sig->tok;
            d.related.label = MSG1("sema.325", "宣言では '{0}' を投げます", decl.raises[i]->name);
            d.hint = MSG0("sema.573", "インタフェース越しに呼ぶ側は宣言しか見ません。同じ raises を書いてください");
            diag_fail(&d);
        }
    }
}

// クラスがそのインタフェースを実装しているか
static bool class_implements(Class *c, Iface *ifc) {
    for (IfaceList *l = c->impls; l; l = l->next)
        if (l->iface == ifc) return true;
    return false;
}

static void declare_class_members(Sema *s, Node *n) {
    Class *c = n->cls;

    // ★ 実装するインタフェースを先に決めます。
    //   レイアウト（隠しフィールドの有無）がこれで変わるためです。
    //   注意: メソッドの照合は、メソッドを登録した**後**に行います。
    IfaceList *itail = NULL;
    for (Node *ir = n->ifaces; ir; ir = ir->next) {
        Iface *ifc = resolve_iface_ref(s, ir);
        for (IfaceList *l = c->impls; l; l = l->next)
            if (l->iface == ifc)
                error_at_hint_m(ir->tok, MSG0("sema.327", "同じインタフェースを 2 度書けません"), MSG1("sema.326", "'{0}' は既に書かれています", ifc->name));
        IfaceList *nl = xmalloc(sizeof(IfaceList));
        nl->iface = ifc;
        nl->next = NULL;
        if (itail) itail->next = nl; else c->impls = nl;
        itail = nl;
    }

    // ① フィールド（宣言順にリストの末尾へ足す。並び順がレイアウトになる）
    Field tail = {0};
    Field *cur = &tail;
    for (Node *m = n->body; m; m = m->next) {
        if (m->kind != ND_FIELDDECL) continue;

        Field *prev = lookup_field(c, m->name);
        if (prev) {
            Diag d = {0};
            d.message = MSG1("sema.328", "フィールド '{0}' は既に宣言されています", m->name);
            d.primary.tok = m->tok;
            d.primary.label = MSG0("sema.403", "ここで再宣言されています");
            d.related.tok = prev->tok;
            d.related.label = MSG0("sema.404", "最初の宣言はここです");
            diag_fail(&d);
        }

        Type *ft = resolve_type(s, m->type_ref);
        if (ft->kind == TY_NONE)
            error_at_hint_m(m->tok, MSG0("sema.330", "None 型の値は存在しないのでフィールドにできません"), MSG0("sema.329", "フィールドの型に None は使えません"));

        Field *f = xmalloc(sizeof(Field));
        f->name = m->name;
        f->type = ft;
        f->tok = m->tok;
        cur->next = f;
        cur = f;
        c->fields = tail.next;  // ★ lookup_field を回すために毎回つなぎ直す
        m->type = ft;
    }
    c->fields = tail.next;
    layout_class(c);

    // ② メソッド
    for (Node *m = n->body; m; m = m->next)
        if (m->kind == ND_FUNC) declare_method(s, c, m);

    // ③ クラス型のフィールドが None から始まらないことを確かめる
    check_fields_initialized(s, c);

    // ④ 宣言したインタフェースを本当に実装しているか
    for (Node *ir = n->ifaces; ir; ir = ir->next)
        check_implements(s, c, resolve_iface_ref(s, ir), ir->tok);
}

// extern の引数と戻り値に使える型か。
//
// 注意: bool（i1）だけは通しません。C の _Bool との ABI が環境依存で、
//    「たまたま動く」形になりやすいためです。境界は狭く保ちます。
static void check_extern_type(Type *t, Token *tok, const char *what) {
    if (t->kind != TY_BOOL) return;
    Diag d = {0};
    d.message = MSG1("sema.331", "extern の{0}に bool は使えません", what);
    d.primary.tok = tok;
    d.primary.label = MSG0("sema.575", "この型は C との境界を越えられません");
    d.hint = MSG0("sema.576", "int で受け取り、本言語側で 'n == 1' と書いてください");
    diag_fail(&d);
}


// 境界に出せる型か（設計 ffi.md §3）。
//
// ★ 出せるのは「写して意味が変わらない型」だけです。範囲型は int の形でも
//   断ります（外から来た値に範囲の検査を挟む口が、段階 1 には無いため）。
static bool export_scalar_ok(Type *t) {
    if (ty_is_range(t)) return false;
    return t->kind == TY_INT || t->kind == TY_FLOAT || t->kind == TY_BOOL ||
           t->kind == TY_STR;
}

static bool export_type_ok(Type *t, bool is_ret) {
    if (is_ret && t->kind == TY_NONE) return true;
    if (t->kind == TY_LIST) return t->elem && export_scalar_ok(t->elem);
    return export_scalar_ok(t);
}

static void check_export_type(Type *t, Token *tok, const char *what) {
    Diag d = {0};
    d.code = "E-EXPORT-1";
    d.message = MSG2("sema.332", "'{0}' は外へ出す関数の{1}に使えません", type_name(t), what);
    d.primary.tok = tok;
    d.primary.label = MSG0("sema.577", "この型は境界を越えられません");
    d.hint = MSG0("sema.578", "使えるのは int / float / bool / str と、その list です（クラスなどは外へ出さず、この関数の中で使ってください）");
    diag_fail(&d);
}

static void check_export_sig(Node *n, FuncSig *f) {
    if (!export_type_ok(f->ret, true)) check_export_type(f->ret, n->tok, MSG0("sema.579", "戻り値"));
    int i = 0;
    for (Node *pm = n->params; pm; pm = pm->next, i++) {
        if (!export_type_ok(f->params[i], false))
            check_export_type(f->params[i], pm->tok, MSG0("sema.580", "引数"));
        // ★ 境界では写すので、書き換えても外には戻りません（ffi.md §3.1）
        if (pm->mode == PM_MUT) {
            Diag d = {0};
            d.code = "E-EXPORT-2";
            d.message = MSG1("sema.333", "外へ出す関数の引数 '{0}' は mut にできません", pm->name);
            d.primary.tok = pm->tok;
            d.primary.label = MSG0("sema.581", "外から来た値は写しなので、書き換えても呼んだ側には戻りません");
            d.hint = MSG0("sema.582", "書き換えた結果は戻り値で返してください");
            diag_fail(&d);
        }
    }
}

// raises 節を解決する。
//
// ★ エラー型は「ふつうのクラス」です（仕様 §8.3。継承はありません）。
//   ここで ID も割り当てます。割り当て順は「モジュールの依存順 → 出現順」で、
//   これは declare のパスを回る順序そのものです。
static void resolve_raises(Sema *s, Node *fn, FuncSig *f) {
    int n = 0;
    for (Node *r = fn->raises; r; r = r->next) n++;
    f->nraises = n;
    if (n == 0) return;

    f->raises = xmalloc(sizeof(Class *) * (size_t)n);
    int i = 0;
    for (Node *r = fn->raises; r; r = r->next, i++) {
        Type *t = resolve_type(s, r);
        if (t->kind != TY_CLASS) {
            Diag d = {0};
            d.message = MSG1("sema.281", "'{0}' はエラー型として使えません", type_name(t));
            d.primary.tok = r->tok;
            d.primary.label = MSG0("sema.583", "raises に書けるのはクラスだけです");
            d.hint = MSG0("sema.584", "エラーはふつうのクラスとして定義してください（例: class IOError:\n                 message: str）");
            diag_fail(&d);
        }
        f->raises[i] = t->cls;
        // ★ ここで ID を確定させる。外へ出す関数の入口がエラーを見分けるために、
        //   節のノードにも書き写します（ffi.md §5.1）。
        r->err_tag = err_tag_of(s, t->cls);
        r->type = t;
    }
}

static void declare_func(Sema *s, Node *n) {
    reject_module_name(s, n->name, n->tok, MSG0("sema.455", "関数"));
    if (lookup_class(s, n->name)) {
        Diag d = {0};
        d.message = MSG1("sema.334", "'{0}' はクラス名として使われています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.585", "この名前の関数は定義できません");
        d.related.tok = lookup_class(s, n->name)->tok;
        d.related.label = MSG0("sema.444", "クラスの定義はここです");
        d.hint = MSG0("sema.586", "クラス名は「インスタンス生成」の呼び出しに使われます（例: Token(1, \"x\")）");
        diag_fail(&d);
    }
    if (is_builtin_name(n->name))
        error_at_hint_m(n->tok, MSG1("sema.336", "{0} は組み込み関数です。別の名前を使ってください", n->name), MSG1("sema.335", "'{0}' は再定義できません", n->name));

    FuncSig *prev = lookup_func(s, n->name);
    if (prev) {
        Diag d = {0};
        d.message = MSG1("sema.337", "関数 '{0}' は既に定義されています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.552", "ここで再定義されています");
        d.related.tok = prev->tok;
        d.related.label = MSG0("parse.224", "最初の定義はここです");
        diag_fail(&d);
    }

    // ★ ジェネリックなテンプレートは **型を解決しません**。
    //   T が何なのかまだ決まっていないためです。名前だけ登録して、
    //   呼ばれたときに実引数から決めて実体を作ります。
    if (n->targs) {
        if (n->is_export) {
            Diag d = {0};
            d.code = "E-EXPORT-1";
            d.message = MSG0("sema.587", "型引数を持つ関数は外へ出せません");
            d.primary.tok = n->tok;
            d.primary.label = MSG0("sema.588", "外から見ると、型が決まりません");
            d.hint = MSG0("sema.589", "型を決めた関数を extern def で書き、その中からこの関数を呼んでください");
            diag_fail(&d);
        }
        FuncSig *t = xmalloc(sizeof(FuncSig));
        t->name = n->name;
        t->ir_name = n->name;
        t->tok = n->tok;
        t->owner = s->cur;
        t->tmpl = n;
        t->next = s->funcs;
        s->funcs = t;
        return;
    }

    Type *ret = resolve_type(s, n->type_ref);

    int nparams = 0;
    for (Node *pm = n->params; pm; pm = pm->next) nparams++;

    FuncSig *f = xmalloc(sizeof(FuncSig));
    f->name = n->name;
    f->ret = ret;
    f->nparams = nparams;
    f->params = nparams ? xmalloc(sizeof(Type *) * (size_t)nparams) : NULL;
    f->pnames = nparams ? xmalloc(sizeof(char *) * (size_t)nparams) : NULL;
    f->pmodes = nparams ? xmalloc(sizeof(ParamMode) * (size_t)nparams) : NULL;
    f->defaults = xmalloc(sizeof(Node *) * (size_t)(nparams ? nparams : 1));
    f->is_lambda = n->is_lambda;   // A-42
    f->node = n;                   // A-43
    f->tok = n->tok;

    int i = 0;
    for (Node *pm = n->params; pm; pm = pm->next, i++) {
        Type *pt = resolve_type(s, pm->type_ref);
        if (pt->kind == TY_NONE)
            error_at_hint_m(pm->tok, MSG0("sema.296", "None 型の値は存在しないので引数にできません"), MSG0("sema.295", "引数の型に None は使えません"));
        f->params[i] = pt;
        f->pnames[i] = pm->name;
        f->pmodes[i] = pm->mode;   // A-21e
        f->defaults[i] = pm->rhs;  // A-38（無ければ NULL）
        pm->type = pt;
    }
    check_defaults(s, f, n);

    // ★ extern は C 側で名前が決まっているので修飾しません
    //   （言語仕様 5.11）。修飾の目的は「本言語側の名前どうしの衝突を
    //   避けること」なので、C のシンボルにはその目的が成立しません。
    bool is_extern_decl = n->body == NULL;
    if (is_extern_decl) {
        check_extern_type(ret, n->tok, MSG0("sema.579", "戻り値"));
        for (Node *pm = n->params; pm; pm = pm->next)
            check_extern_type(pm->type, pm->tok, MSG0("sema.580", "引数"));
    }

    if (n->is_export) check_export_sig(n, f);

    resolve_raises(s, n, f);
    if (is_extern_decl && f->nraises)
        error_at_hint_m(n->tok, MSG0("sema.339", "extern の関数は C 側の約束に従うので raises は書けません"), MSG0("sema.338", "extern に raises は書けません"));

    f->ir_name = is_extern_decl ? n->name : mod_mangle(s, n->name);
    f->owner = s->cur;
    f->next = s->funcs;
    s->funcs = f;
    n->ir_name = f->ir_name;
    n->type = ret;
}

// ── ジェネリック関数の実体化 ──────────────────────────
//
// ★ **実引数の型から型引数を決めます。** クラスと違い、左辺の型からは
//   決められないためです（f(xs) の左辺には戻り値の型しかない）。
// ── lambda を実体にする（A-42）───────────────────────────
//
// ★ lambda には**型が書いてありません**。どんな関数になるかは
//   「置かれた場所」で決まります。
//
//     p: fn(int) -> bool = lambda x: x > 0
//        ^^^^^^^^^^^^^^^ これが答え
//
//   ジェネリック関数の単相化と同じ道を通します——パーサが .T0 / .R という
//   型引数を作っておき、ここで `fn(int) -> bool` の中身を束ねます。
//   **新しい仕組みを足していません。**
//
// 注意: 同じ lambda を違う型で 2 度使えば、実体が 2 つできます
//   （ジェネリック関数と同じです）。
// lambda の本体から「外の変数」を集める（A-43）。
//
// ★ **ここが捕獲の入口です。** lambda はトップレベルへ持ち上げるので、
//   外の名前はそのままでは見えません。**作った場所で**（＝いまの scope が
//   まだ生きているうちに）どれを捕まえるか決め、**引数として渡す**形に
//   書き換えます。
//
// 注意: グローバルと関数は捕まえません（持ち上げた先からも見えます）。
static void collect_captures(Sema *s, Node *n, Node *params, Node **out) {
    for (; n; n = n->next) {
        if (n->kind == ND_VAR) {
            bool is_param = false;
            for (Node *pm = params; pm; pm = pm->next)
                if (strcmp(pm->name, n->name) == 0) is_param = true;
            VarEntry *v = is_param ? NULL : lookup(s, n->name);
            if (v && !v->is_global) {
                bool seen = false;
                for (Node *c = *out; c; c = c->next)
                    if (strcmp(c->name, n->name) == 0) seen = true;
                if (!seen) {
                    Node *c = new_var_node(n->tok, n->name);
                    c->ir_name = v->ir_name;
                    c->type = v->type;
                    Node **tail = out;
                    while (*tail) tail = &(*tail)->next;
                    *tail = c;
                }
            }
        }
        collect_captures(s, n->lhs, params, out);
        collect_captures(s, n->rhs, params, out);
        collect_captures(s, n->els, params, out);
        collect_captures(s, n->body, params, out);
        collect_captures(s, n->incr, params, out);
        collect_captures(s, n->args, params, out);
    }
}

// 捕まえられる型か（A-43）。
//
// ★ **値型だけ**です。`str` や `list` を捕まえると「借りものを枠の上に
//   持ち回る」話になり、元を動かせるかどうかの検査が要ります。
//   まずは黙って壊れない範囲から入れます（design/closures.md §4）。
// 値ごと写して捕まえる型（A-43）
static bool capture_by_value(Type *t) {
    switch (t->kind) {
        case TY_INT:
        case TY_FLOAT:
        case TY_BOOL: return true;
        case TY_ENUM: return !(t->en && t->en->has_payload);
        default: return false;
    }
}

// ★ A-51 から、`str` / `list` / クラスなども**借りて**捕まえられます。
//   記録には借りたポインタを置くだけで、写しも数え札も増やしません。
//   捕まえたあとで元を書き換えたり移動したりしてから lambda を使うと、
//   所有権検査が止めます（ownck の record_capture_loans）。
//   注意: Thread / mutex / 生ポインタは捕まえません（借りて持ち回る意味がない）。
static bool capturable(Type *t) {
    if (capture_by_value(t)) return true;
    switch (t->kind) {
        case TY_STR:
        case TY_LIST:
        case TY_CLASS:
        case TY_IFACE:
        case TY_TUPLE:
        case TY_RC:
        case TY_WEAK:
        case TY_ENUM: return true;
        case TY_OPT: return capturable(t->elem);
        // 注意: fn の値は捕まえません。その値がさらに借りているものを、
        //   所有権検査が追えなくなるためです。
        default: return false;
    }
}

static FuncSig *instantiate_lambda(Sema *s, FuncSig *tmpl, Node *ref) {
    Node *tn = tmpl->tmpl;

    int np = 0;
    for (Node *pm = tn->params; pm; pm = pm->next) np++;

    Type *want = s->expected;
    if (!want || want->kind != TY_FN) {
        Diag d = {0};
        d.message = MSG0("sema.590", "この lambda がどんな関数になるのか決められません");
        d.primary.tok = ref->tok;
        d.primary.label = MSG0("sema.591", "ここでは引数と戻り値の型が決まりません");
        d.hint = MSG0("sema.592", "型注釈のある変数に入れるか、関数型の引数に渡してください:\n             p: fn(int) -> bool = lambda x: x > 0\n             count_if(xs, lambda x: x > 0)");
        diag_fail(&d);
    }
    if (want->nparams != np) {
        Diag d = {0};
        d.message = MSG2("sema.340", "この lambda は {0} 個の引数を取りますが、'{1}' が要ります", diag_fmt("%d", np), type_name(want));
        d.primary.tok = ref->tok;
        d.primary.label = MSG1("sema.341", "引数が {0} 個です", diag_fmt("%d", np));
        d.hint = MSG1("sema.342", "ここに置けるのは {0} 引数の lambda です", diag_fmt("%d", want->nparams));
        diag_fail(&d);
    }

    // ★ 外の変数を集めます（A-43）。**いまの scope が生きているうち**に
    //   決めます（この後で持ち上げ先のモジュールへ移ります）。
    Node *caps = NULL;
    collect_captures(s, tn->body, tn->params, &caps);
    int ncap = 0;
    for (Node *c = caps; c; c = c->next) {
        if (!capturable(c->type)) {
            Diag d = {0};
            d.message = MSG2("sema.343", "'{0}' は捕まえられません（'{1}' 型）", c->name, type_name(c->type));
            d.primary.tok = ref->tok;
            d.primary.label = MSG0("sema.593", "この lambda が外の変数を使っています");
            d.hint = MSG0("sema.642", "関数の値・Thread・mutex・ptr は捕まえられません。引数で受け取るか、def で書いた関数にしてください");
            diag_fail(&d);
        }
        ncap++;
    }
    ref->caps = caps;

    // 束ねる型の並び（引数 … と戻り型 … と捕まえた値の型）
    Type *args[MAX_TARGS];
    int nt = np + 1 + ncap;
    if (nt > MAX_TARGS) error_at_m(ref->tok, MSG0("sema.344", "lambda の引数が多すぎます"));
    for (int i = 0; i < np; i++) args[i] = want->params[i];
    args[np] = want->elem;
    {
        int k = np + 1;
        for (Node *c = caps; c; c = c->next, k++) args[k] = c->type;
    }

    char *iname = mangle_inst(tmpl->name, args, nt);

    // 既に作ってあれば、それを返します
    for (FuncSig *f = tmpl->owner->funcs; f; f = f->next)
        if (!f->tmpl && strcmp(f->name, iname) == 0) return f;

    ModuleSyms *saved_mod = s->cur;
    Scope *saved_scope = s->scope;
    TBind *saved_tbind = s->tbind;
    FuncSig *saved_cur_func = s->cur_func;
    UsedName *saved_used = s->used;

    enter_module(s, tmpl->owner);

    TBind *binds = NULL;
    int ti = 0;
    for (Node *tp = tn->targs; tp; tp = tp->next, ti++) {
        TBind *b = xmalloc(sizeof(TBind));
        b->name = tp->name;
        b->type = args[ti];
        b->next = binds;
        binds = b;
    }

    Node *inst = ast_clone(tn);
    inst->name = iname;
    inst->targs = NULL;
    inst->next = NULL;

    // ★ 捕まえた値は**末尾の引数**として受け取ります（A-43）。
    //   こうすると本体の名前解決は今までどおりで済みます——捕まえた名前が
    //   そのまま引数の名前になるので、書き換えるところがありません。
    //   codegen が「記録から読んで渡す」包みを作ります。
    if (ncap > 0) {
        Node *ptail = inst->params;
        while (ptail && ptail->next) ptail = ptail->next;
        int ci = 0;
        for (Node *c = caps; c; c = c->next, ci++) {
            char *tname = diag_fmt("%s.C%d", tmpl->name, ci);
            TBind *b = xmalloc(sizeof(TBind));
            b->name = tname;
            b->type = c->type;
            b->next = binds;
            binds = b;

            Node *pm = new_node(ND_PARAM, ref->tok);
            pm->name = c->name;
            Node *tr = new_node(ND_TYPEREF, ref->tok);
            tr->name = tname;
            pm->type_ref = tr;
            if (ptail) ptail->next = pm; else inst->params = pm;
            ptail = pm;
        }
    }

    s->tbind = binds;

    declare_func(s, inst);

    // codegen が拾えるように、そのモジュールの AST に足します
    Node *ast = tmpl->owner->mod->ast;
    Node *last = ast->body;
    while (last->next) last = last->next;
    last->next = inst;

    Instance *q = xmalloc(sizeof(Instance));
    q->node = inst;
    q->owner = tmpl->owner;
    q->binds = binds;
    q->site = ref->tok;
    q->iname = iname;
    q->is_func = true;
    q->next = s->pending;
    s->pending = q;

    FuncSig *made = lookup_func(s, iname);

    enter_module(s, saved_mod);
    s->scope = saved_scope;
    s->cur_func = saved_cur_func;
    s->used = saved_used;
    s->tbind = saved_tbind;
    return made;
}

static FuncSig *instantiate_func(Sema *s, FuncSig *tmpl, Node *call) {
    Node *tn = tmpl->tmpl;

    int nt = 0;
    for (Node *tp = tn->targs; tp; tp = tp->next) nt++;

    int want = 0;
    for (Node *pm = tn->params; pm; pm = pm->next) want++;

    // ★ 並べ替えと既定値の穴埋め（A-38）。**型引数を決める前に**やります。
    //   型引数は「何番目の引数が何型か」から決まるので、並びが定義どおりに
    //   なっていないと、T が別の引数から決まってしまいます。
    //   名前と既定値はテンプレートの引数リストから取ります（この関数には
    //   まだ FuncSig がありません）。
    if (want > 0) {
        char **pnames = xmalloc(sizeof(char *) * (size_t)want);
        Node **defaults = xmalloc(sizeof(Node *) * (size_t)want);
        int pi = 0;
        for (Node *pm = tn->params; pm; pm = pm->next, pi++) {
            pnames[pi] = pm->name;
            defaults[pi] = pm->rhs;
        }
        bind_args(call, want, pnames, defaults, tn->tok,
                  MSG1("sema.345", "関数 '{0}'", tmpl->name), false);
    }

    int nargs = 0;
    for (Node *a = call->args; a; a = a->next) nargs++;
    if (nargs != want)
        error_at_hint_m(call->tok, MSG2("sema.224", "'{0}' は {1} 個の引数を取ります", tmpl->name, diag_fmt("%d", want)), MSG1("sema.164", "引数の個数が違います（{0} 個渡されました）", diag_fmt("%d", nargs)));

    // ── 実引数の型を先に求める ──
    Type *atypes[MAX_TARGS * 4];
    if (nargs > (int)(sizeof(atypes) / sizeof(atypes[0])))
        error_at_m(call->tok, MSG0("sema.346", "引数が多すぎます"));
    int k = 0;
    for (Node *a = call->args; a; a = a->next, k++) atypes[k] = check_expr(s, a);

    // ── 型引数を決める ──
    Type *args[MAX_TARGS];
    int ti = 0;
    for (Node *tp = tn->targs; tp; tp = tp->next, ti++) {
        Type *got = NULL;
        int pi = 0;
        for (Node *pm = tn->params; pm && !got; pm = pm->next, pi++)
            unify_tparam(tp->name, pm->type_ref, atypes[pi], &got);
        if (!got) {
            Diag d = {0};
            d.message = MSG1("sema.347", "型引数 '{0}' を決められません", tp->name);
            d.primary.tok = call->tok;
            d.primary.label = MSG0("sema.595", "実引数から決まりません");
            d.related.tok = tn->tok;
            d.related.label = MSG0("sema.596", "この関数の定義です");
            d.hint = MSG0("sema.597", "型引数は **引数の型から**決まります（戻り型にしか現れない型引数は書けません）");
            diag_fail(&d);
        }
        args[ti] = got;
    }

    char *iname = mangle_inst(tmpl->name, args, nt);

    // 既に作ってあれば、それを返す
    for (FuncSig *f = tmpl->owner->funcs; f; f = f->next)
        if (!f->tmpl && strcmp(f->name, iname) == 0) return f;

    ModuleSyms *saved_mod = s->cur;
    Scope *saved_scope = s->scope;
    TBind *saved_tbind = s->tbind;
    FuncSig *saved_cur_func = s->cur_func;
    UsedName *saved_used = s->used;

    enter_module(s, tmpl->owner);

    TBind *binds = NULL;
    ti = 0;
    for (Node *tp = tn->targs; tp; tp = tp->next, ti++) {
        TBind *b = xmalloc(sizeof(TBind));
        b->name = tp->name;
        b->type = args[ti];
        b->next = binds;
        binds = b;
    }
    s->tbind = binds;

    Node *inst = ast_clone(tn);
    inst->name = iname;
    inst->targs = NULL;
    inst->next = NULL;

    declare_func(s, inst);

    // codegen が拾えるように、そのモジュールの AST に足す
    Node *ast = tmpl->owner->mod->ast;
    Node *last = ast->body;
    while (last->next) last = last->next;
    last->next = inst;

    Instance *q = xmalloc(sizeof(Instance));
    q->node = inst;
    q->owner = tmpl->owner;
    q->binds = binds;
    q->site = call->tok;
    q->iname = iname;
    q->is_func = true;          // ★ クラスではなく関数の実体
    q->next = s->pending;
    s->pending = q;

    FuncSig *made = lookup_func(s, iname);

    enter_module(s, saved_mod);
    s->scope = saved_scope;
    s->cur_func = saved_cur_func;
    s->used = saved_used;
    s->tbind = saved_tbind;
    return made;
}

// グローバル変数の登録（言語仕様 6.2）
static void declare_global(Sema *s, Node *n) {
    reject_module_name(s, n->name, n->tok, MSG0("sema.598", "グローバル変数"));
    Type *declared = resolve_type(s, n->type_ref);
    if (declared->kind == TY_NONE)
        error_at_hint_m(n->tok, MSG0("sema.084", "None 型の値は存在しないので変数にできません"), MSG0("sema.083", "変数の型に None は使えません"));

    VarEntry *prev = lookup_local(s, n->name);
    if (prev) {
        Diag d = {0};
        d.message = MSG1("sema.085", "変数 '{0}' は既に宣言されています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.403", "ここで再宣言されています");
        d.related.tok = prev->decl_tok;
        d.related.label = MSG0("sema.404", "最初の宣言はここです");
        diag_fail(&d);
    }

    // 注意: 初期化式はコンパイル時定数のみ（言語仕様 6.2 の v1 制限）。
    //    計算を許すと「どちらを先に初期化するか」という初期化順序問題が起きます。
    if (n->rhs->kind != ND_INT && n->rhs->kind != ND_BOOL &&
        n->rhs->kind != ND_STR && n->rhs->kind != ND_FLOAT) {
        Diag d = {0};
        d.message = MSG0("sema.599", "グローバル変数の初期化式は定数でなければなりません");
        d.primary.tok = n->rhs->tok;
        d.primary.label =
            MSG0("sema.600", "ここには整数・浮動小数点数・True / False・文字列リテラルだけが書けます");
        d.hint = MSG0("sema.601", "計算が必要なら main の中でローカル変数にしてください");
        diag_fail(&d);
    }

    Type *actual = check_expr(s, n->rhs);
    if (!type_assignable(actual, declared)) {
        Diag d = {0};
        d.message = MSG0("sema.409", "型が一致しません");
        d.primary.tok = n->rhs->tok;
        d.primary.label = MSG1("sema.089", "型 '{0}' の式", type_name(actual));
        d.related.tok = n->tok;
        d.related.label = MSG2("sema.090", "変数 '{0}' は '{1}' 型として宣言されています", n->name, type_name(declared));
        diag_fail(&d);
    }

    // ★ グローバルは定数だけなので、ここはコンパイル時の判定になります（A-28）
    n->rhs = range_coerce(s, n->rhs, declared);

    // グローバルの IR 名は @g.<モジュール>.<名前>。
    // ★ モジュール名を挟むことで、別ファイルの同名グローバルと
    //   リンク時に衝突しなくなります（@g. は C のシンボルとの衝突よけ）。
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "@g.%s.%s", s->cur->mod->name, n->name);

    VarEntry *v = xmalloc(sizeof(VarEntry));
    v->name = n->name;
    v->ir_name = sb_str(&sb);
    v->is_global = true;
    v->declared = declared;
    v->type = declared;
    v->decl_tok = n->tok;
    v->next = s->scope->vars;
    s->scope->vars = v;

    n->ir_name = v->ir_name;
    n->is_global = true;
    n->type = declared;
}

// ── パス 2：本体の検査 ─────────────────────────────────────

static void check_func(Sema *s, Node *n) {
    // ★ メソッドは修飾名で表に載っています。モジュールが入ってからは、そこに
    //   モジュール名も付くので、定義ノードの IR 名から引きます。
    s->cur_func = lookup_func_by_ir(s, n->ir_name);
    s->used = NULL;  // IR 名は関数ごとに振り直す（別の関数なら衝突しない）

    scope_push(s);

    // 引数をローカル変数として登録する
    for (Node *pm = n->params; pm; pm = pm->next) {
        VarEntry *v = declare(s, pm->name, pm->type, pm->tok);
        pm->ir_name = v->ir_name;
    }

    // ★ 契約は**本体の先頭**にしか置けません（A-29）。
    //
    // なぜ場所を縛るのか
    //   requires は入口で、ensures は出口で確かめます。途中に書けると
    //   「書いた場所と確かめる場所が違う」ことになり、読む人が誤解します。
    //   注意: 先頭に並べる限り、順序は自由です（requires と ensures を混ぜても
    //     かまいません）。
    {
        bool seen_other = false;
        for (Node *st = n->body->body; st; st = st->next) {
            bool is_contract =
                st->kind == ND_REQUIRES || st->kind == ND_ENSURES;
            if (is_contract && seen_other) {
                Diag d = {0};
                d.message = MSG1("sema.348", "{0} は関数の本体の先頭に書きます", st->kind == ND_REQUIRES ? "requires"
                                                             : "ensures");
                d.primary.tok = st->tok;
                d.primary.label = MSG0("parse.063", "ここには書けません");
                d.hint = MSG0("sema.602", "requires / ensures は def の直後に並べてください（入口と出口で確かめるものだからです）");
                diag_fail(&d);
            }
            if (!is_contract) seen_other = true;
        }
    }

    rewrite_old(n);

    check_stmt_list(s, n->body->body);
    scope_pop(s);

    // 全経路で return するか（型システム 6.1）
    if (n->type->kind != TY_NONE && !always_returns(n->body)) {
        Diag d = {0};
        d.message = MSG1("sema.349", "関数 '{0}' は値を返さずに終わる経路があります", n->name);
        d.primary.tok = n->tok;
        d.primary.label = MSG1("sema.350", "戻り型は '{0}' です", type_name(n->type));
        d.hint = MSG0("sema.603", "すべての経路で return してください（if に else が無いと、条件が偽のとき素通りします）");
        diag_fail(&d);
    }
    s->cur_func = NULL;
}

// 外へ出す名前（C のシンボル）を作る。`pkg.mod` の `f` なら `pkg_mod_f`。
// ★ 対になる定義: codegen が同じ規則で名前を付けます（export_symbol）。
char *export_symbol(const char *mod, const char *fn) {
    StrBuf b;
    sb_init(&b);
    for (const char *c = mod; *c; c++) sb_printf(&b, "%c", *c == '.' ? '_' : *c);
    sb_printf(&b, "_%s", fn);
    return sb_str(&b);
}

// 外へ出す名前が重ならないか（E-EXPORT-5）。
//
// ★ 見るのは 2 つです。C のシンボル（`a_b.c` と `a.b_c` は同じ `a_b_c` になる）と、
//   Python から見た名前（関数名そのもの。生成する .py は 1 つの名前空間です）。
static void check_export_names(Module *mods) {
    for (Module *m = mods; m; m = m->next) {
        for (Node *d = m->ast->body; d; d = d->next) {
            if (d->kind != ND_FUNC || !d->is_export) continue;
            char *sym = export_symbol(m->name, d->name);
            for (Module *m2 = mods; m2; m2 = m2->next) {
                for (Node *e = m2->ast->body; e; e = e->next) {
                    if (e == d) goto next_d;  // 自分より前だけ比べる（1 回だけ報告する）
                    if (e->kind != ND_FUNC || !e->is_export) continue;
                    bool same_sym = strcmp(sym, export_symbol(m2->name, e->name)) == 0;
                    if (!same_sym && strcmp(d->name, e->name) != 0) continue;
                    Diag g = {0};
                    g.code = "E-EXPORT-5";
                    g.message = same_sym
                        ? MSG1("sema.351", "外へ出す名前 '{0}' が重なっています", sym)
                        : MSG1("sema.352", "外へ出す関数 '{0}' が 2 つあります", d->name);
                    g.primary.tok = d->tok;
                    g.primary.label = MSG0("sema.604", "こちらと");
                    g.related.tok = e->tok;
                    g.related.label = MSG0("sema.605", "こちらが同じ名前になります");
                    g.hint = MSG0("sema.606", "どちらかの名前を変えてください（Python からは関数名で、C からは「モジュール名_関数名」で呼びます）");
                    diag_fail(&g);
                }
            }
        next_d:;
        }
    }
}

// main の検査（言語仕様 6.1）
static void check_main(Sema *s, Node *ast) {
    FuncSig *m = lookup_func(s, "main");
    // ★ 外へ出す関数があれば、それはライブラリです（設計 ffi.md §6）。
    //   入口は外から呼ぶ側が持つので、main は要りません。
    if (!m) {
        for (Node *d = ast->body; d; d = d->next)
            if (d->kind == ND_FUNC && d->is_export) return;
    }
    if (!m) {
        Diag d = {0};
        d.message = MSG0("sema.607", "main 関数がありません");
        d.primary.tok = ast->tok;
        d.primary.label = MSG0("sema.608", "このファイルには入口がありません");
        d.hint = MSG0("sema.609", "プログラムの入口として次を定義してください:\n             def main() -> int:\n                 return 0");
        diag_fail(&d);
    }
    if (m->nparams != 0)
        error_at_hint_m(m->tok, MSG0("sema.354", "main は引数なしで定義してください（def main() -> int:）"), MSG0("sema.353", "main は引数を取れません"));
    if (m->ret->kind != TY_INT)
        error_at_hint_m(m->tok, MSG0("sema.356", "main の戻り値がプロセスの終了コードになります"), MSG0("sema.355", "main の戻り型は int でなければなりません"));
    // ★ R4：main の失敗を受け取る相手はいません。
    if (m->nraises) {
        Diag d = {0};
        d.code = "E-RAISE-4";
        d.message = MSG0("sema.610", "main は raises を宣言できません");
        d.primary.tok = m->tok;
        d.primary.label = MSG0("sema.611", "この失敗を受け取る相手がいません");
        d.hint = MSG0("sema.612", "main の中で try で捕まえるか、panic で終わらせてください");
        diag_fail(&d);
    }
}

// 範囲型を登録する（パス 1 の最初。A-28）
//
// ★ **クラスより先に**登録します。クラスのフィールドやメソッドの型注釈に
//   範囲型を書けるようにするためです（依存の向きは 範囲型 → int だけなので、
//   互いに参照し合うことはありません）。
// 既定値の並びと型を確かめる（A-38）。
//
// ★ 見るのは 2 つです。
//   ① **既定値のある引数は後ろにまとめる。** 途中に置けると
//      `f(1, , 3)` のような「飛ばし方」が要るか、位置引数が当たらなく
//      なります。まとめておけば「前から順に当てて、余りは既定値」で済みます。
//   ② **既定値の型が引数の型に入るか。** ここで確かめておかないと、
//      呼び出しごとに同じエラーが出ます（間違っているのは定義側です）。
static void check_defaults(Sema *s, FuncSig *f, Node *fn) {
    int first_default = -1;
    int i = 0;
    for (Node *pm = fn->params; pm; pm = pm->next, i++) {
        if (!f->defaults[i]) {
            if (first_default >= 0) {
                Diag d = {0};
                d.message = MSG1("sema.357", "既定値のある引数より後ろに、既定値の無い引数 '{0}' があります", pm->name);
                d.primary.tok = pm->tok;
                d.primary.label = MSG0("sema.613", "この引数にも既定値が要ります");
                d.related.tok = f->defaults[first_default]->tok;
                d.related.label = MSG1("sema.358", "'{0}' に既定値が付いています", f->pnames[first_default]);
                d.hint = MSG0("sema.614", "既定値のある引数は後ろにまとめてください（そうしないと、前から順に当てられません）");
                diag_fail(&d);
            }
            continue;
        }
        if (first_default < 0) first_default = i;

        // ★ 既定値の型を確かめます。**列挙の枝はここで定数に畳まれます**。
        Type *dt = check_expr(s, f->defaults[i]);
        if (!type_assignable(dt, f->params[i])) {
            Diag d = {0};
            d.message = MSG3("sema.359", "引数 '{0}' の既定値の型が違います（'{1}' に '{2}' は入りません）", pm->name, type_name(f->params[i]), type_name(dt));
            d.primary.tok = f->defaults[i]->tok;
            d.primary.label = MSG1("sema.051", "これは '{0}' 型です", type_name(dt));
            d.hint = no_implicit_hint(dt, f->params[i]);
            diag_fail(&d);
        }
        f->defaults[i] = range_coerce(s, f->defaults[i], f->params[i]);  // A-28
    }
}

// 列挙を登録する（A-37）。
//
// ★ **範囲型やクラスより先に**登録します。クラスのフィールドやメソッドの
//   型注釈に列挙を書けるようにするためです（列挙は何にも依存しないので、
//   互いに参照し合うことはありません）。
static void declare_enum(Sema *s, Node *n) {
    if (type_from_name(n->name))
        error_at_hint_m(n->tok, MSG1("sema.290", "'{0}' は組み込みの型名です", n->name), MSG0("sema.360", "この名前は使えません"));
    EnumDef *old = lookup_enum(s, n->name);
    if (old) {
        Diag d = {0};
        d.message = MSG1("sema.361", "型 '{0}' はすでに定義されています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.615", "同じ名前の列挙が 2 つあります");
        d.related.tok = old->tok;
        d.related.label = MSG0("parse.224", "最初の定義はここです");
        diag_fail(&d);
    }
    if (lookup_class(s, n->name))
        error_at_hint_m(n->tok, MSG0("sema.362", "クラスと同じ名前の列挙は作れません"), MSG1("sema.334", "'{0}' はクラス名として使われています", n->name));
    if (lookup_range(s, n->name))
        error_at_hint_m(n->tok, MSG0("sema.364", "範囲型と同じ名前の列挙は作れません"), MSG1("sema.363", "'{0}' は範囲型として使われています", n->name));

    EnumDef *e = xmalloc(sizeof(EnumDef));
    e->name = n->name;
    e->tok = n->tok;
    e->vals = NULL;
    e->nvals = 0;
    e->owner = s->cur;

    e->has_payload = n->has_payload;   // A-41

    // 枝を宣言した順に並べます（番号は parser が振ってあります）。
    EnumVal *tail = NULL;
    for (Node *v = n->body; v; v = v->next) {
        EnumVal *ev = xmalloc(sizeof(EnumVal));
        ev->name = v->name;
        ev->val = v->ival;
        ev->tok = v->tok;
        ev->next = NULL;
        // ★ 中身を持つ枝の数だけ数えておきます（A-41）。
        //   隠しクラスは**あとで**結びます（branch_class）——列挙は
        //   **クラスより先に**登録するので、この時点ではまだありません。
        for (Node *f = v->params; f; f = f->next) ev->nfields++;
        if (tail) tail->next = ev; else e->vals = ev;
        tail = ev;
        e->nvals++;
    }

    e->type = type_enum(e->name, e);
    e->next = s->enums;
    s->enums = e;
    s->cur->enums = e;
    n->en = e;
    n->type = e->type;
}

static void declare_range(Sema *s, Node *n) {
    if (type_from_name(n->name))
        error_at_hint_m(n->tok, MSG1("sema.290", "'{0}' は組み込みの型名です", n->name), MSG0("sema.360", "この名前は使えません"));
    RangeTy *old = lookup_range(s, n->name);
    if (old) {
        Diag d = {0};
        d.message = MSG1("sema.361", "型 '{0}' はすでに定義されています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = MSG0("sema.616", "同じ名前の範囲型が 2 つあります");
        d.related.tok = old->tok;
        d.related.label = MSG0("parse.224", "最初の定義はここです");
        diag_fail(&d);
    }
    if (lookup_class(s, n->name))
        error_at_hint_m(n->tok, MSG0("sema.365", "クラスと同じ名前の範囲型は作れません"), MSG1("sema.334", "'{0}' はクラス名として使われています", n->name));

    RangeTy *r = xmalloc(sizeof(RangeTy));
    r->name = n->name;
    r->tok = n->tok;
    r->type = type_range(n->name, n->lhs->ival, n->rhs->ival);
    r->next = s->ranges;
    s->ranges = r;
    n->type = r->type;
}

// モジュール 1 つぶんの宣言を登録する（パス 1a / 1b / 1c）
static void declare_module(Sema *s, Node *ast) {
    // ★ 列挙を最初に登録します（クラスのフィールドにも書けるように。A-37）
    for (Node *d = ast->body; d; d = d->next)
        if (d->kind == ND_ENUM) declare_enum(s, d);

    // ★ 範囲型を次に登録します（クラスのフィールドにも書けるように）
    for (Node *d = ast->body; d; d = d->next)
        if (d->kind == ND_RANGEDECL) declare_range(s, d);

    // ★ インタフェースを最初に登録します（クラスが実装を宣言するため）
    for (Node *d = ast->body; d; d = d->next)
        if (d->kind == ND_IFACE) declare_iface(s, d);

    // 1a：クラス名だけ先に登録する（クラスどうしが互いを参照できるように）
    for (Node *d = ast->body; d; d = d->next)
        if (d->kind == ND_CLASS) declare_class(s, d);

    // 1b：フィールドとメソッド（型注釈に他のクラスを書ける）
    //
    // 注意: **ジェネリックなテンプレートはここでは並べません。**
    //   K や V が何なのかまだ決まっていないので、フィールドの大きさも
    //   メソッドの型も決められません。実体ができたときに行います。
    for (Node *d = ast->body; d; d = d->next)
        if (d->kind == ND_CLASS && !d->targs) declare_class_members(s, d);

    // 1c：トップレベルの関数とグローバル変数（引数の型にクラスを書ける）
    for (Node *d = ast->body; d; d = d->next) {
        if (d->kind == ND_FUNC) declare_func(s, d);
        else if (d->kind == ND_VARDECL) declare_global(s, d);
        else if (d->kind == ND_CLASS) continue;  // 1a / 1b で済んでいる
        else if (d->kind == ND_IMPORT) continue;  // 読み込みは module.c が済ませた
        else if (d->kind == ND_PRAGMA) continue;  // 設定（宣言ではない）
        else if (d->kind == ND_IFACE) continue;   // 上で済んでいる
        else if (d->kind == ND_RANGEDECL) continue;  // 上で済んでいる
        else if (d->kind == ND_ENUM) continue;       // 上で済んでいる（A-37）
        else UNREACHABLE();  // parser が保証している
    }
}

// モジュール 1 つぶんの本体を検査する（パス 2）
static void check_module(Sema *s, Node *ast) {
    for (Node *d = ast->body; d; d = d->next) {
        // 注意: extern は本体を持たないので検査するものがありません
        // 注意: ジェネリックなテンプレートの本体は検査しません
        if (d->kind == ND_FUNC) { if (d->body && !d->targs) check_func(s, d); }
        // メソッドの本体も、ふつうの関数とまったく同じ手順で検査します。
        // self はもう「型が入った引数」なので、特別扱いは 1 つも要りません。
        // 注意: ジェネリックなテンプレートの本体は検査しません。
        //   実体ができてから、その実体の本体を検査します。
        else if (d->kind == ND_CLASS && !d->targs)
            for (Node *m = d->body; m; m = m->next)
                if (m->kind == ND_FUNC) check_func(s, m);
    }
}

// ★ 意味解析の単位が「1 つの AST」から「全モジュール」になりました。
//
//   パス 0   読み込みと構文解析（module.c が依存順に並べて渡してくる）
//   パス 1   モジュールごとに宣言を登録する    ← 依存順なので、
//   パス 2   モジュールごとに本体を検査する       先に登録済みのものだけを参照する
//
// 関数の前方参照やクラスの相互参照と同じ「先に全部登録」を、
// ファイル単位でもう 1 回やっているだけです。
void sema_program(Module *mods, Module *entry) {
    Sema s = {0};

    // ★ 型の代入互換に「実装しているか」を教える
    class_implements_hook = class_implements;

    // 各モジュールのシンボル表を用意する（この時点では空）
    ModuleSyms *tail = NULL;
    for (Module *m = mods; m; m = m->next) {
        if (m->ast->kind != ND_BLOCK) UNREACHABLE();
        ModuleSyms *ms = xmalloc(sizeof(ModuleSyms));
        ms->mod = m;
        ms->globals = xmalloc(sizeof(Scope));
        m->syms = ms;
        if (tail) tail->next = ms;
        else s.mods = ms;
        tail = ms;
    }

    // パス 1：依存が先に並んでいるので、この順で登録すれば
    //         「他モジュールの型注釈」は必ず解決できる
    for (ModuleSyms *ms = s.mods; ms; ms = ms->next) {
        enter_module(&s, ms);
        declare_module(&s, ms->mod->ast);
    }

    // パス 2：本体
    for (ModuleSyms *ms = s.mods; ms; ms = ms->next) {
        enter_module(&s, ms);
        check_module(&s, ms->mod->ast);
    }

    // ★ パス 3：実体化したクラスの本体を検査する
    //
    // 注意: 検査の途中で **さらに実体が増える**ことがあります
    //   （Dict[str, Box[int]] のように入れ子になっている場合）。
    //   増えなくなるまで繰り返します。
    while (s.pending) {
        Instance *q = s.pending;
        s.pending = NULL;                 // ★ 先に外す（この回の分だけを処理する）
        for (Instance *it = q; it; it = it->next) {
            enter_module(&s, it->owner);
            s.tbind = it->binds;          // 型引数を戻してから本体を読む
            s.inst_site = it->site;       // ★ 発端を控える
            s.inst_name = it->iname;
            if (it->is_func) {
                check_func(&s, it->node);      // 関数の実体
            } else {
                for (Node *m = it->node->body; m; m = m->next)
                    if (m->kind == ND_FUNC) check_func(&s, m);
            }
            s.tbind = NULL;
            s.inst_site = NULL;
        }
    }

    // ★ vtable の長さを codegen に伝える
    pl_iface_slots = s.next_slot;

    // main は入口モジュールにだけ要る（他のモジュールにあっても構わない）
    enter_module(&s, entry->syms);
    check_main(&s, entry->ast);

    check_export_names(mods);
}
