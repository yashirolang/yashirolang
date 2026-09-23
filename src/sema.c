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
//   「まず動かす、測ってから直す」が原則（docs/spec/type-system.md 7.2）。

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
    //   （docs/design/error-handling.md §2）。
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
    d.message = diag_fmt("'%s' は import したモジュールの名前です", name);
    d.primary.tok = tok;
    d.primary.label = diag_fmt("この名前の%sは宣言できません", what);
    d.hint = diag_fmt("モジュール名と同じ名前を使うと '%s.x' が曖昧になります",
                      name);
    diag_fail(&d);
}

// ローカル変数として登録する（IR 名は %x 形式）
static VarEntry *declare(Sema *s, char *name, Type *type, Token *tok) {
    reject_module_name(s, name, tok, "変数");

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
static Type *check_method(Sema *s, Node *n);
static Type *check_class_method(Sema *s, Node *n, Class *c);
static Type *check_field(Sema *s, Node *n);
// 既定値の並びと型を確かめる（A-38）
static void check_defaults(Sema *s, FuncSig *f, Node *fn);
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
            d.message = diag_fmt("%lld は '%s' の範囲（%lld..%lld）の外です",
                                 val->ival, want->name, want->lo, want->hi);
            d.primary.tok = val->tok;
            d.primary.label = "この値はこの型に入りません";
            d.hint = diag_fmt("'%s' に入れられるのは %lld から %lld までです",
                              want->name, want->lo, want->hi);
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
        return t;
    }

    Type *base = resolve_base_type(s, tr);
    if (!tr->nullable) return base;

    if (!type_can_be_opt(base)) {
        Diag d = {0};
        d.message = diag_fmt("'%s | None' は書けません", type_name(base));
        d.primary.tok = tr->tok;
        d.primary.label = "この型は None になれません";
        d.hint = "None はヌルポインタとして表すので、int や bool には付けられません"
                 "（nullable にできるのは str / list[T] / class です）";
        diag_fail(&d);
    }
    return type_opt(base);
}

// ── ジェネリクス（単相化） ─────────────────────────────
//
// ★ 方針は docs/design/generics-and-interfaces.md §1 のとおり **単相化**です。
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
        d.message = diag_fmt("'%s' は型引数を %d 個取りますが、%d 個書かれました",
                             tmpl->name, want, nargs);
        d.primary.tok = at;
        d.primary.label = "型引数の個数が違います";
        d.related.tok = tmpl->tok;
        d.related.label = "このクラスの定義です";
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
            error_at(tr->tok, "型引数が多すぎます（最大 %d 個です）", max);
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
            d.message = diag_fmt("モジュール '%s' を import していません", tr->mod_name);
            d.primary.tok = tr->tok;
            d.primary.label = "この修飾を解決できません";
            d.hint = diag_fmt("ファイルの先頭に 'import %s' を書いてください",
                              tr->mod_name);
            diag_fail(&d);
        }
        // ★ 他のモジュールの範囲型（A-28）
        RangeTy *mr = lookup_range_in(ms, tr->name);
        if (mr) {
            if (tr->lhs)
                error_at_hint(tr->tok, "範囲型は要素型を取りません",
                              "型 '%s.%s' は要素型を取りません", tr->mod_name,
                              tr->name);
            return mr->type;
        }

        // ★ 他のモジュールのインタフェース
        // ★ 他のモジュールの列挙（A-37）
        EnumDef *me = lookup_enum_in(ms, tr->name);
        if (me) {
            if (tr->lhs)
                error_at_hint(tr->tok, "列挙は要素型を取りません",
                              "型 '%s.%s' は要素型を取りません", tr->mod_name,
                              tr->name);
            return me->type;
        }

        Iface *mi = lookup_iface_in(ms, tr->name);
        if (mi) return type_iface(mi->name, mi);

        Class *c = lookup_class_in(ms, tr->name);
        if (!c) {
            Diag d = {0};
            d.message = diag_fmt("モジュール '%s' にクラス '%s' はありません",
                                 tr->mod_name, tr->name);
            d.primary.tok = tr->tok;
            d.primary.label = "このクラスは定義されていません";
            d.hint = "他のモジュールから使えるのはクラスだけです"
                     "（int や list はモジュール修飾なしで書きます）";
            diag_fail(&d);
        }
        // ★ 他のモジュールのジェネリッククラスも実体化できます
        if (c->node && c->node->targs) {
            Type *args[MAX_TARGS];
            int n = collect_targs(s, tr, args, MAX_TARGS);
            return instantiate_class(s, c, args, n, tr->tok);
        }
        if (tr->lhs)
            error_at_hint(tr->tok, "要素型を取るのは list / rc / mutex / Thread だけです",
                          "型 '%s.%s' は要素型を取りません", tr->mod_name, tr->name);
        return c->type;
    }

    if (strcmp(tr->name, "list") == 0) {
        if (!tr->lhs)
            error_at_hint(tr->tok, "要素型を書いてください（例: list[int]）",
                          "list には要素型が必要です");
        Type *elem = resolve_type(s, tr->lhs);  // ★ 再帰
        if (elem->kind == TY_NONE)
            error_at_hint(tr->tok, "None 型の値は存在しないので要素にできません",
                          "list の要素型に None は使えません");
        return type_list(elem);
    }

    // ── ptr[T]（生ポインタ）──
    //
    // ★ 中身に取れるのは int だけにしてあります。OS で触るのはメモリの番地で、
    //   そこに「本言語の型」は載っていないからです（仕様 §10.2）。
    if (strcmp(tr->name, "ptr") == 0) {
        if (!tr->lhs)
            error_at_hint(tr->tok, "中身の型を書いてください（例: ptr[int]）",
                          "ptr には中身の型が必要です");
        Type *elem = resolve_type(s, tr->lhs);
        if (elem->kind != TY_INT)
            error_at_hint(tr->tok, "いま ptr に書けるのは int だけです（例: ptr[int]）",
                          "'%s' は ptr に入れられません", type_name(elem));
        return type_ptr(elem);
    }

    // ── rc[T]（共有所有）──
    //
    // ★ 中身に取れるのはクラスだけにしてあります。
    //   「所有者を 1 つに決められない」のは木やグラフの節点で、
    //   それはクラスとして書かれるからです（仕様 §7.1）。
    if (strcmp(tr->name, "rc") == 0) {
        if (!tr->lhs)
            error_at_hint(tr->tok, "中身の型を書いてください（例: rc[Node]）",
                          "rc には中身の型が必要です");
        Type *elem = resolve_type(s, tr->lhs);
        if (elem->kind != TY_CLASS)
            error_at_hint(tr->tok,
                          "rc に入れられるのはクラスだけです（例: rc[Node]）",
                          "'%s' は rc に入れられません", type_name(elem));
        return type_rc(elem);
    }

    // ── Thread[R] / mutex[T]（A-18）──
    //
    // ★ Thread[R] の R は**戻り型**です。spawn した関数が返すものを、
    //   join が返します。
    if (strcmp(tr->name, "Thread") == 0) {
        if (!tr->lhs)
            error_at_hint(tr->tok, "戻り型を書いてください（例: Thread[int]）",
                          "Thread には戻り型が必要です");
        return type_thread(resolve_type(s, tr->lhs));
    }

    // ★ mutex に入れられるのは「共有したい実体」なので、rc[T] と同じく
    //   クラスとリストに限ります。int を包んでも取り出せないためです
    //   （lock に渡した関数が受け取るのは中身で、書き換えるには参照が要る）。
    if (strcmp(tr->name, "mutex") == 0) {
        if (!tr->lhs)
            error_at_hint(tr->tok, "中身の型を書いてください（例: mutex[Counter]）",
                          "mutex には中身の型が必要です");
        Type *elem = resolve_type(s, tr->lhs);
        if (elem->kind != TY_CLASS && elem->kind != TY_LIST)
            error_at_hint(tr->tok,
                          "mutex に入れられるのはクラスかリストだけです"
                          "（例: mutex[Counter] / mutex[list[float]]）",
                          "'%s' は mutex に入れられません", type_name(elem));
        return type_mutex(elem);
    }

    // ★ 型引数の名前（class Dict[K, V] の K）。
    //   注意: **クラス名より先に**引きます。テンプレートを読む間だけ有効です。
    Type *tv = lookup_tbind(s, tr->name);
    if (tv) {
        if (tr->lhs)
            error_at_hint(tr->tok, "型引数そのものは型引数を取りません",
                          "型 '%s' は型引数を取りません", tr->name);
        return tv;
    }

    Type *t = type_from_name(tr->name);
    if (t) {
        if (tr->lhs)
            error_at_hint(tr->tok, "要素型を取るのは list と rc だけです",
                          "型 '%s' は要素型を取りません", tr->name);
        return t;
    }

    // ★ 範囲型（部分型。A-28）。クラス名より先に引きます。
    RangeTy *rt = lookup_range(s, tr->name);
    if (rt) {
        if (tr->lhs)
            error_at_hint(tr->tok, "範囲型は要素型を取りません",
                          "型 '%s' は要素型を取りません", tr->name);
        return rt->type;
    }

    // ★ 列挙（A-37）。クラス名より先に引きます。
    EnumDef *et0 = lookup_enum(s, tr->name);
    if (et0) {
        if (tr->lhs)
            error_at_hint(tr->tok, "列挙は要素型を取りません",
                          "型 '%s' は要素型を取りません", tr->name);
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
                error_at_hint(a->tok, "None 型の値は存在しないので要素にできません",
                              "タプルの要素に None は使えません");
            t->params[k++] = et;
        }
        return t;
    }

    // ★ インタフェース名も型として書けます（list[Show] など）
    Iface *ifc0 = lookup_iface(s, tr->name);
    if (ifc0) {
        if (tr->lhs)
            error_at_hint(tr->tok, "インタフェースは型引数を取りません",
                          "型 '%s' は型引数を取りません", tr->name);
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
            error_at_hint(tr->tok, "要素型を取るのは list と rc だけです",
                          "型 '%s' は要素型を取りません", tr->name);
        return c->type;
    }

    Diag d = {0};
    d.message = diag_fmt("未知の型名 '%s' です", tr->name);
    d.primary.tok = tr->tok;
    d.primary.label = "この型は存在しません";
    d.hint = diag_fmt("現在使える型: %s、および定義したクラス名", type_name_list());
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
        return diag_fmt("'%s' と '%s' は名前が同じだけの別のクラスです"
                        "（同じ型かどうかは名前ではなく定義で決まります）",
                        got->cls->ir_name, want->cls->ir_name);
    // ★ 関数型どうしなら、**どこが違うのか**を言います。
    //   「暗黙の型変換がありません」では、何を直せばよいか分かりません。
    if (got->kind == TY_FN && want->kind == TY_FN) {
        if (got->nparams != want->nparams)
            return diag_fmt("引数の数が違います（%d 個と %d 個）", got->nparams,
                            want->nparams);
        for (int i = 0; i < got->nparams; i++)
            if (!type_equal(got->params[i], want->params[i]))
                return diag_fmt("%d 番目の引数の型が違います（'%s' と '%s'）",
                                i + 1, type_name(got->params[i]),
                                type_name(want->params[i]));
        return diag_fmt("戻り型が違います（'%s' と '%s'）", type_name(got->elem),
                        type_name(want->elem));
    }

    // ★ クラス → インタフェースなら、実装宣言の書き忘れを疑います。
    if (want->kind == TY_IFACE && got->kind == TY_CLASS)
        return diag_fmt("'%s' が '%s' を実装すると宣言していません"
                        "（'class %s(%s):' と書きます）",
                        got->cls->name, want->iface->name, got->cls->name,
                        want->iface->name);

    return "本言語には暗黙の型変換がありません（言語仕様 3.5）";
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
        d.message = diag_fmt("%s は None との比較にだけ使えます", op_symbol(n->op));
        d.primary.tok = n->rhs->tok;
        d.primary.label = diag_fmt("ここには None を書いてください（型 '%s' の式です）",
                                   type_name(r));
        d.hint = "値が等しいかを調べるには == を使ってください";
        diag_fail(&d);
    }
    n->rhs->type = ty_null;

    if (l->kind != TY_OPT) {
        Diag d = {0};
        d.message = diag_fmt("型 '%s' の値が None になることはありません",
                             type_name(l));
        d.primary.tok = n->lhs->tok;
        d.primary.label = "この式は必ず値を持ちます";
        d.hint = type_can_be_opt(l)
                     ? diag_fmt("None を入れたいなら、型注釈を '%s | None' に"
                                "してください", type_name(l))
                     : "None になれるのは str / list[T] / class だけです";
        diag_fail(&d);
    }
    return ty_bool;
}

// 二項演算子が、その型に適用できるか
static bool op_supports(OpKind op, Type *t) {
    // ★ クラスと list は「参照」なので、比べられるのは
    //   同一性（== / !=）だけです。大小関係には意味がありません
    //   （言語仕様 4.3 / docs/spec/type-system.md 5.6）。
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
    // 言語仕様 4.3 / docs/spec/type-system.md 5.5
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
    d.primary.label = diag_fmt("これは '%s' 型です", type_name(actual));
    d.related.tok = where_tok;
    d.related.label = where_label;
    d.hint = "本言語は int を真偽値として扱いません（言語仕様 4.4）。"
             "比較を書いてください（例: x != 0）";
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
                error_at(n->tok,
                         "'!=' に使うには __eq__ が bool を返す必要があります"
                         "（いまは '%s' を返しています）",
                         type_name(rt));
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

    // ★ 検査は 2 段構え（docs/spec/type-system.md 5.3）
    //   ① 両辺の型が等しいか
    //   ② その型がその演算子を支持するか
    //   この順にするとコードが短くなり、エラーメッセージも的確になります。
    if (!type_equal(l, r)) {
        Diag d = {0};
        d.message = diag_fmt("型 '%s' と '%s' に演算子 '%s' は適用できません",
                             type_name(l), type_name(r), op_symbol(n->op));
        d.primary.tok = n->tok;
        d.primary.label = "この演算子の両辺の型が違います";
        d.hint = no_implicit_hint(l, r);
        diag_fail(&d);
    }

    if (!op_supports(n->op, l)) {
        if (n->op == OP_TRUEDIV) {
            // 当初は codegen で弾いていた検査を、本来の担当である
            // 意味解析パスに移しました。
            error_at_hint(n->tok,
                          "切り捨て除算の '//' を使ってください"
                          "（本言語には暗黙の型変換がないため、'/' は float 専用です）",
                          "整数の除算に '/' は使えません");
        }
        if (l->kind == TY_CLASS && op_method_name(n->op))
            error_at_hint(n->tok,
                          diag_fmt("クラス '%s' に '%s(self, other) -> …' を"
                                   "定義すると、この演算子が使えます",
                                   l->cls->name, op_method_name(n->op)),
                          "型 '%s' に演算子 '%s' は適用できません", type_name(l),
                          op_symbol(n->op));
        error_at(n->tok, "型 '%s' に演算子 '%s' は適用できません", type_name(l),
                 op_symbol(n->op));
    }

    // 0 除算のうち、右辺がリテラル 0 の場合はここで弾く。
    // 右辺が式の場合は実行時 SIGFPE。実行時チェックを入れます
    if ((n->op == OP_FLOORDIV || n->op == OP_MOD) && n->rhs->kind == ND_INT &&
        n->rhs->ival == 0) {
        Diag d = {0};
        d.message = "0 で除算しています";
        d.primary.tok = n->rhs->tok;
        d.primary.label = "この 0 で割ろうとしています";
        d.related.tok = n->tok;
        d.related.label = diag_fmt("演算子 '%s' はここです", op_symbol(n->op));
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

    char *msg = diag_fmt("演算子 '%s' には bool が必要です", op_symbol(n->op));
    char *lbl = diag_fmt("演算子 '%s' はここです", op_symbol(n->op));
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
        d.message = diag_fmt("タプルの要素の数が違います（%d 個と %d 個）",
                             n_elems, want->nparams);
        d.primary.tok = n->tok;
        d.primary.label = diag_fmt("ここは %d 個です", n_elems);
        d.hint = diag_fmt("'%s' が必要です", type_name(want));
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
            d.message = diag_fmt("タプルの %d 番目の型が合いません", k + 1);
            d.primary.tok = x->tok;
            d.primary.label = diag_fmt("これは '%s' 型です", type_name(et));
            d.hint = diag_fmt("ここには '%s' が必要です",
                              type_name(want->params[k]));
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
        d.message = diag_fmt("'%s' 型はスライスできません", type_name(t));
        d.primary.tok = n->lhs->tok;
        d.primary.label = "ここには str か list[T] が必要です";
        diag_fail(&d);
    }
    if (n->rhs) {
        Type *a = check_expr(s, n->rhs);
        if (a->kind != TY_INT)
            error_at(n->rhs->tok, "スライスの開始は int です（'%s' 型でした）",
                     type_name(a));
    }
    if (n->els) {
        Type *b = check_expr(s, n->els);
        if (b->kind != TY_INT)
            error_at(n->els->tok, "スライスの終端は int です（'%s' 型でした）",
                     type_name(b));
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
                error_at(n->tok,
                         "'in' に使うには __contains__ が bool を返す必要が"
                         "あります（いまは '%s' を返しています）",
                         type_name(rt));
            return ty_bool;
        }

        // not in は結果を反転させます
        Node *call = new_node(ND_METHOD, n->tok);
        call->lhs = obj;
        call->name = "__contains__";
        call->args = arg;
        Type *rt = check_class_method(s, call, r->cls);
        if (rt->kind != TY_BOOL)
            error_at(n->tok,
                     "'not in' に使うには __contains__ が bool を返す必要が"
                     "あります（いまは '%s' を返しています）",
                     type_name(rt));
        call->type = rt;
        n->kind = ND_UNARY;
        n->op = OP_NOT;
        n->lhs = call;
        n->rhs = NULL;
        return ty_bool;
    }

    if (r->kind == TY_STR) {
        if (l->kind != TY_STR)
            error_at(n->tok,
                     "str の 'in' には str が必要です（左辺は '%s' 型です）",
                     type_name(l));
        return ty_bool;
    }
    if (r->kind == TY_LIST) {
        if (!type_assignable(l, r->elem)) {
            Diag d = {0};
            d.message = "'in' の左辺が要素の型と合いません";
            d.primary.tok = n->lhs->tok;
            d.primary.label = diag_fmt("これは '%s' 型です", type_name(l));
            d.related.tok = n->rhs->tok;
            d.related.label = diag_fmt("こちらの要素は '%s' 型です",
                                       type_name(r->elem));
            diag_fail(&d);
        }
        return ty_bool;
    }

    Diag d = {0};
    d.message = diag_fmt("'%s' 型に 'in' は使えません", type_name(r));
    d.primary.tok = n->rhs->tok;
    d.primary.label = "ここには list[T] か str が必要です";
    d.hint = r->kind == TY_CLASS
                 ? diag_fmt("クラス '%s' に '__contains__(self, x) -> bool' を"
                            "定義すると、'in' が使えます", r->cls->name)
                 : "dict の鍵を調べるには d.has(k) を使ってください";
    diag_fail(&d);
    return ty_bool;
}

// 三項演算子 a if c else b
// 注意: 名前は check_ternary。check_cond は「文の条件式」用に既にあります。
static Type *check_ternary(Sema *s, Node *n) {
    Type *c = check_expr(s, n->lhs);
    if (c->kind != TY_BOOL)
        bool_required("三項演算子の条件には bool が必要です",
                      "この 'if' の条件です", n->tok, n->lhs, c);

    Type *a = check_expr(s, n->rhs);
    Type *b = check_expr(s, n->els);

    // ★ どちらかが None リテラルなら、もう一方に合わせます
    //   x if c else None が書けるように。代入互換と同じ扱い
    if (type_assignable(b, a)) return a;
    if (type_assignable(a, b)) return b;

    Diag d = {0};
    d.message = "三項演算子の両側で型が違います";
    d.primary.tok = n->els->tok;
    d.primary.label = diag_fmt("こちらは '%s' 型です", type_name(b));
    d.related.tok = n->rhs->tok;
    d.related.label = diag_fmt("こちらは '%s' 型です", type_name(a));
    d.hint = "式の型は 1 つに決まらなければなりません（どちらかを合わせてください）";
    diag_fail(&d);
    return a;
}

static Type *check_unary(Sema *s, Node *n) {
    Type *t = check_expr(s, n->lhs);

    // not は bool を取り bool を返す（言語仕様 4.4）
    if (n->op == OP_NOT) {
        if (t->kind != TY_BOOL)
            return bool_required(
                diag_fmt("演算子 '%s' には bool が必要です", op_symbol(n->op)),
                diag_fmt("演算子 '%s' はここです", op_symbol(n->op)), n->tok, n->lhs,
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
        error_at(n->tok, "型 '%s' に単項演算子 '%s' は適用できません", type_name(t),
                 op_symbol(n->op));
    }

    // - + ~ は int のみ
    if (t->kind != TY_INT)
        error_at(n->tok, "型 '%s' に単項演算子 '%s' は適用できません", type_name(t),
                 op_symbol(n->op));
    return t;
}

// FuncSig から関数型を作る
static Type *fn_type_of(FuncSig *f) {
    Type *t = xmalloc(sizeof(Type));
    t->kind = TY_FN;
    t->nparams = f->nparams;
    t->params = f->params;
    t->elem = f->ret;
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
            d.message = "戻り値の無い関数では 'result' を書けません";
            d.primary.tok = n->tok;
            d.primary.label = "この関数は値を返しません";
            d.hint = "戻り値を見ない ensures（グローバルの条件など）にするか、"
                     "戻り型を付けてください";
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
        if (f && f->tmpl && f->tmpl->is_lambda) f = instantiate_lambda(s, f, n);
        if (f && f->tmpl) {
            // ジェネリック関数そのものは値にできません（型引数が決まらない）。
            Diag d = {0};
            d.message = diag_fmt("'%s' は型引数を取る関数なので、値にできません",
                                 n->name);
            d.primary.tok = n->tok;
            d.primary.label = "ここでは値として使えません";
            d.hint = "値にするには型が 1 つに決まっている必要があります"
                     "（呼び出しなら実引数から決まります）";
            diag_fail(&d);
        }
        if (f) {
            if (f->nraises > 0) {
                Diag d = {0};
                d.message = diag_fmt("'%s' は raises する関数なので値にできません",
                                     n->name);
                d.primary.tok = n->tok;
                d.primary.label = "ここでは値として使えません";
                d.hint = "関数型はエラーの受け渡しを表せません"
                         "（raises しない関数で包んでください）";
                diag_fail(&d);
            }
            n->ir_name = f->ir_name;
            n->is_func_ref = true;
            return fn_type_of(f);
        }
    }

    if (!v) {
        // ★ モジュール名そのものは値ではありません。
        if (lookup_import(s, n->name)) {
            Diag d = {0};
            d.message = diag_fmt("モジュール '%s' は値として使えません", n->name);
            d.primary.tok = n->tok;
            d.primary.label = "ここにはモジュール名を書けません";
            d.hint = diag_fmt("モジュールの中身は '%s.名前' の形で使います", n->name);
            diag_fail(&d);
        }

        // 同名のモジュールが存在するのに import していない場合。
        // ★ 「未定義の名前です」で突き放さず、書き忘れを指摘します。
        if (module_file_exists(s->cur->mod->dir, n->name)) {
            Diag d = {0};
            d.message = diag_fmt("モジュール '%s' を import していません", n->name);
            d.primary.tok = n->tok;
            d.primary.label = "このモジュールはここからは見えません";
            d.hint = diag_fmt("ファイルの先頭に 'import %s' を書いてください",
                              n->name);
            diag_fail(&d);
        }

        // ★ lambda の中から外の変数を使おうとした場合（A-42）。
        //   持ち上げた先はトップレベルなので、外の名前はそこから見えません。
        //   「未定義の名前です」で突き放すと、何が起きたのか分かりません。
        if (s->cur_func && s->cur_func->is_lambda) {
            Diag d = {0};
            d.message = diag_fmt("lambda の中から外の変数 '%s' は使えません",
                                 n->name);
            d.primary.tok = n->tok;
            d.primary.label = "この名前は lambda の外のものです";
            d.hint = "捕獲（クロージャ）はまだありません。"
                     "使う値は引数で受け取るか、def で書いた関数にしてください"
                     "（グローバルなら使えます）";
            diag_fail(&d);
        }

        Diag d = {0};
        d.message = diag_fmt("未定義の名前 '%s' です", n->name);
        d.primary.tok = n->tok;
        d.primary.label = "この名前は宣言されていません";
        d.hint = diag_fmt("使う前に宣言してください（例: %s: int = 0）", n->name);
        diag_fail(&d);
    }
    n->ir_name = v->ir_name;  // ★ codegen はこれを使う
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
            error_at_hint(n->tok, "None 型の値は存在しないので変数にできません",
                          "変数の型に None は使えません");
    }

    // ② 同じスコープでの再宣言を禁止（言語仕様 5.1）
    VarEntry *prev = lookup_local(s, n->name);
    if (prev) {
        Diag d = {0};
        d.message = diag_fmt("変数 '%s' は既に宣言されています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = "ここで再宣言されています";
        d.related.tok = prev->decl_tok;
        d.related.label = "最初の宣言はここです";
        d.hint = "既存の変数に代入するなら型注釈を外してください（例: x = 1）";
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
        d.message = diag_fmt("変数 '%s' は外側のスコープの変数を隠しています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = "シャドーイングは禁止されています（言語仕様 5.1）";
        d.related.tok = outer->decl_tok;
        d.related.label = "外側の宣言はここです";
        d.hint = "別の名前にするか、型注釈を外して既存の変数に代入してください"
                 "（例: x = 1）";
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
            error_at_hint(n->rhs->tok, "値を返さない式は変数に入れられません",
                          "None 型の値は変数にできません");
        declared = actual;
    }

    if (!type_assignable(actual, declared)) {
        Diag d = {0};
        d.message = "型が一致しません";
        d.primary.tok = n->rhs->tok;
        d.primary.label = diag_fmt("型 '%s' の式", type_name(actual));
        d.related.tok = n->tok;
        d.related.label =
            diag_fmt("変数 '%s' は '%s' 型として宣言されています", n->name,
                     type_name(declared));
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
                error_at_hint(target->tok,
                              diag_fmt("クラス '%s' に '__setitem__(self, …, 値) -> None' を"
                                       "定義すると、添字への代入が使えます",
                                       ot->cls->name),
                              "型 '%s' の添字には代入できません", type_name(ot));

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
            error_at_hint(target->tok,
                          "タプルは作ったら変わりません（新しいタプルを作って"
                          "ください）",
                          "タプルの要素には代入できません");

        // 注意: str は不変（immutable）なので s[0] = "x" は書けません（言語仕様 3.1）
        if (target->lhs->type->kind == TY_STR)
            error_at_hint(target->tok,
                          "str は不変（immutable）です。新しい文字列を作ってください",
                          "文字列の要素には代入できません");

        s->expected = et;
        Type *actual = check_expr(s, n->rhs);
        s->expected = NULL;

        if (!type_assignable(actual, et)) {
            Diag d = {0};
            d.message = "型が一致しません";
            d.primary.tok = n->rhs->tok;
            d.primary.label = diag_fmt("型 '%s' の式", type_name(actual));
            d.related.tok = target->tok;
            d.related.label = diag_fmt("この要素は '%s' 型です", type_name(et));
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
        s->expected = NULL;

        if (!type_assignable(actual, ft)) {
            Diag d = {0};
            d.message = "型が一致しません";
            d.primary.tok = n->rhs->tok;
            d.primary.label = diag_fmt("型 '%s' の式", type_name(actual));
            d.related.tok = target->field->tok;
            d.related.label = diag_fmt("フィールド '%s' は '%s' 型として宣言されています",
                                       target->field->name, type_name(ft));
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
        d.message = diag_fmt("未定義の名前 '%s' に代入しています", target->name);
        d.primary.tok = target->tok;
        d.primary.label = "この名前は宣言されていません";
        d.hint = diag_fmt("初めて使うときは型注釈が必要です（例: %s: int = 0）",
                          target->name);
        diag_fail(&d);
    }
    target->type = v->type;
    target->ir_name = v->ir_name;

    // ★ 代入できるかは「宣言された型」で判定します。
    //   絞り込みで一時的に狭くなっていても、代入できる範囲は変わりません。
    s->expected = v->declared;  // ★ xs = [] のため
    Type *actual = check_expr(s, n->rhs);
    s->expected = NULL;
    if (!type_assignable(actual, v->declared)) {
        Diag d = {0};
        d.message = "型が一致しません";
        d.primary.tok = n->rhs->tok;
        d.primary.label = diag_fmt("型 '%s' の式", type_name(actual));
        d.related.tok = v->decl_tok;
        d.related.label = diag_fmt("変数 '%s' は '%s' 型として宣言されています",
                                   v->name, type_name(v->declared));
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
        bool_required(diag_fmt("%sには bool が必要です", where),
                      diag_fmt("%sはここです", where), stmt_node->tok, cond, t);
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
    {"copy", TY_STR, TY_STR, "pl_str_copy"},
    // ★ **値型にも copy を許します**（そのまま返すだけ）。
    //   ジェネリックなコードの中では T が int かもしれず、str のときだけ
    //   copy が要るのに T では書き分けられない、という行き止まりがありました
    //   （lib/set を書いていて見つけました）。
    //   注意: 実装名 "pl_copy_id" は codegen が**呼び出しを出さない印**です。
    {"copy", TY_INT, TY_INT, "pl_copy_id"},
    {"copy", TY_FLOAT, TY_FLOAT, "pl_copy_id"},
    {"copy", TY_BOOL, TY_BOOL, "pl_copy_id"},
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
        d.message = diag_fmt("%s は 1 個の引数を取りますが、%d 個渡されました",
                             n->name, nargs);
        d.primary.tok = n->tok;
        d.primary.label = "引数の個数が違います";
        d.hint = diag_fmt("%s(値) の形で使ってください", n->name);
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
            d.message = diag_fmt("'%s' はそのまま %s できません", type_name(at),
                                 n->name);
            d.primary.tok = n->args->tok;
            d.primary.label = diag_fmt("要素が '%s' 型です", type_name(at->elem));
            d.hint = "そのまま出せるのは list[int] / list[float] / list[str] / "
                     "list[bool] だけです。ほかは for でまわしてください";
            diag_fail(&d);
        }
        n->is_list_str = true;
        return strcmp(n->name, "print") == 0 ? ty_none : ty_str;
    }

    // ── move_out(場所) — 所有権を取り出し、その場所は空にする ──────
    //
    // ★ なぜ要るか（docs/roadmap.md A-21d）
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
            d.message = "move_out は「場所」からしか取り出せません";
            d.primary.tok = a->tok;
            d.primary.label = "ここは書き戻せる場所ではありません";
            d.hint = "move_out(self.xs) / move_out(obj.f) / move_out(xs[i]) の形で使ってください"
                     "（局所変数は、そのまま返せば所有権ごと動きます）";
            diag_fail(&d);
        }
        // ② 空の値を作れる型だけ。str は ""、list[T] は [] を書き戻します。
        if (at->kind != TY_STR && at->kind != TY_LIST) {
            Diag d = {0};
            d.message = diag_fmt("move_out は '%s' 型を取り出せません", type_name(at));
            d.primary.tok = a->tok;
            d.primary.label = diag_fmt("これは '%s' 型です", type_name(at));
            d.hint = "move_out が受け取れるのは str と list[T] です"
                     "（取り出したあとに書き戻す「空の値」が要るためです）";
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
    d.message = diag_fmt("%s は '%s' 型を受け取れません", n->name, type_name(at));
    d.primary.tok = n->args->tok;
    d.primary.label = diag_fmt("これは '%s' 型です", type_name(at));
    if (s->inst_site)
        d.related = (DiagLabel){s->inst_site,
                                diag_fmt("この実体化（%s）で使われました",
                                         s->inst_name)};
    d.hint = diag_fmt("%s が受け取れるのは %s です", n->name,
                      builtin_arg_types(n->name));
    diag_fail(&d);
}

// リストリテラルの検査
static Type *check_list_lit(Sema *s, Node *n) {
    Type *want = s->expected;  // ★ 使う前に控える（下で check_expr が上書きするため）

    if (!n->body) {
        // 空リストは、それ自身から要素型が決まらない
        if (!want || want->kind != TY_LIST) {
            Diag d = {0};
            d.message = "空のリストの要素型が決まりません";
            d.primary.tok = n->tok;
            d.primary.label = "この [] がどんなリストなのか分かりません";
            d.hint = "型注釈を書いてください（例: xs: list[int] = []）。"
                     "関数の引数に直接渡す場合は、いったん変数に入れてください";
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
        if (!type_assignable(t, et)) {
            Diag d = {0};
            d.message = diag_fmt("リストの要素の型がそろっていません（第 %d 要素）", i);
            d.primary.tok = el->tok;
            d.primary.label = diag_fmt("これは '%s' 型です", type_name(t));
            d.related.tok = n->body->tok;
            d.related.label = diag_fmt("最初の要素は '%s' 型です", type_name(et));
            d.hint = "リストの要素はすべて同じ型でなければなりません";
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
                error_at_hint(a->tok, "range の引数は int です",
                              "これは '%s' 型です", type_name(at));
        }
        elem_t = ty_int;
    } else {
        Type *it = check_expr(s, n->rhs);
        if (it->kind != TY_LIST) {
            Diag d = {0};
            d.message = diag_fmt("'%s' 型は内包表記の対象にできません",
                                 type_name(it));
            d.primary.tok = n->rhs->tok;
            d.primary.label = diag_fmt("これは '%s' 型です", type_name(it));
            d.hint = "対象にできるのは list[T] と range(...) です";
            diag_fail(&d);
        }
        elem_t = it->elem;
    }

    // ── ループ変数の見えるところで、要素の式と条件を検査する ──
    scope_push(s);
    lv->type = elem_t;
    VarEntry *vlv = declare(s, lv->name, elem_t, lv->tok);
    lv->ir_name = vlv->ir_name;

    Type *et = check_expr(s, n->lhs);
    if (et->kind == TY_NONE) {
        Diag d = {0};
        d.message = "内包表記の要素が値を持ちません";
        d.primary.tok = n->lhs->tok;
        d.primary.label = "この式は None 型です";
        d.hint = "値を返す式を書いてください";
        diag_fail(&d);
    }

    if (n->els) {
        Type *ct = check_expr(s, n->els);
        if (ct->kind != TY_BOOL)
            error_at_hint(n->els->tok, "内包表記の 'if' には bool が必要です",
                          "これは '%s' 型です", type_name(ct));
    }
    scope_pop(s);

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
            error_at_hint(n->tok,
                          diag_fmt("クラス '%s' に '__getitem__(self, …) -> …' を"
                                   "定義すると、添字が使えます", ot->cls->name),
                          "型 '%s' は添字を取れません", type_name(ot));
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
            error_at_hint(n->rhs->next->tok,
                          "タプルの添字は 1 つだけです",
                          "添字が 2 つ以上あります");
        if (n->rhs->kind != ND_INT) {
            Diag d = {0};
            d.message = "タプルの添字は整数リテラルでなければなりません";
            d.primary.tok = n->rhs->tok;
            d.primary.label = "ここは定数である必要があります";
            d.hint = "要素ごとに型が違うので、添字が実行時に決まると"
                     "式の型が決められません（分解代入 a, b = t も使えます）";
            diag_fail(&d);
        }
        long long k = n->rhs->ival;
        if (k < 0 || k >= ot->nparams) {
            Diag d = {0};
            d.message = diag_fmt("タプルの添字が範囲外です（%lld）", k);
            d.primary.tok = n->rhs->tok;
            d.primary.label = diag_fmt("要素は %d 個です（0 〜 %d）",
                                       ot->nparams, ot->nparams - 1);
            diag_fail(&d);
        }
        n->rhs->type = ty_int;
        return ot->params[k];
    }

    // list[T] と str の添字は 1 つだけです
    if (n->rhs->next) {
        Diag d = {0};
        d.message = diag_fmt("型 '%s' に添字を 2 つ以上は書けません", type_name(ot));
        d.primary.tok = n->rhs->next->tok;
        d.primary.label = "2 つめの添字はここです";
        d.hint = "2 次元の添字（m[i, j]）が使えるのは __getitem__ を"
                 "定義したクラスだけです";
        diag_fail(&d);
    }

    Type *it = check_expr(s, n->rhs);

    if (it->kind != TY_INT) {
        Diag d = {0};
        d.message = "添字は int でなければなりません";
        d.primary.tok = n->rhs->tok;
        d.primary.label = diag_fmt("これは '%s' 型です", type_name(it));
        diag_fail(&d);
    }

    if (ot->kind == TY_LIST) return ot->elem;
    // ★ str の添字は 1 文字の str を返す（char 型は作らない。型システム 5.8）
    if (ot->kind == TY_STR) return ty_str;

    Diag d = {0};
    d.message = diag_fmt("型 '%s' は添字を取れません", type_name(ot));
    d.primary.tok = n->lhs->tok;
    d.primary.label = diag_fmt("これは '%s' 型です", type_name(ot));
    d.hint = "添字が使えるのは list[T] と str です";
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
        d.message = diag_fmt("モジュール '%s' に '%s' はありません", ms->mod->name,
                             n->name);
        d.primary.tok = n->tok;
        d.primary.label = "この名前は定義されていません";
        if (lookup_func_in(ms, n->name))
            d.hint = diag_fmt("'%s' は関数です。'%s.%s(...)' と呼んでください",
                              n->name, ms->mod->name, n->name);
        else if (lookup_class_in(ms, n->name))
            d.hint = diag_fmt("'%s' はクラスです。生成するには '%s.%s(...)' と"
                              "書いてください",
                              n->name, ms->mod->name, n->name);
        else
            d.hint = "モジュールから使えるのは、そのファイルのトップレベルの"
                     "関数・クラス・グローバル変数です";
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
    d.message = diag_fmt("型 '%s' の値には%sがありません", type_name(ot), what);
    d.primary.tok = at->tok;
    d.primary.label = "None かもしれない値です";
    d.related.tok = obj->tok;
    d.related.label = diag_fmt("この式は '%s' 型です", type_name(ot));
    if (obj->kind == ND_VAR)
        d.hint = diag_fmt("先に None を除いてください:\n"
                          "             if %s is not None:\n"
                          "                 ...",
                          obj->name);
    else
        d.hint = "一度ローカル変数に入れてから絞り込んでください:\n"
                 "             x: T | None = ...\n"
                 "             if x is not None:\n"
                 "                 ...";
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
        d.message = diag_fmt("列挙 '%s' に枝 '%s' はありません", e->name, n->name);
        d.primary.tok = n->tok;
        d.primary.label = "この枝は定義されていません";
        d.related.tok = e->tok;
        d.related.label = "列挙の定義はここです";
        StrBuf sb;
        sb_init(&sb);
        sb_printf(&sb, "書ける枝は ");
        int k = 0;
        for (EnumVal *q = e->vals; q; q = q->next)
            sb_printf(&sb, "%s%s", k++ ? " / " : "", q->name);
        sb_printf(&sb, " です");
        d.hint = sb_str(&sb);
        diag_fail(&d);
    }

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

    if (ot->kind == TY_OPT) reject_opt_access(n->lhs, n, "フィールド", ot);

    if (ot->kind != TY_CLASS) {
        Diag d = {0};
        d.message = diag_fmt("型 '%s' にフィールドはありません", type_name(ot));
        d.primary.tok = n->lhs->tok;
        d.primary.label = diag_fmt("これは '%s' 型です", type_name(ot));
        d.hint = "'.' でフィールドを読めるのは class のインスタンスだけです";
        diag_fail(&d);
    }

    Field *f = lookup_field(ot->cls, n->name);
    if (!f) {
        Diag d = {0};
        d.message = diag_fmt("クラス '%s' にフィールド '%s' はありません",
                             ot->cls->name, n->name);
        d.primary.tok = n->tok;
        d.primary.label = "このフィールドは宣言されていません";
        d.related.tok = ot->cls->tok;
        d.related.label = "クラスの定義はここです";
        d.hint = "クラス本体の先頭に「名前: 型」の形で宣言してください";
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
        d.message = diag_fmt("クラス '%s' にメソッド '%s' はありません", c->name,
                             n->name);
        d.primary.tok = n->tok;
        d.primary.label = "このメソッドは定義されていません";
        d.related.tok = c->tok;
        d.related.label = "クラスの定義はここです";
        if (lookup_field(c, n->name))
            d.hint = diag_fmt("'%s' はフィールドです。'()' を外してください", n->name);
        diag_fail(&d);
    }

    // ★ 並べ替えと既定値の穴埋め（A-38）。self のぶん 1 つ飛ばします。
    bind_args_sig(n, f, 1, diag_fmt("メソッド '%s'", mname));

    // 引数の個数（self は数えない）
    int nargs = 0;
    for (Node *a = n->args; a; a = a->next) nargs++;
    if (nargs != f->nparams - 1) {
        Diag d = {0};
        d.message = diag_fmt("メソッド '%s' は %d 個の引数を取りますが、%d 個渡されました",
                             mname, f->nparams - 1, nargs);
        d.primary.tok = n->tok;
        d.primary.label = "呼び出しの引数の個数が違います";
        d.related.tok = f->tok;
        d.related.label = "このメソッドはここで定義されています";
        d.hint = "self は自動的に渡されるので、書く必要はありません";
        diag_fail(&d);
    }

    // ★ 第 1 引数は self なので、実引数は params[i + 1] と比べます
    int i = 0;
    for (Node *a = n->args; a; a = a->next, i++) {
        s->expected = f->params[i + 1];
        Type *at = check_expr(s, a);
        s->expected = NULL;
        mark_arg_own_rc(a, f, i + 1);
        if (!type_assignable(at, f->params[i + 1])) {
            Diag d = {0};
            d.message = diag_fmt("メソッド '%s' の第 %d 引数: 型 '%s' を '%s' に渡せません",
                                 mname, i + 1, type_name(at),
                                 type_name(f->params[i + 1]));
            d.primary.tok = a->tok;
            d.primary.label = diag_fmt("これは '%s' 型です", type_name(at));
            d.related.tok = f->tok;
            d.related.label = diag_fmt("引数 '%s' は '%s' 型です", f->pnames[i + 1],
                                       type_name(f->params[i + 1]));
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
        d.message = diag_fmt("モジュール '%s' に関数 '%s' はありません",
                             ms->mod->name, n->name);
        d.primary.tok = n->tok;
        d.primary.label = "この関数は定義されていません";
        d.hint = lookup_global_in(ms, n->name)
                     ? diag_fmt("'%s' はグローバル変数です。'()' を外してください",
                                n->name)
                     : "そのモジュールのトップレベルに def があるか確認してください";
        diag_fail(&d);
    }

    n->ir_name = f->ir_name;
    n->is_extern = f->owner != s->cur;
    return check_call_sig(s, n, f, "関数");
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
                if (n->can_fail && try_depth == 0) what = "失敗しうる呼び出し";
                break;
            default: break;
        }
        if (what) {
            Diag d = {0};
            d.code = "E-SCOPE-1";
            d.message = diag_fmt("scope: の中から '%s' で抜けることはできません",
                                 what);
            d.primary.tok = n->tok;
            d.primary.label = "ここで scope: の外へ出ようとしています";
            d.hint = "scope: の出口では、始めたスレッドを全部 join します。"
                     "途中で抜けるとそれが飛ぶので、"
                     "結果を変数に受けてからブロックを出てください";
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
        d.primary.label = diag_fmt("'%s = …' と書いています", a->arg_name);
        d.hint = why;
        diag_fail(&d);
    }
}

static Type *check_method(Sema *s, Node *n) {
    ModuleSyms *ms = dot_module(s, n);
    if (ms) return check_module_call(s, n, ms);

    Type *ot = auto_deref(check_expr(s, n->lhs));
    if (ot->kind == TY_OPT) reject_opt_access(n->lhs, n, "メソッド", ot);

    if (ot->kind == TY_CLASS) return check_class_method(s, n, ot->cls);

    // ★ ここから先は、引数の名前を持たない呼び先です（A-38）。
    //   インタフェース越しは**実行時に実装が決まる**ので、名前も既定値も
    //   使えません（どの実装の名前を見ればよいか決まらないため）。
    reject_kwargs(n->args,
                  ot->kind == TY_IFACE
                      ? "インタフェース越しの呼び出しでは、名前で引数を渡せません"
                      : diag_fmt("'%s' は名前で引数を受け取りません", n->name),
                  ot->kind == TY_IFACE
                      ? "どの実装が呼ばれるかは実行時に決まります。"
                        "名前と既定値が使えるのは、クラスの型が分かっている"
                        "ときだけです"
                      : "名前で渡せるのは、この言語で定義した"
                        "関数・メソッド・生成だけです");

    // ★ インタフェース越しの呼び出し。
    //   どの実装が呼ばれるかは **実行時に**決まります（vtable を引く）。
    if (ot->kind == TY_IFACE) {
        Iface *ifc = ot->iface;
        IMethod *im = NULL;
        for (IMethod *q = ifc->methods; q; q = q->next)
            if (strcmp(q->name, n->name) == 0) { im = q; break; }
        if (!im) {
            Diag d = {0};
            d.message = diag_fmt("インタフェース '%s' に '%s' はありません",
                                 ifc->name, n->name);
            d.primary.tok = n->tok;
            d.primary.label = "このメソッドは宣言されていません";
            d.related.tok = ifc->tok;
            d.related.label = "インタフェースの定義はここです";
            d.hint = "インタフェース越しに呼べるのは、そこに宣言したものだけです";
            diag_fail(&d);
        }

        Node *sig = im->sig;
        int want = 0;
        for (Node *pm = sig->params; pm; pm = pm->next) want++;
        want--;                       // self は数えない
        int nargs = 0;
        for (Node *a = n->args; a; a = a->next) nargs++;
        if (nargs != want)
            error_at_hint(n->tok,
                          diag_fmt("'%s.%s' は %d 個の引数を取ります", ifc->name,
                                   n->name, want),
                          "引数の個数が違います（%d 個渡されました）", nargs);

        int k = 1;
        Node *pm = sig->params->next;
        for (Node *a = n->args; a; a = a->next, pm = pm->next, k++) {
            Type *wt = resolve_type(s, pm->type_ref);
            s->expected = wt;
            Type *at = check_expr(s, a);
            s->expected = NULL;
            if (!type_assignable(at, wt))
                error_at_hint(a->tok,
                              diag_fmt("ここには '%s' が必要です", type_name(wt)),
                              "%d 番目の引数が '%s' 型です", k, type_name(at));
        }

        n->iface_slot = im->slot;
        n->is_iface_call = true;
        return resolve_type(s, sig->type_ref);
    }

    // ── Thread[R].join() — 終わるまで待って戻り値を受け取る ──
    //
    // 注意: join は所有を消費します（2 回 join できません）。それを保証するのは
    //    ownck 側です（Thread[R] は移動する型として扱われます）。
    if (ot->kind == TY_THREAD) {
        if (strcmp(n->name, "join") != 0)
            error_at_hint(n->tok, "Thread にあるのは join() だけです",
                          "'Thread' に '%s' はありません", n->name);
        if (n->args)
            error_at_hint(n->tok, "t.join() の形で使ってください",
                          "join は引数を取りません");
        return ot->elem;
    }

    // ── mutex[T].lock(f) — ロックを取り、f(中身) を呼び、必ず解く ──
    //
    // ★ 生の lock / unlock は**出しません**。解き忘れが起きえない形に
    //   閉じ込めます（設計文書 §4）。
    if (ot->kind == TY_MUTEX) {
        if (strcmp(n->name, "lock") != 0)
            error_at_hint(n->tok, "mutex にあるのは lock(関数) だけです",
                          "'mutex' に '%s' はありません", n->name);
        int nargs = 0;
        for (Node *a = n->args; a; a = a->next) nargs++;
        if (nargs != 1)
            error_at_hint(n->tok, "m.lock(関数) の形で使ってください",
                          "lock は 1 個の引数（関数）を取ります"
                          "（%d 個渡されました）", nargs);
        Type *ft = check_expr(s, n->args);
        if (ft->kind != TY_FN || ft->nparams != 1)
            error_at_hint(n->args->tok,
                          diag_fmt("lock には 'fn(%s) -> R' の関数を渡してください",
                                   type_name(ot->elem)),
                          "'%s' はその形の関数ではありません", type_name(ft));
        if (!type_assignable(ot->elem, ft->params[0]))
            error_at_hint(n->args->tok,
                          diag_fmt("中身は '%s' です", type_name(ot->elem)),
                          "この関数は '%s' を受け取ります",
                          type_name(ft->params[0]));
        return ft->elem;
    }

    // ★ list のメソッドを増やしました。
    //   引数と戻り型は「表」で持ちます。1 つずつ if を書くと、増やすたびに
    //   同じ形のコードが並ぶためです。
    //     argk: 'e'=要素型 / 'i'=int / 'l'=同じ list / '-'=引数なし
    //     retk: 'e'=要素型 / 'i'=int / 'n'=None / 'l'=同じ list
    if (ot->kind == TY_LIST) {
        static const struct { const char *name; char argk; char retk;
                              const char *usage; } LM[] = {
            {"pop",     '-', 'e', "xs.pop()"},
            {"insert",  'i', 'n', "xs.insert(位置, 値)"},   // 引数 2 個（下で特別扱い）
            {"remove",  'i', 'e', "xs.remove(位置)"},
            {"index",   'e', 'i', "xs.index(値)"},
            {"reverse", '-', 'n', "xs.reverse()"},
            {"clear",   '-', 'n', "xs.clear()"},
            {"copy",    '-', 'l', "xs.copy()"},
            {"extend",  'l', 'n', "xs.extend(別のリスト)"},
            {NULL, 0, 0, NULL},
        };
        for (int i = 0; LM[i].name; i++) {
            if (strcmp(n->name, LM[i].name) != 0) continue;

            int want = LM[i].argk == '-' ? 0 : 1;
            if (strcmp(n->name, "insert") == 0) want = 2;
            int nargs = 0;
            for (Node *a = n->args; a; a = a->next) nargs++;
            if (nargs != want) {
                Diag d = {0};
                d.message = diag_fmt("%s は %d 個の引数を取りますが、%d 個渡されました",
                                     n->name, want, nargs);
                d.primary.tok = n->tok;
                d.primary.label = "引数の個数が違います";
                d.hint = diag_fmt("%s の形で使ってください", LM[i].usage);
                diag_fail(&d);
            }

            // 引数の型検査
            if (strcmp(n->name, "insert") == 0) {
                Type *a0 = check_expr(s, n->args);
                if (a0->kind != TY_INT)
                    error_at(n->args->tok, "insert の位置は int です（'%s' 型でした）",
                             type_name(a0));
                s->expected = ot->elem;
                Type *a1 = check_expr(s, n->args->next);
                s->expected = NULL;
                if (!type_assignable(a1, ot->elem))
                    error_at(n->args->next->tok,
                             "'%s' のリストに '%s' は入れられません",
                             type_name(ot->elem), type_name(a1));
            } else if (LM[i].argk == 'i') {
                Type *a0 = check_expr(s, n->args);
                if (a0->kind != TY_INT)
                    error_at(n->args->tok, "%s の引数は int です（'%s' 型でした）",
                             n->name, type_name(a0));
            } else if (LM[i].argk == 'e') {
                s->expected = ot->elem;
                Type *a0 = check_expr(s, n->args);
                s->expected = NULL;
                if (!type_assignable(a0, ot->elem))
                    error_at(n->args->tok, "'%s' のリストから '%s' は探せません",
                             type_name(ot->elem), type_name(a0));
            } else if (LM[i].argk == 'l') {
                Type *a0 = check_expr(s, n->args);
                if (!type_assignable(a0, ot))
                    error_at(n->args->tok, "extend には同じ型のリストが必要です"
                             "（'%s' でした）", type_name(a0));
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
            d.message = diag_fmt("append は 1 個の引数を取りますが、%d 個渡されました",
                                 nargs);
            d.primary.tok = n->tok;
            d.primary.label = "引数の個数が違います";
            d.hint = "xs.append(値) の形で使ってください";
            diag_fail(&d);
        }

        s->expected = ot->elem;
        Type *at = check_expr(s, n->args);
        s->expected = NULL;

        // 注意: ここでも代入互換の検査。list[list[int]] に list[str] を
        //    append するのを弾くには、要素型の再帰比較が要ります。
        if (!type_assignable(at, ot->elem)) {
            Diag d = {0};
            d.message = diag_fmt("'%s' のリストに '%s' を追加できません",
                                 type_name(ot->elem), type_name(at));
            d.primary.tok = n->args->tok;
            d.primary.label = diag_fmt("これは '%s' 型です", type_name(at));
            d.hint = no_implicit_hint(at, ot->elem);
            diag_fail(&d);
        }

        n->args = range_coerce(s, n->args, ot->elem);   // A-28
        return ty_none;
    }

    Diag d = {0};
    d.message = diag_fmt("型 '%s' にメソッド '%s' はありません", type_name(ot),
                         n->name);
    d.primary.tok = n->tok;
    d.primary.label = "このメソッドは存在しません";
    d.hint = "list[T] で使えるのは append / pop / insert / remove / index / "
             "reverse / clear / copy / extend です"
             "（class のメソッドは自分で定義できます）";
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
            d.message = diag_fmt("'%s' のどの実体を作るのか決められません", c->name);
            d.primary.tok = n->tok;
            d.primary.label = "型引数が決まりません";
            d.related.tok = c->tok;
            d.related.label = "このクラスは型引数を取ります";
            d.hint = diag_fmt("変数の型から決めます。"
                              "'x: %s[型, ...] = %s(...)' の形で書いてください",
                              c->name, c->name);
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
            d.message = diag_fmt("クラス '%s' には init が無いので引数を渡せません",
                                 c->name);
            d.primary.tok = n->tok;
            d.primary.label = diag_fmt("%d 個の引数が渡されています", nargs);
            d.related.tok = c->tok;
            d.related.label = "クラスの定義はここです";
            d.hint = "引数を受け取るには init メソッドを定義してください:\n"
                     "             def init(self, ...) -> None:";
            diag_fail(&d);
        }
        return c->type;
    }

    // init があるなら、その引数と突き合わせる（self は飛ばす）
    FuncSig *f = lookup_func_in(c->owner, mangle(c->name, "init"));

    // ★ 並べ替えと既定値の穴埋め（A-38）。self のぶん 1 つ飛ばします。
    bind_args_sig(n, f, 1, diag_fmt("クラス '%s'", c->name));
    nargs = 0;
    for (Node *a = n->args; a; a = a->next) nargs++;

    if (nargs != f->nparams - 1) {
        Diag d = {0};
        d.message = diag_fmt("'%s' の生成には %d 個の引数が必要ですが、%d 個渡されました",
                             c->name, f->nparams - 1, nargs);
        d.primary.tok = n->tok;
        d.primary.label = "引数の個数が違います";
        d.related.tok = f->tok;
        d.related.label = "init はここで定義されています";
        d.hint = "self は自動的に渡されるので、書く必要はありません";
        diag_fail(&d);
    }

    int i = 0;
    for (Node *a = n->args; a; a = a->next, i++) {
        s->expected = f->params[i + 1];
        Type *at = check_expr(s, a);
        s->expected = NULL;
        mark_arg_own_rc(a, f, i + 1);
        if (!type_assignable(at, f->params[i + 1])) {
            Diag d = {0};
            d.message = diag_fmt("'%s' の生成の第 %d 引数: 型 '%s' を '%s' に渡せません",
                                 c->name, i + 1, type_name(at),
                                 type_name(f->params[i + 1]));
            d.primary.tok = a->tok;
            d.primary.label = diag_fmt("これは '%s' 型です", type_name(at));
            d.related.tok = f->tok;
            d.related.label = diag_fmt("引数 '%s' は '%s' 型です", f->pnames[i + 1],
                                       type_name(f->params[i + 1]));
            d.hint = no_implicit_hint(at, f->params[i + 1]);
            diag_fail(&d);
        }
    }
    return c->type;
}

// 関数呼び出しの検査（docs/spec/type-system.md 5.7 の順序に従う）
// ── 低レベルの組み込み ──────────────────────────────────
//
// ★ 引数の型が「表」で書けない（ptr[int] を取る／返す）ので、
//   rc(x) と同じくここで特別扱いします。
//
//   ptr_at(addr)        番地からポインタを作る
//   addr_of(p)          ポインタを番地に戻す
//   peek8/peek64(p, i)  読む（volatile）
//   poke8/poke64(p,i,v) 書く（volatile）
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
    {"peek64", 2, false, true},
    {"poke8", 3, false, true},
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
        d.message = diag_fmt("'%s' は unsafe: の中でしか使えません", n->name);
        d.primary.tok = n->tok;
        d.primary.label = "生ポインタを触っています";
        d.hint = "unsafe: ブロックで囲んでください:\n"
                 "             unsafe:\n"
                 "                 poke8(p, 0, 65)";
        diag_fail(&d);
    }

    int nargs = 0;
    for (Node *a = n->args; a; a = a->next) nargs++;
    if (nargs != ll->nargs)
        error_at_hint(n->tok, diag_fmt("%s は %d 個の引数を取ります", n->name, ll->nargs),
                      "引数の個数が違います");

    // ★ asm 系は第 1 引数が「文字列リテラル」（実行時に組み立てられては困る）
    bool is_asm = strncmp(n->name, "asm", 3) == 0;
    if (is_asm && (!n->args || n->args->kind != ND_STR))
        error_at_hint(n->tok, "命令はリテラルで書いてください（例: asm(\"wfi\")）",
                      "asm の第 1 引数は文字列リテラルです");

    int i = 0;
    for (Node *a = n->args; a; a = a->next, i++) {
        if (is_asm && i == 0) {
            check_expr(s, a);
            continue;
        }
        Type *at = check_expr(s, a);
        bool want_ptr = ll->takes_ptr && i == 0;
        if (want_ptr && at->kind != TY_PTR)
            error_at_hint(a->tok, "第 1 引数には ptr[int] を渡してください",
                          "'%s' はポインタではありません", type_name(at));
        if (!want_ptr && at->kind != TY_INT)
            error_at_hint(a->tok, "低レベルの操作が扱うのは int だけです",
                          "'%s' はここに渡せません", type_name(at));
    }

    n->builtin = NULL;
    if (ll->ret_ptr) return type_ptr(ty_int);
    if (strcmp(n->name, "poke8") == 0 || strcmp(n->name, "poke64") == 0)
        return ty_none;
    if (strcmp(n->name, "asm") == 0 || strcmp(n->name, "asm_in") == 0) return ty_none;
    return ty_int;
}

static Type *check_call(Sema *s, Node *n) {
    // ★ 名前が **関数型の変数**なら間接呼び出しです。
    //   注意: 変数を先に見ます。同名の関数があっても変数が勝ちます。
    VarEntry *fv = lookup(s, n->name);
    if (fv && fv->type && fv->type->kind == TY_FN) {
        Type *ft = fv->type;
        // ★ 関数型には引数の**名前が入っていません**（A-38）。
        reject_kwargs(n->args,
                      diag_fmt("関数の値 '%s' は名前で引数を受け取りません",
                               n->name),
                      diag_fmt("この変数の型は '%s' です。"
                               "関数の型には引数の名前が入らないので、"
                               "位置で渡してください", type_name(ft)));
        int nargs = 0;
        for (Node *a = n->args; a; a = a->next) nargs++;
        if (nargs != ft->nparams) {
            Diag d = {0};
            d.message = diag_fmt("'%s' は %d 個の引数を取りますが、%d 個渡されました",
                                 n->name, ft->nparams, nargs);
            d.primary.tok = n->tok;
            d.primary.label = "引数の個数が違います";
            d.hint = diag_fmt("この変数の型は '%s' です", type_name(ft));
            diag_fail(&d);
        }
        int i = 0;
        for (Node *a = n->args; a; a = a->next, i++) {
            s->expected = ft->params[i];
            Type *at = check_expr(s, a);
            s->expected = NULL;
            if (!type_assignable(at, ft->params[i])) {
                Diag d = {0};
                d.message = diag_fmt("%d 番目の引数の型が合いません", i + 1);
                d.primary.tok = a->tok;
                d.primary.label = diag_fmt("これは '%s' 型です", type_name(at));
                d.hint = diag_fmt("ここには '%s' が必要です",
                                  type_name(ft->params[i]));
                diag_fail(&d);
            }
        }
        n->ir_name = fv->ir_name;
        n->is_indirect = true;
        n->type = ft->elem;
        return ft->elem;
    }

    // ★ 組み込み（print / len / append …）には引数の名前がありません（A-38）。
    //   注意: クラス名なら生成なので、ここでは断りません（init の名前が使えます）。
    if (!lookup_func(s, n->name) && !lookup_class(s, n->name))
        reject_kwargs(n->args,
                      diag_fmt("'%s' は名前で引数を受け取りません", n->name),
                      "名前で渡せるのは、この言語で定義した"
                      "関数・メソッド・生成だけです");

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
            error_at_hint(n->tok, diag_fmt("%s(a, b) の形で使ってください", n->name),
                          diag_fmt("%s は 2 個の引数を取ります", n->name));
        Type *a0 = check_expr(s, n->args);
        Type *a1 = check_expr(s, n->args->next);
        if (!type_equal(a0, a1))
            error_at(n->args->next->tok,
                     "%s の 2 つの引数は同じ型である必要があります（'%s' と '%s'）",
                     n->name, type_name(a0), type_name(a1));
        if (a0->kind != TY_INT && a0->kind != TY_FLOAT)
            error_at_hint(n->args->tok,
                          "min / max が使えるのは int と float です",
                          "'%s' 型には使えません", type_name(a0));
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
            error_at_hint(n->tok, diag_fmt("%s(a, b) の形で使ってください", n->name),
                          diag_fmt("%s は 2 個の引数を取ります", n->name));
        Type *a0 = check_expr(s, n->args);
        Type *a1 = check_expr(s, n->args->next);
        // 注意: **問題のある側の引数**を指します（2 つとも int でないときは左から）
        if (a0->kind != TY_INT)
            error_at_hint(n->args->tok,
                          "折り返す算術が使えるのは int だけです",
                          "'%s' 型には使えません", type_name(a0));
        if (a1->kind != TY_INT)
            error_at_hint(n->args->next->tok,
                          "折り返す算術が使えるのは int だけです",
                          "'%s' 型には使えません", type_name(a1));
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
            error_at_hint(n->tok, "rc(値) の形で使ってください",
                          "rc は 1 個の引数を取ります");
        Type *at = check_expr(s, n->args);
        if (at->kind != TY_CLASS)
            error_at_hint(n->args->tok,
                          "rc に入れられるのはクラスのインスタンスだけです",
                          "'%s' は rc に入れられません", type_name(at));
        n->is_extern = false;
        n->name = "rc";
        return type_rc(at);
    }

    // ── spawn(f, a…) — 別スレッドで f(a…) を始める（A-18）──
    //
    // ★ 新しい構文は作りません。**呼び出しの形のまま**です
    //   （docs/design/concurrency.md 3）。rc(x) と同じ理由でここに置きます:
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
            error_at_hint(n->tok, "spawn(関数, 引数…) の形で使ってください",
                          "spawn には少なくとも関数が要ります");
        Type *ft = check_expr(s, n->args);
        if (ft->kind != TY_FN)
            error_at_hint(n->args->tok,
                          "spawn の 1 つ目は関数です（例: spawn(work, job)）",
                          "'%s' は関数ではありません", type_name(ft));
        if (ft->nparams != nargs - 1)
            error_at_hint(n->args->tok,
                          diag_fmt("'%s' は %d 個の引数を取ります", type_name(ft),
                                   ft->nparams),
                          "spawn に渡した引数が %d 個です", nargs - 1);
        int k = 0;
        for (Node *a = n->args->next; a; a = a->next, k++) {
            s->expected = ft->params[k];
            Type *at = check_expr(s, a);
            s->expected = NULL;
            if (!type_assignable(at, ft->params[k]))
                error_at_hint(a->tok,
                              diag_fmt("ここには '%s' が必要です",
                                       type_name(ft->params[k])),
                              "%d 番目の引数が '%s' 型です", k + 1, type_name(at));
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
            error_at_hint(n->tok, "mutex(値) の形で使ってください",
                          "mutex は 1 個の引数を取ります");
        Type *at = check_expr(s, n->args);
        if (at->kind != TY_CLASS && at->kind != TY_LIST)
            error_at_hint(n->args->tok,
                          "mutex に入れられるのはクラスかリストだけです",
                          "'%s' は mutex に入れられません", type_name(at));
        n->is_extern = false;
        n->name = "mutex";
        return type_mutex(at);
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
        d.message = diag_fmt("未定義の関数 '%s' です", n->name);
        d.primary.tok = n->tok;
        d.primary.label = "この関数は定義されていません";
        d.hint = "関数名の綴りを確認してください"
                 "（定義の順序は問いません。後ろで定義した関数も呼べます）";
        diag_fail(&d);
    }

    // ★ 呼ぶ相手の IR 名（モジュール修飾済み）を codegen に渡す
    n->ir_name = f->ir_name;
    n->is_extern = f->owner != s->cur;

    // ③④ 引数の個数と型
    return check_call_sig(s, n, f, "関数");
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
        d.related.label = diag_fmt("'%s' はここで宣言されています", shown);
        if (s->cur_func && s->cur_func->nraises == 0) {
            d.code = "E-RAISE-1";
            d.message = diag_fmt("失敗しうる呼び出し '%s' を処理していません", shown);
            d.primary.label = diag_fmt("この呼び出しは '%s' を返すことがあります",
                                       ec->name);
            d.hint = diag_fmt("try で捕まえるか、この関数に 'raises %s' を足してください",
                              ec->name);
        } else {
            d.code = "E-RAISE-2";
            d.message = diag_fmt("エラー '%s' が宣言されていません", ec->name);
            d.primary.label = diag_fmt("'%s' は '%s' を返すことがあります", shown,
                                       ec->name);
            d.hint = diag_fmt("この関数の raises に '%s' を足すか、try で捕まえてください",
                              ec->name);
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
                d.message = "位置で渡す引数は、名前で渡す引数より前に書きます";
                d.primary.tok = a->tok;
                d.primary.label = "ここは名前つきの引数より後ろです";
                d.related.tok = kw_first->arg_name_tok;
                d.related.label = "名前で渡し始めたのはここです";
                d.hint = "名前を付けるか、この引数を前へ動かしてください";
                diag_fail(&d);
            }
            if (pos >= nparams) {
                Diag d = {0};
                d.message = diag_fmt("%s に渡せる引数は多くとも %d 個です（%d 個渡されました）",
                                     subject, nparams, nargs);
                d.primary.tok = a->tok;
                d.primary.label = "この引数に当たるものがありません";
                d.related.tok = deftok;
                d.related.label = "この関数はここで定義されています";
                if (has_self)
                    d.hint = "self は自動的に渡されるので、書く必要はありません";
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
            sb_printf(&sb, "書ける名前は ");
            for (int i = 0; i < nparams; i++)
                sb_printf(&sb, "%s%s", i ? " / " : "", pnames[i]);
            sb_printf(&sb, " です");
            Diag d = {0};
            d.message = diag_fmt("%s に引数 '%s' はありません", subject,
                                 a->arg_name);
            d.primary.tok = a->arg_name_tok;
            d.primary.label = "この名前の引数は定義されていません";
            d.related.tok = deftok;
            d.related.label = "この関数はここで定義されています";
            d.hint = sb_str(&sb);
            diag_fail(&d);
        }
        if (slot[at]) {
            Diag d = {0};
            d.message = diag_fmt("引数 '%s' に 2 回渡しています", a->arg_name);
            d.primary.tok = a->arg_name_tok;
            d.primary.label = "2 回目です";
            d.related.tok = slot[at]->tok;
            d.related.label = "1 回目はここです";
            d.hint = at < pos ? "位置で渡したものに、名前でもう一度渡しています"
                              : "同じ名前を 2 回書いています";
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
            d.message = diag_fmt("%s の引数 '%s' が渡されていません", subject,
                                 pnames[i]);
            d.primary.tok = n->tok;
            d.primary.label = diag_fmt("引数 '%s' に当たるものがありません",
                                       pnames[i]);
            d.related.tok = deftok;
            d.related.label = "この関数はここで定義されています";
            d.hint = "この引数には既定値がないので、呼ぶときに必ず書きます";
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
        d.message = diag_fmt("%s '%s' は %d 個の引数を取りますが、%d 個渡されました",
                             what, shown, f->nparams, nargs);
        d.primary.tok = n->tok;
        d.primary.label = "呼び出しの引数の個数が違います";
        d.related.tok = f->tok;
        d.related.label = "この関数はここで定義されています";
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
        // ★ codegen へ「この実引数は借用で渡す」と**分かっている**ことを伝えます。
        //   借用なら相手は所有権を受け取らないので、呼び出し後に一時値を
        //   解放できます（A-21e）。
        // 注意: ここは check_call_sig＝**通常の関数**だけを通ります。メソッドは
        //   別経路なので旗が立たず、codegen は解放しません（安全側）。
        a->arg_is_borrowed = f->pmodes && f->pmodes[i] != PM_OWN;
        mark_arg_own_rc(a, f, i);
        if (!type_assignable(at, f->params[i])) {
            Diag d = {0};
            d.message = diag_fmt("%s '%s' の第 %d 引数: 型 '%s' を '%s' に渡せません",
                                 what, shown, i + 1, type_name(at),
                                 type_name(f->params[i]));
            d.primary.tok = a->tok;
            d.primary.label = diag_fmt("これは '%s' 型です", type_name(at));
            d.related.tok = f->tok;
            d.related.label = diag_fmt("引数 '%s' は '%s' 型です", f->pnames[i],
                                       type_name(f->params[i]));
            d.hint = no_implicit_hint(at, f->params[i]);
            diag_fail(&d);
        }
    }
    return f->ret;
}

// return の検査
static void check_return(Sema *s, Node *n) {
    Type *want = s->cur_func->ret;

    if (!n->lhs) {  // return（値なし）
        if (want->kind != TY_NONE) {
            Diag d = {0};
            d.message = diag_fmt("関数 '%s' は '%s' を返さなければなりません",
                                 s->cur_func->name, type_name(want));
            d.primary.tok = n->tok;
            d.primary.label = "この return には値がありません";
            d.related.tok = s->cur_func->tok;
            d.related.label = "戻り型はここで宣言されています";
            diag_fail(&d);
        }
        return;
    }

    s->expected = want;  // ★ return [] のため
    Type *got = check_expr(s, n->lhs);
    s->expected = NULL;
    if (want->kind == TY_NONE) {
        Diag d = {0};
        d.message = diag_fmt("戻り型が None の関数 '%s' は値を返せません",
                             s->cur_func->name);
        d.primary.tok = n->lhs->tok;
        d.primary.label = diag_fmt("型 '%s' の式", type_name(got));
        d.related.tok = s->cur_func->tok;
        d.related.label = "戻り型はここで宣言されています";
        diag_fail(&d);
    }
    if (!type_assignable(got, want)) {
        Diag d = {0};
        d.message = "return の型が戻り型と一致しません";
        d.primary.tok = n->lhs->tok;
        d.primary.label = diag_fmt("型 '%s' の式", type_name(got));
        d.related.tok = s->cur_func->tok;
        d.related.label = diag_fmt("関数 '%s' の戻り型は '%s' です", s->cur_func->name,
                                   type_name(want));
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
        d.message = "分解代入の右辺がタプルではありません";
        d.primary.tok = n->rhs->tok;
        d.primary.label = diag_fmt("これは '%s' 型です", type_name(rt));
        d.hint = "分解できるのは (A, B) を返すものだけです"
                 "（複数の値を返す関数を書いてください）";
        diag_fail(&d);
    }

    int nvars = 0;
    for (Node *v = n->params; v; v = v->next) nvars++;
    if (nvars != rt->nparams) {
        Diag d = {0};
        d.message = diag_fmt("受け取る数が合いません（%d 個と %d 個）", nvars,
                             rt->nparams);
        d.primary.tok = n->tok;
        d.primary.label = diag_fmt("ここは %d 個です", nvars);
        d.hint = diag_fmt("右辺は '%s' です", type_name(rt));
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
    d.message = diag_fmt("'%s.%s' の形が for の規約と違います", c->name, name);
    d.primary.tok = at;
    d.primary.label = "この for がその規約を使います";
    d.related.tok = f->tok;
    d.related.label = "このメソッドです";
    d.hint = strcmp(name, "__get__") == 0
                 ? "def __get__(self, cur: int) -> T の形で書いてください"
                 : diag_fmt("def %s(%s) -> int の形で書いてください", name,
                            nparams == 2 ? "self, cur: int" : "self");
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
            d.message = diag_fmt("クラス '%s' は for で回せません", c->name);
            d.primary.tok = iter->tok;
            d.primary.label = diag_fmt("'%s' 型です", type_name(ct));
            d.related.tok = c->tok;
            d.related.label = "クラスの定義はここです";
            d.hint = "for で回すには 3 つのメソッドが要ります:\n"
                     "             def __first__(self) -> int          "
                     "最初のカーソル（無ければ -1）\n"
                     "             def __next__(self, cur: int) -> int "
                     "次のカーソル（無ければ -1）\n"
                     "             def __get__(self, cur: int) -> T    "
                     "そのカーソルの要素";
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
        d.message = diag_fmt("'%s' 型は for で回せません", type_name(ct));
        d.primary.tok = iter->tok;
        d.primary.label = "ここは回せる形ではありません";
        d.hint = "回せるのは list / str / range(...) と、"
                 "__first__ / __next__ / __get__ を持つクラスです";
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
            check_cond(s, "if の条件", n, n->lhs);

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

            bool is_enum = st->kind == TY_ENUM;
            if (!is_enum && st->kind != TY_INT && st->kind != TY_STR) {
                Diag d = {0};
                d.message = diag_fmt("'%s' は match で調べられません",
                                     type_name(st));
                d.primary.tok = n->lhs->tok;
                d.primary.label = "ここに書けるのは 列挙 / int / str です";
                d.hint = "クラスの場合分けはインタフェースで書きます"
                         "（どの枝かを型が持ちます）";
                diag_fail(&d);
            }
            if (is_enum) n->en = st->en;

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
                        d.message = "case _ が 2 つあります";
                        d.primary.tok = c->tok;
                        d.primary.label = "2 つめです";
                        d.related.tok = default_at->tok;
                        d.related.label = "最初の case _ はここです";
                        diag_fail(&d);
                    }
                    has_default = true;
                    default_at = c;
                    if (c->next)
                        error_at_hint(c->next->tok,
                                      "case _ より後ろの case は決して選ばれません",
                                      "この case は届きません");
                    check_block(s, c->body);
                    continue;
                }
                if (has_default) UNREACHABLE();  // 上で断っている

                Type *ct = check_expr(s, c->lhs);
                if (!type_equal(ct, st)) {
                    Diag d = {0};
                    d.message = diag_fmt("case の値の型が違います"
                                         "（'%s' を調べているのに '%s' です）",
                                         type_name(st), type_name(ct));
                    d.primary.tok = c->lhs->tok;
                    d.primary.label = diag_fmt("ここは '%s' です", type_name(ct));
                    d.hint = "match は暗黙の変換をしません（言語全体と同じです）";
                    diag_fail(&d);
                }

                // ★ **値がコンパイル時に決まらないものは断ります。**
                //   変数を書けるようにすると、上から順に比べるだけの
                //   「if の連なり」と同じになり、網羅を確かめられません。
                if (c->lhs->kind != ND_INT && c->lhs->kind != ND_STR) {
                    Diag d = {0};
                    d.message = "case には決まった値を書きます";
                    d.primary.tok = c->lhs->tok;
                    d.primary.label = "ここはコンパイル時に決まりません";
                    d.hint = is_enum
                        ? "列挙の枝（例: Color.Red）を書いてください"
                        : "リテラルを書いてください（変えられる値は if で比べます）";
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
                        d.message = "同じ値の case が 2 つあります";
                        d.primary.tok = c->lhs->tok;
                        d.primary.label = "2 つめは決して選ばれません";
                        d.related.tok = q->lhs->tok;
                        d.related.label = "最初の case はここです";
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
                    for (EnumVal *v = st->en->vals; v; v = v->next) {
                        bool found = false;
                        for (Node *c = n->body; c && !found; c = c->next)
                            if (c->lhs && c->lhs->ival == v->val) found = true;
                        if (!found)
                            sb_printf(&missing, "%s%s.%s", nmiss++ ? " / " : "",
                                      st->en->name, v->name);
                    }
                    if (nmiss) {
                        Diag d = {0};
                        d.message = diag_fmt("match に書いていない枝があります: %s",
                                             sb_str(&missing));
                        d.primary.tok = n->tok;
                        d.primary.label = "ここで全部の枝を扱ってください";
                        d.related.tok = st->en->tok;
                        d.related.label = "列挙の定義はここです";
                        d.hint = "どれにも当たらないときの動きが要るなら "
                                 "case _: を書いてください";
                        diag_fail(&d);
                    }
                    // ★ 全部書いてあるなら case _ は要りません（書くと
                    //   「決して選ばれない case」になるので、上で断ります）。
                } else {
                    // 枝を全部書いたうえでの case _ は届きません。
                    int ncase = 0;
                    for (Node *c = n->body; c; c = c->next)
                        if (c->lhs) ncase++;
                    if (ncase == st->en->nvals)
                        error_at_hint(default_at->tok,
                                      "枝を全部書いてあるので case _ は届きません",
                                      "この case は選ばれません");
                }
            } else if (!has_default) {
                // ★ int / str は値が無限にあるので、網羅を静的に示せません。
                //   **case _ を必須にします** — 無いと「どれにも当たらない」
                //   ときの動きが書かれていないことになります。
                Diag d = {0};
                d.message = diag_fmt("'%s' の match には case _ が要ります",
                                     type_name(st));
                d.primary.tok = n->tok;
                d.primary.label = "どれにも当たらないときの動きがありません";
                d.hint = "最後に case _: を書いてください"
                         "（値が無限にあるので、全部を書き尽くせません）";
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
            check_cond(s, "while の条件", n, n->lhs);
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
                error_at_hint(n->tok, "いま使える pragma は target と no_runtime です",
                              "未知の pragma '%s' です", n->name);
            if (is_target && !n->sval)
                error_at_hint(n->tok, "pragma target \"riscv64-unknown-elf\" の形で書きます",
                              "pragma target には文字列が必要です");
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
            if (t->kind != TY_BOOL) {
                Diag d = {0};
                d.message = diag_fmt("%s には bool の式を書きます",
                                     n->kind == ND_REQUIRES ? "requires" : "ensures");
                d.primary.tok = n->lhs->tok;
                d.primary.label = diag_fmt("これは '%s' 型です", type_name(t));
                d.hint = "比べる式を書いてください（例: requires b != 0）";
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
                    d.message = diag_fmt("'%s' はエラー型として使えません", type_name(t));
                    d.primary.tok = ex->tok;
                    d.primary.label = "except に書けるのはクラスだけです";
                    d.hint = "エラーはふつうのクラスとして定義してください";
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
                d.message = "この try の中に、失敗しうる呼び出しがありません";
                d.primary.tok = n->tok;
                d.primary.label = "except は決して実行されません";
                d.hint = "raises を宣言した関数を呼んでいるか確かめてください";
                diag_emit(&d);
            }
            break;
        }

        case ND_RAISE: {
            Type *t = check_expr(s, n->lhs);
            if (t->kind != TY_CLASS) {
                Diag d = {0};
                d.message = diag_fmt("'%s' は raise できません", type_name(t));
                d.primary.tok = n->lhs->tok;
                d.primary.label = "raise にはエラーオブジェクトを渡します";
                d.hint = "エラーはふつうのクラスです（例: raise IOError(\"見つかりません\")）";
                diag_fail(&d);
            }

            // ★ 呼び出しと同じ判定：try で捕まえるか、自分の raises に書いてあるか
            if (!try_catches(s, t->cls) && !func_declares(s->cur_func, t->cls)) {
                Diag d = {0};
                d.primary.tok = n->tok;
                if (s->cur_func && s->cur_func->nraises == 0) {
                    d.code = "E-RAISE-1";
                    d.message = diag_fmt("'%s' を raise していますが、宣言がありません",
                                         t->cls->name);
                    d.primary.label = "この関数は失敗しないと宣言されています";
                    d.hint = diag_fmt("関数の宣言に 'raises %s' を足してください",
                                      t->cls->name);
                } else {
                    d.code = "E-RAISE-2";
                    d.message = diag_fmt("エラー '%s' が宣言されていません", t->cls->name);
                    d.primary.label = "raises に含まれていません";
                    d.hint = diag_fmt("この関数の raises に '%s' を足してください",
                                      t->cls->name);
                }
                d.related.tok = s->cur_func ? s->cur_func->tok : NULL;
                d.related.label = "関数の宣言はここです";
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
            d.message = diag_fmt("'%s' はループの外では使えません", kw);
            d.primary.tok = n->tok;
            d.primary.label = diag_fmt("この '%s' を囲む while がありません", kw);
            d.hint = diag_fmt("'%s' は while の中でだけ使えます", kw);
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
    reject_module_name(s, n->name, n->tok, "クラス");
    if (type_from_name(n->name))
        error_at_hint(n->tok, diag_fmt("'%s' は組み込みの型名です", n->name),
                      "クラス名 '%s' は使えません", n->name);
    if (is_builtin_name(n->name))
        error_at_hint(n->tok, diag_fmt("'%s' は組み込み関数の名前です", n->name),
                      "クラス名 '%s' は使えません", n->name);

    Class *prev = lookup_class(s, n->name);
    if (prev) {
        Diag d = {0};
        d.message = diag_fmt("クラス '%s' は既に定義されています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = "ここで再定義されています";
        d.related.tok = prev->tok;
        d.related.label = "最初の定義はここです";
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
// ★ docs/design/memory-model.md 5 節の表がそのまま実装になっています。
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
        d.message = diag_fmt("メソッド '%s' は既に定義されています", mname);
        d.primary.tok = fn->tok;
        d.primary.label = "ここで再定義されています";
        d.related.tok = prev->tok;
        d.related.label = "最初の定義はここです";
        diag_fail(&d);
    }

    Field *clash = lookup_field(c, fn->name);
    if (clash) {
        Diag d = {0};
        d.message = diag_fmt("'%s' はフィールドと同じ名前です", fn->name);
        d.primary.tok = fn->tok;
        d.primary.label = "メソッド名がフィールド名と衝突しています";
        d.related.tok = clash->tok;
        d.related.label = "同名のフィールドはここです";
        d.hint = "t.f が「フィールド」か「メソッド」か決められなくなるため禁止です";
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
    f->tok = fn->tok;

    int i = 0;
    for (Node *pm = fn->params; pm; pm = pm->next, i++) {
        // ★ 第 1 引数 self には型注釈がありません（parser が保証している）。
        //   そのクラスの型をここで入れます。これが「self の暗黙の型」です。
        Type *pt = pm->type_ref ? resolve_type(s, pm->type_ref) : c->type;
        if (pt->kind == TY_NONE)
            error_at_hint(pm->tok, "None 型の値は存在しないので引数にできません",
                          "引数の型に None は使えません");
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
            error_at_hint(fn->tok,
                          "init は戻り値を持てません（-> None と書いてください）",
                          "init の戻り型は None でなければなりません");
        c->has_init = true;
    }

    resolve_raises(s, fn, f);
    if (strcmp(fn->name, "init") == 0 && f->nraises)
        error_at_hint(fn->tok, "init は失敗できません（生成に失敗した値は誰も受け取れません）",
                      "init に raises は書けません");

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
        d.message = diag_fmt("フィールド '%s' は init で代入されていません", f->name);
        d.primary.tok = f->tok;
        d.primary.label = "このフィールドは None から始まってしまいます";
        if (init) {
            d.related.tok = init->tok;
            d.related.label = "init はここです";
        } else {
            d.related.tok = c->tok;
            d.related.label = "このクラスには init がありません";
        }
        d.hint = diag_fmt("次のどちらかにしてください:\n"
                          "             ・型を '%s | None' にする\n"
                          "             ・init の中で self.%s = ... と代入する",
                          type_name(f->type), f->name);
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
        d.message = diag_fmt("'%s' は既に定義されています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = "ここで再定義されています";
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
                error_at_hint(m->tok,
                              "同じ名前のメソッドを 2 度書くことはできません",
                              "メソッド '%s' が重複しています", m->name);
        // ★ インタフェースの宣言に既定値は書けません（A-38）。
        //   呼ぶ側はどの実装が入るか知らないので、既定値を埋める人が
        //   決まりません（実装ごとに違う既定値を書けてしまいます）。
        for (Node *pm = m->params; pm; pm = pm->next) {
            if (!pm->rhs) continue;
            Diag d = {0};
            d.message = "インタフェースの宣言に既定値は書けません";
            d.primary.tok = pm->rhs->tok;
            d.primary.label = "ここには既定値を書けません";
            d.hint = "どの実装が呼ばれるかは実行時に決まるので、"
                     "既定値を埋める人が決まりません。"
                     "既定値はクラス側のメソッドに書いてください"
                     "（インタフェース越しには使えません）";
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
            error_at_hint(tr->tok,
                          diag_fmt("ファイルの先頭に 'import %s' を書いてください",
                                   tr->mod_name),
                          "モジュール '%s' を import していません", tr->mod_name);
        Iface *i = lookup_iface_in(ms, tr->name);
        if (!i)
            error_at_hint(tr->tok, "インタフェース名を確認してください",
                          "モジュール '%s' にインタフェース '%s' はありません",
                          tr->mod_name, tr->name);
        return i;
    }
    Iface *i = lookup_iface(s, tr->name);
    if (!i) {
        Diag d = {0};
        d.message = diag_fmt("インタフェース '%s' が見つかりません", tr->name);
        d.primary.tok = tr->tok;
        d.primary.label = "ここに書けるのはインタフェース名だけです";
        d.hint = lookup_class(s, tr->name)
                     ? diag_fmt("'%s' はクラスです。継承はありません", tr->name)
                     : "interface で宣言してから使ってください";
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
            d.message = diag_fmt("クラス '%s' に '%s' がありません", c->name,
                                 im->name);
            d.primary.tok = at;
            d.primary.label = diag_fmt("'%s' を実装すると宣言しています",
                                       ifc->name);
            d.related.tok = im->sig->tok;
            d.related.label = "このメソッドが必要です";
            diag_fail(&d);
        }

        // 引数の型と戻り型が一致するか（self は数えない）
        Node *sig = im->sig;
        int want = 0;
        for (Node *pm = sig->params; pm; pm = pm->next) want++;
        if (f->nparams != want) {
            Diag d = {0};
            d.message = diag_fmt("'%s.%s' の引数の数が宣言と違います", c->name,
                                 im->name);
            d.primary.tok = f->tok;
            d.primary.label = diag_fmt("%d 個です", f->nparams);
            d.related.tok = sig->tok;
            d.related.label = diag_fmt("宣言では %d 個です", want);
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
                d.message = diag_fmt("'%s.%s' の %d 番目の引数の名前が宣言と違います",
                                     c->name, im->name, k);
                d.primary.tok = f->tok;
                d.primary.label = diag_fmt("実装では '%s' です", f->pnames[k]);
                d.related.tok = pm->tok;
                d.related.label = diag_fmt("宣言では '%s' です", pm->name);
                d.hint = "引数の名前は呼び出し側から見える約束です"
                         "（名前で渡すときに使います）。宣言に合わせてください";
                diag_fail(&d);
            }
            Type *wt = resolve_type(s, pm->type_ref);
            if (!type_equal(f->params[k], wt)) {
                Diag d = {0};
                d.message = diag_fmt("'%s.%s' の %d 番目の引数の型が宣言と違います",
                                     c->name, im->name, k);
                d.primary.tok = f->tok;
                d.primary.label = diag_fmt("'%s' 型です", type_name(f->params[k]));
                d.related.tok = pm->tok;
                d.related.label = diag_fmt("宣言では '%s' 型です", type_name(wt));
                diag_fail(&d);
            }
        }
        Type *wr = resolve_type(s, sig->type_ref);
        if (!type_equal(f->ret, wr)) {
            Diag d = {0};
            d.message = diag_fmt("'%s.%s' の戻り型が宣言と違います", c->name,
                                 im->name);
            d.primary.tok = f->tok;
            d.primary.label = diag_fmt("'%s' を返します", type_name(f->ret));
            d.related.tok = sig->tok;
            d.related.label = diag_fmt("宣言では '%s' です", type_name(wr));
            diag_fail(&d);
        }
        if (f->nraises > 0)
            error_at_hint(f->tok,
                          "インタフェース越しには呼べません"
                          "（エラーの受け渡しを表せないため）",
                          "'%s.%s' は raises します", c->name, im->name);
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
                error_at_hint(ir->tok, "同じインタフェースを 2 度書けません",
                              "'%s' は既に書かれています", ifc->name);
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
            d.message = diag_fmt("フィールド '%s' は既に宣言されています", m->name);
            d.primary.tok = m->tok;
            d.primary.label = "ここで再宣言されています";
            d.related.tok = prev->tok;
            d.related.label = "最初の宣言はここです";
            diag_fail(&d);
        }

        Type *ft = resolve_type(s, m->type_ref);
        if (ft->kind == TY_NONE)
            error_at_hint(m->tok, "None 型の値は存在しないのでフィールドにできません",
                          "フィールドの型に None は使えません");

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
    d.message = diag_fmt("extern の%sに bool は使えません", what);
    d.primary.tok = tok;
    d.primary.label = "この型は C との境界を越えられません";
    d.hint = "int で受け取り、本言語側で 'n == 1' と書いてください";
    diag_fail(&d);
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
            d.message = diag_fmt("'%s' はエラー型として使えません", type_name(t));
            d.primary.tok = r->tok;
            d.primary.label = "raises に書けるのはクラスだけです";
            d.hint = "エラーはふつうのクラスとして定義してください"
                     "（例: class IOError:\n                 message: str）";
            diag_fail(&d);
        }
        f->raises[i] = t->cls;
        err_tag_of(s, t->cls);  // ★ ここで ID を確定させる
        r->type = t;
    }
}

static void declare_func(Sema *s, Node *n) {
    reject_module_name(s, n->name, n->tok, "関数");
    if (lookup_class(s, n->name)) {
        Diag d = {0};
        d.message = diag_fmt("'%s' はクラス名として使われています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = "この名前の関数は定義できません";
        d.related.tok = lookup_class(s, n->name)->tok;
        d.related.label = "クラスの定義はここです";
        d.hint = "クラス名は「インスタンス生成」の呼び出しに使われます"
                 "（例: Token(1, \"x\")）";
        diag_fail(&d);
    }
    if (is_builtin_name(n->name))
        error_at_hint(n->tok, diag_fmt("%s は組み込み関数です。別の名前を使ってください",
                                       n->name),
                      "'%s' は再定義できません", n->name);

    FuncSig *prev = lookup_func(s, n->name);
    if (prev) {
        Diag d = {0};
        d.message = diag_fmt("関数 '%s' は既に定義されています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = "ここで再定義されています";
        d.related.tok = prev->tok;
        d.related.label = "最初の定義はここです";
        diag_fail(&d);
    }

    // ★ ジェネリックなテンプレートは **型を解決しません**。
    //   T が何なのかまだ決まっていないためです。名前だけ登録して、
    //   呼ばれたときに実引数から決めて実体を作ります。
    if (n->targs) {
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
    f->tok = n->tok;

    int i = 0;
    for (Node *pm = n->params; pm; pm = pm->next, i++) {
        Type *pt = resolve_type(s, pm->type_ref);
        if (pt->kind == TY_NONE)
            error_at_hint(pm->tok, "None 型の値は存在しないので引数にできません",
                          "引数の型に None は使えません");
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
        check_extern_type(ret, n->tok, "戻り値");
        for (Node *pm = n->params; pm; pm = pm->next)
            check_extern_type(pm->type, pm->tok, "引数");
    }

    resolve_raises(s, n, f);
    if (is_extern_decl && f->nraises)
        error_at_hint(n->tok, "extern の関数は C 側の約束に従うので raises は書けません",
                      "extern に raises は書けません");

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
static FuncSig *instantiate_lambda(Sema *s, FuncSig *tmpl, Node *ref) {
    Node *tn = tmpl->tmpl;

    int np = 0;
    for (Node *pm = tn->params; pm; pm = pm->next) np++;

    Type *want = s->expected;
    if (!want || want->kind != TY_FN) {
        Diag d = {0};
        d.message = "この lambda がどんな関数になるのか決められません";
        d.primary.tok = ref->tok;
        d.primary.label = "ここでは引数と戻り値の型が決まりません";
        d.hint = "型注釈のある変数に入れるか、関数型の引数に渡してください:\n"
                 "             p: fn(int) -> bool = lambda x: x > 0\n"
                 "             count_if(xs, lambda x: x > 0)";
        diag_fail(&d);
    }
    if (want->nparams != np) {
        Diag d = {0};
        d.message = diag_fmt("この lambda は %d 個の引数を取りますが、'%s' が要ります",
                             np, type_name(want));
        d.primary.tok = ref->tok;
        d.primary.label = diag_fmt("引数が %d 個です", np);
        d.hint = diag_fmt("ここに置けるのは %d 引数の lambda です", want->nparams);
        diag_fail(&d);
    }

    // 束ねる型の並び（引数 … と戻り型）
    Type *args[MAX_TARGS];
    int nt = np + 1;
    if (nt > MAX_TARGS) error_at(ref->tok, "lambda の引数が多すぎます");
    for (int i = 0; i < np; i++) args[i] = want->params[i];
    args[np] = want->elem;

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
    s->tbind = binds;

    Node *inst = ast_clone(tn);
    inst->name = iname;
    inst->targs = NULL;
    inst->next = NULL;

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
                  diag_fmt("関数 '%s'", tmpl->name), false);
    }

    int nargs = 0;
    for (Node *a = call->args; a; a = a->next) nargs++;
    if (nargs != want)
        error_at_hint(call->tok,
                      diag_fmt("'%s' は %d 個の引数を取ります", tmpl->name, want),
                      "引数の個数が違います（%d 個渡されました）", nargs);

    // ── 実引数の型を先に求める ──
    Type *atypes[MAX_TARGS * 4];
    if (nargs > (int)(sizeof(atypes) / sizeof(atypes[0])))
        error_at(call->tok, "引数が多すぎます");
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
            d.message = diag_fmt("型引数 '%s' を決められません", tp->name);
            d.primary.tok = call->tok;
            d.primary.label = "実引数から決まりません";
            d.related.tok = tn->tok;
            d.related.label = "この関数の定義です";
            d.hint = "型引数は **引数の型から**決まります"
                     "（戻り型にしか現れない型引数は書けません）";
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
    reject_module_name(s, n->name, n->tok, "グローバル変数");
    Type *declared = resolve_type(s, n->type_ref);
    if (declared->kind == TY_NONE)
        error_at_hint(n->tok, "None 型の値は存在しないので変数にできません",
                      "変数の型に None は使えません");

    VarEntry *prev = lookup_local(s, n->name);
    if (prev) {
        Diag d = {0};
        d.message = diag_fmt("変数 '%s' は既に宣言されています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = "ここで再宣言されています";
        d.related.tok = prev->decl_tok;
        d.related.label = "最初の宣言はここです";
        diag_fail(&d);
    }

    // 注意: 初期化式はコンパイル時定数のみ（言語仕様 6.2 の v1 制限）。
    //    計算を許すと「どちらを先に初期化するか」という初期化順序問題が起きます。
    if (n->rhs->kind != ND_INT && n->rhs->kind != ND_BOOL &&
        n->rhs->kind != ND_STR && n->rhs->kind != ND_FLOAT) {
        Diag d = {0};
        d.message = "グローバル変数の初期化式は定数でなければなりません";
        d.primary.tok = n->rhs->tok;
        d.primary.label =
            "ここには整数・浮動小数点数・True / False・文字列リテラルだけが書けます";
        d.hint = "計算が必要なら main の中でローカル変数にしてください";
        diag_fail(&d);
    }

    Type *actual = check_expr(s, n->rhs);
    if (!type_assignable(actual, declared)) {
        Diag d = {0};
        d.message = "型が一致しません";
        d.primary.tok = n->rhs->tok;
        d.primary.label = diag_fmt("型 '%s' の式", type_name(actual));
        d.related.tok = n->tok;
        d.related.label = diag_fmt("変数 '%s' は '%s' 型として宣言されています",
                                   n->name, type_name(declared));
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
                d.message = diag_fmt("%s は関数の本体の先頭に書きます",
                                     st->kind == ND_REQUIRES ? "requires"
                                                             : "ensures");
                d.primary.tok = st->tok;
                d.primary.label = "ここには書けません";
                d.hint = "requires / ensures は def の直後に並べてください"
                         "（入口と出口で確かめるものだからです）";
                diag_fail(&d);
            }
            if (!is_contract) seen_other = true;
        }
    }

    check_stmt_list(s, n->body->body);
    scope_pop(s);

    // 全経路で return するか（型システム 6.1）
    if (n->type->kind != TY_NONE && !always_returns(n->body)) {
        Diag d = {0};
        d.message = diag_fmt("関数 '%s' は値を返さずに終わる経路があります", n->name);
        d.primary.tok = n->tok;
        d.primary.label = diag_fmt("戻り型は '%s' です", type_name(n->type));
        d.hint = "すべての経路で return してください"
                 "（if に else が無いと、条件が偽のとき素通りします）";
        diag_fail(&d);
    }
    s->cur_func = NULL;
}

// main の検査（言語仕様 6.1）
static void check_main(Sema *s, Node *ast) {
    FuncSig *m = lookup_func(s, "main");
    if (!m) {
        Diag d = {0};
        d.message = "main 関数がありません";
        d.primary.tok = ast->tok;
        d.primary.label = "このファイルには入口がありません";
        d.hint = "プログラムの入口として次を定義してください:\n"
                 "             def main() -> int:\n"
                 "                 return 0";
        diag_fail(&d);
    }
    if (m->nparams != 0)
        error_at_hint(m->tok, "main は引数なしで定義してください（def main() -> int:）",
                      "main は引数を取れません");
    if (m->ret->kind != TY_INT)
        error_at_hint(m->tok, "main の戻り値がプロセスの終了コードになります",
                      "main の戻り型は int でなければなりません");
    // ★ R4：main の失敗を受け取る相手はいません。
    if (m->nraises) {
        Diag d = {0};
        d.code = "E-RAISE-4";
        d.message = "main は raises を宣言できません";
        d.primary.tok = m->tok;
        d.primary.label = "この失敗を受け取る相手がいません";
        d.hint = "main の中で try で捕まえるか、panic で終わらせてください";
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
                d.message = diag_fmt("既定値のある引数より後ろに、既定値の無い引数 '%s' があります",
                                     pm->name);
                d.primary.tok = pm->tok;
                d.primary.label = "この引数にも既定値が要ります";
                d.related.tok = f->defaults[first_default]->tok;
                d.related.label = diag_fmt("'%s' に既定値が付いています",
                                           f->pnames[first_default]);
                d.hint = "既定値のある引数は後ろにまとめてください"
                         "（そうしないと、前から順に当てられません）";
                diag_fail(&d);
            }
            continue;
        }
        if (first_default < 0) first_default = i;

        // ★ 既定値の型を確かめます。**列挙の枝はここで定数に畳まれます**。
        Type *dt = check_expr(s, f->defaults[i]);
        if (!type_assignable(dt, f->params[i])) {
            Diag d = {0};
            d.message = diag_fmt("引数 '%s' の既定値の型が違います（'%s' に '%s' は入りません）",
                                 pm->name, type_name(f->params[i]), type_name(dt));
            d.primary.tok = f->defaults[i]->tok;
            d.primary.label = diag_fmt("これは '%s' 型です", type_name(dt));
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
        error_at_hint(n->tok, diag_fmt("'%s' は組み込みの型名です", n->name),
                      "この名前は使えません");
    EnumDef *old = lookup_enum(s, n->name);
    if (old) {
        Diag d = {0};
        d.message = diag_fmt("型 '%s' はすでに定義されています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = "同じ名前の列挙が 2 つあります";
        d.related.tok = old->tok;
        d.related.label = "最初の定義はここです";
        diag_fail(&d);
    }
    if (lookup_class(s, n->name))
        error_at_hint(n->tok, "クラスと同じ名前の列挙は作れません",
                      "'%s' はクラス名として使われています", n->name);
    if (lookup_range(s, n->name))
        error_at_hint(n->tok, "範囲型と同じ名前の列挙は作れません",
                      "'%s' は範囲型として使われています", n->name);

    EnumDef *e = xmalloc(sizeof(EnumDef));
    e->name = n->name;
    e->tok = n->tok;
    e->vals = NULL;
    e->nvals = 0;
    e->owner = s->cur;

    // 枝を宣言した順に並べます（番号は parser が振ってあります）。
    EnumVal *tail = NULL;
    for (Node *v = n->body; v; v = v->next) {
        EnumVal *ev = xmalloc(sizeof(EnumVal));
        ev->name = v->name;
        ev->val = v->ival;
        ev->tok = v->tok;
        ev->next = NULL;
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
        error_at_hint(n->tok, diag_fmt("'%s' は組み込みの型名です", n->name),
                      "この名前は使えません");
    RangeTy *old = lookup_range(s, n->name);
    if (old) {
        Diag d = {0};
        d.message = diag_fmt("型 '%s' はすでに定義されています", n->name);
        d.primary.tok = n->tok;
        d.primary.label = "同じ名前の範囲型が 2 つあります";
        d.related.tok = old->tok;
        d.related.label = "最初の定義はここです";
        diag_fail(&d);
    }
    if (lookup_class(s, n->name))
        error_at_hint(n->tok,
                      "クラスと同じ名前の範囲型は作れません",
                      "'%s' はクラス名として使われています", n->name);

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
}
