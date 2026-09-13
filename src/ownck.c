// ownck.c — 所有権検査
//
// 仕様は docs/spec/safety-spec.md §3、設計は docs/design/ownership.md §3〜4。
//
// この章でやること：**移動済みの値を使っていないか**（S1: use-after-move）。
//
//     xs: list[int] = [1, 2]
//     ys: list[int] = xs     ← ここで xs は ys へ移動した
//     print(len(xs))         ← 移動済みの値を使っている（E-MOVE-1）
//
// ★ 検査の単位は「値」ではなく **場所（Place）** です。
//   x / self.name / xs[…] / mod.g の 4 種類を追いかけ、それぞれに
//   Valid / MaybeMoved / Moved の 3 状態を持たせます。

#include "ownck.h"

#include <stdio.h>
#include <string.h>

#include "ast.h"
#include "diag.h"
#include "util.h"

// ── ① 場所（Place）─────────────────────────────────────────
//
//   x        → Local(x)
//   self.f   → Field(Local(self), f)
//   xs[i]    → Index(Local(xs))     ★ 添字は区別しない（実行時に決まるため）
//   mod.g    → Global(mod.g)

typedef enum {
    PL_LOCAL,
    PL_FIELD,
    PL_INDEX,
    PL_GLOBAL,
} PlaceKind;

typedef struct Place Place;
struct Place {
    PlaceKind kind;
    Place *base;       // FIELD / INDEX の親
    const char *key;   // 同一性の判定に使う名前
                       //   LOCAL / GLOBAL … sema が振った IR 名（%x / @g.x）
                       //   FIELD          … フィールド名
    const char *disp;  // 診断に出す見た目（"xs" / "self.name" / "xs[…]"）
};

static Place *new_place(PlaceKind kind, Place *base, const char *key,
                        const char *disp) {
    Place *p = xmalloc(sizeof(Place));
    p->kind = kind;
    p->base = base;
    p->key = key;
    p->disp = disp;
    return p;
}

// 式が「場所」なら Place を作る。そうでなければ NULL。
//
// ⚠️ 呼び出しの戻り値やリテラルは場所ではありません（NULL を返します）。
//    一時的な値なので、移動しても誰も困らないからです。
static Place *place_of(Node *n) {
    if (!n) return NULL;

    switch (n->kind) {
        case ND_VAR:
            // ★ 名前ではなく IR 名で識別します。兄弟スコープの同名変数
            //   （%x と %x.1）を別の場所として扱うためです。
            if (!n->ir_name) return NULL;
            return new_place(n->ir_name[0] == '@' ? PL_GLOBAL : PL_LOCAL, NULL,
                             n->ir_name, n->name);

        case ND_FIELD: {
            // ★ 'mod.g'（他モジュールのグローバル）は「常に生きている場所」。
            //   名前が一意なので、そのまま 1 つの Global にします。
            if (n->mod_name)
                return n->ir_name ? new_place(PL_GLOBAL, NULL, n->ir_name, n->name)
                                  : NULL;
            Place *base = place_of(n->lhs);
            if (!base) return NULL;
            return new_place(PL_FIELD, base, n->name,
                             diag_fmt("%s.%s", base->disp, n->name));
        }

        case ND_INDEX: {
            // ⚠️ str の添字は「場所」ではありません。`s[i]` は 1 文字の
            //    **新しい文字列**を作って返すからです（runtime の pl_str_index）。
            //    list[T] の要素と違い、元の文字列を借りているわけではないので、
            //    `for c in s:` で取り出した文字は保存しても構いません。
            if (n->lhs->type && n->lhs->type->kind == TY_STR) return NULL;
            Place *base = place_of(n->lhs);
            if (!base) return NULL;
            return new_place(PL_INDEX, base, "[]",
                             diag_fmt("%s[…]", base->disp));
        }

        default: return NULL;
    }
}

static bool place_eq(Place *a, Place *b) {
    if (a == b) return true;
    if (!a || !b || a->kind != b->kind) return false;
    if (strcmp(a->key, b->key) != 0) return false;
    return place_eq(a->base, b->base);
}

// a が b の接頭辞か（a == b も含む）。
//   Local(a) は Field(Local(a), x) の接頭辞
static bool place_prefix_of(Place *a, Place *b) {
    for (Place *p = b; p; p = p->base)
        if (place_eq(a, p)) return true;
    return false;
}

// 2 つの場所が重なるか。
//
// ★ 衝突するのは「片方がもう片方の接頭辞」のときだけです。
//   self.x と self.y は重なりません（別々に移動できる）。
static bool place_overlaps(Place *a, Place *b) {
    return place_prefix_of(a, b) || place_prefix_of(b, a);
}

// ── ② 所有型かどうか ───────────────────────────────────────

// rc[T] か rc[T] | None か。**共有型はこの 2 つ**です。
bool ty_is_rc(Type *t) {
    if (!t) return false;
    if (t->kind == TY_RC) return true;
    return t->kind == TY_OPT && t->elem && t->elem->kind == TY_RC;
}

bool ty_is_owned(Type *t) {
    if (!t) return false;
    switch (t->kind) {
        case TY_STR:
        case TY_LIST:
        case TY_CLASS:
        case TY_RC: return true;  // rc[T] も「後始末が要る値」
        case TY_OPT: return ty_is_owned(t->elem);  // Token | None も所有型
        default: return false;                     // int / bool / None
    }
}

// ── ③ 格子（lattice）と流れの状態 ───────────────────────────
//
//      Valid              使える
//        │
//   MaybeMoved            分岐によっては移動済み
//        │
//      Moved              移動済み
//
// ★ 合流は保守的な結合：Valid ⊔ Moved = MaybeMoved。

typedef enum {
    ST_VALID = 0,
    ST_MAYBE = 1,
    ST_MOVED = 2,
} OwnState;

static OwnState st_join(OwnState a, OwnState b) {
    return a == b ? a : ST_MAYBE;
}

typedef struct Ent Ent;
struct Ent {
    Place *pl;
    OwnState st;
    Token *at;  // 移動した位置（診断の note: に出す）
    Ent *next;
};

// ある時点での「全ての場所の状態」。
// ★ 表に無い場所は Valid です（移動されていないものを全部並べる必要はない）。
typedef struct {
    Ent *ents;
    bool dead;  // この経路は return / break / continue で終わっている
} Flow;

static Flow flow_copy(const Flow *src) {
    Flow out = {NULL, src->dead};
    Ent *tail = NULL;
    for (Ent *e = src->ents; e; e = e->next) {
        Ent *c = xmalloc(sizeof(Ent));
        *c = *e;
        c->next = NULL;
        if (tail) tail->next = c;
        else out.ents = c;
        tail = c;
    }
    return out;
}

static Ent *flow_find(const Flow *f, Place *p) {
    for (Ent *e = f->ents; e; e = e->next)
        if (place_eq(e->pl, p)) return e;
    return NULL;
}

// p（またはそれと重なる場所）の状態。いちばん悪いものを返す。
//
// ★ 重なりを見るのがここです。'a' を移動した後に 'a.f' を読むのも、
//   'a.f' を移動した後に 'a' を丸ごと読むのも、どちらもエラーです。
static OwnState state_of(const Flow *f, Place *p, Ent **who) {
    OwnState worst = ST_VALID;
    Ent *w = NULL;
    for (Ent *e = f->ents; e; e = e->next) {
        if (e->st == ST_VALID) continue;
        if (!place_overlaps(e->pl, p)) continue;
        if (!w || e->st > worst) {
            worst = e->st;
            w = e;
        }
    }
    if (who) *who = w;
    return w ? worst : ST_VALID;
}

// p と重なる記録を消す（＝ふたたび Valid にする）
static void flow_clear(Flow *f, Place *p) {
    Ent **link = &f->ents;
    while (*link) {
        if (place_overlaps((*link)->pl, p)) *link = (*link)->next;
        else link = &(*link)->next;
    }
}

static void flow_move(Flow *f, Place *p, Token *at) {
    flow_clear(f, p);
    Ent *e = xmalloc(sizeof(Ent));
    e->pl = p;
    e->st = ST_MOVED;
    e->at = at;
    e->next = f->ents;
    f->ents = e;
}

// dst ← dst ⊔ src（合流）
static void flow_join(Flow *dst, const Flow *src) {
    if (src->dead) return;  // 到達しない経路は合流に参加しない（設計 4.2）
    if (dst->dead) {
        *dst = flow_copy(src);
        return;
    }

    // dst 側にある場所：src に記録が無ければ、src では Valid だったということ
    for (Ent *e = dst->ents; e; e = e->next) {
        Ent *s = flow_find(src, e->pl);
        OwnState other = s ? s->st : ST_VALID;
        OwnState joined = st_join(e->st, other);
        if (joined != e->st && s && s->at)
            e->at = s->at;  // 位置は「片方の枝」を指せれば足りる
        e->st = joined;
    }

    // src にしか無い場所：dst 側は Valid なので、必ず MaybeMoved になる
    for (Ent *s = src->ents; s; s = s->next) {
        if (s->st == ST_VALID) continue;
        if (flow_find(dst, s->pl)) continue;
        Ent *c = xmalloc(sizeof(Ent));
        *c = *s;
        c->st = st_join(ST_VALID, s->st);  // 片方が Valid なので MaybeMoved
        c->next = dst->ents;
        dst->ents = c;
    }
}

static bool flow_eq(const Flow *a, const Flow *b) {
    if (a->dead != b->dead) return false;
    for (Ent *e = a->ents; e; e = e->next) {
        Ent *o = flow_find(b, e->pl);
        if ((o ? o->st : ST_VALID) != e->st) return false;
    }
    for (Ent *e = b->ents; e; e = e->next) {
        if (e->st == ST_VALID) continue;
        if (!flow_find(a, e->pl)) return false;
    }
    return true;
}

// ── ④ 解析の状態 ───────────────────────────────────────────

// 関数の定義を IR 名から引く表。
// ★ 呼び出し側で「この引数は own か」を知るために要ります。
//   sema は FuncSig に受け取り方を持っていないので（ ND_PARAM に
//   持たせた）、ここでは定義ノードそのものを引きます。
typedef struct FuncEnt FuncEnt;
struct FuncEnt {
    const char *ir_name;
    Node *fn;
    FuncEnt *next;
};

// 借用している場所の根。
//
// ★ 借用は「関数の引数」からしか生まれません（D3：`&` も借用束縛も書かせない）。
//   だから、いま検査している関数の仮引数を並べておけば、
//   「この場所は借りものか」は根をたどるだけで分かります。
typedef struct BorrowRoot BorrowRoot;
struct BorrowRoot {
    const char *key;   // 借りている変数の IR 名（%xs）
    Node *origin;      // 貸し手の宣言（ND_PARAM または ND_VARDECL）
    bool is_param;     // 引数から借りているか（ローカルから借りることもある）
    bool is_self;      // self か（仕様 §4.5 の例外）
    bool is_mut;       // 書き換えてよいか
    int depth;         // 貸し手が宣言されたスコープの深さ（寿命の検査に使う）
    BorrowRoot *next;
};

// 変数宣言の表。
//
// ★ 「この変数は借りものを束縛している」という印は、代入した場所ではなく
//   **宣言のノード**に付けます。codegen は宣言を見て drop を出すからです。
typedef struct DeclEnt DeclEnt;
struct DeclEnt {
    const char *key;  // IR 名
    Node *decl;       // ND_VARDECL / ND_PARAM
    int depth;        // 宣言されたスコープの深さ
    DeclEnt *next;
};

// ループ 1 つぶんの出口情報（break / continue が積む場所）
typedef struct Loop Loop;
struct Loop {
    Flow brk;   // break で抜けたときの状態の結合
    Flow cont;  // continue で戻るときの状態の結合
    Loop *outer;
};

// 1 回のコンパイルで出す警告の上限。
// ★ selfhost/ を書き換えるまで、ここは何百件も出ます。
//   全部見たいときは -DOWNCK_MAX_REPORT=100000 でビルドしてください。
#ifndef OWNCK_MAX_REPORT
#define OWNCK_MAX_REPORT 20
#endif

typedef struct {
    FuncEnt *funcs;
    OwnckOptions opt;  // どの検査をエラーに昇格するか（--deny-* / --explain-mut）
    int quiet;         // >0 なら診断を出さない（while の不動点反復中）
    int nreport;       // 出した件数
    int nmore;         // 上限を超えて省略した件数
    Loop *loop;
    BorrowRoot *roots;  // いま検査中の関数で「借りている」変数
    struct DeclEnt *decls;  // いま検査中の関数の変数宣言
    int depth;          // 今いるスコープの深さ（借用の寿命の検査に使う）
    Node *cur_fn;       // いま検査中の関数（診断の「直し方」に名前が要る）
    int scope_depth;    // scope: の中にいる深さ（0 なら外）
} Own;

static DeclEnt *find_decl(Own *o, const char *ir_name);

// 場所の根が借用引数なら、その根を返す。
//
// ★ self.name も t.text も、根をたどれば仮引数です。
//   「借りたものの一部」もまた借りものである、という規則がこの 4 行です。
static BorrowRoot *borrow_root_of(Own *o, Place *p) {
    Place *root = p;
    while (root->base) root = root->base;
    if (root->kind != PL_LOCAL) return NULL;  // グローバルは常に生きている（設計 §5.3）
    for (BorrowRoot *b = o->roots; b; b = b->next)
        if (strcmp(b->key, root->key) == 0) return b;
    return NULL;
}

static Node *lookup_func(Own *o, const char *ir_name) {
    if (!ir_name) return NULL;
    for (FuncEnt *e = o->funcs; e; e = e->next)
        if (strcmp(e->ir_name, ir_name) == 0) return e->fn;
    return NULL;
}

// ── ⑤ 診断 ─────────────────────────────────────────────────

// 1 件の診断を出す。deny なら **エラーとして即終了**する。
//
// ★ 上限（OWNCK_MAX_REPORT）で打ち切るのは警告のときだけです。
//   エラーなら 1 件目で終わるので、そもそも上限に届きません。
static void emit_ownck(Own *o, Diag *d, bool deny) {
    if (o->quiet) return;
    if (o->opt.explain_mut) return;  // --explain-mut は一覧を出すだけの道具
    if (!deny && o->nreport >= OWNCK_MAX_REPORT) {
        o->nmore++;
        return;
    }
    o->nreport++;
    d->severity = deny ? "error" : "warning";
    if (deny) diag_fail(d);
    diag_emit(d);
}

static void report_use(Own *o, Place *p, Ent *moved, Node *at) {
    if (o->quiet) return;

    bool maybe = moved->st == ST_MAYBE;
    // ★ 移動した場所と使った場所が同じなら、それはループの前の反復です。
    //   「分岐によっては」と言われても読み手には意味が通らないので言い換えます。
    bool prev_round = moved->at == at->tok;

    Diag d = {0};
    d.code = "E-MOVE-1";
    d.message = maybe ? diag_fmt("移動済みかもしれない値 '%s' を使っています", p->disp)
                      : diag_fmt("移動済みの値 '%s' を使っています", p->disp);
    d.primary.tok = at->tok;
    d.primary.label = "ここで使われています";
    d.related.tok = moved->at;
    if (prev_round)
        d.related.label = diag_fmt("'%s' は前の繰り返しで、ここで移動しています",
                                   moved->pl->disp);
    else if (maybe)
        d.related.label = diag_fmt("分岐によっては、'%s' はここで移動しています",
                                   moved->pl->disp);
    else
        d.related.label = diag_fmt("'%s' はここで移動しました", moved->pl->disp);

    if (prev_round)
        d.hint = "繰り返しのたびに移動するので、2 周目には値がありません"
                 "（ループの中で作り直すか、借用で足りないか確かめてください）";
    else if (maybe)
        d.hint = "どの経路を通っても有効になるように、分岐の後で代入し直してください";
    else
        d.hint = "移動した後も使うなら、値を作り直して代入してください（例: xs = [...]）";

    emit_ownck(o, &d, o->opt.deny_move);
}

// 値が移動する「文脈」（借用のときに何と言うかが変わる）
typedef enum {
    MV_ASSIGN,   // ys = xs        変数への代入
    MV_FIELD,    // self.xs = a    フィールドへ保存
    MV_APPEND,   // xss.append(a)  コンテナへ保存
    MV_RETURN,   // return a       返す
    MV_OWN_ARG,  // take(a)        own 引数へ渡す
} MoveCtx;

// 借用した値を移動しようとした（仕様 §4.2 / §4.4）。
//
// ★ 「なぜ駄目か」より「どう直すか」を先に出します（仕様 §12）。
//   直し方はいつも同じ：**その引数を own にする**。
static void report_borrow(Own *o, BorrowRoot *br, Place *p, Node *at, MoveCtx ctx) {
    if (o->quiet) return;

    const char *code = "E-BORROW-1";
    const char *msg = diag_fmt("借用した値 '%s' は移動できません", p->disp);
    const char *label = "ここで移動しようとしています";

    if (ctx == MV_FIELD) {
        code = "E-BORROW-3";
        msg = diag_fmt("借用した値 '%s' をフィールドに保存できません", p->disp);
        label = "保存すると、貸してくれた相手より長生きしてしまいます";
    } else if (ctx == MV_APPEND) {
        code = "E-BORROW-3";
        msg = diag_fmt("借用した値 '%s' をリストに保存できません", p->disp);
        label = "保存すると、貸してくれた相手より長生きしてしまいます";
    } else if (ctx == MV_RETURN) {
        code = "E-BORROW-4";
        msg = diag_fmt("借用した値 '%s' は返せません", p->disp);
        label = "返すと、呼び出しが終わった後も生き続けてしまいます";
    }

    Diag d = {0};
    d.code = code;
    d.message = msg;
    d.primary.tok = at->tok;
    d.primary.label = label;
    d.related.tok = br->origin->tok;
    d.related.label =
        br->is_self ? "'self' は借用です（メソッドはインスタンスを借りているだけです）"
        : br->is_param ? diag_fmt("引数 '%s' は借用です（既定）", br->origin->name)
                       : diag_fmt("'%s' が所有しています（借りているだけです）",
                                  br->origin->name);

    if (br->is_self)
        d.hint = "返してよいのは self のフィールドの借用だけです（仕様 §4.5）。"
                 "値そのものが要るなら copy(...) を使ってください";
    else if (!br->is_param)
        d.hint = diag_fmt("'%s' が生きている間しか使えません。"
                          "所有権ごと渡すなら、作った値を直接渡してください",
                          br->origin->name);
    else if (ctx == MV_ASSIGN)
        d.hint = diag_fmt("別の名前を付けずに、そのまま使ってください"
                          "（所有権ごと要るなら '%s: own %s' にします）",
                          br->origin->name,
                          br->origin->type ? type_name(br->origin->type) : "T");
    else
        d.hint = diag_fmt("引数を '%s: own %s' にすると、所有権を受け取れます",
                          br->origin->name,
                          br->origin->type ? type_name(br->origin->type) : "T");

    emit_ownck(o, &d, o->opt.deny_borrow);
}


// 自分が所有しているものの「一部」を返した、と報告する（A-21d）。
//
// ★ なぜ危ないか（docs/roadmap.md A-21d）
//   `return hits[0]` や `return self.out` は、**その場所の持ち主が
//   関数の出口で解放される**なら、解放済みを返すことになります。
//   戻り値の型に「これは借用だ」と書く手段が無いので、呼ぶ側は
//   所有として受け取り、自分でも解放します。
//
//   直し方は 2 つだけです：
//     move_out(場所)  … 持ち主から**取り上げて**返す（場所は空になる）
//     copy(場所)  … 複製して返す（場所はそのまま）
static void report_return_borrow(Own *o, Place *p, Node *at) {
    if (o->quiet) return;

    Place *root = p;
    while (root->base) root = root->base;
    Node *lender = NULL;
    if (root->kind == PL_LOCAL) {
        DeclEnt *d = find_decl(o, root->key);
        if (d) lender = d->decl;
    }

    Diag d = {0};
    d.code = "E-BORROW-8";
    d.message = diag_fmt("借用した値 '%s' を返しています", p->disp);
    d.primary.tok = at->tok;
    d.primary.label = "返した先では所有になりますが、ここでは借りものです";
    if (lender) {
        d.related.tok = lender->tok;
        d.related.label = diag_fmt("'%s' が所有していて、この関数の出口で解放されます",
                                   lender->name);
    }
    d.hint = "持ち主から取り上げるなら move_out(...)、複製するなら copy(...) を"
             "使ってください";
    emit_ownck(o, &d, o->opt.deny_store_borrow);
}

// 所有しているスロット（フィールド／リストの要素）へ「借りもの」を入れた、と報告する。
//
// ★ report_borrow は BorrowRoot（借用引数などの貸し手）が分かっている場合の版です。
//   `a.next = xs[1]` のように、**自分が所有している入れ物から読んだ値**には
//   BorrowRoot がありません。それでも所有スロットへ入れれば所有者が 2 つになり、
//   --drop すると二重解放になります（実測：tests/mods/mod_class_across が segfault）。
//
// ⚠️ **own 引数へ渡す形（MV_OWN_ARG）もここで見ます。** 以前は MV_FIELD と
//   MV_APPEND だけを見ていたので、`Box(xs[0])` のように借りものを own 引数へ
//   渡す形が**診断なしで二重解放**になっていました。受け取った側は own なので
//   解放し、貸し手も解放します（実測：--drop 版のコンパイラが自分自身を
//   通せませんでした。docs/roadmap.md A-21c）。
static void report_store_borrow(Own *o, Place *p, Node *at, MoveCtx ctx) {
    if (o->quiet) return;

    Place *root = p;
    while (root->base) root = root->base;
    Node *lender = NULL;
    if (root->kind == PL_LOCAL) {
        DeclEnt *d = find_decl(o, root->key);
        if (d) lender = d->decl;
    }

    Diag d = {0};
    d.code = "E-BORROW-7";
    d.message = ctx == MV_APPEND
        ? diag_fmt("借用した値 '%s' をリストに保存しています", p->disp)
        : ctx == MV_OWN_ARG
        ? diag_fmt("借用した値 '%s' を own 引数へ渡しています", p->disp)
        : diag_fmt("借用した値 '%s' をフィールドに保存しています", p->disp);
    d.primary.tok = at->tok;
    d.primary.label = ctx == MV_OWN_ARG
        ? "渡すと、所有者が 2 つになります"
        : "保存すると、所有者が 2 つになります";
    if (lender) {
        d.related.tok = lender->tok;
        d.related.label = diag_fmt("'%s' が所有しています（読んだだけでは借りものです）",
                                   lender->name);
    }
    d.hint = "入れる値をその場で作るか、copy(...) で複製してください"
             "（共有したままにするなら rc[T] です）";
    emit_ownck(o, &d, o->opt.deny_store_borrow);
}


// ── 可変性（B3）と借用の衝突（B1） ─────────────────────

// 書き換えの種類（診断の言い回しだけが変わる）
typedef enum {
    WR_ASSIGN,  // p.f = v / xs[i] = v
    WR_METHOD,  // mut self のメソッドを呼んだ
    WR_APPEND,  // append した
    WR_ARG,     // mut 引数に渡した
} WriteKind;

// 呼び出し 1 つぶんの実引数（self を含む）。
// ★ 借用の衝突（B1）は「1 つの呼び出しの中」だけを見ればよいので、
//   この小さなリストで足ります（設計 ownership.md §5.1）。
typedef struct ArgRef ArgRef;
struct ArgRef {
    Node *expr;      // 実引数の式（self なら受け手）
    Node *param;     // 対応する仮引数（self / 不明なら NULL）
    bool is_mut;     // 可変借用として渡すか
    WriteKind kind;  // 診断の言い回し
    ArgRef *next;
};

// 読み取り専用の借用を書き換えていないか（B3。仕様 §5.2）
static void check_mut(Own *o, Node *at, Node *target, WriteKind kind) {
    Place *p = place_of(target);
    if (!p) return;

    BorrowRoot *br = borrow_root_of(o, p);
    if (!br) return;      // 借りものではない＝自分のもの。書き換え自由（仕様 §5.1）
    if (br->is_mut) return;  // mut で借りている

    Diag d = {0};
    d.code = "E-MUT-1";
    d.message = diag_fmt("読み取り専用の借用 '%s' を書き換えています", p->disp);
    d.primary.tok = at->tok;
    switch (kind) {
        case WR_ASSIGN: d.primary.label = "この代入で書き換えています"; break;
        case WR_METHOD: d.primary.label = "このメソッドは self を書き換えます"; break;
        case WR_APPEND: d.primary.label = "append はリストを書き換えます"; break;
        case WR_ARG:    d.primary.label = "この引数は 'mut' で受け取られます"; break;
    }
    d.related.tok = br->origin->tok;
    d.related.label = br->is_self
        ? "'self' は読み取り専用で借りています"
        : diag_fmt("引数 '%s' は読み取り専用の借用です（既定）", br->origin->name);
    d.hint = br->is_self
        ? diag_fmt("メソッドの宣言を 'def %s(mut self, ...)' にしてください",
                   o->cur_fn ? o->cur_fn->name : "メソッド名")
        : diag_fmt("引数を '%s: mut %s' にしてください", br->origin->name,
                   br->origin->type ? type_name(br->origin->type) : "T");

    emit_ownck(o, &d, o->opt.deny_mut);
}

// 同じ場所を、可変借用と他の借用で同時に渡していないか（B1。仕様 §4.3）
static void report_alias(Own *o, ArgRef *m, ArgRef *other, Place *p) {
    if (o->quiet) return;

    Diag d = {0};
    d.code = "E-BORROW-5";
    d.message = other->is_mut
        ? diag_fmt("'%s' を 2 つの可変借用として同時に渡しています", p->disp)
        : diag_fmt("'%s' を可変借用と共有借用で同時に渡しています", p->disp);
    d.primary.tok = m->expr->tok;
    d.primary.label = "こちらは可変借用（書き換える側）です";
    d.related.tok = other->expr->tok;
    d.related.label = other->is_mut ? "こちらも可変借用です" : "こちらは共有借用です";
    d.hint = "同じ値を同時に貸せるのは「共有借用を何個でも」か"
             "「可変借用を 1 つだけ」のどちらかです（仕様 §4.3）";

    emit_ownck(o, &d, o->opt.deny_borrow);
}

// --explain-mut の 1 行（仕様 §5.3 の補償）。
//
// ★ 呼び出し側に何も書かせない代わりに、**聞けば答える**道具を用意します。
//   形式は「file:line:col: 説明」。エディタから飛べて、grep でも読めます。
static void explain_one(Own *o, ArgRef *a, const char *callee) {
    if (o->quiet || !o->opt.explain_mut) return;

    Place *p = place_of(a->expr);
    const char *what = p ? p->disp : "一時的な値";
    Token *t = a->expr->tok;

    if (a->param)
        printf("%s:%d:%d: '%s' が変更されます（%s の引数 '%s: mut %s'）\n", t->file,
               t->line, t->col, what, callee, a->param->name,
               a->param->type ? type_name(a->param->type) : "T");
    else if (a->kind == WR_APPEND)
        printf("%s:%d:%d: '%s' が変更されます（%s）\n", t->file, t->line, t->col, what,
               callee);
    else
        printf("%s:%d:%d: '%s' が変更されます（%s の 'mut self'）\n", t->file, t->line,
               t->col, what, callee);
}

// 呼び出しの見た目（--explain-mut と診断に出す名前）
static const char *callee_label(Node *n) {
    if (n->kind == ND_METHOD) {
        if (n->mod_name) return diag_fmt("%s.%s", n->mod_name, n->name);
        if (n->lhs && n->lhs->type && n->lhs->type->kind == TY_CLASS)
            return diag_fmt("%s.%s", type_name(n->lhs->type), n->name);
        return diag_fmt("list.%s", n->name);
    }
    return n->name ? n->name : "この呼び出し";
}

static void check_call_borrows(Own *o, Node *n, ArgRef *args) {
    const char *callee = NULL;

    // ① 可変で渡すものが、読み取り専用の借用でないか（B3）
    for (ArgRef *a = args; a; a = a->next) {
        if (!a->is_mut) continue;
        check_mut(o, a->expr, a->expr, a->kind);
        if (o->opt.explain_mut) {
            if (!callee) callee = callee_label(n);
            explain_one(o, a, callee);
        }
    }

    // ② 同じ場所を可変借用と他の借用で同時に渡していないか（B1）
    //
    // ★ 設計 ownership.md §5.1 に書いたとおり、これは二重ループだけです。
    //   借用の寿命が「呼び出しの間」に固定されているので、
    //   比べる範囲が 1 つの呼び出しの中に閉じています。
    for (ArgRef *a = args; a; a = a->next) {
        for (ArgRef *b = a->next; b; b = b->next) {
            if (!a->is_mut && !b->is_mut) continue;
            Place *pa = place_of(a->expr);
            Place *pb = place_of(b->expr);
            if (!pa || !pb || !place_overlaps(pa, pb)) continue;
            report_alias(o, a->is_mut ? a : b, a->is_mut ? b : a, pa);
        }
    }
}

static ArgRef *arg_ref(Node *expr, Node *param, bool is_mut, WriteKind kind) {
    ArgRef *a = xmalloc(sizeof(ArgRef));
    a->expr = expr;
    a->param = param;
    a->is_mut = is_mut;
    a->kind = kind;
    a->next = NULL;
    return a;
}

// p を「読む」。移動済みならその場で報告する。
static void check_use(Own *o, Flow *f, Place *p, Node *at) {
    Ent *who = NULL;
    if (state_of(f, p, &who) != ST_VALID && who) report_use(o, p, who, at);
}

// ── ⑥ 式をたどる ───────────────────────────────────────────

static void use_expr(Own *o, Flow *f, Node *n);
static bool move_expr(Own *o, Flow *f, Node *n, MoveCtx ctx);
static void stmt(Own *o, Flow *f, Node *n);

// 呼び出し先の定義ノード（ND_FUNC）。組み込み関数なら NULL。
static Node *callee_of(Own *o, Node *n) {
    if (n->builtin) return NULL;
    if (n->cls) {  // インスタンス生成 Token(1, "x") → init の引数と突き合わせる
        if (!n->cls->has_init || !n->cls->node) return NULL;
        for (Node *m = n->cls->node->body; m; m = m->next)
            if (m->kind == ND_FUNC && strcmp(m->name, "init") == 0) return m;
        return NULL;
    }
    return lookup_func(o, n->ir_name);
}

// 実引数を、仮引数の受け取り方（ParamMode）に従って評価する。
//
//   既定（借用）… 読むだけ。所有権は呼び出し側に残る
//   own         … 移動する
//   mut         … 借用（書き換えるが、所有権は移らない）
static void args_by_mode(Own *o, Flow *f, Node *args, Node *params) {
    Node *pm = params;
    for (Node *a = args; a; a = a->next) {
        if (pm && pm->mode == PM_OWN) move_expr(o, f, a, MV_OWN_ARG);
        else use_expr(o, f, a);
        if (pm) pm = pm->next;
    }
}


// ── 送出可能性の検査（A-18。設計文書 2.1 の表） ────────────
//
// ★ **新しい注釈は 1 つも足しません。** すでにある own / mut / 借用の規則に、
//   「スレッドの境界を越えられるか」という 1 つの問いを足すだけです。
//
//   own T      … ✅ 渡した側はもう触れない。競合しようがない
//   mutex[T]   … ✅ 触るにはロックが要る
//   借り        … ❌ 借りは「呼び出しより長生きしない」規則で守られている。
//                    スレッドは呼び出しより長生きしうる（E-SEND-1）
//   rc[T]      … ❌ 参照数の増減が競合する（E-SEND-2）
//   グローバル書き込み … ❌ 誰が触っているか静的に分からない（E-SEND-3）
//
// ⚠️ 既定でエラーです（警告ではありません）。所有権検査が既定で警告なのは
//    「既存コードがそのまま動く」ためでしたが、**spawn は新機能なので
//    既存コードがありません**。最初からエラーにできます（設計文書 2.3）。

// 関数の本体（と、そこから呼ぶ関数）がグローバルに書いていないか。
// 書いていれば、その代入のノードを返す。
//
// ⚠️ 呼び先までたどります。直下だけ見ると「1 枚かませば通る」検査になり、
//    保証として意味を持ちません。visited は再帰呼び出しで止まらないため。
typedef struct SeenFn SeenFn;
struct SeenFn {
    Node *fn;
    SeenFn *next;
};

static Node *scan_global_write(Own *o, Node *n, SeenFn **seen);

static Node *scan_global_write_list(Own *o, Node *n, SeenFn **seen) {
    for (; n; n = n->next) {
        Node *hit = scan_global_write(o, n, seen);
        if (hit) return hit;
    }
    return NULL;
}

static Node *scan_global_write(Own *o, Node *n, SeenFn **seen) {
    if (!n) return NULL;

    // ① グローバルへの代入そのもの
    if (n->kind == ND_ASSIGN && n->lhs) {
        Place *p = place_of(n->lhs);
        if (p) {
            Place *root = p;
            while (root->base) root = root->base;
            if (root->kind == PL_GLOBAL) return n;
        }
    }

    // ② 呼び先へ降りる（同じ関数は 1 回だけ）
    if ((n->kind == ND_CALL || n->kind == ND_METHOD) && n->ir_name) {
        Node *callee = lookup_func(o, n->ir_name);
        if (callee && callee->body) {
            bool done = false;
            for (SeenFn *q = *seen; q; q = q->next)
                if (q->fn == callee) { done = true; break; }
            if (!done) {
                SeenFn *q = xmalloc(sizeof(SeenFn));
                q->fn = callee;
                q->next = *seen;
                *seen = q;
                Node *hit = scan_global_write_list(o, callee->body->body, seen);
                if (hit) return hit;
            }
        }
    }

    // ③ 子をたどる
    Node *kids[] = {n->lhs, n->rhs, n->incr, n->els};
    for (unsigned i = 0; i < sizeof(kids) / sizeof(kids[0]); i++) {
        Node *hit = scan_global_write(o, kids[i], seen);
        if (hit) return hit;
    }
    Node *lists[] = {n->body, n->args};
    for (unsigned i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
        Node *hit = scan_global_write_list(o, lists[i], seen);
        if (hit) return hit;
    }
    return NULL;
}


// 型 t の中（たどれる範囲すべて）に rc[T] があるか。見つけたらその名前を返す。
//
// ⚠️ **これが無いと検査に穴が開きます。** `own Job` を渡すのは安全に見えても、
//    Job のフィールドに rc[Node] があれば、**その数え札は 2 つのスレッドから
//    増減されます**（渡した側が同じ節点への rc を別に持っていることがある）。
//    渡すものの型だけを見て「クラスだから安全」とは言えません。
typedef struct SeenCls SeenCls;
struct SeenCls {
    struct Class *cls;
    SeenCls *next;
};

static const char *rc_inside(Type *t, SeenCls **seen) {
    if (!t) return NULL;
    switch (t->kind) {
        case TY_RC:
            return type_name(t);
        case TY_LIST:
        case TY_OPT:
            return rc_inside(t->elem, seen);
        case TY_TUPLE: {
            for (int i = 0; i < t->nparams; i++) {
                const char *hit = rc_inside(t->params[i], seen);
                if (hit) return hit;
            }
            return NULL;
        }
        case TY_MUTEX:
            // ⚠️ mutex に入っていても同じです。ロックが守るのは**中身**で、
            //    数え札は箱の外（rc が指す先）にあるためです。
            return rc_inside(t->elem, seen);
        case TY_CLASS: {
            if (!t->cls) return NULL;
            // ★ 再帰的なクラス（Node が Node を持つ）で止まらないように。
            for (SeenCls *q = *seen; q; q = q->next)
                if (q->cls == t->cls) return NULL;
            SeenCls *q = xmalloc(sizeof(SeenCls));
            q->cls = t->cls;
            q->next = *seen;
            *seen = q;
            for (Field *f = t->cls->fields; f; f = f->next) {
                const char *hit = rc_inside(f->type, seen);
                if (hit) return hit;
            }
            return NULL;
        }
        default:
            return NULL;
    }
}

// spawn(f, a…) 1 か所ぶんの検査。
static void check_spawn(Own *o, Flow *f, Node *n) {
    Node *fexpr = n->args;
    if (!fexpr) return;  // sema がすでに止めているはず

    use_expr(o, f, fexpr);

    // 始める関数が分かれば、引数の受け取り方（own / 借り / mut）が分かります。
    Node *spawned = lookup_func(o, fexpr->ir_name);
    Node *pm = spawned ? spawned->params : NULL;

    for (Node *aexpr = fexpr->next; aexpr; aexpr = aexpr->next,
                                    pm = pm ? pm->next : NULL) {
        // ── E-SEND-2: rc[T] はスレッドをまたげない ──
        //
        // ★ 参照数はただの整数の増減で、競合すると数が壊れます
        //   （早すぎる解放＝解放後の使用）。原子的な増減にすれば直りますが、
        //   **すべての rc[T] が遅くなります**。コンパイラ自身が rc[T] だらけなので
        //   これは払えません（設計文書 2.2）。
        if (aexpr->type) {
            SeenCls *seen = NULL;
            const char *rcname = rc_inside(aexpr->type, &seen);
            if (rcname) {
                bool direct = aexpr->type->kind == TY_RC;
                Diag d = {0};
                d.code = "E-SEND-2";
                d.message =
                    direct ? diag_fmt("%s はスレッドに渡せません", rcname)
                           : diag_fmt("'%s' は中に %s を持つので、スレッドに渡せません",
                                      type_name(aexpr->type), rcname);
                d.primary.tok = aexpr->tok;
                d.primary.label = "参照数の増減が競合します（早すぎる解放になります）";
                d.hint = direct ? "mutex[…] に入れるか、own で所有ごと渡してください"
                                : "渡す値から rc[…] を外してください"
                                  "（必要なぶんを写して持たせる形にします）";
                emit_ownck(o, &d, true);
            }
        }

        Place *p = place_of(aexpr);
        bool is_mut = pm && pm->mode == PM_MUT;
        bool is_own = pm && pm->mode == PM_OWN;

        // ── E-SEND-4: 可変借用はスレッドに渡せない ──
        //
        // 🤔 なぜ scope: の中でも許さないのか
        //   scope: が保証するのは**寿命**だけです。同じ値への可変借用を
        //   2 本のスレッドが持てば、寿命が足りていてもデータ競合になります。
        //   そして「配る先が重なっていないか」は、ループの中で spawn する形
        //   （まさに並列化したい形）では静的に分かりません。
        //   ⚠️ 書き換えたいものは own で渡すか、mutex[…] に入れてください。
        if (is_mut) {
            Diag d = {0};
            d.code = "E-SEND-4";
            d.message = diag_fmt("可変借用 '%s' はスレッドに渡せません",
                                 p ? p->disp : "この値");
            d.primary.tok = aexpr->tok;
            d.primary.label = "2 本のスレッドが同時に書き換えうるので、"
                              "重なっていないことを静的に言えません";
            d.hint = "own で所有ごと渡す（結果は join で受け取る）か、"
                     "mutex[…] に入れてください";
            emit_ownck(o, &d, true);
        }

        // ── E-SEND-1: 借りは渡せない（scope: の中を除く）──
        //
        // 🤔 ここが設計の要点です。借用検査がすでに持っている
        //   「借りは呼び出しより長生きしない」という不変条件が、そのまま
        //   「借りはスレッドに渡せない」に翻訳されます。**規則を足すのではなく、
        //   既にある規則の帰結として出てきます。**
        //
        // ★ **scope: の中だけは渡せます。** ブロックの出口で必ず
        //   join されるので、その不変条件が**成り立ったまま**になるためです。
        //   規則を緩めたのではなく、規則の前提を実際に満たすようにした形です。
        if (p && o->scope_depth == 0) {
            BorrowRoot *br = borrow_root_of(o, p);
            if (br) {
                Diag d = {0};
                d.code = "E-SEND-1";
                d.message = diag_fmt("借りている値 '%s' はスレッドに渡せません",
                                     p->disp);
                d.primary.tok = aexpr->tok;
                d.primary.label = "スレッドは、この呼び出しより長生きしえます";
                d.hint = "scope: ブロックで囲む（出口で必ず join されます）か、"
                         "所有ごと渡す（引数を 'own' で受ける）か、"
                         "mutex[…] に入れてください";
                emit_ownck(o, &d, true);
            }
        }

        // ★ 渡し方は普通の呼び出しと同じです。own なら移動、借りなら借用。
        if (is_own || !pm) move_expr(o, f, aexpr, MV_OWN_ARG);
        else use_expr(o, f, aexpr);
    }

    // ── E-SEND-3: スレッドの中からグローバルを書き換えている ──
    if (spawned && spawned->body) {
        SeenFn *seen = xmalloc(sizeof(SeenFn));
        seen->fn = spawned;
        seen->next = NULL;
        Node *hit = scan_global_write_list(o, spawned->body->body, &seen);
        if (hit) {
            Diag d = {0};
            d.code = "E-SEND-3";
            d.message = diag_fmt("'%s' はグローバル変数を書き換えるので、"
                                 "スレッドで始められません", spawned->name);
            d.primary.tok = fexpr->tok;
            d.primary.label = "この関数をスレッドで始めようとしています";
            d.related.tok = hit->tok;
            d.related.label = "ここでグローバルに書いています";
            d.hint = "書き換える値を mutex[…] に入れて、引数で渡してください";
            emit_ownck(o, &d, true);
        }
    }
}

static void call_args(Own *o, Flow *f, Node *n, bool skip_self) {
    // ★ spawn(f, a) は「引数 1 つを別スレッドへ手放す」呼び出しです。
    //   ふつうの呼び出しとは渡し方が違うので、ここで分けます。
    if (n->kind == ND_CALL && n->type && n->type->kind == TY_THREAD && !n->ir_name) {
        check_spawn(o, f, n);
        return;
    }

    Node *fn = callee_of(o, n);
    if (!fn) {  // 組み込み関数・定義が引けないもの → すべて借用として扱う
        for (Node *a = n->args; a; a = a->next) use_expr(o, f, a);
        return;
    }

    Node *params = fn->params;
    Node *self_param = NULL;
    if (skip_self && params) {  // self は実引数に現れない
        self_param = params;
        params = params->next;
    }

    // ① 値の受け渡し（移動か借用か）
    args_by_mode(o, f, n->args, params);

    // ② 借用としての検査（B1 / B3）
    //
    // ★ obj.m(args) は m(obj, args) と同じ扱いです（設計 §5.2）。
    //   self を第 0 引数として並べれば、あとは同じ検査で済みます。
    ArgRef head = {0};
    ArgRef *tail = &head;
    if (self_param && n->kind == ND_METHOD && n->lhs)
        tail = tail->next =
            arg_ref(n->lhs, NULL, self_param->mode == PM_MUT, WR_METHOD);

    Node *pm = params;
    for (Node *a = n->args; a; a = a->next) {
        tail = tail->next = arg_ref(a, pm, pm && pm->mode == PM_MUT, WR_ARG);
        if (pm) pm = pm->next;
    }
    check_call_borrows(o, n, head.next);
}

// 値を「読む」文脈で式をたどる（借用）。
static void remember_decl(Own *o, Node *n);
static void bind_alias(Own *o, Node *target, Node *rhs);
static void use_expr(Own *o, Flow *f, Node *n) {
    if (!n) return;

    switch (n->kind) {
        case ND_VAR:
        case ND_FIELD: {
            Place *p = place_of(n);
            if (p) {
                check_use(o, f, p, n);
                return;  // 場所そのものなので、これ以上たどるものは無い
            }
            use_expr(o, f, n->lhs);
            return;
        }

        case ND_INDEX: {
            Place *p = place_of(n);
            if (p) check_use(o, f, p, n);
            else use_expr(o, f, n->lhs);
            use_expr(o, f, n->rhs);  // 添字の式（呼び出しかもしれない）
            return;
        }

        case ND_BINOP:
        case ND_LOGICAL:
            use_expr(o, f, n->lhs);
            use_expr(o, f, n->rhs);
            return;

        case ND_UNARY:
        case ND_PRINT:
            use_expr(o, f, n->lhs);
            return;

        // ★ 内包表記 [E for x in xs if C]
        //
        //   ⚠️ **ここを書かないと、警告が静かに消えます。** 同じことを
        //     手で書くと E-BORROW-3（借りたものをリストに保存）が出るのに、
        //     内包表記では出ない、という穴でした（`--drop` を付けると
        //     どちらも二重解放で落ちます）。
        //
        //   ループ変数は要素を**借りている**だけです（for 文と同じ）。
        //   要素の式は `append` と同じ扱い（MV_APPEND）で見ます。
        case ND_LISTCOMP: {
            if (n->args)
                for (Node *a = n->args; a; a = a->next) use_expr(o, f, a);
            else
                use_expr(o, f, n->rhs);

            Node *lv = n->body;      // ループ変数の宣言
            lv->binds_borrow = true; // 要素は借りもの（list が持ち主のまま）
            remember_decl(o, lv);

            // ★ 「対象の要素を借りている」ことを登録します。
            //   脱糖した for が `x = for.it.N[for.ix.N]` を作るのと同じ関係を、
            //   添字の節点を 1 つ組み立てて伝えます（それが無いと
            //   「誰から借りたか」が分からず、警告が出ません）。
            if (n->rhs) {
                Node elem_ref = {0};
                elem_ref.kind = ND_INDEX;
                elem_ref.tok = lv->tok;
                elem_ref.lhs = n->rhs;
                elem_ref.rhs = n->rhs;  // 添字の中身は見ないので何でもよい
                bind_alias(o, lv, &elem_ref);
            }

            if (n->els) use_expr(o, f, n->els);
            move_expr(o, f, n->lhs, MV_APPEND);
            return;
        }

        case ND_LIST:
            // ⚠️ リテラルの要素は本来「リストへの移動」です。当初は
            //    移動として扱いません（設計 ownership.md §4 の 5 か所に限る）。
            //    積み残した宿題です。
            for (Node *e = n->body; e; e = e->next) use_expr(o, f, e);
            return;

        case ND_CALL:
            call_args(o, f, n, n->cls != NULL);
            return;

        case ND_METHOD:
            // xs.append(v) … コンテナが v の所有権を受け取る（仕様 v2 §3.1）
            if (n->lhs && n->lhs->type && n->lhs->type->kind == TY_LIST &&
                strcmp(n->name, "append") == 0) {
                use_expr(o, f, n->lhs);
                move_expr(o, f, n->args, MV_APPEND);

                // ★ append はリストを書き換えます。
                //   xs.append(xs) のような自己参照も、ここで B1 に引っかかります。
                ArgRef *recv = arg_ref(n->lhs, NULL, true, WR_APPEND);
                if (n->args) recv->next = arg_ref(n->args, NULL, false, WR_ARG);
                check_call_borrows(o, n, recv);
                return;
            }
            // ⚠️ 'mod.f(args)'（他モジュールの関数・クラス）は ND_METHOD ですが
            //    self を取りません。第 1 引数をずらすかどうかは
            //    「モジュール修飾か」「インスタンス生成か」で決まります。
            //      obj.m(args)     → m(obj, args)。self を飛ばす
            //      mod.f(args)     → f(args)。飛ばさない
            //      mod.C(args)     → C.init(new, args)。self を飛ばす
            use_expr(o, f, n->lhs);
            call_args(o, f, n, n->mod_name ? n->cls != NULL : true);
            return;

        default: return;  // リテラル・None・型注釈など
    }
}

// 値を「移動しうる」文脈で式をたどる（代入の右辺・own 引数・return など）。
//
// 戻り値：**実際に移動したか**。false なら「借りているだけ」で、
// 束縛先はその値を所有しません（drop 挿入がこれを見ます）。
static bool move_expr(Own *o, Flow *f, Node *n, MoveCtx ctx) {
    if (!n) return false;

    // ── rc[T] は共有型。代入しても元は無効になりません ──
    //
    // ★ 「所有者を 1 つに決められない」ためにある型なので、移動として扱いません。
    //   束縛した側は**新しい参照**を持ちます（カウント +1）。
    //
    // ⚠️ **`rc[T] | None` も共有です。** ここを `TY_RC` だけで見ていたので、
    //   `lhs: rc[Node] | None` のような**いちばん普通の形**が移動と見なされ、
    //   移行しても指摘が減りませんでした（実測 345 → 340）。
    if (ty_is_rc(n->type)) {
        use_expr(o, f, n);
        return true;
    }

    Place *p = place_of(n);
    if (!p || !ty_is_owned(n->type)) {  // 一時値、またはコピー型 → ただの読み
        use_expr(o, f, n);
        if (p || !ty_is_owned(n->type)) return false;

        // ★ 一時値（呼び出しの戻り値やリテラル）は、束縛した側が所有します。
        //   ただし「self の借用を返す関数」（仕様 §4.5）の戻り値は借りもの。
        if (n->kind == ND_CALL || n->kind == ND_METHOD) {
            Node *fn = callee_of(o, n);
            if (fn && fn->binds_borrow) {
                n->binds_borrow = true;  // codegen はこれを見て解放しない
                return false;
            }
        }
        return true;
    }

    // ── 借りものは、呼び出しより長生きする場所へ渡せない（仕様 §4.4）──
    //
    // ★ 局所変数への束縛（`t: str = s`）は**許します**。
    //   局所変数は呼び出しより長生きしないので、別名を作っても危険がありません。
    //   その代わり、束縛された側も「借用」として扱います（bind_alias）。
    //
    // ★ もう 1 つの例外：**self の借用を返すこと**（仕様 §4.5）。
    //   メソッドが自分の一部を貸すのは、実質的に self を貸すのと同じだからです。
    BorrowRoot *br = borrow_root_of(o, p);
    if (br) {
        if (ctx != MV_ASSIGN && !(ctx == MV_RETURN && br->is_self))
            report_borrow(o, br, p, n, ctx);
        // ⚠️ 移動として記録しません。借りものは動いていないので、
        //    この後で使っても E-MOVE-1 にはなりません（1 つの問題は 1 回だけ報告する）。
        use_expr(o, f, n);
        return false;
    }

    // ⚠️ 要素を 1 つだけ move out することは許しません（設計 ownership.md §3）。
    //    添字はコンパイル時に分からないので、xs[0] と xs[1] を区別できません。
    //    for のループ変数もここを通ります（仕様 v2 §3.1「for の要素は借用」）。
    //    所有権ごと取り出す xs.pop() は次章で入れます。
    if (p->kind == PL_INDEX) {
        if (ctx == MV_FIELD || ctx == MV_APPEND || ctx == MV_OWN_ARG)
            report_store_borrow(o, p, n, ctx);
        else if (ctx == MV_RETURN) report_return_borrow(o, p, n);
        use_expr(o, f, n);
        return false;
    }

    // ⚠️ グローバルはプログラムが終わるまで生きているので、
    //    読み出しは「借りているだけ」として扱います（解放もしません）。
    if (p->kind == PL_GLOBAL) {
        if (ctx == MV_FIELD || ctx == MV_APPEND || ctx == MV_OWN_ARG)
            report_store_borrow(o, p, n, ctx);
        use_expr(o, f, n);
        return false;
    }

    // ── フィールドの読み出しは「借用」──
    //
    // ⚠️ 当初は「取り出し禁止（E-MOVE-2）」にしていました。撤回した理由は
    //   次のとおりです：この規則では **コンパイラ自身が書けません**
    //   （`nx = cur.next` で連結リストをたどることすらできない）。
    //
    // ★ 借用として扱えば、解放も安全です（借りものは解放しないため）。
    //   代わりに「貸し手より長生きしないか」を検査します（E-BORROW-6）。
    if (p->kind == PL_FIELD) {
        if (ctx == MV_FIELD || ctx == MV_APPEND || ctx == MV_OWN_ARG)
            report_store_borrow(o, p, n, ctx);
        else if (ctx == MV_RETURN) report_return_borrow(o, p, n);
        use_expr(o, f, n);
        return false;
    }

    check_use(o, f, p, n);  // 移動済みのものを再び移動するのもエラー
    flow_move(f, p, n->tok);

    // ★ codegen はこの印を見て drop フラグを 0 にします。
    if (n->kind == ND_VAR) n->moved_out = true;
    return true;
}

// 借用に根ざした値を局所変数に束縛したら、その変数も「借用」にする。
//
// ★ これが無いと、`t: str = s` で名前を変えるだけで検査をすり抜けます。
//   `for x in xs:` の脱糖（`for.it.0 = xs` → `x = for.it.0[i]`）もここを通るので、
//   借用したリストの要素を保存しようとすると、ちゃんと止まります。
static DeclEnt *find_decl(Own *o, const char *ir_name) {
    if (!ir_name) return NULL;
    for (DeclEnt *d = o->decls; d; d = d->next)
        if (strcmp(d->key, ir_name) == 0) return d;
    return NULL;
}

static void bind_alias(Own *o, Node *target, Node *rhs) {
    if (!target->ir_name) return;

    // ⚠️ **rc[T] は借りものではありません。**
    //   `m = mm.next`（`rc[Module] | None`）のような**共有の付け替え**を
    //   「mm の一部を借りた」と記録していたので、内側で作った rc を外側の
    //   変数に入れると E-BORROW-6 が出ていました。rc は独立した参照です。
    if (rhs && ty_is_rc(rhs->type)) {
        BorrowRoot **cut = &o->roots;
        while (*cut) {
            if (strcmp((*cut)->key, target->ir_name) == 0) *cut = (*cut)->next;
            else cut = &(*cut)->next;
        }
        return;
    }

    // 右辺が「借りもの」なら、その根を引き継ぐ
    BorrowRoot proto = {0};
    bool found = false;

    if (rhs) {
        Place *rp = place_of(rhs);
        if (rp) {
            BorrowRoot *src = borrow_root_of(o, rp);
            if (src) {
                proto = *src;
                found = true;
            } else if (rp->kind == PL_FIELD || rp->kind == PL_INDEX) {
                // ★ 自分が所有しているオブジェクトの一部を読んだ場合も
                //   「借用」です（そのオブジェクトが所有者のまま）。
                Place *root = rp;
                while (root->base) root = root->base;
                DeclEnt *d = root->kind == PL_LOCAL ? find_decl(o, root->key) : NULL;
                if (d) {
                    proto.origin = d->decl;
                    proto.is_param = false;
                    proto.is_self = false;
                    proto.is_mut = true;  // 自分のものなので書き換えてよい
                    proto.depth = d->depth;
                    found = true;
                }
            }
        }
    }

    // 既に登録されていれば付け替える（同じ関数の中で ir_name は一意）
    BorrowRoot **link = &o->roots;
    while (*link) {
        if (strcmp((*link)->key, target->ir_name) == 0) *link = (*link)->next;
        else link = &(*link)->next;
    }
    if (!found) return;  // 借用でない値を入れ直したら、もう借りものではない

    // ── 借りものが貸し手より長生きしないか（E-BORROW-6）──
    //
    // ★ 借用の寿命を「呼び出しの間」に固定した（仕様 §4.4）のと同じ考えを、
    //   ローカル変数どうしにも当てはめます。内側のスコープで作った値を、
    //   外側の変数に貸したままにはできません。
    DeclEnt *tgt = find_decl(o, target->ir_name);
    if (!o->quiet && tgt && !proto.is_param && tgt->depth < proto.depth) {
        Diag d = {0};
        d.code = "E-BORROW-6";
        d.message = diag_fmt("借りたもの（'%s' の一部）は、'%s' より長く持てません",
                             proto.origin->name, proto.origin->name);
        d.primary.tok = target->tok;
        d.primary.label = "こちらのほうが長生きします";
        d.related.tok = proto.origin->tok;
        d.related.label = diag_fmt("'%s' はこのスコープが終わると消えます",
                                   proto.origin->name);
        d.hint = "内側で作った値は、内側で使い切ってください"
                 "（外へ渡すなら所有権ごと渡します）";
        emit_ownck(o, &d, o->opt.deny_borrow);
    }

    BorrowRoot *b = xmalloc(sizeof(BorrowRoot));
    *b = proto;
    b->key = target->ir_name;
    b->next = o->roots;
    o->roots = b;
}

// 代入先を評価して、その場所をふたたび有効にする（仕様 v2 §3.2）。
static void assign_to(Own *o, Flow *f, Node *target) {
    if (target->kind != ND_VAR) {
        // self.f = v / xs[i] = v … 「入れ物」を読む必要がある
        use_expr(o, f, target->lhs);
        if (target->kind == ND_INDEX) use_expr(o, f, target->rhs);

        // ★ 入れ物が読み取り専用の借用なら書き換えられません（B3）。
        //   局所変数への代入（ND_VAR）は借用を作らないので、対象外です（仕様 §5.1）。
        check_mut(o, target, target, WR_ASSIGN);
    }
    Place *p = place_of(target);
    if (p) flow_clear(f, p);
}

// ── ⑦ 文をたどる ───────────────────────────────────────────

// コンパイラが作った隠し変数か（for / 複合代入の脱糖。parser.c）。
//
// ★ for.it.0 = xs や aug.obj.0 = t は、対象を 1 回だけ評価するための別名です。
//   利用者が書いた代入ではないので、**移動ではなく借用**として扱います
//   （仕様 v2 §3.1「for の要素は移動しない」）。
//   名前に '.' が入るのは脱糖で作った変数だけなので、これで見分けられます。
static bool is_hidden_var(const char *name) {
    return name && strchr(name, '.') != NULL;
}

// ★ 隠し変数のうち、`swap.N` だけは**所有します**。
//
//   ⚠️ ほかの隠し変数（`for.it.N` / `aug.obj.N` / `aug.idx.N`）は
//     「対象を 1 回だけ評価するための借り」です。ところが `swap.N` は
//     **右辺の値そのものを預かる場所**なので、借りにすると壊れます。
//
//     a, b = b, a  で swap.N を借りにすると:
//       swap.0 = b            ← b は借りたまま（b は自分のものと思っている）
//       a = swap.0            ← a の古い値を解放する。だが swap.1 がそれを指している
//       b = swap.1            ← **解放済みの領域**を b に入れてしまう
//
//     所有にすると、右辺を読んだ時点で b / a が null 化されるので、
//     解放が 1 回だけになります（`--drop` 付きで実際に壊れて分かりました）。
static bool is_owning_hidden(const char *name) {
    return name && strncmp(name, "swap.", 5) == 0;
}

// 「この変数は借りものを束縛している」と記録する。
static void mark_borrow_bind(Own *o, const char *ir_name) {
    if (!ir_name) return;
    for (DeclEnt *d = o->decls; d; d = d->next)
        if (strcmp(d->key, ir_name) == 0) {
            d->decl->binds_borrow = true;
            return;
        }
}

static void remember_decl(Own *o, Node *n) {
    if (!n->ir_name) return;
    for (DeclEnt *d = o->decls; d; d = d->next)
        if (strcmp(d->key, n->ir_name) == 0) return;  // while の 2 周目
    DeclEnt *d = xmalloc(sizeof(DeclEnt));
    d->key = n->ir_name;
    d->decl = n;
    d->depth = o->depth;
    d->next = o->decls;
    o->decls = d;
}

static void stmt_list(Own *o, Flow *f, Node *first) {
    for (Node *n = first; n; n = n->next) {
        if (f->dead) return;  // return の後ろは実行されない
        stmt(o, f, n);
    }
}

// while を 1 周ぶん解析する。
//   back … 本体を通って入口へ戻るときの状態（逆辺）
//   exit … ループから抜けるときの状態（条件が偽 / break）
static void while_once(Own *o, Node *n, const Flow *entry, Flow *back, Flow *exit) {
    Flow cur = flow_copy(entry);
    use_expr(o, &cur, n->lhs);   // 条件は毎周評価される
    *exit = flow_copy(&cur);     // 条件が偽なら、ここで抜ける

    Loop lp = {{NULL, true}, {NULL, true}, o->loop};
    o->loop = &lp;
    stmt(o, &cur, n->body);
    flow_join(&cur, &lp.cont);            // continue は本体末尾と同じ場所へ合流
    if (n->incr) stmt(o, &cur, n->incr);  // for の増分
    o->loop = lp.outer;

    flow_join(exit, &lp.brk);
    *back = cur;
}

// while の解析。**CFG は作りません**（設計 ownership.md §4.2）。
static void check_while(Own *o, Flow *f, Node *n) {
    // ── ① 入口の状態を不動点まで下げる（診断は出さない）──
    //
    // 🤔 なぜ 2 周で収束するのか
    //   格子の高さが 2（Valid → MaybeMoved → Moved）で、状態は単調にしか
    //   下がらないためです。3 周目で変化することはありません。
    //   崩れたらコンパイラのバグなので、assert して落とします。
    Flow entry = flow_copy(f);
    Flow back, exit;
    int round = 0;

    o->quiet++;
    for (;; round++) {
        while_once(o, n, &entry, &back, &exit);
        Flow next = flow_copy(&entry);
        flow_join(&next, &back);
        if (flow_eq(&next, &entry)) break;
        entry = next;
        if (round >= 3)
            internal_error(__FILE__, __LINE__,
                           "while の所有権解析が収束しません（格子が壊れています）");
    }
    o->quiet--;

    // ── ② 収束した入口で、もう一度だけ解析して診断を出す ──
    //
    // ★ 反復のたびに報告すると、同じ警告が何度も出てしまいます。
    while_once(o, n, &entry, &back, &exit);
    *f = exit;
}

static void stmt(Own *o, Flow *f, Node *n) {
    if (!n || f->dead) return;

    switch (n->kind) {
        case ND_VARDECL: {
            // ★ 右辺を所有したかどうかで、この変数を解放するかが決まります。
            //   脱糖が作った隠し変数（for.it.0 など）は必ず借りものです。
            bool owns = false;
            if (is_hidden_var(n->name) && !is_owning_hidden(n->name))
                use_expr(o, f, n->rhs);
            else
                owns = move_expr(o, f, n->rhs, MV_ASSIGN);
            if (!owns && ty_is_owned(n->type)) n->binds_borrow = true;
            remember_decl(o, n);
            bind_alias(o, n, n->rhs);
            if (n->ir_name) {
                Place *p = new_place(n->ir_name[0] == '@' ? PL_GLOBAL : PL_LOCAL,
                                     NULL, n->ir_name, n->name);
                flow_clear(f, p);
            }
            return;
        }

        case ND_ASSIGN:
            {
                bool owns = false;
                if (n->lhs->kind == ND_VAR && is_hidden_var(n->lhs->name))
                    use_expr(o, f, n->rhs);
                else
                    // ★ グローバルはプログラムが終わるまで残るので、
                    //   フィールドへの保存と同じ扱いにします（B2）。
                    owns = move_expr(o, f, n->rhs,
                                     (n->lhs->kind == ND_FIELD ||
                                      (n->lhs->ir_name && n->lhs->ir_name[0] == '@'))
                                         ? MV_FIELD
                                         : MV_ASSIGN);
                // 借りものを入れ直した変数は、もう自分のものではない
                if (n->lhs->kind == ND_VAR && !owns && ty_is_owned(n->rhs->type))
                    mark_borrow_bind(o, n->lhs->ir_name);
            }
            if (n->lhs->kind == ND_VAR) bind_alias(o, n->lhs, n->rhs);
            assign_to(o, f, n->lhs);
            return;

        case ND_BLOCK:
            // ★ 借用の寿命を見るために、スコープの深さを数えます。
            o->depth++;
            stmt_list(o, f, n->body);
            o->depth--;
            return;

        // ★ scope: ブロック（A-18 の scoped spawn）。
        //   この中では spawn に借りを渡せます（出口で必ず join されるため）。
        case ND_SCOPE:
            o->scope_depth++;
            stmt(o, f, n->body);
            o->scope_depth--;
            return;

        case ND_IF: {
            use_expr(o, f, n->lhs);
            Flow then_f = flow_copy(f);
            stmt(o, &then_f, n->body);

            Flow else_f = flow_copy(f);
            if (n->els) stmt(o, &else_f, n->els);

            *f = then_f;
            flow_join(f, &else_f);
            return;
        }

        case ND_WHILE:
            check_while(o, f, n);
            return;

        // ── try / except ──
        //
        // ★ try の本体は「途中で抜けることがある」ので、except の入口は
        //   **try に入る前の状態**から始めます（保守的）。
        //   本当は「どこまで進んだか」で変わりますが、分からないものは
        //   安全側に倒すのがデータフロー解析の原則です。
        case ND_TRY: {
            Flow body = flow_copy(f);
            stmt(o, &body, n->body);

            Flow result = body;
            for (Node *ex = n->els; ex; ex = ex->next) {
                Flow ef = flow_copy(f);
                stmt(o, &ef, ex->body);
                flow_join(&result, &ef);
            }
            *f = result;
            return;
        }

        case ND_RAISE:
            // ★ エラーオブジェクトは呼び出し元へ渡る＝ return と同じ移動
            move_expr(o, f, n->lhs, MV_RETURN);
            f->dead = true;
            return;

        case ND_BREAK:
            flow_join(&o->loop->brk, f);
            f->dead = true;
            return;

        case ND_CONTINUE:
            flow_join(&o->loop->cont, f);
            f->dead = true;
            return;

        case ND_RETURN:
            // ★ return は移動です。戻り値の所有権は呼び出し側に渡ります。
            move_expr(o, f, n->lhs, MV_RETURN);
            f->dead = true;
            return;

        case ND_PASS:
        case ND_IMPORT:
        case ND_FIELDDECL:
            return;

        default:
            use_expr(o, f, n);  // 式文（呼び出し）
            return;
    }
}

// ── ⑧ 入口 ─────────────────────────────────────────────────

// 代入ノードに、その変数の「借りものか」の印を写す。
//
// ⚠️ codegen は代入のときに **古い値を解放**します。借りものを束縛している
//    変数では、それをやると他人の値を解放してしまいます。
//    印が付くのは解析の途中（後の行の代入かもしれない）なので、
//    **解析が終わってから**まとめて写します。
static void propagate_borrow_binds(Own *o, Node *n) {
    if (!n) return;
    if (n->kind == ND_ASSIGN && n->lhs->kind == ND_VAR && n->lhs->ir_name) {
        for (DeclEnt *d = o->decls; d; d = d->next)
            if (strcmp(d->key, n->lhs->ir_name) == 0) {
                if (d->decl->binds_borrow) n->binds_borrow = true;
                break;
            }
    }
    propagate_borrow_binds(o, n->lhs);
    propagate_borrow_binds(o, n->rhs);
    propagate_borrow_binds(o, n->els);
    propagate_borrow_binds(o, n->incr);
    for (Node *st = n->body; st; st = st->next) propagate_borrow_binds(o, st);
}

static void check_func(Own *o, Node *fn) {
    if (!fn->body) return;  // extern 宣言には本体が無い

    // ── この関数が「借りている」ものを並べる ──
    //
    // ★ own の引数は借りものではありません（所有権を受け取っている）。
    //   コピー型（int / bool）はそもそも移動しないので、入れても意味がありません。
    o->roots = NULL;
    o->decls = NULL;
    o->depth = 0;
    for (Node *pm = fn->params; pm; pm = pm->next) remember_decl(o, pm);
    for (Node *pm = fn->params; pm; pm = pm->next) {
        if (pm->mode == PM_OWN) continue;
        if (!ty_is_owned(pm->type)) continue;
        if (!pm->ir_name) continue;
        BorrowRoot *b = xmalloc(sizeof(BorrowRoot));
        b->key = pm->ir_name;
        b->origin = pm;
        b->is_param = true;
        b->depth = 0;
        // self は「型注釈の無い第 1 引数」（parser がそう作る）
        b->is_self = pm == fn->params && pm->type_ref == NULL;
        // ★ init だけは self を可変として扱います。
        //   生成中のオブジェクトは、まだ誰にも貸していない「自分のもの」だからです。
        //   仕様 §4.2 の例（def init(self, name: own str)）もそう書いています。
        b->is_mut = pm->mode == PM_MUT ||
                    (b->is_self && strcmp(fn->name, "init") == 0);
        // ★ 借りている引数は所有していないので、解放しません。
        pm->binds_borrow = true;
        b->next = o->roots;
        o->roots = b;
    }

    // 引数は、借用でも own でも、関数に入った時点では必ず有効です。
    Flow f = {NULL, false};
    o->cur_fn = fn;
    stmt_list(o, &f, fn->body->body);
    propagate_borrow_binds(o, fn->body);
    o->cur_fn = NULL;
    o->roots = NULL;
    o->decls = NULL;
}

// この式は「引数に根ざした場所」か（self を含む）。
static bool rooted_in_param(Node *fn, Node *n) {
    while (n && (n->kind == ND_FIELD || n->kind == ND_INDEX)) n = n->lhs;
    if (!n || n->kind != ND_VAR) return false;
    for (Node *pm = fn->params; pm; pm = pm->next)
        if (pm->ir_name && n->ir_name && strcmp(pm->ir_name, n->ir_name) == 0)
            return true;
    return false;
}

// 関数の中に「借用を返す return」があるか。
//
// ★ 仕様 §4.5 は self のフィールドを返すことを許しています。
//   その戻り値は **借りもの**なので、呼び出し側が解放してはいけません。
//   誰が所有者かは呼び出し側からは見えないので、定義を見て先に印を付けます。
static bool returns_borrow(Node *fn, Node *n) {
    if (!n) return false;
    if (n->kind == ND_RETURN) return rooted_in_param(fn, n->lhs);
    if (returns_borrow(fn, n->body)) return true;
    if (returns_borrow(fn, n->els)) return true;
    if (n->kind != ND_FUNC && returns_borrow(fn, n->next)) return true;
    return false;
}

static void mark_returns_borrow(Node *fn) {
    if (!fn->body) return;
    // ★ ND_FUNC の binds_borrow は「戻り値が借りもの」という意味で使います。
    fn->binds_borrow = returns_borrow(fn, fn->body->body);
}

static void collect_funcs(Own *o, Node *ast) {
    for (Node *d = ast->body; d; d = d->next) {
        if (d->kind == ND_FUNC && d->ir_name) {
            mark_returns_borrow(d);
            FuncEnt *e = xmalloc(sizeof(FuncEnt));
            e->ir_name = d->ir_name;
            e->fn = d;
            e->next = o->funcs;
            o->funcs = e;
        }
        if (d->kind == ND_CLASS) {
            for (Node *m = d->body; m; m = m->next) {
                if (m->kind != ND_FUNC || !m->ir_name) continue;
                mark_returns_borrow(m);
                FuncEnt *e = xmalloc(sizeof(FuncEnt));
                e->ir_name = m->ir_name;
                e->fn = m;
                e->next = o->funcs;
                o->funcs = e;
            }
        }
    }
}


// この式の木のどこかに spawn があるか。
// ⚠️ next は**ループで**たどります。再帰にすると、木の深さではなく
//    ノードの総数ぶんスタックを積むことになり、大きいファイルで落ちます。
static bool has_spawn(Node *n) {
    for (; n; n = n->next) {
        if (n->kind == ND_CALL && n->type && n->type->kind == TY_THREAD &&
            !n->ir_name)
            return true;
        Node *kids[] = {n->lhs, n->rhs, n->incr, n->els, n->body, n->args};
        for (unsigned i = 0; i < sizeof(kids) / sizeof(kids[0]); i++)
            if (has_spawn(kids[i])) return true;
    }
    return false;
}

void ownck_program(Module *mods, const OwnckOptions *opt) {
    Own o = {0};
    o.opt = *opt;

    // ★ 表は先に全モジュールぶん作ります。呼び出しの向きは依存順とは
    //   限らない（同じモジュール内の相互再帰）ためです。
    for (Module *m = mods; m; m = m->next) collect_funcs(&o, m->ast);

    // ── spawn を使ったら、所有権の検査を**エラーに上げる** ──
    //
    // 🤔 なぜここだけ扱いを変えるのか
    //   所有権検査が既定で警告なのは「既存のコードがそのまま動く」ためです
    //   （§0 ③ の折衷）。ところが **警告のままだと、データ競合が無いという
    //   保証も警告どまり**になります。spawn は新機能で既存コードがないので、
    //   「spawn を使ったファイルでは昇格する」という形なら誰も壊れません
    //   （設計文書 §5 の宿題）。
    for (Module *m = mods; m; m = m->next) {
        if (!has_spawn(m->ast)) continue;
        o.opt.deny_move = true;
        o.opt.deny_borrow = true;
        o.opt.deny_mut = true;
        break;
    }

    for (Module *m = mods; m; m = m->next) {
        for (Node *d = m->ast->body; d; d = d->next) {
            if (d->kind == ND_FUNC) check_func(&o, d);
            if (d->kind == ND_CLASS)
                for (Node *mm = d->body; mm; mm = mm->next)
                    if (mm->kind == ND_FUNC) check_func(&o, mm);
        }
    }

    // ⚠️ 上限を超えたぶんは件数だけ知らせます。selfhost/ を
    //    書き換えるまで、ここは何百件も出うるためです。
    if (o.nmore > 0)
        fprintf(stderr,
                "warning: 所有権の指摘が他に %d 件あります"
                "（表示したのは先頭 %d 件です）\n",
                o.nmore, OWNCK_MAX_REPORT);
}
