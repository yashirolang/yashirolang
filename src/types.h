// types.h — 型の表現
//
// 扱う範囲：int / bool / None / str / list[T] / ユーザー定義クラス。
// float は将来、T | None（nullable）はのちに足します。
//
// 型付け規則の全体像は docs/spec/type-system.md にあります。
#ifndef PLC_TYPES_H
#define PLC_TYPES_H

#include <stdbool.h>

typedef enum {
    TY_INT,   // int  → i64
    TY_BOOL,  // bool → i1（メモリ上は i8。規約 R5）
    TY_NONE,  // None → void（値を返さない。メモリ上の表現は無い）
    TY_STR,   // str  → ptr（参照型）
    TY_LIST,  // list[T] → ptr（複合型）
    TY_CLASS, // ユーザー定義クラス → ptr（参照型）

    // ── nullable ──
    //
    // ★ 当初は Type に bool nullable を足す設計でした（type-system.md 2 節）。
    //   やめた理由は「書き忘れたときにどうなるか」です。フラグ方式だと
    //   `if (t->kind == TY_CLASS)` という既存の判定を Token | None が
    //   すり抜け、None のときに壊れます。種類を分ければ、判定を書き忘れた
    //   場所は「型 'Token | None' にフィールドはありません」で止まります。
    TY_OPT,   // T | None → ptr（elem が中身の型）
    TY_NULL,  // None リテラルの型。変数の型にはならない（代入互換だけで使う）

    // ── 共有所有 ──
    //
    // ★ rc[T] は「所有者を 1 つに決められない」データのための逃げ道です
    //   （仕様 v2 §7）。代入は**移動ではなく共有**（カウント +1）になります。
    TY_RC,    // rc[T] → ptr（ヒープに { strong, borrow, 中身へのポインタ }）

    // ── 低レベル（OS 開発向け）──
    //
    // ★ 生ポインタ。自動解放はしません（誰が解放するかは人間の責任。仕様 §10.2）。
    //   触れるのは unsafe: の中だけです。
    TY_PTR,   // ptr[T] → ptr

    // ⚠️ 追加は**末尾に**。selfhost/ast が値を明示しているので、
    //    途中に足すと 2 つの実装で番号がずれます。
    TY_FLOAT, // float → double（IEEE 754 倍精度）
    TY_FN,    // fn(A, B) -> C → ptr（関数へのポインタ）
    TY_IFACE, // interface → ptr（実体へのポインタ）
    TY_TUPLE, // (A, B) → ptr（要素を並べた構造体へのポインタ）

    // ── 並行実行（A-18）──
    //
    // ★ どちらも「中身の型」を elem に持ちます（list[T] / rc[T] と同じ場所）。
    //   Thread[R] の elem は **戻り型** R です（fn の elem と同じ使い方）。
    TY_THREAD,  // Thread[R] → ptr（ランタイムの PlThread への不透明なポインタ）
    TY_MUTEX,   // mutex[T]  → ptr（ランタイムの PlMutex への不透明なポインタ）

} TypeKind;

// クラス定義の実体は ast.h にあります（フィールドの並びとメソッドを持つため）。
// ★ 型そのものは「どのクラスか」を指せれば十分なので、ここでは前方宣言だけ。
struct Class;

typedef struct Type Type;
struct Type {
    TypeKind kind;

    // ── 関数型 fn(A, B) -> C ──
    // ★ 引数の型は個数が可変なので配列で持ちます。戻り型は elem に入れます
    //   （list[T] の要素型と同じ場所を使い回します）。
    Type **params;
    int nparams;

    // list[T] の要素型。fn では **戻り型**です。

    // ★ ここが埋まる型はシングルトンにできません（T ごとに違うため）。
    Type *elem;

    // ── class 用──
    char *name;         // クラス名（エラーメッセージ用）
    struct Class *cls;  // 定義への参照。★ 型の同一性はこのポインタで判定する
    struct Iface *iface;  // TY_IFACE のときの定義への参照

    // ──
    // この型の「T | None」版（1 個だけ作ってここに覚えておく）
    Type *opt;
};

// ★ プリミティブ型はシングルトン（起動時に 1 個だけ作る）。
//
// 🤔 なぜシングルトンにするのか
//   `int` 型のオブジェクトを毎回 xmalloc するのは無駄です。
//   1 個だけ作ってポインタを共有すれば、確保が減るうえに
//   「プリミティブ型どうしの比較はポインタ比較で済む」という利点も得られます。
//   ただし将来 list[int] のような複合型が入るので、
//   型の比較は必ず type_equal() を通します（== で直接比べない）。
extern Type *ty_int;
extern Type *ty_bool;
extern Type *ty_float;


extern Type *ty_none;
extern Type *ty_str;
extern Type *ty_null;  // None リテラルの型

// プリミティブ型のシングルトンを作る。main の最初に 1 回だけ呼ぶ。
void types_init(void);

// 2 つの型が同じか。
// list の要素型は再帰で比べます。
bool type_equal(Type *a, Type *b);

// エラーメッセージ用の型名（"int" など）
const char *type_name(Type *t);

// 型注釈の名前から型を引く。未知の名前なら NULL。
//   "int" → ty_int
Type *type_from_name(const char *name);

// TypeKind からシングルトンを引く（組み込み関数の表で使う）
Type *type_from_kind(int kind);

// list[T] を作る。
// ⚠️ シングルトンではありません。書かれた場所ごとに新しく作られるので、
//    型の比較は必ず type_equal() を通すこと。
Type *type_list(Type *elem);

// rc[T] を作る。list[T] と同じく、書かれた場所ごとに作ります。
Type *type_rc(Type *elem);

// ptr[T] を作る
Type *type_ptr(Type *elem);

// Thread[R] / mutex[T] を作る。rc[T] と同じ作り。
Type *type_thread(Type *ret);
Type *type_mutex(Type *elem);

// T | None を作る。
// ★ 同じ T に対しては 1 個だけ作ります（elem 側にキャッシュする）。
Type *type_opt(Type *elem);

// nullable にできる型か（参照型だけ。int | None は書けない）
bool type_can_be_opt(Type *t);

// T | None なら中身の型、そうでなければ自分自身を返す
Type *type_strip_opt(Type *t);

// S の値を T の場所に置けるか（代入互換性。type-system.md 4 節）。
//
// ★ ここが type_equal と分かれました。
//   T → T | None は許し、T | None → T は許しません（一方向）。
bool type_assignable(Type *from, Type *to);

// クラスの型を作る。
// ★ list[T] と違い、クラス定義ごとに 1 個だけ作ります
//   （`Token` と書かれた型注釈は、すべて同じ Type * を指す）。
Type *type_class(char *name, struct Class *cls);
Type *type_iface(char *name, struct Iface *i);
// クラスがインタフェースを実装しているかを返す関数。
// ⚠️ types.c は sema の表を知らないので、sema 側から差し込みます。
extern bool (*class_implements_hook)(struct Class *c, struct Iface *i);

// 値のバイト数とアラインメント（クラスのレイアウト計算に使う）。
// docs/design/memory-model.md 5 節の表がそのまま実装になっています。
int type_size(Type *t);
int type_align(Type *t);

// 型注釈に書ける名前の一覧（エラーメッセージのヒスト用）。
// 例: "int, bool"
const char *type_name_list(void);

#endif  // PLC_TYPES_H
