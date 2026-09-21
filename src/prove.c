// prove.c — 区間解析で実行時検査を消す（A-34 段 1・2）
//
// ★ 道具は 1 つだけです：**区間**（この値は lo 以上 hi 以下）。
//   SMT ソルバもループ不変条件も使いません。
//
// なぜ区間だけで足りるのか
//   この言語は、証明したい性質のほとんどを**型と契約が先に言っています**。
//     ・範囲型（A-28）   … `Percent` なら [0, 100] から始められる
//     ・契約（A-29）     … `requires b != 0` なら除数は 0 でない
//     ・所有権（A-24）   … **別名が無い**ので、呼び出しで勝手に値が変わらない
//   関数をまたぐ情報が型と契約に書いてあるので、関数間解析が要りません。
//
// 注意: 関係（`i < len(xs)`）だけは区間で表せないので、**1 つだけ**関係を
//   覚えます（「この変数は、この list の長さより小さい」）。添字の検査は
//   これが無いと 1 つも消えません。
#include "prove.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "sema.h"
#include "types.h"
#include "util.h"

// ── 区間 ───────────────────────────────────────────────────
//
// ★ ±∞ は LLONG_MIN / LLONG_MAX で表します。桁あふれの検査そのものを
//   消すのが目的なので、**区間の計算では絶対に折り返しません**
//   （危ないと思ったら ⊤ に落とします）。
typedef struct {
    long long lo, hi;
} Iv;

static const Iv IV_TOP = {LLONG_MIN, LLONG_MAX};

static Iv iv_of(long long v) {
    Iv r = {v, v};
    return r;
}
static bool iv_is_top(Iv a) { return a.lo == LLONG_MIN && a.hi == LLONG_MAX; }
static bool iv_fits(Iv a, long long lo, long long hi) {
    return a.lo >= lo && a.hi <= hi;
}

// 足し算・引き算・掛け算が折り返すか。
//
// 注意: **組み込みの __builtin_*_overflow は使いません。** セルフホスト版にも
//   同じ判断をさせる必要があり（IR が 1 バイトでも違うと落ちます）、
//   向こうには組み込みがないためです。**両方で同じ式**を書きます。
//
// ★ 掛け算だけは保守的です（|MIN| が表せないので、MIN が絡んだら溢れる扱い）。
//   消せる検査が少し減るだけで、**間違った方向には倒れません**。
static bool add_ovf(long long a, long long b, long long *out) {
    if (b > 0 && a > LLONG_MAX - b) return true;
    if (b < 0 && a < LLONG_MIN - b) return true;
    *out = a + b;
    return false;
}
static bool sub_ovf(long long a, long long b, long long *out) {
    if (b < 0 && a > LLONG_MAX + b) return true;
    if (b > 0 && a < LLONG_MIN + b) return true;
    *out = a - b;
    return false;
}
static bool mul_ovf(long long a, long long b, long long *out) {
    if (a == 0 || b == 0) { *out = 0; return false; }
    if (a == LLONG_MIN || b == LLONG_MIN) return true;
    long long ua = a < 0 ? -a : a;
    long long ub = b < 0 ? -b : b;
    if (ua > LLONG_MAX / ub) return true;
    *out = a * b;
    return false;
}

// ★ **端が溢れたら、そこで止める（飽和）**。⊤ には落としません。
//
// **なぜこれが健全なのか。** 桁あふれの検査が残っているからです。
//   `i + 1` が本当に溢れるなら、そこで**実行が止まります**（折り返しません）。
//   つまり「この先へ進んだ」のなら結果は必ず表せる範囲に収まっています。
//   溢れる側の端を LLONG_MAX / LLONG_MIN で止めるのは、**進めた場合の値の
//   集合**を正しく覆います。
//
//   注意: **だから `--no-overflow-check` のときは、これをしません。**
//     検査が無ければ本当に折り返し、`i` が負になりえます。そのまま
//     「0 以上」と信じると、添字の検査を誤って消します（下の no_ovf_mode）。
//
//   注意: 堂々巡りにはなりません。`i = i + 1` 自身の検査は「端が溢れた」＝
//     示せなかった側に入るので、**消えずに残ります**。残った検査が上の
//     理屈を支える、という順序です。
static bool sat_mode = true;   // false … --no-overflow-check（折り返しうる）

static long long add_sat(long long a, long long b, bool *ok) {
    long long r;
    if (!add_ovf(a, b, &r)) return r;
    *ok = false;
    return b > 0 ? LLONG_MAX : LLONG_MIN;
}

static long long sub_sat(long long a, long long b, bool *ok) {
    long long r;
    if (!sub_ovf(a, b, &r)) return r;
    *ok = false;
    return b < 0 ? LLONG_MAX : LLONG_MIN;
}

static long long mul_sat(long long a, long long b, bool *ok) {
    long long r;
    if (!mul_ovf(a, b, &r)) return r;
    *ok = false;
    return (a < 0) == (b < 0) ? LLONG_MAX : LLONG_MIN;
}

// exact … どの端でも溢れなかったか（**桁あふれの検査を消してよいか**）
static Iv iv_add(Iv a, Iv b, bool *exact) {
    bool ok = true;
    if (iv_is_top(a) || iv_is_top(b)) { *exact = false; return IV_TOP; }
    long long lo = add_sat(a.lo, b.lo, &ok);
    long long hi = add_sat(a.hi, b.hi, &ok);
    *exact = ok;
    if (!sat_mode && !ok) return IV_TOP;
    if (lo > hi) return IV_TOP;
    Iv r = {lo, hi};
    return r;
}

static Iv iv_sub(Iv a, Iv b, bool *exact) {
    bool ok = true;
    if (iv_is_top(a) || iv_is_top(b)) { *exact = false; return IV_TOP; }
    long long lo = sub_sat(a.lo, b.hi, &ok);
    long long hi = sub_sat(a.hi, b.lo, &ok);
    *exact = ok;
    if (!sat_mode && !ok) return IV_TOP;
    if (lo > hi) return IV_TOP;
    Iv r = {lo, hi};
    return r;
}

static Iv iv_mul(Iv a, Iv b, bool *exact) {
    bool ok = true;
    if (iv_is_top(a) || iv_is_top(b)) { *exact = false; return IV_TOP; }
    // ★ 4 つの組み合わせの最小と最大（符号をまたぐときのため）
    long long c[4];
    c[0] = mul_sat(a.lo, b.lo, &ok);
    c[1] = mul_sat(a.lo, b.hi, &ok);
    c[2] = mul_sat(a.hi, b.lo, &ok);
    c[3] = mul_sat(a.hi, b.hi, &ok);
    long long lo = c[0], hi = c[0];
    for (int i = 1; i < 4; i++) {
        if (c[i] < lo) lo = c[i];
        if (c[i] > hi) hi = c[i];
    }
    *exact = ok;
    if (!sat_mode && !ok) return IV_TOP;
    Iv r = {lo, hi};
    return r;
}

static Iv iv_join(Iv a, Iv b) {
    Iv r = {a.lo < b.lo ? a.lo : b.lo, a.hi > b.hi ? a.hi : b.hi};
    return r;
}

// ── 環境（変数ごとの区間と、1 つだけの関係）────────────────
//
// 注意: 表は**出現順のリスト**です。2 つの実装で同じ結果にするため、
//    順序が結果に影響しない形（名前で引く）にしてあります。
typedef struct Ent Ent;
struct Ent {
    const char *name;   // IR 名（%x）
    Iv iv;
    // ★ 「この変数は、この list の長さより小さい」（添字の検査を消すため）
    //   注意: 区間では表せない唯一の関係なので、1 つだけ覚えます。
    const char *lt_len;  // list の IR 名。無ければ NULL
    Ent *next;
};

// ★ 「この 2 つの list は長さが同じ」（A-34 段 1 の続き）
//
//   注意: 区間でも lt_len でも言えない関係です。これが無いと、
//   `if len(a) != len(b): return` で守った後の `a[i]` と `b[i]` のうち
//   **片方しか**検査を消せません（いちばん多く残っていた型です）。
//   注意: 推移律は取りません（a==b と b==c から a==c は出しません）。
//     2 実装で同じ結果にするため、持ち方を単純に保ちます。
typedef struct LenEq LenEq;
struct LenEq {
    const char *a;
    const char *b;
    LenEq *next;
};

typedef struct {
    Ent *head;
    LenEq *eq;
} Env;

// 事後条件 1 つぶんの記録（**すべての return で示せたときだけ**消せます）
typedef struct EnsRec EnsRec;
struct EnsRec {
    Node *node;
    bool all_ok;    // ここまでの return で全部示せているか
    bool seen;      // return を 1 つ以上見たか
    EnsRec *next;
};

typedef struct {
    ProveStats *st;
    EnsRec *ens;    // いま検査している関数の ensures
    // ★ **印を立ててよい回か。**
    //
    // ここが安全の要です。ループの不動点は、入口を**少しずつ広げながら**
    //   何周も本体を歩きます。途中の回の入口は**まだ狭い**（＝楽観的）ので、
    //   その環境で印を立てると、**消してはいけない検査を消します**。
    //   例: `while i < n: s = s + i * 1000000000000`
    //     1 周目の入口は i ∈ [0, 0] なので「桁あふれしない」と見えますが、
    //     広げ切った入口は i ∈ [0, ∞) で、実際には折り返します。
    //   注意: 印は**入口が決まってからの最後の 1 周だけ**で立てます。
    bool mark;
} Prove;

// グローバルか（IR 名が @ で始まる）。
//
// 注意: **グローバルは追いません。** 他の関数がいつでも書き換えられるので、
//   ここで覚えた値を信じると、消してはいけない検査を消します。
static bool is_global_name(const char *name) { return name && name[0] == '@'; }

// この組をすでに持っているか（注意: **向きは問いません**）
static bool leneq_has(Env *e, const char *x, const char *y) {
    if (!x || !y) return false;
    for (LenEq *q = e->eq; q; q = q->next) {
        if (strcmp(q->a, x) == 0 && strcmp(q->b, y) == 0) return true;
        if (strcmp(q->a, y) == 0 && strcmp(q->b, x) == 0) return true;
    }
    return false;
}

// 組を足す（末尾に足します。並びは入れた順なので、2 実装で同じになります）
static void leneq_add(Env *e, const char *x, const char *y) {
    if (!x || !y || strcmp(x, y) == 0) return;
    if (leneq_has(e, x, y)) return;
    LenEq *q = xmalloc(sizeof(LenEq));
    q->a = x;
    q->b = y;
    q->next = NULL;
    LenEq **tail = &e->eq;
    while (*tail) tail = &(*tail)->next;
    *tail = q;
}

static bool key_under(const char *name, const char *key);

// その場所が関わる組を落とす（長さが変わったかもしれないため）
static void leneq_kill(Env *e, const char *key) {
    if (!key || !key[0]) return;
    LenEq **pp = &e->eq;
    while (*pp) {
        LenEq *q = *pp;
        if (strcmp(q->a, key) == 0 || strcmp(q->b, key) == 0 ||
            key_under(q->a, key) || key_under(q->b, key))
            *pp = q->next;
        else
            pp = &q->next;
    }
}

// グローバルが関わる組を落とす（誰でもいつでも短くできるため）
static void leneq_kill_globals(Env *e) {
    LenEq **pp = &e->eq;
    while (*pp) {
        LenEq *q = *pp;
        if (q->a[0] == '@' || q->b[0] == '@')
            *pp = q->next;
        else
            pp = &q->next;
    }
}

static Ent *env_find(Env *e, const char *name) {
    if (!name || is_global_name(name)) return NULL;
    for (Ent *p = e->head; p; p = p->next)
        if (strcmp(p->name, name) == 0) return p;
    return NULL;
}

static Ent *env_get(Env *e, const char *name) {
    if (is_global_name(name)) {
        // ★ 呼び出し側が書き込んでも捨てられるよう、使い捨てを返します
        Ent *q = xmalloc(sizeof(Ent));
        q->name = name;
        q->iv = IV_TOP;
        q->lt_len = NULL;
        q->next = NULL;
        return q;
    }
    Ent *p = env_find(e, name);
    if (p) return p;
    p = xmalloc(sizeof(Ent));
    p->name = name;
    p->iv = IV_TOP;
    p->lt_len = NULL;
    p->next = e->head;
    e->head = p;
    return p;
}

static void env_set(Env *e, const char *name, Iv v) {
    if (!name || is_global_name(name)) return;
    Ent *p = env_get(e, name);
    p->iv = v;
    p->lt_len = NULL;   // 値が変わったら関係は消える
}

static Iv env_iv(Env *e, const char *name) {
    Ent *p = env_find(e, name);
    return p ? p->iv : IV_TOP;
}

static Env env_copy(Env *e) {
    Env out = {NULL, NULL};
    Ent **tail = &out.head;
    for (Ent *p = e->head; p; p = p->next) {
        Ent *q = xmalloc(sizeof(Ent));
        *q = *p;
        q->next = NULL;
        *tail = q;
        tail = &q->next;
    }
    LenEq **etail = &out.eq;
    for (LenEq *q = e->eq; q; q = q->next) {
        LenEq *r = xmalloc(sizeof(LenEq));
        *r = *q;
        r->next = NULL;
        *etail = r;
        etail = &r->next;
    }
    return out;
}

// 合流（if の後・ループの入口）。区間は和、関係は両方に在るものだけ。
static void env_join(Env *dst, Env *src) {
    for (Ent *p = dst->head; p; p = p->next) {
        Ent *q = env_find(src, p->name);
        if (!q) { p->iv = IV_TOP; p->lt_len = NULL; continue; }
        p->iv = iv_join(p->iv, q->iv);
        if (!q->lt_len || !p->lt_len || strcmp(p->lt_len, q->lt_len) != 0)
            p->lt_len = NULL;
    }
    // src にしか無い変数は dst では「未知」なので、何もしません（⊤ 扱い）。
    // ★ 長さの等しさも、**両方の道で言えたときだけ**残します。
    LenEq **pp = &dst->eq;
    while (*pp) {
        LenEq *q = *pp;
        if (!leneq_has(src, q->a, q->b))
            *pp = q->next;
        else
            pp = &q->next;
    }
}

static bool env_same(Env *a, Env *b) {
    for (Ent *p = a->head; p; p = p->next) {
        Ent *q = env_find(b, p->name);
        if (!q) return false;
        if (p->iv.lo != q->iv.lo || p->iv.hi != q->iv.hi) return false;
        if ((p->lt_len == NULL) != (q->lt_len == NULL)) return false;
        if (p->lt_len && q->lt_len && strcmp(p->lt_len, q->lt_len) != 0)
            return false;
    }
    for (Ent *q = b->head; q; q = q->next)
        if (!env_find(a, q->name)) return false;
    for (LenEq *q = a->eq; q; q = q->next)
        if (!leneq_has(b, q->a, q->b)) return false;
    for (LenEq *q = b->eq; q; q = q->next)
        if (!leneq_has(a, q->a, q->b)) return false;
    return true;
}

// 広げる（widening）。動いた端を ±∞ に飛ばして、反復を必ず止めます。
static void env_widen(Env *dst, Env *old) {
    for (Ent *p = dst->head; p; p = p->next) {
        Ent *q = env_find(old, p->name);
        if (!q) continue;
        if (p->iv.lo < q->iv.lo) p->iv.lo = LLONG_MIN;
        if (p->iv.hi > q->iv.hi) p->iv.hi = LLONG_MAX;
    }
}

// ── 式の区間を求める ───────────────────────────────────────

static Iv eval(Prove *pr, Env *env, Node *n);

// 型そのものが言っている範囲（範囲型 A-28 / bool）
static Iv iv_of_type(Type *t) {
    if (!t) return IV_TOP;
    if (ty_is_range(t)) {
        Iv r = {t->lo, t->hi};
        return r;
    }
    if (t->kind == TY_BOOL) {
        Iv r = {0, 1};
        return r;
    }
    return IV_TOP;
}

// 場所の鍵。**同じ場所なら同じ文字列**になります。
//
// ★ 変数（%x）と、1 段のフィールド（%self.toks）まで扱います。
//   注意: ここを深くすると「本当に同じ場所か」の判断が難しくなるので、
//     1 段で止めます（この処理系自身の添字は、ほぼこの 2 つの形です）。
static const char *place_key(Node *n) {
    if (!n) return NULL;
    if (n->kind == ND_VAR && n->ir_name) return n->ir_name;
    if (n->kind == ND_FIELD && n->name && n->lhs && n->lhs->kind == ND_VAR &&
        n->lhs->ir_name) {
        StrBuf sb;
        sb_init(&sb);
        sb_printf(&sb, "%s.%s", n->lhs->ir_name, n->name);
        return sb_str(&sb);
    }
    return NULL;
}

// `len(xs)` なら xs の鍵を返す（添字の関係を作るため）
static const char *len_target(Node *n) {
    if (!n || n->kind != ND_CALL || !n->builtin || !n->builtin->impl) return NULL;
    if (strcmp(n->builtin->impl, "pl_list_len") != 0 &&
        strcmp(n->builtin->impl, "pl_str_len") != 0)
        return NULL;
    return place_key(n->args);
}

static Iv eval(Prove *pr, Env *env, Node *n) {
    if (!n) return IV_TOP;

    switch (n->kind) {
        case ND_INT:
            return iv_of(n->ival);

        case ND_BOOL:
            return iv_of(n->ival ? 1 : 0);

        case ND_VAR: {
            Iv t = iv_of_type(n->type);
            if (!n->ir_name) return t;
            Iv v = env_iv(env, n->ir_name);
            // ★ 型が言っている範囲と重ねます（範囲型の引数はここで効きます）
            Iv r = {v.lo > t.lo ? v.lo : t.lo, v.hi < t.hi ? v.hi : t.hi};
            if (r.lo > r.hi) return v;   // 食い違ったら値のほうを信じます
            return r;
        }

        case ND_RANGECHK:
            // 検査を通った後なので、入れ先の範囲に入っています
            return iv_of_type(n->type);

        case ND_RESULT:
            // ★ ensures の中の result。いま返そうとしている値の区間です
            //   （return の場所で env に入れてあります）。
            return env_iv(env, "%prove.result");

        case ND_BINOP: {
            Iv a = eval(pr, env, n->lhs);
            Iv b = eval(pr, env, n->rhs);
            bool ex;
            switch (n->op) {
                case OP_ADD: return iv_add(a, b, &ex);
                case OP_SUB: return iv_sub(a, b, &ex);
                case OP_MUL: return iv_mul(a, b, &ex);
                case OP_BITAND: {
                    // ★ 定数マスクは範囲をきっちり決めます（bytes で効きます）
                    if (b.lo == b.hi && b.lo >= 0) {
                        Iv r = {0, b.lo};
                        return r;
                    }
                    if (a.lo == a.hi && a.lo >= 0) {
                        Iv r = {0, a.lo};
                        return r;
                    }
                    if (a.lo >= 0 || b.lo >= 0) {
                        Iv r = {0, LLONG_MAX};
                        return r;
                    }
                    return IV_TOP;
                }
                case OP_SHR: {
                    // 右シフトは絶対値を小さくします（算術シフト）
                    if (a.lo >= 0 && b.lo >= 0 && b.lo == b.hi && b.lo < 63) {
                        Iv r = {a.lo >> b.lo, a.hi >> b.lo};
                        return r;
                    }
                    return IV_TOP;
                }
                case OP_MOD: {
                    // 注意: Python と同じ切り下げなので、b > 0 なら結果は [0, b-1]
                    if (b.lo > 0) {
                        Iv r = {0, b.hi - 1};
                        return r;
                    }
                    return IV_TOP;
                }
                case OP_FLOORDIV: {
                    if (a.lo >= 0 && b.lo > 0) {
                        Iv r = {a.lo / b.hi, a.hi / b.lo};
                        return r;
                    }
                    return IV_TOP;
                }
                default:
                    if (is_compare(n->op)) {
                        Iv r = {0, 1};
                        return r;
                    }
                    return IV_TOP;
            }
        }

        case ND_UNARY:
            if (n->op == OP_NEG) {
                Iv a = eval(pr, env, n->lhs);
                if (iv_is_top(a) || a.lo == LLONG_MIN) return IV_TOP;
                Iv r = {-a.hi, -a.lo};
                return r;
            }
            if (n->op == OP_NOT) {
                Iv r = {0, 1};
                return r;
            }
            return IV_TOP;

        case ND_CALL:
        case ND_METHOD: {
            // ★ len(...) は 0 以上（長さは負になりません）
            if (len_target(n) ||
                (n->builtin && n->builtin->impl &&
                 (strcmp(n->builtin->impl, "pl_list_len") == 0 ||
                  strcmp(n->builtin->impl, "pl_str_len") == 0))) {
                Iv r = {0, LLONG_MAX};
                return r;
            }
            // ★ 戻り型が範囲型なら、その範囲から始められます（A-28 の効き目）
            return iv_of_type(n->type);
        }

        default:
            return iv_of_type(n->type);
    }
}

// ── 条件で絞る ─────────────────────────────────────────────
//
// ★ `if i < len(xs):` の then 側では i < len(xs) が成り立ちます。
//   区間（i の上限）と関係（i < len(xs)）の**両方**を更新します。
static void narrow(Prove *pr, Env *env, Node *cond, bool truth);

static void narrow_cmp(Prove *pr, Env *env, Node *n, bool truth) {
    OpKind op = n->op;
    if (!truth) {
        // 否定は逆の比較に読み替えます（!= は絞れないので飛ばします）
        switch (op) {
            case OP_LT: op = OP_GE; break;
            case OP_LE: op = OP_GT; break;
            case OP_GT: op = OP_LE; break;
            case OP_GE: op = OP_LT; break;
            case OP_EQ: return;
            case OP_NE: op = OP_EQ; break;
            default: return;
        }
    }

    Node *l = n->lhs, *r = n->rhs;
    if (!l || !r) return;

    // ① 関係（i < len(xs)）
    if (op == OP_LT && l->kind == ND_VAR && l->ir_name) {
        const char *lt = len_target(r);
        if (lt) env_get(env, l->ir_name)->lt_len = lt;
    }

    // ①' 長さの等しさ（len(a) == len(b)）
    //
    // ★ `if len(a) != len(b): return` の**通り抜けた側**がここに来ます
    //   （否定は上で OP_NE → OP_EQ に読み替えてあります）。
    if (op == OP_EQ) {
        const char *ea = len_target(l), *eb = len_target(r);
        if (ea && eb) leneq_add(env, ea, eb);
    }

    // ② 区間
    Iv li = eval(pr, env, l), ri = eval(pr, env, r);
    if (l->kind == ND_VAR && l->ir_name && l->type && l->type->kind == TY_INT) {
        Ent *p = env_get(env, l->ir_name);
        Iv v = p->iv;
        if (v.lo < li.lo) v.lo = li.lo;
        if (v.hi > li.hi) v.hi = li.hi;
        switch (op) {
            case OP_LT: if (ri.hi != LLONG_MIN && ri.hi - 1 < v.hi) v.hi = ri.hi - 1; break;
            case OP_LE: if (ri.hi < v.hi) v.hi = ri.hi; break;
            case OP_GT: if (ri.lo != LLONG_MAX && ri.lo + 1 > v.lo) v.lo = ri.lo + 1; break;
            case OP_GE: if (ri.lo > v.lo) v.lo = ri.lo; break;
            case OP_EQ:
                if (ri.lo > v.lo) v.lo = ri.lo;
                if (ri.hi < v.hi) v.hi = ri.hi;
                break;
            default: break;
        }
        if (v.lo <= v.hi) p->iv = v;
    }
    // ③ 右辺が変数のときは向きを入れ替えて同じことをします
    if (r->kind == ND_VAR && r->ir_name && r->type && r->type->kind == TY_INT) {
        Ent *p = env_get(env, r->ir_name);
        Iv v = p->iv;
        switch (op) {
            case OP_LT: if (li.lo != LLONG_MAX && li.lo + 1 > v.lo) v.lo = li.lo + 1; break;
            case OP_LE: if (li.lo > v.lo) v.lo = li.lo; break;
            case OP_GT: if (li.hi != LLONG_MIN && li.hi - 1 < v.hi) v.hi = li.hi - 1; break;
            case OP_GE: if (li.hi < v.hi) v.hi = li.hi; break;
            case OP_EQ:
                if (li.lo > v.lo) v.lo = li.lo;
                if (li.hi < v.hi) v.hi = li.hi;
                break;
            default: break;
        }
        if (v.lo <= v.hi) p->iv = v;
    }
}

static void narrow(Prove *pr, Env *env, Node *cond, bool truth) {
    if (!cond) return;
    if (cond->kind == ND_BINOP && is_compare(cond->op)) {
        narrow_cmp(pr, env, cond, truth);
        return;
    }
    // `a and b` の真の側は両方が真
    if (cond->kind == ND_LOGICAL && truth && cond->op == OP_AND) {
        narrow(pr, env, cond->lhs, true);
        narrow(pr, env, cond->rhs, true);
        return;
    }
    // `a or b` の偽の側は両方が偽
    if (cond->kind == ND_LOGICAL && !truth && cond->op == OP_OR) {
        narrow(pr, env, cond->lhs, false);
        narrow(pr, env, cond->rhs, false);
        return;
    }
    if (cond->kind == ND_UNARY && cond->op == OP_NOT) {
        narrow(pr, env, cond->lhs, !truth);
        return;
    }
}

// 条件が「必ず真」と言えるか（契約の検査を消すため。段 2）
typedef enum { TRI_UNKNOWN, TRI_TRUE, TRI_FALSE } Tri;

static Tri prove_cond(Prove *pr, Env *env, Node *n) {
    if (!n) return TRI_UNKNOWN;
    if (n->kind == ND_BOOL) return n->ival ? TRI_TRUE : TRI_FALSE;

    if (n->kind == ND_UNARY && n->op == OP_NOT) {
        Tri t = prove_cond(pr, env, n->lhs);
        if (t == TRI_TRUE) return TRI_FALSE;
        if (t == TRI_FALSE) return TRI_TRUE;
        return TRI_UNKNOWN;
    }
    if (n->kind == ND_LOGICAL) {
        Tri a = prove_cond(pr, env, n->lhs), b = prove_cond(pr, env, n->rhs);
        if (n->op == OP_AND) {
            if (a == TRI_FALSE || b == TRI_FALSE) return TRI_FALSE;
            if (a == TRI_TRUE && b == TRI_TRUE) return TRI_TRUE;
            return TRI_UNKNOWN;
        }
        if (a == TRI_TRUE || b == TRI_TRUE) return TRI_TRUE;
        if (a == TRI_FALSE && b == TRI_FALSE) return TRI_FALSE;
        return TRI_UNKNOWN;
    }
    if (n->kind != ND_BINOP || !is_compare(n->op)) return TRI_UNKNOWN;

    Iv a = eval(pr, env, n->lhs), b = eval(pr, env, n->rhs);
    if (iv_is_top(a) || iv_is_top(b)) return TRI_UNKNOWN;

    switch (n->op) {
        case OP_LT: if (a.hi < b.lo) return TRI_TRUE;
                    if (a.lo >= b.hi) return TRI_FALSE;
                    return TRI_UNKNOWN;
        case OP_LE: if (a.hi <= b.lo) return TRI_TRUE;
                    if (a.lo > b.hi) return TRI_FALSE;
                    return TRI_UNKNOWN;
        case OP_GT: if (a.lo > b.hi) return TRI_TRUE;
                    if (a.hi <= b.lo) return TRI_FALSE;
                    return TRI_UNKNOWN;
        case OP_GE: if (a.lo >= b.hi) return TRI_TRUE;
                    if (a.hi < b.lo) return TRI_FALSE;
                    return TRI_UNKNOWN;
        case OP_EQ: if (a.lo == a.hi && b.lo == b.hi && a.lo == b.lo) return TRI_TRUE;
                    if (a.hi < b.lo || b.hi < a.lo) return TRI_FALSE;
                    return TRI_UNKNOWN;
        case OP_NE: if (a.hi < b.lo || b.hi < a.lo) return TRI_TRUE;
                    if (a.lo == a.hi && b.lo == b.hi && a.lo == b.lo) return TRI_FALSE;
                    return TRI_UNKNOWN;
        default: return TRI_UNKNOWN;
    }
}

// ── 式をたどって印を立てる ─────────────────────────────────
//
// ★ 「消す」判断はここだけで行います。**分からなければ何もしません**
//   （印が立たなければ、今までどおり実行時に確かめます）。
static void kill_mut_args(Env *env, Node *n);

static void walk_expr(Prove *pr, Env *env, Node *n) {
    if (!n) return;

    switch (n->kind) {
        case ND_BINOP: {
            walk_expr(pr, env, n->lhs);
            walk_expr(pr, env, n->rhs);
            // ★ `//` と `%` を、呼び出しではなく命令 1 つにする条件。
            //
            //   ① 両方 0 以上（切り下げ＝切り捨てになる。Python と LLVM の差）
            //   ② 除数が **コンパイル時に決まっている正の数**
            //
            // なぜ ② が要るのか（測って決めました）
            //   除数が定数なら、LLVM が掛け算とシフトに置き換えます
            //   （20,000,000 回の `i % 7` が **93 ms → 26 ms**）。
            //   注意: **定数でないときは、命令にすると逆に遅くなりました**
            //     （105 ms 対 66 ms）。64 ビットの除算命令が重く、ランタイムの
            //     呼び出しと変わらないためです。速くならない最適化は入れません。
            if (n->type && n->type->kind == TY_INT &&
                (n->op == OP_FLOORDIV || n->op == OP_MOD)) {
                Iv a = eval(pr, env, n->lhs), b = eval(pr, env, n->rhs);
                if (!pr->mark) {
                    // 不動点の途中。まだ印は立てません
                } else if (a.lo >= 0 && b.lo > 0 && b.lo == b.hi) {
                    if (!n->no_div_check) pr->st->div++;
                    n->no_div_check = true;
                } else if (!n->no_div_check) {
                    pr->st->div_left++;
                }
            }
            if (n->type && n->type->kind == TY_INT &&
                (n->op == OP_ADD || n->op == OP_SUB || n->op == OP_MUL)) {
                Iv a = eval(pr, env, n->lhs), b = eval(pr, env, n->rhs);
                bool exact = false;
                Iv r = n->op == OP_ADD ? iv_add(a, b, &exact)
                     : n->op == OP_SUB ? iv_sub(a, b, &exact)
                                       : iv_mul(a, b, &exact);
                (void)r;
                // ★ **どの端でも溢れなかった**ときだけ消します。
                //   注意: 飽和した区間（端が止まっただけ）では消せません。
                //     消さずに残すからこそ、飽和が健全でいられます。
                if (!pr->mark) {
                    // 不動点の途中。まだ印は立てません
                } else if (exact && !iv_is_top(a) && !iv_is_top(b)) {
                    if (!n->no_ovf_check) pr->st->ovf++;
                    n->no_ovf_check = true;
                } else if (!n->no_ovf_check) {
                    pr->st->ovf_left++;
                }
            }
            return;
        }

        case ND_UNARY:
            walk_expr(pr, env, n->lhs);
            return;

        case ND_LOGICAL:
            walk_expr(pr, env, n->lhs);
            // 注意: 右側は評価されないことがあるので、区間は触りません
            walk_expr(pr, env, n->rhs);
            return;

        case ND_RANGECHK: {
            walk_expr(pr, env, n->lhs);
            Iv v = eval(pr, env, n->lhs);
            if (!pr->mark) {
                // 不動点の途中。まだ印は立てません
            } else if (n->type && ty_is_range(n->type) &&
                       iv_fits(v, n->type->lo, n->type->hi)) {
                if (!n->no_range_check) pr->st->range++;
                n->no_range_check = true;
            } else if (!n->no_range_check) {
                pr->st->range_left++;
            }
            return;
        }

        case ND_INDEX: {
            walk_expr(pr, env, n->lhs);
            walk_expr(pr, env, n->rhs);
            // ★ 添字は「0 以上」と「長さ未満」の両方が要ります。
            //   長さ未満は区間では言えないので、関係（lt_len）を見ます。
            Iv i = eval(pr, env, n->rhs);
            bool nonneg = i.lo >= 0;
            bool inb = false;
            const char *lk = place_key(n->lhs);
            if (n->rhs && n->rhs->kind == ND_VAR && n->rhs->ir_name && lk) {
                Ent *p = env_find(env, n->rhs->ir_name);
                if (p && p->lt_len &&
                    (strcmp(p->lt_len, lk) == 0 ||
                     leneq_has(env, p->lt_len, lk)))
                    inb = true;
            }
            // 注意: str の添字も同じ形ですが、こちらは要素の取り出し方が
            //   違うので触りません（list のときだけ消します）。
            bool is_list = n->lhs && n->lhs->type && n->lhs->type->kind == TY_LIST;
            if (!pr->mark) {
                // 不動点の途中。まだ印は立てません
            } else if (is_list && nonneg && inb) {
                if (!n->no_bounds_check) pr->st->bounds++;
                n->no_bounds_check = true;
            } else if (is_list && !n->no_bounds_check) {
                pr->st->bounds_left++;
            }
            return;
        }

        case ND_CALL:
        case ND_METHOD:
            walk_expr(pr, env, n->lhs);
            for (Node *a = n->args; a; a = a->next) walk_expr(pr, env, a);
            // 注意: 呼び出しの後は、渡したものについて分かっていたことを落とします
            kill_mut_args(env, n);
            return;

        default:
            break;
    }

    // ★ それ以外は子をたどるだけです（印は立てません）
    walk_expr(pr, env, n->lhs);
    walk_expr(pr, env, n->rhs);
    walk_expr(pr, env, n->els);
    for (Node *a = n->args; a; a = a->next) walk_expr(pr, env, a);
}

// 呼び出しの後に、書き換えられうる変数の区間を捨てる
//
// ★ **所有権検査（A-24）のおかげでここが軽くなります。** 別名が無いので、
//   呼び先が触れるのは `mut` で渡したものだけです。
// その鍵（とその下のフィールド）について分かっていたことを落とす。
//
// 注意: **フィールドまで落とすのが要点です。** `self` を渡した先で
//   `self.toks.append(...)` をされると、`i < len(self.toks)` は崩れます。
//   `%self` を落とすときに `%self.toks` も落とさないと、**消してはいけない
//   検査を消します**。
static bool key_under(const char *name, const char *key) {
    size_t n = strlen(key);
    return strncmp(name, key, n) == 0 && name[n] == '.';
}

static void kill_place(Env *env, const char *key) {
    if (!key || !key[0]) return;
    leneq_kill(env, key);
    for (Ent *p = env->head; p; p = p->next) {
        if (strcmp(p->name, key) == 0 || key_under(p->name, key)) {
            p->iv = IV_TOP;
            p->lt_len = NULL;
        }
        // ★ その list を指していた関係も落とします（短くなったかもしれない）
        if (p->lt_len &&
            (strcmp(p->lt_len, key) == 0 || key_under(p->lt_len, key)))
            p->lt_len = NULL;
    }
}

// グローバルの list を指す関係を落とす（呼び先がいつでも短くできるため）
static void kill_global_rels(Env *env) {
    for (Ent *p = env->head; p; p = p->next)
        if (p->lt_len && p->lt_len[0] == '@') p->lt_len = NULL;
    leneq_kill_globals(env);
}

// 呼び出しで壊れうるものを落とす。
//
// ★ **所有権検査（A-24）のおかげでここが軽くなります。** 別名が無いので、
//   呼び先が触れるのは渡したものだけです。渡していない変数は無傷です。
//   注意: 借りだけを渡したなら本当は縮みませんが、仮引数の受け取り方をここで
//     引く仕掛けがないので、**渡したものは落とす**（安全側）にします。
static void kill_mut_args(Env *env, Node *n) {
    // ★ **`len(x)` は長さを変えません。** ここを落としていたせいで、
    //   `while i < len(xs)` の条件を歩いた瞬間に `i < len(xs)` の関係が
    //   消えていました（＝添字の検査がほぼ 1 つも消えていませんでした）。
    if (len_target(n)) return;
    for (Node *a = n->args; a; a = a->next) kill_place(env, place_key(a));
    // メソッドの受け手（xs.append(v) の xs）も落とします
    if (n->kind == ND_METHOD) kill_place(env, place_key(n->lhs));
    // 注意: グローバルは誰でも触れるので、指していた関係を落とします
    kill_global_rels(env);
}

// 注意: **要素を渡すこと（f(xs[i])）では list を落としません。**
//   渡っているのは要素で、list そのものではありません。長さを変えるには
//   list を `mut` で渡すか、list のメソッドを呼ぶ必要があります（所有権検査
//   がそれを保証しています）。注意: ここを落としていたせいで、**関係が消えない
//   はずの場所で 499 件消えていました**（計測して分かりました）。

// 本体に break / continue があるか（ループの道が増えるかどうか）
//
// 注意: 内側のループの break は、そのループのものなので数えません。
static bool has_jump(Node *n) {
    for (; n; n = n->next) {
        if (n->kind == ND_BREAK || n->kind == ND_CONTINUE) return true;
        if (n->kind == ND_WHILE) continue;   // 内側のループは別の話
        if (has_jump(n->lhs) || has_jump(n->rhs) || has_jump(n->els) ||
            has_jump(n->body) || has_jump(n->incr))
            return true;
    }
    return false;
}

// この文の並びで書き換わる場所を、全部「分からない」に戻す
static void kill_assigned(Env *env, Node *n) {
    for (; n; n = n->next) {
        if (n->kind == ND_ASSIGN) kill_place(env, place_key(n->lhs));
        if (n->kind == ND_VARDECL) kill_place(env, n->ir_name);
        if (n->kind == ND_UNPACK)
            for (Node *v = n->params; v; v = v->next) kill_place(env, v->ir_name);
        if (n->kind == ND_CALL || n->kind == ND_METHOD) kill_mut_args(env, n);
        kill_assigned(env, n->lhs);
        kill_assigned(env, n->rhs);
        kill_assigned(env, n->els);
        kill_assigned(env, n->body);
        kill_assigned(env, n->incr);
        for (Node *a = n->args; a; a = a->next) kill_assigned(env, a);
    }
}

static void walk_stmt(Prove *pr, Env *env, Node *n);

// この文の並びは、**必ず**この先へ進まずに抜けるか。
//
// ★ これが分かると `if len(a) != len(b): return` の**後ろ**で
//   「長さは同じ」と言えます（then 側は合流に混ぜません）。
//   注意: 分からないときは false を返します（混ぜる＝安全側）。
static bool always_exits(Node *first) {
    for (Node *s = first; s; s = s->next) {
        switch (s->kind) {
            case ND_RETURN:
            case ND_RAISE:
            case ND_BREAK:
            case ND_CONTINUE:
                return true;
            case ND_BLOCK:
            case ND_SCOPE:
                if (always_exits(s->body)) return true;
                break;
            case ND_IF:
                // 両側とも抜けるなら、この if から先へは進みません
                if (s->els && always_exits(s->body) && always_exits(s->els))
                    return true;
                break;
            default:
                break;
        }
    }
    return false;
}

static void walk_list(Prove *pr, Env *env, Node *first) {
    for (Node *s = first; s; s = s->next) walk_stmt(pr, env, s);
}

static void walk_stmt(Prove *pr, Env *env, Node *n) {
    if (!n) return;

    switch (n->kind) {
        case ND_BLOCK:
            walk_list(pr, env, n->body);
            return;

        case ND_VARDECL:
            walk_expr(pr, env, n->rhs);
            if (n->rhs && (n->rhs->kind == ND_CALL || n->rhs->kind == ND_METHOD))
                kill_mut_args(env, n->rhs);
            if (n->type && n->type->kind == TY_INT && n->ir_name) {
                Iv v = eval(pr, env, n->rhs);
                Iv t = iv_of_type(n->type);
                Iv r = {v.lo > t.lo ? v.lo : t.lo, v.hi < t.hi ? v.hi : t.hi};
                env_set(env, n->ir_name, r.lo <= r.hi ? r : v);
            }
            return;

        case ND_ASSIGN: {
            walk_expr(pr, env, n->rhs);
            if (n->rhs && (n->rhs->kind == ND_CALL || n->rhs->kind == ND_METHOD))
                kill_mut_args(env, n->rhs);
            Node *t = n->lhs;
            if (t && t->kind == ND_VAR && t->ir_name) {
                if (t->type && t->type->kind == TY_INT)
                    env_set(env, t->ir_name, eval(pr, env, n->rhs));
                else
                    env_set(env, t->ir_name, IV_TOP);
            } else {
                walk_expr(pr, env, t);
            }
            return;
        }

        case ND_IF: {
            walk_expr(pr, env, n->lhs);
            Env then_e = env_copy(env);
            narrow(pr, &then_e, n->lhs, true);
            walk_stmt(pr, &then_e, n->body);

            Env else_e = env_copy(env);
            narrow(pr, &else_e, n->lhs, false);
            if (n->els) walk_stmt(pr, &else_e, n->els);

            // ★ 片側が**必ず抜ける**なら、その側は合流に混ぜません。
            //   これで `if 〜: return` の後ろに、絞り込んだ事実が残ります。
            bool then_out = always_exits(n->body);
            bool else_out = n->els && always_exits(n->els);
            if (then_out && !else_out) {
                *env = else_e;
            } else if (else_out && !then_out) {
                *env = then_e;
            } else {
                // 注意: 両方抜けるときもここに来ます（この先は届かないので
                //   どちらでも構いません）。合流は「両方で言えること」だけ。
                env_join(&then_e, &else_e);
                *env = then_e;
            }
            return;
        }

        // ── 場合分け（A-37）──
        //
        // ★ **if の合流と同じです。** どの case も「入る前の状態」から始まり、
        //   出たところで合流します。
        //
        // ★ **必ず抜ける case は合流に混ぜません**（if と同じ扱い）。
        //   これで `match 〜: case 〜: return` の後ろに事実が残ります。
        //
        // 注意: case の中で絞り込める事実はありません（調べているのは
        //   列挙か定数で、区間解析が使える形ではないため）。ここは
        //   「分岐して合流する」ことだけを正しく扱います。
        case ND_MATCH: {
            walk_expr(pr, env, n->lhs);

            Env merged;
            bool any = false;
            for (Node *c = n->body; c; c = c->next) {
                Env ce = env_copy(env);
                walk_stmt(pr, &ce, c->body);
                if (always_exits(c->body)) continue;   // この先へは落ちない
                if (!any) { merged = ce; any = true; }
                else env_join(&merged, &ce);
            }
            // 注意: どの case も抜けるなら、この先は届きません。入る前の
            //   状態をそのまま残します（if で両方抜けるときと同じ扱い）。
            if (any) *env = merged;
            return;
        }

        // ★ 列挙の宣言は実行時に何もしません（A-37）
        case ND_ENUM:
            return;

        case ND_WHILE: {
            // ★ ループの入口で成り立つことを求めます。
            //
            // なぜ「確かめてから使う」のか
            //   不動点の反復は、本体を**まっすぐ 1 本の道**として歩きます。
            //   `break` / `continue` があると道が増えるので、その前提が崩れます。
            //   注意: 崩れたまま使うと、**消してはいけない検査を消します**。
            //   だから ① 飛び出しがあるときは保守的な入口（本体で書き換わる
            //   ものを全部忘れる）を使い、② 無いときも「本当に不動点か」を
            //   最後に確かめます。
            Env in;
            bool ok = false;
            // **不動点を探している間は印を立てません**（入口がまだ狭い＝
            //   楽観的なので、ここで消すと消しすぎます）。注意: 内側のループも
            //   自分の探索中だけ消すので、元の値に戻します。
            bool mark_save = pr->mark;
            pr->mark = false;
            if (!has_jump(n->body)) {
                in = env_copy(env);
                for (int round = 0; round < 8; round++) {
                    Env body = env_copy(&in);
                    narrow(pr, &body, n->lhs, true);
                    walk_stmt(pr, &body, n->body);
                    if (n->incr) walk_stmt(pr, &body, n->incr);

                    Env next = env_copy(&in);
                    env_join(&next, &body);
                    if (round >= 1) env_widen(&next, &in);
                    if (env_same(&next, &in)) { ok = true; break; }
                    in = next;
                }
                if (ok) {
                    // ★ 念のため、もう 1 周して本当に動かないことを確かめます
                    Env chk = env_copy(&in);
                    narrow(pr, &chk, n->lhs, true);
                    walk_stmt(pr, &chk, n->body);
                    if (n->incr) walk_stmt(pr, &chk, n->incr);
                    Env joined = env_copy(&in);
                    env_join(&joined, &chk);
                    ok = env_same(&joined, &in);
                }
            }
            if (!ok) {
                // 保守的な入口：本体で書き換わるものは何も分からないことにする
                in = env_copy(env);
                kill_assigned(&in, n->body);
                if (n->incr) kill_assigned(&in, n->incr);
            }

            // 入口が決まったので、本体をもう一度（★ 印を立てるのはこの回だけ）
            pr->mark = mark_save;
            Env body = env_copy(&in);
            narrow(pr, &body, n->lhs, true);
            walk_expr(pr, &body, n->lhs);
            walk_stmt(pr, &body, n->body);
            if (n->incr) walk_stmt(pr, &body, n->incr);

            // ★ ループを抜けた後。注意: break で抜けたなら条件は偽とは限りません。
            if (!has_jump(n->body)) narrow(pr, &in, n->lhs, false);
            *env = in;
            return;
        }

        case ND_RETURN: {
            walk_expr(pr, env, n->lhs);
            // ★ 事後条件は**出口ごと**に確かめます（すべての出口で示せたら消す）
            if (pr->ens) {
                Env tmp = env_copy(env);
                if (n->lhs) env_set(&tmp, "%prove.result", eval(pr, env, n->lhs));
                for (EnsRec *r = pr->ens; r; r = r->next) {
                    r->seen = true;
                    if (prove_cond(pr, &tmp, r->node->lhs) != TRI_TRUE)
                        r->all_ok = false;
                }
            }
            return;
        }

        // ── 契約（A-29。段 2）────────────────────────────
        //
        // ★ 区間で「必ず真」と言えたら、実行時の検査を消します。
        //   注意: 言えなくても**赤にはしません**（今までどおり実行時に確かめる）。
        case ND_REQUIRES: {
            walk_expr(pr, env, n->lhs);
            if (!pr->mark) {
                // 不動点の途中。まだ印は立てません
            } else if (prove_cond(pr, env, n->lhs) == TRI_TRUE) {
                if (!n->no_contract) pr->st->contract++;
                n->no_contract = true;
            } else if (!n->no_contract) {
                pr->st->contract_left++;
            }
            // ★ **通った後は、その条件が成り立っています。**
            //   `requires x >= 0` の後で x の区間が [0, ∞) になるので、
            //   そのあとの添字や桁あふれの証明に効きます。
            narrow(pr, env, n->lhs, true);
            return;
        }

        case ND_ENSURES:
            // 注意: ここ（関数の先頭）では確かめません。**return の場所**で、
            //   返す値を result に入れてから確かめます（下の ND_RETURN）。
            walk_expr(pr, env, n->lhs);
            return;

        case ND_TRY:
            walk_stmt(pr, env, n->body);
            for (Node *ex = n->els; ex; ex = ex->next) walk_stmt(pr, env, ex->body);
            // 注意: try の後は「どこで抜けたか」が分からないので、全部忘れます
            *env = (Env){NULL, NULL};
            return;

        case ND_SCOPE:
        case ND_UNSAFE:
            walk_stmt(pr, env, n->body);
            return;

        default:
            walk_expr(pr, env, n->lhs);
            walk_expr(pr, env, n->rhs);
            for (Node *a = n->args; a; a = a->next) walk_expr(pr, env, a);
            if (n->body) walk_list(pr, env, n->body);
            return;
    }
}

static void prove_func(Prove *pr, Node *fn) {
    if (!fn->body) return;

    // ① この関数の ensures を集める（本体の先頭に並んでいます）
    EnsRec *ens = NULL, **tail = &ens;
    for (Node *st = fn->body->body; st; st = st->next) {
        if (st->kind != ND_REQUIRES && st->kind != ND_ENSURES) break;
        if (st->kind != ND_ENSURES) continue;
        EnsRec *r = xmalloc(sizeof(EnsRec));
        r->node = st;
        r->all_ok = true;
        r->seen = false;
        r->next = NULL;
        *tail = r;
        tail = &r->next;
    }
    pr->ens = ens;

    Env env = {NULL, NULL};
    // ★ 引数の型が言っていることから始めます（範囲型はここで効きます）
    for (Node *pm = fn->params; pm; pm = pm->next)
        if (pm->ir_name && pm->type && pm->type->kind == TY_INT)
            env_set(&env, pm->ir_name, iv_of_type(pm->type));
    walk_list(pr, &env, fn->body->body);

    // ② すべての出口で示せた事後条件だけ消します
    //   注意: 値を返さない関数は「最後まで落ちてくる出口」もあるので、
    //     return を 1 つも見ていないときは消しません（安全側）。
    for (EnsRec *r = ens; r; r = r->next) {
        bool ok = r->seen && r->all_ok && fn->type && fn->type->kind != TY_NONE;
        if (ok) {
            if (!r->node->no_contract) pr->st->contract++;
            r->node->no_contract = true;
        } else if (!r->node->no_contract) {
            pr->st->contract_left++;
        }
    }
    pr->ens = NULL;
}

void prove_program(Module *mods, ProveStats *out, bool no_ovf_mode) {
    sat_mode = !no_ovf_mode;
    ProveStats st = {0};
    Prove pr = {&st, NULL, true};

    for (Module *m = mods; m; m = m->next) {
        for (Node *d = m->ast->body; d; d = d->next) {
            if (d->kind == ND_FUNC && !d->targs) prove_func(&pr, d);
            if (d->kind == ND_CLASS && !d->targs)
                for (Node *mm = d->body; mm; mm = mm->next)
                    if (mm->kind == ND_FUNC) prove_func(&pr, mm);
        }
    }
    if (out) *out = st;
}
