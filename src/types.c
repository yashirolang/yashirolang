#include "types.h"

#include <string.h>

#include "util.h"

Type *ty_int;
Type *ty_bool;
Type *ty_none;
Type *ty_str;
Type *ty_float;
Type *ty_null;

static Type *new_type(TypeKind kind) {
    Type *t = xmalloc(sizeof(Type));
    t->kind = kind;
    return t;
}

void types_init(void) {
    ty_int = new_type(TY_INT);
    ty_bool = new_type(TY_BOOL);
    ty_float = new_type(TY_FLOAT);
    ty_none = new_type(TY_NONE);
    ty_str = new_type(TY_STR);
    ty_null = new_type(TY_NULL);
}

// ── nullable ──────────────────────────────────────────

bool type_can_be_opt(Type *t) {
    // ★ None はヌルポインタとして表すので、ポインタで表される型だけ。
    //   int を nullable にするには箱に入れる必要があり、そこから
    //   「int は値か参照か」という別の話が始まります（v1 では入れない）。
    // ★ rc[T] もポインタ 1 個なので nullable にできます
    //   （木の「子が無い」を表すのに要ります）。
    return t->kind == TY_STR || t->kind == TY_LIST || t->kind == TY_CLASS ||
           t->kind == TY_RC;
}

Type *type_opt(Type *elem) {
    if (elem->kind == TY_OPT) return elem;  // (T | None) | None は T | None
    // ★ 同じ T には 1 個だけ作る（elem 側に覚えておく）
    if (elem->opt) return elem->opt;
    Type *t = new_type(TY_OPT);
    t->elem = elem;
    elem->opt = t;
    return t;
}

Type *type_strip_opt(Type *t) { return t->kind == TY_OPT ? t->elem : t; }

// 代入互換性（type-system.md 4 節）。
//
//   assignable(S → T) =
//       type_equal(S, T)                             (a) 完全一致
//       または (T が T2|None で S が None リテラル)     (b)
//       または (T が T2|None で assignable(S → T2))    (c) 広げる方向だけ許す
// クラスがインタフェースを実装しているか（sema が入れる）
bool (*class_implements_hook)(struct Class *c, struct Iface *i) = NULL;

bool type_assignable(Type *from, Type *to) {
    if (type_equal(from, to)) return true;

    // ★ クラス → インタフェース（実装していれば代入できる）。
    //   注意: **値の変換は起きません。** vtable へのポインタはオブジェクトの
    //     先頭に入っているので、ポインタはそのままです。
    if (to->kind == TY_IFACE && from->kind == TY_CLASS && class_implements_hook)
        return class_implements_hook(from->cls, to->iface);
    if (to->kind != TY_OPT) return false;
    if (from->kind == TY_NULL) return true;
    return type_assignable(from, to->elem);
}

Type *type_list(Type *elem) {
    Type *t = new_type(TY_LIST);
    t->elem = elem;
    return t;
}

// rc[T]。★ list[T] と同じ作り（シングルトンにはしない）。
Type *type_rc(Type *elem) {
    Type *t = xmalloc(sizeof(Type));
    t->kind = TY_RC;
    t->elem = elem;
    return t;
}

// ptr[T]。★ rc[T] と同じ作り
Type *type_ptr(Type *elem) {
    Type *t = xmalloc(sizeof(Type));
    t->kind = TY_PTR;
    t->elem = elem;
    return t;
}

// Thread[R]（elem は**戻り型**）と mutex[T]。★ rc[T] と同じ作り
Type *type_thread(Type *ret) {
    Type *t = xmalloc(sizeof(Type));
    t->kind = TY_THREAD;
    t->elem = ret;
    return t;
}

Type *type_mutex(Type *elem) {
    Type *t = xmalloc(sizeof(Type));
    t->kind = TY_MUTEX;
    t->elem = elem;
    return t;
}

// インタフェースの型
Type *type_iface(char *name, struct Iface *i) {
    Type *t = new_type(TY_IFACE);
    t->name = name;
    t->iface = i;
    return t;
}

// enum 型（A-37）。★ 同一性は定義ポインタで決めます（名前ではありません）。
//   import が入ると lexer.Kind と parser.Kind が同時に存在しえます。
Type *type_enum(const char *name, struct EnumDef *e) {
    Type *t = new_type(TY_ENUM);
    t->name = (char *)name;
    t->en = e;
    return t;
}

Type *type_class(char *name, struct Class *cls) {
    Type *t = new_type(TY_CLASS);
    t->name = name;
    t->cls = cls;
    return t;
}

// ── サイズとアラインメント ──────────────────────────────
//
// docs/design/memory-model.md 5 節の表のとおり。
// ★ 参照型（str / list / class）は「ポインタ 1 個」なので 8 バイトです。
//   指す先の大きさは関係ありません。
int type_size(Type *t) {
    switch (t->kind) {
        case TY_BOOL: return 1;  // メモリ上は i8（規約 R5）
        case TY_INT:
        case TY_IFACE:  // 実体へのポインタ
        case TY_TUPLE:  // 構造体へのポインタ
        case TY_FN:     // 関数へのポインタ
        case TY_FLOAT:  // double も 8 バイト
        case TY_STR:
        case TY_LIST:
        case TY_CLASS:
        case TY_RC:   // rc[T] もポインタ 1 個（指す先に数え札が付く）
        case TY_PTR:  // 生ポインタ
        case TY_THREAD:  // ランタイムの箱への不透明なポインタ
        case TY_MUTEX:
        case TY_OPT: return 8;  // T | None もポインタ 1 個
        default: UNREACHABLE();  // None は値を持たない
    }
}

int type_align(Type *t) { return type_size(t); }

bool type_equal(Type *a, Type *b) {
    // シングルトンなので、プリミティブ型どうしはここで済む
    if (a == b) return true;
    if (a->kind != b->kind) return false;

    // ★ 複合型は中身まで見る。
    //   list[int] と list[str] はどちらも kind == TY_LIST なので、
    //   ここが無いと「同じ型」と判定されてしまいます
    //   （コメントで予告していた穴）。
    if (a->kind == TY_LIST) return type_equal(a->elem, b->elem);

    // ★ 関数型は「引数の並びと戻り型が全部同じ」なら同じ型です。
    //   注意: 引数名は見ません（型だけが同一性を決めます）。
    if (a->kind == TY_FN) {
        if (a->nparams != b->nparams) return false;
        for (int i = 0; i < a->nparams; i++)
            if (!type_equal(a->params[i], b->params[i])) return false;
        return type_equal(a->elem, b->elem);
    }

    // ★ rc[T] も中身まで見る（rc[Node] と rc[Token] は別の型）
    if (a->kind == TY_RC) return type_equal(a->elem, b->elem);
    if (a->kind == TY_PTR) return type_equal(a->elem, b->elem);

    // ★ Thread[R] / mutex[T] も中身まで見る
    if (a->kind == TY_THREAD || a->kind == TY_MUTEX)
        return type_equal(a->elem, b->elem);

    // ★ クラスは「同じ定義か」で比べます。名前の一致ではありません。
    //   今は 1 ファイルなので同名クラスは 1 つだけですが、のちに import が
    //   入ると lexer.Token と parser.Token が同時に存在しえます。
    //   定義ポインタで比べておけば、そのとき何も直さずに済みます。
    if (a->kind == TY_CLASS) return a->cls == b->cls;

    // ★ インタフェースも定義で比べます（名前ではありません）
    if (a->kind == TY_IFACE) return a->iface == b->iface;

    // ★ 列挙も定義で比べます（A-37）。同名の enum が別モジュールにあっても
    //   別の型です。
    if (a->kind == TY_ENUM) return a->en == b->en;

    // ★ タプルは「並びが同じ」なら同じ型です（名前はありません）
    if (a->kind == TY_TUPLE) {
        if (a->nparams != b->nparams) return false;
        for (int i = 0; i < a->nparams; i++)
            if (!type_equal(a->params[i], b->params[i])) return false;
        return true;
    }

    // ★ T | None は中身どうしを比べる。
    //   type_equal は、型が増えるたびに手を入れてきた関数です。
    if (a->kind == TY_OPT) return type_equal(a->elem, b->elem);

    return true;
}

Type *type_range(char *name, long long lo, long long hi) {
    Type *t = new_type(TY_INT);
    t->name = name;
    t->lo = lo;
    t->hi = hi;
    return t;
}

bool ty_is_range(Type *t) { return t && t->kind == TY_INT && t->name; }

const char *type_name(Type *t) {
    switch (t->kind) {
        // ★ 範囲型は名前で呼びます（'Percent' と出したい）
        case TY_INT: return t->name ? t->name : "int";
        case TY_BOOL: return "bool";
        case TY_FLOAT: return "float";
        case TY_NONE: return "None";
        case TY_STR: return "str";
        case TY_LIST: {
            // 注意: 動的に組み立てるので、返り値は毎回新しい文字列になります。
            //    解放しない方針（メモリモデル 3 節）なので問題ありません。
            StrBuf sb;
            sb_init(&sb);
            sb_printf(&sb, "list[%s]", type_name(t->elem));
            return sb_str(&sb);
        }
        case TY_CLASS: return t->name;
        case TY_IFACE: return t->name;
        case TY_ENUM: return t->name;
        case TY_TUPLE: {
            StrBuf sb;
            sb_init(&sb);
            sb_printf(&sb, "(");
            for (int i = 0; i < t->nparams; i++)
                sb_printf(&sb, "%s%s", i ? ", " : "", type_name(t->params[i]));
            sb_printf(&sb, ")");
            return sb_str(&sb);
        }
        case TY_PTR: {
            StrBuf sb;
            sb_init(&sb);
            sb_printf(&sb, "ptr[%s]", type_name(t->elem));
            return sb_str(&sb);
        }
        case TY_RC: {
            StrBuf sb;
            sb_init(&sb);
            sb_printf(&sb, "rc[%s]", type_name(t->elem));
            return sb_str(&sb);
        }
        case TY_THREAD: {
            StrBuf sb;
            sb_init(&sb);
            sb_printf(&sb, "Thread[%s]", type_name(t->elem));
            return sb_str(&sb);
        }
        case TY_MUTEX: {
            StrBuf sb;
            sb_init(&sb);
            sb_printf(&sb, "mutex[%s]", type_name(t->elem));
            return sb_str(&sb);
        }
        case TY_OPT: {
            StrBuf sb;
            sb_init(&sb);
            sb_printf(&sb, "%s | None", type_name(t->elem));
            return sb_str(&sb);
        }
        case TY_FN: {
            StrBuf sb;
            sb_init(&sb);
            sb_printf(&sb, "fn(");
            for (int i = 0; i < t->nparams; i++)
                sb_printf(&sb, "%s%s", i ? ", " : "", type_name(t->params[i]));
            sb_printf(&sb, ") -> %s", type_name(t->elem));
            return sb_str(&sb);
        }
        case TY_NULL: return "None";
        default: UNREACHABLE();
    }
}

Type *type_from_name(const char *name) {
    if (strcmp(name, "int") == 0) return ty_int;
    if (strcmp(name, "bool") == 0) return ty_bool;
    if (strcmp(name, "float") == 0) return ty_float;
    if (strcmp(name, "None") == 0) return ty_none;
    if (strcmp(name, "str") == 0) return ty_str;
    return NULL;  // 未知の型名
}

Type *type_from_kind(int kind) {
    switch (kind) {
        case TY_INT: return ty_int;
        case TY_BOOL: return ty_bool;
        case TY_FLOAT: return ty_float;
        case TY_NONE: return ty_none;
        case TY_STR: return ty_str;
        default: UNREACHABLE();
    }
}

const char *type_name_list(void) {
    return "int, float, bool, str, None, list[T], T | None, fn(...) -> T, (A, B)";
}
