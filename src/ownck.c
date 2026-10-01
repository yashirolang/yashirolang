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
// 注意: 呼び出しの戻り値やリテラルは場所ではありません（NULL を返します）。
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
            // 注意: str の添字は「場所」ではありません。`s[i]` は 1 文字の
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
        // ★ 中身を持つ列挙（A-41）はヒープの物体です。名前だけの列挙は
        //   ただの i64 なので、後始末は要りません。
        case TY_ENUM: return t->en && t->en->has_payload;
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
    // ★ 「移動」ではなく「借りていた先が書き換えられた」ことで使えなくなった
    //   別名なら、書き換えた場所を持ちます（E-BORROW-9。NULL なら普通の移動）。
    //   格子は移動と同じものを使います——分岐・ループでの合流の規則が
    //   まったく同じだからです。
    Place *by;
    Place *src;       // その別名が借りていた場所（by の中のどこか）
    bool by_move;     // by は書き換えではなく「手放した」（移動）
    bool by_rc;       // 同じ物体を指しうる別の rc 越しの書き換え（E-BORROW-10）
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
    e->by = NULL;
    e->src = NULL;
    e->by_move = false;
    e->by_rc = false;
    e->next = f->ents;
    f->ents = e;
}

// 別名 p が借りていた先（by）が at で書き換えられた、と記録する（E-BORROW-9）。
static void flow_stale(Flow *f, Place *p, Place *by, Place *src, bool by_move,
                       Token *at) {
    flow_move(f, p, at);
    f->ents->by = by;
    f->ents->src = src;
    f->ents->by_move = by_move;
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
        if (joined != e->st && s && s->at) {
            e->at = s->at;  // 位置は「片方の枝」を指せれば足りる
            e->by = s->by;
            e->src = s->src;
            e->by_move = s->by_move;
            e->by_rc = s->by_rc;
        }
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

// 局所変数が「どの場所を」借りているか（E-BORROW-9）。
//
// ★ BorrowRoot が答えるのは「誰の持ち物か」（根の宣言）だけです。
//   `q: P = b.p` のあとで `b.reset()` が b.p を解放すると q は宙に浮きますが、
//   それを言うには「q が指しているのは b.p だ」という**経路**が要ります。
//   ここに持つのはその経路で、別名の別名（`r = q.x`）は貸し手まで
//   たどり直して持ちます（r → b.p.x）。
// 借りている場所が「ほかから指されうる物体」の中を通っているとき、
// その物体のクラスと、そこから先の経路（E-BORROW-10）。
//
// ★ 名前の経路（Place）だけでは、同じ物体を指す**別の rc** が見えません。
//     a: rc[Box] = …;  b = a;  q = a.p;  b.p = P(7)   ← q が宙に浮く
//   そこで「a の指す物体（Box）の中の .p」という形でも覚えておき、
//   書き換えを**クラスとフィールド**で突き合わせます。
//
// ほかから指されうる物体は 3 つです。
//   ・rc[T] の中身
//   ・借りている引数（self を含む）の指すクラスと list——呼び出し側では
//     rc[T] の中身（か、その中）かもしれません
//   ・上の 2 つの中を通って届くクラスと list
typedef struct RcStep RcStep;
struct RcStep {
    Type *cls;          // フィールドを持つクラス
    const char *field;  // そのフィールド
    RcStep *next;
};

typedef struct RcHop RcHop;
struct RcHop {
    Type *t;        // 物体の型（クラスか list）
    Place *rest;    // 物体から先の経路（根は "<obj>"）
    RcStep *steps;  // 物体から先で通るフィールド（クラスごと）
    bool via_rc;    // rc[T] の中身として見つけたもの（根を置き換えても中身は残る）
    RcHop *next;
};

typedef struct Loan Loan;
struct Loan {
    const char *key;   // 別名の変数の IR 名（%q）
    const char *disp;  // 診断に出す名前（q）
    Place *src;        // 借りている場所（b.p / xs[…]）
    RcHop *hops;       // 借りている場所が通る「ほかから指されうる物体」（E-BORROW-10）
    Loan *next;
};

// ループ 1 つぶんの出口情報（break / continue が積む場所）
typedef struct Loop Loop;
struct Loop {
    Flow brk;   // break で抜けたときの状態の結合
    Flow cont;  // continue で戻るときの状態の結合
    Loop *outer;
};

// 1 回のコンパイルで出す警告の上限。
// ★ 効くのは --warn-own のときだけです（既定はエラーなので 1 件目で止まります）。
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
    Loan *loans;        // いま検査中の関数の別名と、その借り先（E-BORROW-9）
    struct DeclEnt *decls;  // いま検査中の関数の変数宣言
    int depth;          // 今いるスコープの深さ（借用の寿命の検査に使う）
    Node *cur_fn;       // いま検査中の関数（診断の「直し方」に名前が要る）
    int scope_depth;    // scope: の中にいる深さ（0 なら外）
} Own;

static DeclEnt *find_decl(Own *o, const char *ir_name);
static Place *resolve_place(Own *o, Place *p);
static void invalidate_loans(Own *o, Flow *f, Place *w, bool strict, bool by_move,
                             Node *at);
static RcHop *hops_of(Own *o, Node *n);
static void rc_invalidate(Own *o, Flow *f, Place *w, bool strict, bool by_move,
                          Node *at);

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
// 注意: **own 引数へ渡す形（MV_OWN_ARG）もここで見ます。** 以前は MV_FIELD と
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

static void check_call_borrows(Own *o, Flow *f, Node *n, ArgRef *args) {
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
    //
    // ★ 別名は貸し手の場所に直してから比べます（E-BORROW-9 と同じ表）。
    //   `q = b.p` のあとの `f(b, q)` は、b と b.p を同時に渡しているのと同じです。
    for (ArgRef *a = args; a; a = a->next) {
        for (ArgRef *b = a->next; b; b = b->next) {
            if (!a->is_mut && !b->is_mut) continue;
            Place *pa = place_of(a->expr);
            Place *pb = place_of(b->expr);
            if (!pa || !pb) continue;
            if (!place_overlaps(pa, pb) &&
                !place_overlaps(resolve_place(o, pa), resolve_place(o, pb)))
                continue;
            report_alias(o, a->is_mut ? a : b, a->is_mut ? b : a, pa);
        }
    }

    // ③ 可変で渡した場所の「中」を借りている別名は、呼び出しの後では使えない
    //   （E-BORROW-9）。呼び先が中身を入れ替えて、古い値を解放しうるためです。
    //
    // 注意: append / insert は対象外です。要素を足すだけで、いまある要素を
    //   解放しません（`q = xs[0]` のあとで `xs.append(v)` しても q は無事です）。
    for (ArgRef *a = args; a; a = a->next) {
        if (!a->is_mut || a->kind == WR_APPEND) continue;
        Place *p = place_of(a->expr);
        if (p) invalidate_loans(o, f, p, true, false, a->expr);
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

// 借りていた先が書き換えられた別名を使った（E-BORROW-9）。
//
// ★ なぜ危ないか
//     q: P = b.p      ← q は b.p を指しているだけ（借用。解放しない）
//     b.reset()       ← reset が古い b.p を解放する
//     print(q.v)      ← 解放済みの領域を読む
//   q を解放しないのは「持ち主（b）が生きている間しか使わない」からですが、
//   持ち主が中身を入れ替えると、その前提が崩れます。
static void report_stale(Own *o, Place *p, Ent *who, Node *at) {
    if (o->quiet) return;

    bool maybe = who->st == ST_MAYBE;
    Diag d = {0};
    d.code = "E-BORROW-9";
    // ★ for の隠し変数（for.it.N）なら、回している最中に対象を入れ替えた形です。
    //   隠し変数の名前は利用者に見せても意味が通らないので、言い換えます。
    if (strncmp(who->pl->disp, "for.", 4) == 0) {
        d.message = diag_fmt("for で回している '%s' を、回している途中で入れ替えています",
                             who->by->disp);
        d.primary.tok = at->tok;
        d.primary.label = "次の周で、入れ替える前の（解放済みの）リストを読みます";
        d.related.tok = who->at;
        d.related.label = diag_fmt("ここで '%s' を入れ替えています", who->by->disp);
        d.hint = "入れ替えた後のリストを回したいなら、ループを抜けてから回し直してください"
                 "（回しながら作るなら、別のリストに append します）";
        emit_ownck(o, &d, o->opt.deny_borrow);
        return;
    }
    // ★ 同じ物体を指しうる別の rc 越しの書き換え（E-BORROW-10）。
    //   名前の経路では別物に見えるので、「なぜそれで壊れるのか」を言います。
    if (who->by_rc) {
        d.code = "E-BORROW-10";
        d.message = diag_fmt("'%s' が借りている '%s' は、%s解放されているかもしれません",
                             who->pl->disp, who->src->disp,
                             maybe ? "分岐によっては" : "");
        d.primary.tok = at->tok;
        d.primary.label = "ここで使われています";
        d.related.tok = who->at;
        d.related.label =
            strcmp(who->by->key, "<call>") == 0
                ? diag_fmt("%sこの '%s' の中で、同じ物体を指しうる rc 越しに"
                           "書き換えられます",
                           maybe ? "分岐によっては、" : "", who->by->disp)
                : diag_fmt("%sここで '%s' を書き換えています（同じ物体を指しうる rc 越し）",
                           maybe ? "分岐によっては、" : "", who->by->disp);
        d.hint = diag_fmt("rc は同じ物体を何か所からでも指せるので、別の名前からの"
                          "書き換えでも '%s' の古い値は解放されえます。"
                          "書き換えた後で使うなら読み直し、書き換える前の値が要るなら"
                          " copy(...) で手元に写してください",
                          who->src->disp);
        emit_ownck(o, &d, o->opt.deny_borrow);
        return;
    }
    const char *verb = who->by_move ? "手放しました" : "書き換えました";
    d.message = diag_fmt("'%s' が借りている '%s' は、%s解放されているかもしれません",
                         who->pl->disp, who->src->disp,
                         maybe ? "分岐によっては" : "");
    d.primary.tok = at->tok;
    d.primary.label = "ここで使われています";
    d.related.tok = who->at;
    d.related.label = maybe
        ? diag_fmt("分岐によっては、ここで '%s' を%s", who->by->disp, verb)
        : diag_fmt("ここで '%s' を%s（古い値は解放されえます）", who->by->disp, verb);
    d.hint = who->by_move
        ? diag_fmt("'%s' を手放す前に使い終えるか、copy(...) で手元に写してください",
                   who->by->disp)
        : diag_fmt("書き換えた後で使うなら、読み直してください"
                   "（例: %s = %s）。書き換える前の値が要るなら copy(...) で"
                   "手元に写してください",
                   who->pl->disp, who->src->disp);
    emit_ownck(o, &d, o->opt.deny_borrow);
}

// p を「読む」。移動済みならその場で報告する。
static void check_use(Own *o, Flow *f, Place *p, Node *at) {
    Ent *who = NULL;
    if (state_of(f, p, &who) == ST_VALID || !who) return;
    if (who->by) report_stale(o, p, who, at);
    else report_use(o, p, who, at);
}

static Loan *find_loan(Own *o, const char *key) {
    for (Loan *l = o->loans; l; l = l->next)
        if (strcmp(l->key, key) == 0) return l;
    return NULL;
}

// p の根を root に差し替えた場所を作る（q.x で q → b.p なら b.p.x）。
static Place *splice_place(Place *p, Place *root) {
    if (!p->base) return root;
    Place *base = splice_place(p->base, root);
    return new_place(p->kind, base, p->key,
                     p->kind == PL_INDEX ? diag_fmt("%s[…]", base->disp)
                                         : diag_fmt("%s.%s", base->disp, p->key));
}

// 別名を通した場所を、貸し手の場所に直す。
//
// ★ `c = b.p` のあとの `c.reset()` は b.p を書き換えています。
//   別名のまま比べると、`r = b.p.x` が宙に浮くことを見落とします。
static Place *resolve_place(Own *o, Place *p) {
    if (!p) return NULL;
    Place *root = p;
    while (root->base) root = root->base;
    if (root->kind != PL_LOCAL) return p;
    Loan *l = find_loan(o, root->key);
    return l ? splice_place(p, l->src) : p;
}


// ── 同じ物体を指しうる別の rc 越しの書き換え（E-BORROW-10）─────────

// rc[T] / rc[T] | None なら T
static Type *rc_target(Type *t) {
    if (t && t->kind == TY_OPT) t = t->elem;
    return t && t->kind == TY_RC ? t->elem : NULL;
}

// フィールドを持つ物体のクラス（T / rc[T] / その | None）
static Type *obj_class(Type *t) {
    if (t && t->kind == TY_OPT) t = t->elem;
    if (t && t->kind == TY_RC) t = t->elem;
    return t && t->kind == TY_CLASS ? t : NULL;
}

// ほかから指されうる「入れ物」になる型（クラスか list。T | None は中身）。
// ★ rc[T] は含めません。rc は rc_target で中身として扱います。
static Type *container_of(Type *t) {
    if (t && t->kind == TY_OPT) t = t->elem;
    return t && (t->kind == TY_CLASS || t->kind == TY_LIST) ? t : NULL;
}

static RcHop *hop_new(Type *t, Place *rest, RcStep *steps, RcHop *next) {
    RcHop *h = xmalloc(sizeof(RcHop));
    h->t = t;
    h->rest = rest;
    h->steps = steps;
    h->via_rc = false;
    h->next = next;
    return h;
}

static RcStep *step_new(Type *cls, const char *field, RcStep *next) {
    RcStep *s = xmalloc(sizeof(RcStep));
    s->cls = cls;
    s->field = field;
    s->next = next;
    return s;
}

static Place *obj_root(void) { return new_place(PL_LOCAL, NULL, "<obj>", "<obj>"); }

// 式 n の指す場所が通る「ほかから指されうる物体」を並べる。
static RcHop *hops_of(Own *o, Node *n) {
    if (!n) return NULL;
    RcHop *hs = NULL;
    switch (n->kind) {
        case ND_VAR: {
            if (!n->ir_name || n->ir_name[0] == '@') break;
            // 別名なら、貸し手が通っていたものを引き継ぐ（r = q.x で q → a.p）
            Loan *l = find_loan(o, n->ir_name);
            if (l)
                for (RcHop *h = l->hops; h; h = h->next) {
                    hs = hop_new(h->t, h->rest, h->steps, hs);
                    hs->via_rc = h->via_rc;
                }
            // 借りている引数は、呼び出し側では rc[T] の中身（か、その中）かもしれない。
            // ★ init の self だけは除きます。作ったばかりで、まだ誰も指していません。
            Type *c = container_of(n->type);
            Place *v = c ? place_of(n) : NULL;
            BorrowRoot *b = v ? borrow_root_of(o, v) : NULL;
            if (b && b->is_param && !v->base &&
                !(b->is_self && o->cur_fn && strcmp(o->cur_fn->name, "init") == 0))
                hs = hop_new(c, obj_root(), NULL, hs);
            break;
        }
        case ND_FIELD:
        case ND_INDEX: {
            if (n->kind == ND_FIELD && n->mod_name) break;
            if (n->kind == ND_INDEX && n->lhs->type && n->lhs->type->kind == TY_STR)
                break;
            Type *oc = n->kind == ND_FIELD ? obj_class(n->lhs->type) : NULL;
            for (RcHop *h = hops_of(o, n->lhs); h; h = h->next) {
                Place *r = n->kind == ND_FIELD
                    ? new_place(PL_FIELD, h->rest, n->name,
                                diag_fmt("%s.%s", h->rest->disp, n->name))
                    : new_place(PL_INDEX, h->rest, "[]",
                                diag_fmt("%s[…]", h->rest->disp));
                RcStep *st = oc ? step_new(oc, n->name, h->steps) : h->steps;
                hs = hop_new(h->t, r, st, hs);
                hs->via_rc = h->via_rc;
            }
            // ほかから指されうる物体の中にあるクラス・list も、また指されえます
            // （借りている引数がこれを指しているかもしれません）。
            Type *c = container_of(n->type);
            if (hs && c) hs = hop_new(c, obj_root(), NULL, hs);
            break;
        }
        default: break;
    }
    Type *t = rc_target(n->type);
    if (t) {
        hs = hop_new(t, obj_root(), NULL, hs);
        hs->via_rc = true;
    }
    return hs;
}

// 別名 l を「rc 越しに書き換えられた」として無効にする。
static void rc_stale(Flow *f, Loan *l, Place *by, bool by_move, Token *at) {
    Place *alias = new_place(PL_LOCAL, NULL, l->key, l->disp);
    Ent *cur = flow_find(f, alias);
    if (cur && cur->st == ST_MOVED) return;  // 名前の経路でもう捕まえている
    flow_stale(f, alias, by, l->src, by_move, at);
    f->ents->by_rc = true;
}

// 場所 w（式 at）の書き換え。w が「ほかから指されうる物体」の中なら、
// 同じクラスの物体の同じ場所（かその中）を借りている別名を無効にする。
//
//   strict … invalidate_loans と同じ（w そのものを借りている別名は残す）
static void rc_invalidate(Own *o, Flow *f, Place *w, bool strict, bool by_move,
                          Node *at) {
    RcHop *ws = hops_of(o, at);
    if (!ws) return;
    for (Loan *l = o->loans; l; l = l->next) {
        bool hit = false;
        for (RcHop *lh = l->hops; lh && !hit; lh = lh->next)
            for (RcHop *wh = ws; wh && !hit; wh = wh->next) {
                // ★ 代入・移動で rc の場所そのもの（`self.cur = n`）を書くのは、
                //   参照を差し替えるだけで、指している物体の中身は変えません。
                //   中身を変えるのは mut で渡したとき（strict）だけです。
                //   注意: クラス・list の場所の置き換えは、古い物体を**解放します**。
                if (!strict && !wh->rest->base && wh->via_rc) continue;
                if (!type_equal(lh->t, wh->t)) continue;
                if (!place_prefix_of(wh->rest, lh->rest)) continue;
                if (strict && place_eq(wh->rest, lh->rest)) continue;
                hit = true;
            }
        if (hit) rc_stale(f, l, w, by_move, at->tok);
    }
}

static void forget_loan(Own *o, const char *key) {
    Loan **link = &o->loans;
    while (*link) {
        if (strcmp((*link)->key, key) == 0) *link = (*link)->next;
        else link = &(*link)->next;
    }
}

// target が rhs の場所を「借りて」束縛した、と記録する。
//
// 注意: 呼ぶのは**所有しなかった束縛**だけです（move_expr が false を返したもの・
//   for の隠し変数）。所有した値はもう誰からも借りていません。
static void record_loan(Own *o, Node *target, Node *rhs) {
    if (!target->ir_name) return;
    if (target->type && !ty_is_owned(target->type)) return;  // コピー型
    if (rhs && ty_is_rc(rhs->type)) return;                   // rc は独立した参照
    Place *rp = place_of(rhs);
    if (!rp) return;
    Loan *l = xmalloc(sizeof(Loan));
    l->key = target->ir_name;
    l->disp = target->name;
    l->src = resolve_place(o, rp);  // ★ 付け替える前の表で引く（q = q.next の形）
    l->hops = hops_of(o, rhs);      // ★ 同じく付け替える前に
    forget_loan(o, target->ir_name);
    l->next = o->loans;
    o->loans = l;
}

// 場所 w が書き換えられた。w（またはその中）を借りている別名を無効にする。
//
//   strict … w **そのもの**を借りている別名は残す。`q = b.p` のあとの
//            `b.p.bump()`（mut self）は b.p の中身を変えるだけで、
//            b.p そのものは解放しません。代入（`b.p = …`）や移動は
//            w そのものを手放すので、strict にしません。
static void invalidate_loans(Own *o, Flow *f, Place *w, bool strict, bool by_move,
                             Node *at) {
    w = resolve_place(o, w);
    if (!w) return;
    for (Loan *l = o->loans; l; l = l->next) {
        if (!place_prefix_of(w, l->src)) continue;
        if (strict && place_eq(w, l->src)) continue;
        Place *alias = new_place(PL_LOCAL, NULL, l->key, l->disp);
        flow_stale(f, alias, w, l->src, by_move, at->tok);
    }
    rc_invalidate(o, f, w, strict, by_move, at);
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

// 「この呼び出しの戻り値は借りものか」を呼び出しノードに書き写す。
//
// ★ なぜ要るか（A-21e）
//   codegen は「式の途中の一時値」を解放しますが、**戻り値が借りものの
//   呼び出し**は解放してはいけません（実体の持ち主は別にいる）。
//   `v.field("k").as_str()` のように `return self.text` を返すメソッドが
//   その例で、解放すると持ち主の中身が消えます。
//
// 注意: move_expr でも同じ印を付けていますが、あちらは**結果を束縛するとき**
//   しか通りません。二項演算のオペランドや引数の位置では通らないので、
//   ここで全部の呼び出しに付けます（collect_funcs が事前パスで
//   関数側の binds_borrow を立て終えているので、ここで引けます）。
static void mark_call_binds_borrow(Own *o, Node *n) {
    if (n->binds_borrow) return;
    Node *fn = callee_of(o, n);
    if (fn && fn->binds_borrow) n->binds_borrow = true;
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
//   own T      … 渡した側はもう触れない。競合しようがない
//   mutex[T]   … 触るにはロックが要る
//   借り        … 渡せない。借りは「呼び出しより長生きしない」規則で守られているが、
//                    スレッドは呼び出しより長生きしうる（E-SEND-1）
//   rc[T]      … 渡せない。参照数の増減が競合する（E-SEND-2）
//   グローバル書き込み … 渡せない。誰が触っているか静的に分からない（E-SEND-3）
//
// 注意: 既定でエラーです（警告ではありません）。所有権検査が既定で警告なのは
//    「既存コードがそのまま動く」ためでしたが、**spawn は新機能なので
//    既存コードがありません**。最初からエラーにできます（設計文書 2.3）。

// 関数の本体（と、そこから呼ぶ関数）がグローバルに書いていないか。
// 書いていれば、その代入のノードを返す。
//
// 注意: 呼び先までたどります。直下だけ見ると「1 枚かませば通る」検査になり、
//    保証として意味を持ちません。visited は再帰呼び出しで止まらないため。
typedef struct SeenFn SeenFn;
struct SeenFn {
    Node *fn;
    SeenFn *next;
};

//
// ★ 同じ走査で「スレッドを始めているか」も探します（E-EXPORT-4。設計 ffi.md §5.3）。
//   探すものが違うだけで、呼び先へ降りる理由も同じだからです。
typedef enum {
    HZ_GLOBAL_WRITE,  // グローバルへの代入（E-SEND-3 / E-EXPORT-3）
    HZ_THREAD,        // spawn / scope:（E-EXPORT-4）
} Hazard;

static Node *scan_global_write(Own *o, Node *n, SeenFn **seen, Hazard what);

static Node *scan_global_write_list(Own *o, Node *n, SeenFn **seen, Hazard what) {
    for (; n; n = n->next) {
        Node *hit = scan_global_write(o, n, seen, what);
        if (hit) return hit;
    }
    return NULL;
}

static Node *scan_global_write(Own *o, Node *n, SeenFn **seen, Hazard what) {
    if (!n) return NULL;

    // ① グローバルへの代入そのもの
    if (what == HZ_GLOBAL_WRITE && n->kind == ND_ASSIGN && n->lhs) {
        Place *p = place_of(n->lhs);
        if (p) {
            Place *root = p;
            while (root->base) root = root->base;
            if (root->kind == PL_GLOBAL) return n;
        }
    }
    // ①' スレッドを始めるところ（spawn の呼び出しと scope: ブロック）
    if (what == HZ_THREAD) {
        if (n->kind == ND_SCOPE) return n;
        if (n->kind == ND_CALL && n->type && n->type->kind == TY_THREAD && !n->ir_name)
            return n;
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
                Node *hit = scan_global_write_list(o, callee->body->body, seen, what);
                if (hit) return hit;
            }
        }
    }

    // ③ 子をたどる
    Node *kids[] = {n->lhs, n->rhs, n->incr, n->els};
    for (unsigned i = 0; i < sizeof(kids) / sizeof(kids[0]); i++) {
        Node *hit = scan_global_write(o, kids[i], seen, what);
        if (hit) return hit;
    }
    Node *lists[] = {n->body, n->args};
    for (unsigned i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
        Node *hit = scan_global_write_list(o, lists[i], seen, what);
        if (hit) return hit;
    }
    return NULL;
}


// 型 t の中（たどれる範囲すべて）に rc[T] があるか。見つけたらその名前を返す。
//
// 注意: **これが無いと検査に穴が開きます。** `own Job` を渡すのは安全に見えても、
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
            // 注意: mutex に入っていても同じです。ロックが守るのは**中身**で、
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
        // なぜ scope: の中でも許さないのか
        //   scope: が保証するのは**寿命**だけです。同じ値への可変借用を
        //   2 本のスレッドが持てば、寿命が足りていてもデータ競合になります。
        //   そして「配る先が重なっていないか」は、ループの中で spawn する形
        //   （まさに並列化したい形）では静的に分かりません。
        //   注意: 書き換えたいものは own で渡すか、mutex[…] に入れてください。
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
        // ここが設計の要点です。借用検査がすでに持っている
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
        Node *hit = scan_global_write_list(o, spawned->body->body, &seen,
                                           HZ_GLOBAL_WRITE);
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


// ── 呼び出しが書き換えうるフィールド（E-BORROW-10）──────────────
//
// ★ rc の中身は、`mut` で受けていない関数からも書き換えられます
//   （`c: rc[Box] = b` と写せば、c は独立した参照なので書けます）。
//   だから「mut で渡したか」ではなく「呼び先が（呼び先までたどって）
//   どのクラスのどのフィールドを書くか」で決めます。E-EXPORT-3 と同じ走査です。
//
// 注意: 関数の値・インタフェース越しの呼び出しの先は見ません
//   （E-SEND-3 / E-EXPORT-3 と同じ限界。名前でたどれないため）。
typedef struct FnEff FnEff;
struct FnEff {
    Node *fn;
    RcStep *direct;   // 本体が直に書くフィールド
    Node **callees;   // 本体から呼ぶ関数
    Node **calls;     // その呼び出しのノード（引数の型を見るため。callees と同じ並び）
    int ncallees, cap;
    RcStep *all;      // 呼び先までたどった全体（求めたら埋まる）
    RcHop *globals;   // 本体が触るグローバルの型（t だけ使う）
    RcHop *gall;      // 呼び先までたどった全体
    bool done;
    FnEff *next;
};

static FnEff *fn_effs;

// 並びを倍に広げる（xmalloc で取り直して写す）
static void *grow(void *old, size_t used, size_t size) {
    void *p = xmalloc(size);
    if (old) memcpy(p, old, used);
    return p;
}

static bool step_has(RcStep *s, Type *cls, const char *field) {
    for (; s; s = s->next)
        if (strcmp(s->field, field) == 0 && type_equal(s->cls, cls)) return true;
    return false;
}

// 書き込み先の式から「どのクラスのどのフィールドか」を取り出す（xs[i] は xs）。
static void eff_add_place(FnEff *e, Node *lhs, bool in_init) {
    while (lhs && lhs->kind == ND_INDEX) lhs = lhs->lhs;
    if (!lhs || lhs->kind != ND_FIELD || lhs->mod_name || !lhs->lhs) return;
    Type *c = obj_class(lhs->lhs->type);
    if (!c) return;
    // ★ init の中の self.f = … は、作ったばかりの物体への書き込みです。
    if (in_init && lhs->lhs->kind == ND_VAR && lhs->lhs->name &&
        strcmp(lhs->lhs->name, "self") == 0)
        return;
    if (!step_has(e->direct, c, lhs->name)) e->direct = step_new(c, lhs->name, e->direct);
}

static void eff_add_callee(FnEff *e, Node *fn, Node *call) {
    if (!fn || !fn->body) return;
    if (e->ncallees == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 8;
        e->callees = grow(e->callees, sizeof(Node *) * e->ncallees,
                          sizeof(Node *) * e->cap);
        e->calls = grow(e->calls, sizeof(Node *) * e->ncallees, sizeof(Node *) * e->cap);
    }
    e->callees[e->ncallees] = fn;
    e->calls[e->ncallees++] = call;
}

static void eff_scan(Own *o, FnEff *e, Node *n, bool in_init);

static void eff_scan_list(Own *o, FnEff *e, Node *n, bool in_init) {
    for (; n; n = n->next) eff_scan(o, e, n, in_init);
}

static void eff_add_global(FnEff *e, Type *t) {
    if (!t) return;
    for (RcHop *h = e->globals; h; h = h->next)
        if (type_equal(h->t, t)) return;
    e->globals = hop_new(t, NULL, NULL, e->globals);
}

static void eff_scan(Own *o, FnEff *e, Node *n, bool in_init) {
    if (!n) return;
    // グローバル（と他モジュールのグローバル）を触るなら、その型から届く物体も書けます
    if ((n->kind == ND_VAR || (n->kind == ND_FIELD && n->mod_name)) && n->ir_name &&
        n->ir_name[0] == '@')
        eff_add_global(e, n->type);
    if (n->kind == ND_ASSIGN && n->lhs) eff_add_place(e, n->lhs, in_init);
    // list の組み込みメソッドのうち要素を手放すもの（E-BORROW-9 と同じ 3 つ）
    if (n->kind == ND_METHOD && n->lhs && n->lhs->type &&
        n->lhs->type->kind == TY_LIST &&
        (strcmp(n->name, "pop") == 0 || strcmp(n->name, "remove") == 0 ||
         strcmp(n->name, "clear") == 0))
        eff_add_place(e, n->lhs, in_init);
    if (n->kind == ND_CALL || n->kind == ND_METHOD) eff_add_callee(e, callee_of(o, n), n);

    Node *kids[] = {n->lhs, n->rhs, n->incr, n->els};
    for (unsigned i = 0; i < sizeof(kids) / sizeof(kids[0]); i++)
        eff_scan(o, e, kids[i], in_init);
    eff_scan_list(o, e, n->body, in_init);
    eff_scan_list(o, e, n->args, in_init);
}

static FnEff *eff_of(Own *o, Node *fn) {
    for (FnEff *e = fn_effs; e; e = e->next)
        if (e->fn == fn) return e;
    FnEff *e = xmalloc(sizeof(FnEff));
    memset(e, 0, sizeof(FnEff));
    e->fn = fn;
    e->next = fn_effs;
    fn_effs = e;
    bool in_init = fn->name && strcmp(fn->name, "init") == 0;
    if (fn->body) eff_scan_list(o, e, fn->body->body, in_init);
    return e;
}

// 型 t の値から（フィールド・要素・中身をたどって）クラス cls の物体に届くか。
//
// ★ 呼び先が書けるのは、**手の届く物体**だけです。引数にもグローバルにも
//   Token が出てこない関数は、呼び出し側の借りている Token を書けません
//   （その中で作った Token に書くのは、借りとは関係がありません）。
// 注意: インタフェース・中身を持つ列挙・生ポインタは中が型から決まらないので、
//   届くものとして扱います。
typedef struct TySeen TySeen;
struct TySeen {
    Type *t;
    TySeen *next;
};

static bool ty_reaches(Type *t, Type *cls, TySeen **seen) {
    if (!t) return false;
    switch (t->kind) {
        case TY_CLASS: {
            if (type_equal(t, cls)) return true;
            for (TySeen *q = *seen; q; q = q->next)
                if (q->t == t) return false;
            TySeen *q = xmalloc(sizeof(TySeen));
            q->t = t;
            q->next = *seen;
            *seen = q;
            if (!t->cls) return false;
            for (Field *fl = t->cls->fields; fl; fl = fl->next)
                if (ty_reaches(fl->type, cls, seen)) return true;
            return false;
        }
        case TY_OPT:
        case TY_LIST:
        case TY_RC:
        case TY_MUTEX:
        case TY_THREAD: return ty_reaches(t->elem, cls, seen);
        case TY_TUPLE:
            for (int i = 0; i < t->nparams; i++)
                if (ty_reaches(t->params[i], cls, seen)) return true;
            return false;
        case TY_IFACE:
        case TY_PTR: return true;
        case TY_ENUM: return t->en && t->en->has_payload;
        default: return false;
    }
}

// 呼び出し n の引数（受け手を含む）か、呼び先が触るグローバルから cls に届くか。
static bool call_reaches(Node *n, RcHop *globals, Type *cls) {
    TySeen *seen = NULL;
    if (n->kind == ND_METHOD && n->lhs && ty_reaches(n->lhs->type, cls, &seen))
        return true;
    for (Node *a = n->args; a; a = a->next)
        if (ty_reaches(a->type, cls, &seen)) return true;
    for (RcHop *g = globals; g; g = g->next)
        if (ty_reaches(g->t, cls, &seen)) return true;
    return false;
}

// fn が呼び先までたどって書きうるフィールドの全体。
//
// ★ 呼び先の書き込みは、**その呼び出しの引数（とグローバル）から届く
//   クラスのものだけ**を引き上げます。tokenize(str, str) の中で作った
//   Token に書いても、呼び出し側の Token には届きません。
//   段ごとに絞るので、再帰があっても合うように不動点まで回します。
static bool eff_merge(Own *o, FnEff *dst, FnEff *src, Node *call) {
    bool changed = false;
    for (RcStep *s = src->all; s; s = s->next) {
        if (step_has(dst->all, s->cls, s->field)) continue;
        if (!call_reaches(call, src->gall, s->cls)) continue;
        dst->all = step_new(s->cls, s->field, dst->all);
        changed = true;
    }
    for (RcHop *g = src->gall; g; g = g->next) {
        bool have = false;
        for (RcHop *h = dst->gall; h; h = h->next)
            if (type_equal(h->t, g->t)) { have = true; break; }
        if (have) continue;
        dst->gall = hop_new(g->t, NULL, NULL, dst->gall);
        changed = true;
    }
    (void)o;
    return changed;
}

static RcStep *eff_all(Own *o, Node *fn) {
    FnEff *root = eff_of(o, fn);
    if (root->done) return root->all;
    // 呼び出しの木を集める（同じ関数は 1 回だけ。まだ求めていないものだけ）
    int n = 0, cap = 16;
    FnEff **q = xmalloc(sizeof(FnEff *) * cap);
    q[n++] = root;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < q[i]->ncallees; k++) {
            FnEff *c = eff_of(o, q[i]->callees[k]);
            if (c->done) continue;
            bool seen = false;
            for (int j = 0; j < n; j++)
                if (q[j] == c) { seen = true; break; }
            if (seen) continue;
            if (n == cap) {
                q = grow(q, sizeof(FnEff *) * n, sizeof(FnEff *) * cap * 2);
                cap *= 2;
            }
            q[n++] = c;
        }
    }
    for (int i = 0; i < n; i++) {
        for (RcStep *s = q[i]->direct; s; s = s->next)
            q[i]->all = step_new(s->cls, s->field, q[i]->all);
        for (RcHop *g = q[i]->globals; g; g = g->next)
            q[i]->gall = hop_new(g->t, NULL, NULL, q[i]->gall);
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < n; i++)
            for (int k = 0; k < q[i]->ncallees; k++)
                if (eff_merge(o, q[i], eff_of(o, q[i]->callees[k]), q[i]->calls[k]))
                    changed = true;
    }
    for (int i = 0; i < n; i++) q[i]->done = true;
    return root->all;
}

// 呼び出し n（呼び先 fn）が、借りている別名の通り道を書き換えうるなら無効にする。
static void rc_invalidate_call(Own *o, Flow *f, Node *n, Node *fn) {
    if (!o->loans || !fn) return;
    RcStep *eff = NULL;
    RcHop *gl = NULL;
    bool got = false;
    for (Loan *l = o->loans; l; l = l->next) {
        const char *hitf = NULL;
        for (RcHop *h = l->hops; h && !hitf; h = h->next) {
            if (!h->steps) continue;
            if (!got) {
                eff = eff_all(o, fn);
                gl = eff_of(o, fn)->gall;
                got = true;
            }
            for (RcStep *s = h->steps; s && !hitf; s = s->next)
                if (step_has(eff, s->cls, s->field) && call_reaches(n, gl, s->cls))
                    hitf = s->field;
        }
        if (!hitf) continue;
        Place *by = new_place(PL_LOCAL, NULL, "<call>",
                              diag_fmt("%s(…)", fn->name ? fn->name : "?"));
        rc_stale(f, l, by, false, n->tok);
    }
}

// 呼び先が名前で決まらない呼び出し（インタフェース越し・関数の値）。
//
// ★ E-SEND-3 / E-EXPORT-3 はここを見ませんが、この検査は**見ます**。
//   中身が分からないので「引数（受け手を含む）か、プログラムのどこかで触る
//   グローバルから届く物体なら、どのフィールドでも書かれうる」とします。
static RcHop *all_globals;
static bool all_globals_done;

static RcHop *program_globals(Own *o) {
    if (all_globals_done) return all_globals;
    all_globals_done = true;
    for (FuncEnt *fe = o->funcs; fe; fe = fe->next) {
        FnEff *e = eff_of(o, fe->fn);
        for (RcHop *g = e->globals; g; g = g->next) {
            bool have = false;
            for (RcHop *h = all_globals; h; h = h->next)
                if (type_equal(h->t, g->t)) { have = true; break; }
            if (!have) all_globals = hop_new(g->t, NULL, NULL, all_globals);
        }
    }
    return all_globals;
}

static void rc_invalidate_unknown(Own *o, Flow *f, Node *n) {
    if (!o->loans) return;
    RcHop *gl = NULL;
    bool got = false;
    for (Loan *l = o->loans; l; l = l->next) {
        bool hit = false;
        for (RcHop *h = l->hops; h && !hit; h = h->next)
            for (RcStep *s = h->steps; s && !hit; s = s->next) {
                if (!got) {
                    gl = program_globals(o);
                    got = true;
                }
                if (call_reaches(n, gl, s->cls)) hit = true;
            }
        if (!hit) continue;
        Place *by = new_place(PL_LOCAL, NULL, "<call>",
                              diag_fmt("%s(…)", n->name ? n->name : "…"));
        rc_stale(f, l, by, false, n->tok);
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
        if (n->is_indirect || n->is_iface_call) rc_invalidate_unknown(o, f, n);

        // ★ list の組み込みメソッドのうち、要素を**手放す**もの（E-BORROW-9）。
        //   pop / remove は取り出した要素を呼び出し側へ渡し（使わなければ
        //   その場で解放されます）、clear は要素をまとめて捨てます。
        //   どれも `q = xs[0]` の q を宙に浮かせます。
        if (n->kind == ND_METHOD && n->lhs && n->lhs->type &&
            n->lhs->type->kind == TY_LIST &&
            (strcmp(n->name, "pop") == 0 || strcmp(n->name, "remove") == 0 ||
             strcmp(n->name, "clear") == 0)) {
            Place *p = place_of(n->lhs);
            if (p) invalidate_loans(o, f, p, true, false, n->lhs);
        }
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
    check_call_borrows(o, f, n, head.next);
    rc_invalidate_call(o, f, n, fn);
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
        //   注意: **ここを書かないと、警告が静かに消えます。** 同じことを
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
            // ★ リテラルの要素は **リストへの移動**です（A-26 で直しました）。
            //
            // 注意: ここを「ただの読み」にしていたのが穴でした。`xs.append(v)` は
            //    止まるのに `[v]` は素通りし、**既定で解放するようになった
            //    0.16.0 以降は、借りものを入れると早すぎる解放になります**
            //    （内包表記は A-12 のときに MV_APPEND へ直してありました。
            //      リテラルだけが残っていました）。
            for (Node *e = n->body; e; e = e->next) move_expr(o, f, e, MV_APPEND);
            return;

        case ND_CALL:
            mark_call_binds_borrow(o, n);
            call_args(o, f, n, n->cls != NULL);
            return;

        case ND_METHOD:
            mark_call_binds_borrow(o, n);
            // xs.append(v) … コンテナが v の所有権を受け取る（仕様 v2 §3.1）
            if (n->lhs && n->lhs->type && n->lhs->type->kind == TY_LIST &&
                strcmp(n->name, "append") == 0) {
                use_expr(o, f, n->lhs);
                move_expr(o, f, n->args, MV_APPEND);

                // ★ append はリストを書き換えます。
                //   xs.append(xs) のような自己参照も、ここで B1 に引っかかります。
                ArgRef *recv = arg_ref(n->lhs, NULL, true, WR_APPEND);
                if (n->args) recv->next = arg_ref(n->args, NULL, false, WR_ARG);
                check_call_borrows(o, f, n, recv);
                return;
            }
            // xs.insert(i, v) … append と同じく、コンテナが v の所有権を受け取る。
            //   ★ ここが無かったので v が関数の出口で解放され、リストには
            //     解放済みの値が残っていました。
            if (n->lhs && n->lhs->type && n->lhs->type->kind == TY_LIST &&
                strcmp(n->name, "insert") == 0 && n->args && n->args->next) {
                use_expr(o, f, n->lhs);
                use_expr(o, f, n->args);
                move_expr(o, f, n->args->next, MV_APPEND);
                ArgRef *recv = arg_ref(n->lhs, NULL, true, WR_APPEND);
                recv->next = arg_ref(n->args->next, NULL, false, WR_ARG);
                check_call_borrows(o, f, n, recv);
                return;
            }
            // 注意: 'mod.f(args)'（他モジュールの関数・クラス）は ND_METHOD ですが
            //    self を取りません。第 1 引数をずらすかどうかは
            //    「モジュール修飾か」「インスタンス生成か」で決まります。
            //      obj.m(args)     → m(obj, args)。self を飛ばす
            //      mod.f(args)     → f(args)。飛ばさない
            //      mod.C(args)     → C.init(new, args)。self を飛ばす
            use_expr(o, f, n->lhs);
            call_args(o, f, n, n->mod_name ? n->cls != NULL : true);
            return;

        // ★ 範囲型の検査（A-28）は値を素通しするだけの包みです。
        //   注意: **ここを書かないと、包んだ中の移動が記録されません**
        //     （p: Percent = f(xs) の xs が「渡していない」ことになります）。
        case ND_RANGECHK:
            use_expr(o, f, n->lhs);
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
    // 注意: **`rc[T] | None` も共有です。** ここを `TY_RC` だけで見ていたので、
    //   `lhs: rc[Node] | None` のような**いちばん普通の形**が移動と見なされ、
    //   移行しても指摘が減りませんでした（実測 345 → 340）。
    if (ty_is_rc(n->type)) {
        use_expr(o, f, n);
        return true;
    }

    // ★ タプルのリテラル `(a, b)` は、要素ごとに移動です。
    //   タプルそのものは一時値なので、ここで要素を 1 つずつ動かさないと、
    //   `return (n, s)` の s が関数の出口で解放され、呼び出し側には
    //   解放済みの文字列が渡っていました。
    if (n->kind == ND_TUPLE) {
        for (Node *el = n->body; el; el = el->next) move_expr(o, f, el, ctx);
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
        // 注意: 移動として記録しません。借りものは動いていないので、
        //    この後で使っても E-MOVE-1 にはなりません（1 つの問題は 1 回だけ報告する）。
        use_expr(o, f, n);
        return false;
    }

    // 注意: 要素を 1 つだけ move out することは許しません（設計 ownership.md §3）。
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

    // 注意: グローバルはプログラムが終わるまで生きているので、
    //    読み出しは「借りているだけ」として扱います（解放もしません）。
    if (p->kind == PL_GLOBAL) {
        if (ctx == MV_FIELD || ctx == MV_APPEND || ctx == MV_OWN_ARG)
            report_store_borrow(o, p, n, ctx);
        use_expr(o, f, n);
        return false;
    }

    // ── フィールドの読み出しは「借用」──
    //
    // 注意: 当初は「取り出し禁止（E-MOVE-2）」にしていました。撤回した理由は
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
    // ★ 手放した値の中を借りている別名は、もう使えません（E-BORROW-9）。
    //   `q = b.p` のあとで `eat(b)` すると、b.p は eat の出口で解放されます。
    invalidate_loans(o, f, p, false, true, n);

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

    // 注意: **コピー型は借りものになりません。**
    //   `worst = e.st`（st は int）のように、借りた入れ物から**値をコピー**
    //   しているだけの代入は借用ではありません。ここを見ていなかったので、
    //   内側のスコープの rc から int を読んで外側の変数に入れると
    //   E-BORROW-6 が出ていました（11 行で再現。移植中に見つけました）。
    //   ★ 借用として追うのは、解放の対象になりうる型だけで足ります。
    if (target->type && !ty_is_owned(target->type)) return;

    // 注意: **rc[T] は借りものではありません。**
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
//   注意: ほかの隠し変数（`for.it.N` / `aug.obj.N` / `aug.idx.N`）は
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
    // なぜ 2 周で収束するのか
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
            if (owns) forget_loan(o, n->ir_name ? n->ir_name : "");
            else record_loan(o, n, n->rhs);
            if (n->ir_name) {
                Place *p = new_place(n->ir_name[0] == '@' ? PL_GLOBAL : PL_LOCAL,
                                     NULL, n->ir_name, n->name);
                flow_clear(f, p);
            }
            return;
        }

        case ND_ASSIGN:
            {
                // ── 書き換える場所の中を借りている別名を無効にする（E-BORROW-9）──
                //
                // ★ 右辺より先に「何を手放すか」を決めておきます
                //   （右辺の評価で別名の表が変わる前に）。
                //   変数への代入で古い値を解放するのは、その変数が**自分で
                //   所有しているとき**だけです。借りものを束縛した変数や
                //   借用引数への代入は、何も解放しません。
                Place *w = NULL;
                if (n->lhs->kind != ND_VAR) {
                    w = place_of(n->lhs);
                } else if (n->lhs->ir_name && !is_hidden_var(n->lhs->name) &&
                           !find_loan(o, n->lhs->ir_name)) {
                    Place *v = place_of(n->lhs);
                    if (v && !borrow_root_of(o, v)) w = v;
                }

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
                if (w) invalidate_loans(o, f, w, false, false, n->lhs);
                if (n->lhs->kind == ND_VAR && n->lhs->ir_name) {
                    if (owns) forget_loan(o, n->lhs->ir_name);
                    else record_loan(o, n->lhs, n->rhs);
                }
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

        // ★ 契約（A-29）の式も「読み」としてたどります。
        //   注意: たどらないと、式の中の呼び出しに渡した値の扱いが記録されません。
        case ND_REQUIRES:
        case ND_ENSURES:
            use_expr(o, f, n->lhs);
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

        // ── 場合分け（A-37）──
        //
        // ★ **if の合流と同じ扱いです。** どの case も「入る前の状態」から
        //   始まり、出たところで全部を合流させます。
        //
        //   注意: **調べる式は 1 回だけ使います**（codegen も 1 回しか
        //     評価しません）。case の数だけ数えると、`match xs.pop():` の
        //     ような形で「何度も移動した」と誤って言うことになります。
        case ND_MATCH: {
            use_expr(o, f, n->lhs);

            Flow result = flow_copy(f);
            bool first = true;
            for (Node *c = n->body; c; c = c->next) {
                Flow cf = flow_copy(f);
                // 注意: case の値（Color.Red / 48 / "ja"）は**定数**なので、
                //   use_expr に通す必要がありません（sema がそう縛っています）。
                stmt(o, &cf, c->body);
                if (first) { result = cf; first = false; }
                else flow_join(&result, &cf);
            }
            *f = result;
            return;
        }

        // ★ 列挙の宣言は実行時に何もしません（枝はただの定数。A-37）
        case ND_ENUM:
            return;

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
// 注意: codegen は代入のときに **古い値を解放**します。借りものを束縛している
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
    o->loans = NULL;
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
    o->loans = NULL;
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

// 「借用を返す関数の戻り値を、そのまま返している」か。
//
// ★ なぜ要るか
//   `def header(self) -> str: return self.headers.get_or(k, "")` のように、
//   **借用を返す関数の戻り値をそのまま返す**関数があります。この関数の
//   戻り値も借りものですが、rooted_in_param は「self.x」の形しか見ないので
//   印が立ちません。すると呼び出し側が一時値として解放し、
//   **dict の中の文字列が消えます**（ASan が heap-use-after-free と言う）。
//
// 注意: next は**ループで**たどります（has_spawn と同じ理由）。
static bool returns_borrowed_call(Own *o, Node *n) {
    for (; n; n = n->next) {
        if (n->kind == ND_RETURN && n->lhs &&
            (n->lhs->kind == ND_CALL || n->lhs->kind == ND_METHOD)) {
            Node *f = callee_of(o, n->lhs);
            if (f && f->binds_borrow) return true;
        }
        if (returns_borrowed_call(o, n->body)) return true;
        if (returns_borrowed_call(o, n->els)) return true;
        if (n->kind == ND_FUNC) break;
    }
    return false;
}

// 「戻り値が借りもの」の印を、呼び出しの連なりに沿って広げる。
//
// ★ **増えなくなるまで回します。** 呼び出しの向きは定義の順とも
//   モジュールの依存順とも限らない（相互再帰もある）ためです。
//   関数の数は高々数千なので、素直な反復で足ります。
static void propagate_binds_borrow(Own *o) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (FuncEnt *e = o->funcs; e; e = e->next) {
            if (e->fn->binds_borrow || !e->fn->body) continue;
            if (returns_borrowed_call(o, e->fn->body->body)) {
                e->fn->binds_borrow = true;
                changed = true;
            }
        }
    }
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


// 外へ出す関数（本体つきの extern def）の検査（設計 ffi.md §5.3）。
//
// ★ 外へ出す関数は「どのスレッドから呼ばれるか分からない関数」で、spawn で
//   始める関数と同じ立場です（ctypes は呼び出しのあいだ GIL を手放します）。
//   だから E-SEND-3 と同じ走査で、グローバルに書かないことを確かめます。
//   同じ性質が、panic から境界へ戻ったあとに壊れた状態が残らないことも守ります。
static void check_export(Own *o, Node *fn) {
    if (!fn->body) return;
    Hazard kinds[] = {HZ_GLOBAL_WRITE, HZ_THREAD};
    for (unsigned k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        SeenFn *seen = xmalloc(sizeof(SeenFn));
        seen->fn = fn;
        seen->next = NULL;
        Node *hit = scan_global_write_list(o, fn->body->body, &seen, kinds[k]);
        if (!hit) continue;
        Diag d = {0};
        d.primary.tok = fn->tok;
        d.primary.label = "外から、どのスレッドからでも呼ばれる関数です";
        d.related.tok = hit->tok;
        if (kinds[k] == HZ_GLOBAL_WRITE) {
            d.code = "E-EXPORT-3";
            d.message = diag_fmt("外へ出す関数 '%s' はグローバル変数を書き換えます",
                                 fn->name);
            d.related.label = "ここでグローバルに書いています";
            d.hint = "状態は引数と戻り値で受け渡してください"
                     "（同時に呼ばれると競合し、panic したときに書きかけで残ります）";
        } else {
            d.code = "E-EXPORT-4";
            d.message = diag_fmt("外へ出す関数 '%s' からスレッドを始めています", fn->name);
            d.related.label = "ここで spawn（または scope:）を使っています";
            d.hint = "いまは外へ出す関数からスレッドを使えません"
                     "（panic したときに、動いているスレッドを安全に止められないためです）";
        }
        emit_ownck(o, &d, true);
    }
}

// この式の木のどこかに spawn があるか。
// 注意: next は**ループで**たどります。再帰にすると、木の深さではなく
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
    // ★ 表が揃ってから、「戻り値が借りもの」の印を呼び出しに沿って広げます。
    propagate_binds_borrow(&o);

    // ── spawn を使ったら、--warn-own でも警告に落とさない ──
    //
    // なぜここだけ扱いを変えるのか
    //   A-24 で既定はエラーになりました。ここが効くのは
    //   **--warn-own を付けたとき**だけです。所有権の指摘を警告に落とすと、
    //   「データ競合が無い」という保証も一緒に警告どまりになります
    //   （E-SEND-* は「借りか・所有か・rc か」の判定に乗っているため）。
    //   逃げ道は逃げ道として要りますが、**並行実行のところだけは通しません**。
    for (Module *m = mods; m; m = m->next) {
        if (!has_spawn(m->ast)) continue;
        o.opt.deny_move = true;
        o.opt.deny_borrow = true;
        o.opt.deny_mut = true;
        break;
    }

    for (Module *m = mods; m; m = m->next) {
        for (Node *d = m->ast->body; d; d = d->next) {
            if (d->kind == ND_FUNC && d->is_export) check_export(&o, d);
            if (d->kind == ND_FUNC) check_func(&o, d);
            if (d->kind == ND_CLASS)
                for (Node *mm = d->body; mm; mm = mm->next)
                    if (mm->kind == ND_FUNC) check_func(&o, mm);
        }
    }

    // 注意: 上限を超えたぶんは件数だけ知らせます。selfhost/ を
    //    書き換えるまで、ここは何百件も出うるためです。
    if (o.nmore > 0)
        fprintf(stderr,
                "warning: 所有権の指摘が他に %d 件あります"
                "（表示したのは先頭 %d 件です）\n",
                o.nmore, OWNCK_MAX_REPORT);
}
