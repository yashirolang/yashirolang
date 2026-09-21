// ast.h — 抽象構文木（AST）のノード定義
//
// 扱う範囲：リテラル・二項/単項演算・比較・論理演算・変数・代入・ブロック。
//
// 設計方針：全ノード種別を 1 つの構造体 Node で表します。
// 美しくはありませんが、C で共用体を安全に扱うより読みやすく、
// chibicc / tcc など実績ある小型 C コンパイラと同じ方式です。
// 詳細は docs/design/architecture.md 3.2 節。
#ifndef PLC_AST_H
#define PLC_AST_H

#include <stdbool.h>

#include "lexer.h"
#include "types.h"

typedef enum {
    ND_INT,      // 整数リテラル → ival
    ND_BOOL,     // 真偽値リテラル True / False → ival（0 / 1）
    ND_STR,      // 文字列リテラル → sval, slen
    ND_BINOP,    // 二項演算（比較を含む）→ op, lhs, rhs
    ND_LOGICAL,  // and / or     → op, lhs, rhs
                 // ★ 見た目は二項演算だが「右辺を評価しないことがある」ため
                 //   コード生成がまったく違う。だからノード種別を分ける。
                 //   ノード種別は「構文の形」ではなく「生成のしかた」で分ける。
    ND_UNARY,    // 単項演算（not を含む）→ op, lhs
    ND_VAR,      // 変数参照     → name
    ND_VARDECL,  // 変数宣言 x: T = e → name, type_name, rhs
    ND_ASSIGN,   // 代入 x = e   → lhs（ND_VAR）, rhs
    ND_BLOCK,    // 文のリスト   → body（next で連結）

    // ── 制御構文 ──
    ND_IF,        // if 文    → lhs（条件）, body（then）, els（else）
    ND_WHILE,     // while 文 → lhs（条件）, body
    ND_BREAK,     // break
    ND_CONTINUE,  // continue
    ND_PASS,      // pass（何もしない）
    ND_PRINT,     // print(e) → lhs
                  // 注意: 暫定。のちに本物の関数呼び出しに置き換わります。

    // ── 関数 ──
    ND_FUNC,    // 関数定義 → name, params, type_name（戻り型）, body
    ND_PARAM,   // 仮引数   → name, type_name
    ND_CALL,    // 呼び出し → name, args
    ND_RETURN,  // return   → lhs（値なしなら NULL）

    // ── list[T] ──
    ND_TYPEREF,  // 型注釈    → name, lhs（要素型の型注釈。無ければ NULL）
    ND_LIST,     // [1, 2, 3] → body（要素。next で連結）
    ND_INDEX,    // xs[i]     → lhs（対象）, rhs（添字）
    ND_METHOD,   // xs.f(...) → lhs（対象）, name, args

    // ── class ──
    ND_CLASS,      // クラス定義 → name, body（メンバの列）, cls
    ND_FIELDDECL,  // フィールド宣言 kind: int → name, type_ref
    ND_FIELD,      // フィールドアクセス t.kind → lhs（対象）, name, field

    // ── モジュール ──
    ND_IMPORT,  // import lexer → name（モジュール名）, mod（解決したモジュール）

    // ── nullable ──
    ND_NONE,  // None リテラル（ヌルポインタという「値」）

    // ── エラー処理 ──
    ND_TRY,     // try 文    → body（try の中身）, els（except の並び）
    ND_EXCEPT,  // except 節 → type_ref（エラー型）, name（as で束縛する名前）, body
    ND_RAISE,   // raise 文  → lhs（エラーオブジェクトの式）

    // ── 低レベル ──
    ND_UNSAFE,  // unsafe: ブロック → body
    ND_PRAGMA,  // pragma 文 → name（設定名）, sval（値。文字列のとき）

    // ── 追加していく ──

    // 注意: 追加は**末尾に**。selfhost/ast が値を明示しているためです。
    ND_FLOAT,   // 浮動小数点リテラル → sval（正規化済みの文字列。lexer.c 参照）
    ND_COND,    // 三項演算子 a if c else b → lhs（条件）, rhs（真）, els（偽）
    ND_SLICE,   // xs[a:b] → lhs（対象）, rhs（開始。省略時 NULL）, els（終端。省略時 NULL）
    ND_IFACE,   // interface 宣言 → name, body（本体の無い ND_FUNC の並び）
    ND_TUPLE,   // (a, b) → body（要素。next で連結）。
    ND_UNPACK,  // a, b = f() → params（受け取る名前の並び）, rhs。
    // ★ 内包表記 [E for x in xs if C]
    //   lhs  … 要素の式 E
    //   rhs  … 対象（list の式）。range のときは NULL
    //   args … range(...) の引数（range のときだけ。1〜3 個）
    //   els  … if の条件（省略時 NULL）
    //   body … 隠し宣言 3 つ（ループ変数 / 結果の list / 添字）。alloca はここから拾われる
    ND_LISTCOMP,
    // ★ scope: ブロック（A-18 の scoped spawn）→ body
    //   このブロックの中で始めたスレッドは、**出口で必ず join されます**。
    //   だから中の spawn には借りを渡せます（借りはこの呼び出しより
    //   長生きしないという不変条件が、成り立ったままになるため）。
    ND_SCOPE,

    // ── 範囲型（部分型。A-28）──
    //
    // ★ type Percent = int range(0, 100)
    //   name … 部分型の名前 / lhs … 下端（ND_INT）/ rhs … 上端（ND_INT）
    ND_RANGEDECL,
    // ★ 範囲の検査。**意味解析が入れます**（書く人は書きません）。
    //   lhs … 検査する式 / type … 入れ先の範囲型（両端はここから読む）
    //   注意: 値はそのまま素通しします。変わるのは「外れていたら止まる」ことだけ。
    ND_RANGECHK,

    // ── 契約（事前条件・事後条件。A-29）──
    //
    // ★ どちらも関数の本体の**先頭**に書く文です。
    //   lhs … bool の式
    ND_REQUIRES,  // requires <式>   … 関数の入口で確かめる
    ND_ENSURES,   // ensures  <式>   … return のたびに確かめる
    // ★ ensures の中でだけ使える名前 'result'（戻り値そのもの）。
    //   注意: 箱（alloca）は作りません。**いま返そうとしている値**を指すだけです
    //   （箱を作ると、所有型の戻り値で解放の釣り合いが崩れます）。
    ND_RESULT,

    // ── 列挙と場合分け（A-37）──
    //
    // ★ **中身を持つ枝はありません**（枝は名前だけ）。表現は i64 で、
    //   0 から順に番号を振ります。中身つき（代数的データ型）は、
    //   あとから枝に足しても既存のコードが壊れない形にしてあります。
    //
    //   enum Color:        → ND_ENUM   : name, body（ND_ENUMVAL の並び）, en
    //       Red            → ND_ENUMVAL: name, ival（振った番号）
    //       Green
    //
    //   match c:           → ND_MATCH  : lhs（調べる式）, body（ND_CASE の並び）
    //       case Color.Red:→ ND_CASE   : lhs（型・値。`_` なら NULL）, body
    //           ...
    ND_ENUM,
    ND_ENUMVAL,
    ND_MATCH,
    ND_CASE,
} NodeKind;

// 演算子の種類。
// トークンの文字列（"+"）ではなくこの enum で持つことで、
// コード生成の switch がコンパイラにチェックされるようになります。
typedef enum {
    // 二項
    OP_ADD,       // +
    OP_SUB,       // -
    OP_MUL,       // *
    OP_TRUEDIV,   // /   ← int には使えない（// を使う）。仕様 4.2
    OP_FLOORDIV,  // //
    OP_MOD,       // %
    OP_BITAND,    // &
    OP_BITOR,     // |
    OP_BITXOR,    // ^
    OP_SHL,       // <<
    OP_SHR,       // >>
    OP_POW,       // **  （ランタイムの pl_ipow を呼ぶ）

    // 比較（結果は bool）
    // 注意: この 6 つは連続して並べること。is_compare() が範囲で判定します。
    OP_EQ,  // ==
    OP_NE,  // !=
    OP_LT,  // <
    OP_LE,  // <=
    OP_GT,  // >
    OP_GE,  // >=

    // 論理（ND_LOGICAL。短絡評価する）
    OP_AND,  // and
    OP_OR,   // or

    // None との同一性比較（右辺は必ず None リテラル）
    OP_IS,      // is
    OP_ISNOT,   // is not

    // 単項
    OP_NEG,     // -x
    OP_POS,     // +x
    OP_BITNOT,  // ~x
    OP_NOT,     // not x

    // ★ 所属の検査。注意: **OP_EQ〜OP_GE の連続には入れません**
    //   （is_compare が enum の並びに依存しているため。ast.h 123 行の注意）
    OP_IN,      // x in xs
    OP_NOTIN,   // x not in xs
} OpKind;

// 比較演算子か。
// ★ enum の並び順に依存しています（OP_EQ 〜 OP_GE が連続していること）。
//   switch で 6 個並べるより短く、演算子を足したときの書き忘れも起きません。
static inline bool is_compare(OpKind op) { return OP_EQ <= op && op <= OP_GE; }

// 演算子の記号（エラーメッセージと --dump-ast 用）
const char *op_symbol(OpKind op);

typedef struct Node Node;

// ── クラス定義 ────────────────────────────────────────
//
// ★ types.h の Type は「どのクラスか」を struct Class * で指すだけです。
//   中身（フィールドの並び・サイズ）はここにあります。
//   sema が組み立て、codegen が読みます。

typedef struct Field Field;
struct Field {
    char *name;
    Type *type;
    int index;   // 構造体の何番目のフィールドか（getelementptr に渡す）
    int offset;  // 先頭から何バイト目か
                 // 注意: 読み書きには使いません（LLVM が index から計算する）。
                 //    自分のレイアウト計算が合っているかを確かめるための値です。
    Token *tok;  // 宣言位置（「最初の宣言はここです」用）
    Field *next;
};

// 意味解析が持つモジュールごとのシンボル表（sema.c で定義）
struct ModuleSyms;

typedef struct Class Class;
// ── インタフェース ──────────────────────────────────────
//
// ★ 「振る舞いだけ」を集めたものです。フィールドは持ちません。
//   実装は継承ではなく **宣言**（class C(I): と書く）で表します。
typedef struct IMethod IMethod;
struct IMethod {
    char *name;
    Node *sig;      // 本体の無い ND_FUNC（引数と戻り型を持つ）
    int slot;       // ★ プログラム全体で一意の番号（vtable の添字）
    IMethod *next;
};

typedef struct IfaceList IfaceList;
struct IfaceList {
    struct Iface *iface;
    IfaceList *next;
};

// enum の定義（A-37）。
//
// ★ 枝は**宣言した順**に 0 から番号を振ります。番号を書かせないのは、
//   書けると「番号を合わせるために枝を並べ替える」コードが生まれ、
//   枝の追加が怖い変更になるからです（この言語の enum は外との
//   やり取りのための番号ではありません）。
typedef struct EnumVal EnumVal;
struct EnumVal {
    char *name;
    long long val;
    Token *tok;
    EnumVal *next;
};

typedef struct EnumDef EnumDef;
struct EnumDef {
    char *name;
    Token *tok;
    EnumVal *vals;
    int nvals;
    struct Type *type;          // この定義に対応する TY_ENUM（1 個だけ作る）
    struct ModuleSyms *owner;
    EnumDef *next;
};

typedef struct Iface Iface;
struct Iface {
    char *name;
    char *ir_name;
    Token *tok;
    IMethod *methods;
    int nmethods;
    struct ModuleSyms *owner;
    Iface *next;
};

struct Class {
    char *name;
    char *ir_name;   // IR 上の修飾名（"lexer.Token"）。型は %lexer.Token.type
    Token *tok;      // 定義位置
    Field *fields;   // 宣言順
    int nfields;
    int size;        // インスタンス 1 個のバイト数（pl_alloc に渡す）
    int align;
    Type *type;      // このクラスの Type（★ クラスにつき 1 個だけ作る）
    Node *node;      // ND_CLASS（メソッドをたどるため）
    bool has_init;   // init メソッドを持つか

    // ★ このクラスが定義されているモジュール。
    //   メソッドは「定義されたモジュールの関数表」にいるので、
    //   使う側のモジュールから引くにはここをたどります。
    struct ModuleSyms *owner;

    // ★ この実体の元になったテンプレート（実体でなければ NULL）
    Class *from_template;

    // ★ この実体に渡された型引数（テンプレートなら NULL）。
    //   ジェネリック関数の型引数を **Box[T] のような入れ子から**決めるのに
    //   使います。これが無いと `def f[T](b: Box[T])` を呼べません。
    Type **targs;
    int ntargs;

    // ★ 実装するインタフェースの並び（無ければ NULL）。
    //   1 つでも実装していると、**先頭に隠しフィールド（vtable への
    //   ポインタ）が 1 つ増えます**。これがあるおかげで、クラスから
    //   インタフェースへの変換が「何もしない」で済みます。
    struct IfaceList *impls;

    Class *next;
};

// 仮引数の受け取り方（言語仕様 v2 §4）
//
// ★ 当初は構文として読むだけで、意味は与えませんでした。
//   検査が入るのは所有権検査です（docs/design/ownership.md §2）。
//
// なぜ Type ではなく引数に持たせるのか
//   Type はシングルトンで共有しているので、そこに所有の情報を足すと
//   「int 型」が場所ごとに別物になり、既存のポインタ比較が壊れます。
//   受け取り方は「型の性質」ではなく「その引数の性質」なので、ここに持ちます。
typedef enum {
    PM_BORROW,  // 既定。読むだけ借りる
    PM_MUT,     // mut。書き換えるために借りる
    PM_OWN,     // own。所有権を受け取る
} ParamMode;

// --dump-ast 用の接頭辞（"" / "mut " / "own "）
const char *param_mode_prefix(ParamMode mode);

struct Node {
    NodeKind kind;

    // このノードの代表トークン。
    // ★ エラー報告に必須なので、全ノードが必ず持ちます。
    //   「型が合いません」のようなエラーで位置を示せるかどうかは、
    //   これを最初から持たせているかで決まります。
    Token *tok;

    // ★ この式の型。**意味解析パス (sema.c) が埋めます。**
    //   構文解析の直後は必ず NULL です。
    //   codegen は sema が埋めたこの型を見て命令を選びます。
    Type *type;

    // ── 値 ──
    long long ival;  // ND_INT / ND_BOOL
    char *sval;      // ND_STR（エスケープ解決済み）
    int slen;        // ND_STR のバイト長
    OpKind op;       // ND_BINOP / ND_UNARY

    // 名前。
    //   ND_VAR / ND_VARDECL : 変数名
    char *name;

    // LLVM 上の名前（%x, %x.1, ...）。★ 意味解析パスが割り当てます。
    //
    // なぜ name をそのまま使わないのか
    //   当初は「シャドーイング禁止なので変数名は一意」でしたが、
    //   ブロックスコープが入ると兄弟スコープが同じ名前を使えます。
    //     if a:
    //         x: int = 1      ← %x
    //     if b:
    //         x: int = 2      ← %x（衝突！）
    //   どちらも相手を隠していないのでシャドーイングではありません。
    //   そこで sema が衝突しない名前を割り当てます（名前修飾の入口）。
    //
    // ★ 入口だったものが本番になりました。メソッドの定義（ND_FUNC）と
    //   呼び出し（ND_METHOD）、コンストラクタ（ND_CALL）には、
    //   sema が修飾名「Token.show」を入れます（記号 @ は付けない）。
    char *ir_name;

    // このノードがグローバル変数か。
    // グローバルは alloca せず、@g.x を直接読み書きします。
    bool is_global;

    // 型注釈の木（ND_VARDECL / ND_PARAM / ND_FUNC）。
    // ★ list[list[int]] のように入れ子になるので、文字列 1 個では表せません。
    Node *type_ref;

    // 型注釈が T | None か（ND_TYPEREF）
    bool nullable;

    // 仮引数の受け取り方（ND_PARAM）。
    // ★ 既定は PM_BORROW なので、new_node の calloc がそのまま初期値になります。
    ParamMode mode;

    // ── 解放（drop）のために ownck が書き込む記録 ──
    //
    // ★ builtin・cls と同じ形です。
    //   「どう扱うか」の判断は解析パスが済ませ、codegen は読むだけ。
    //   注意: --dump-ast には出しません（積み上げた AST 比較を壊さないため）。
    bool moved_out;     // ND_VAR : この参照で値が移動した（drop フラグを落とす）
    bool binds_borrow;  // ND_VARDECL : 借りものを束縛している（所有していない＝ drop しない）

    // ── エラー処理（raises / try / except）──
    //
    // ★ 失敗しうる呼び出しには、codegen が「タグを見て分岐する」コードを挿します。
    //   どの関数が失敗しうるかは sema が決めるので、その記録をここに置きます。
    Node *raises;     // ND_FUNC : raises 節に書かれた型注釈の並び（next で連結）
    bool can_fail;    // ND_CALL / ND_METHOD : 呼び出し先が raises 関数か
    int err_tag;      // ND_EXCEPT : 捕まえるエラー型の ID（sema が割り当てる）

    // 型注釈に書かれた名前（ND_TYPEREF）。
    // 「int」のような文字列で、sema が Type * に解決します。
    //
    // なぜ parser が Type * に解決しないのか
    //   名前から型への解決は「意味」の話であって「構文」の話ではありません。
    //   parser は「そこに識別子が書かれている」ことだけを記録し、
    //   それが有効な型かどうかの判断は sema に任せます。
    //   パスの責務を混ぜないための分離です。
    char *type_name;

    // ── 子ノード ──
    // 単項演算は lhs だけを使います（rhs は NULL）。
    Node *lhs, *rhs;

    // ND_BLOCK の中身（先頭の文）。以降は next でたどります。
    // ND_IF の then 節、ND_WHILE の本体もここです。
    Node *body;

    // ND_CALL が組み込み関数のとき、sema が選んだ候補。
    // ユーザー定義関数の呼び出しなら NULL。
    const struct Builtin_ *builtin;

    // ── class ──
    //
    // ★ builtin と同じ形です。「どう扱うか」の判断は sema が済ませ、
    //   codegen は書き込まれた記録を読むだけ。
    //     ND_CLASS : 自分のクラス定義
    //     ND_CALL  : NULL でなければ「インスタンス生成」（Token(1, "x")）
    Class *cls;

    // ★ 列挙の定義（A-37）。
    //     ND_ENUM  : 自分の enum 定義
    //     ND_MATCH : 調べる式が enum のとき、その定義（網羅検査で使う）
    EnumDef *en;

    // ND_FIELD が指すフィールド（sema が解決する）
    Field *field;

    // ── モジュール ──
    //
    // ★ 「'.' の左がモジュールだった」ことの記録（ND_FIELD / ND_METHOD）。
    //   sema が名前解決で判断し、codegen は読むだけ（builtin と同じ形）。
    //   ND_TYPEREF では型注釈に書かれた修飾（lexer.Token の "lexer"）を持ちます。
    char *mod_name;

    // 参照先が別のモジュールにあるか（codegen が declare / external を出す判断）
    bool is_extern;

    // ND_FUNC の仮引数リスト / ND_CALL の実引数リスト（next で連結）。
    Node *params;
    Node *args;

    // ★ この ND_VAR は「関数そのもの」を指しています
    //   （変数の読み出しではなく、@name をそのまま値にする）。
    bool is_func_ref;

    // ★ 関数ポインタ越しの呼び出し（ir_name はその変数の箱）
    bool is_indirect;

    // ★ min / max（2 引数なので組み込みの表に載らない）
    bool is_minmax;
    // ★ wrap_add / wrap_sub / wrap_mul（桁あふれを検査しない算術）。
    //   注意: 旗は**末尾に足します**（列挙値と同じ理由。selfhost 側と揃えるため）。
    bool is_wrap;

    // ★ print(list) / str(list)（要素の型で呼び分ける）
    bool is_list_str;

    // ★ move_out(場所)（所有権を取り出して、その場所は空にする）。
    //   注意: 旗は**末尾に足します**（selfhost 側と揃えるため）。
    bool is_move_out;

    // ★ この実引数は「借用」の仮引数へ渡されると **分かっている** か（A-21e）。
    //
    // 注意: **既定（false）が安全側**であることが大事です。
    //   分からない経路（メソッド呼び出しなど、仮引数のモードを
    //   codegen に渡していない経路）では false のままになり、
    //   codegen は解放しません。漏れるだけで、二重解放にはなりません。
    //   逆向きの旗（passed_by_own）にしていたときは、メソッドの own 引数を
    //   解放してしまい drop_fields / drop_lib_dict が二重解放で落ちました。
    bool arg_is_borrowed;

    // ★ この実引数は「own の仮引数」へ渡す **rc[T]** か（A-21 ⑬）。
    //
    //   rc[T] は移動しません（所有者を 1 つに決められないための型なので、
    //   ownck は共有として扱います）。だから own の仮引数へ渡すときは、
    //   **呼び出し側が参照を 1 つ増やして**渡します。受け取った側は
    //   出口でその 1 つを手放すので、差し引き 0 で釣り合います。
    //
    //   注意: 増やさないと、受け取った側の「出口で手放す」だけが残り、
    //     呼び出し側の分と合わせて**二重解放**になります。
    //     （bootstrap で stage1 が自分自身をコンパイル中に落ちました。）
    bool arg_own_rc;

    // ── 証明（A-34 段 1・2）が立てる印 ─────────────────────
    //
    // ★ **消す方向にしか使いません。** 解析が「分からない」と言えば印は
    //   立たず、今までどおり実行時に確かめます（保守的な既定）。
    //   注意: 印を 1 つ誤って立てると、そこはメモリ安全でなくなります。
    //     だから `--verify-prove` で「検査を残したまま、外れたら専用の
    //     診断で止める」形を用意してあります。
    bool no_ovf_check;    // ND_BINOP / ND_UNARY : 桁あふれしないと示せた
    bool no_range_check;  // ND_RANGECHK : 範囲に入ると示せた（A-28）
    bool no_bounds_check; // ND_INDEX : 添字が 0 以上・長さ未満と示せた
    bool no_contract;     // ND_REQUIRES / ND_ENSURES : 常に真と示せた（A-29）
    // ★ ND_BINOP（// と %）: 0 で割らず、しかも切り下げと切り捨てが
    //   一致すると示せた（両方とも 0 以上）。呼び出しではなく命令を出せます。
    bool no_div_check;

    // ★ この class が実装するインタフェースの並び（ND_TYPEREF）
    Node *ifaces;

    // ★ インタフェース越しの呼び出し（vtable の何番目か）
    bool is_iface_call;
    int iface_slot;

    // ★ ジェネリクス。
    //   ND_TYPEREF … 型引数の並び（Dict[str, int] の str と int）を targs に
    //   ND_CLASS   … 型引数の名前の並び（class Dict[K, V] の K と V）を targs に
    //                （ND_TYPEREF を name だけ使って並べます）
    Node *targs;

    // ND_WHILE の増分処理。for の脱糖でだけ使います。
    // ★ C の for(init; cond; incr) の incr にあたるもの。
    //   ここに置くと continue の飛び先を「増分」にできます。
    Node *incr;

    // ND_IF の else 節。
    // elif は「else の中の if」に脱糖するので、ND_BLOCK か ND_IF が入ります。
    Node *els;

    // ── 兄弟ノード ──
    //
    // ★ 文のリストは「配列」ではなく「next で繋いだ単方向リスト」にします。
    //
    //   トークンは配列にしたのに、なぜ文はリストなのか？
    //      トークンは任意の位置に O(1) でアクセスしたい（先読みのため）。
    //      文は「先頭から順に 1 回たどる」だけなので、リストで十分です。
    //      リストなら要素数を先に数える必要がなく、追加が簡単になります。
    Node *next;
};

// コンストラクタ
Node *new_node(NodeKind kind, Token *tok);
Node *new_int_node(Token *tok, long long value);
Node *new_float_node(Token *tok, char *text);
Node *new_bool_node(Token *tok, bool value);
Node *new_str_node(Token *tok, char *bytes, int len);
Node *new_binop_node(Token *tok, OpKind op, Node *lhs, Node *rhs);
Node *new_logical_node(Token *tok, OpKind op, Node *lhs, Node *rhs);
Node *new_unary_node(Token *tok, OpKind op, Node *operand);

// AST の深い複製（ジェネリクスの単相化）
Node *ast_clone(Node *n);

// ★ プログラム全体のインタフェース・メソッド数（vtable の長さ）。
//   注意: sema が数え、codegen が読みます。codegen の引数を増やさないための
//     割り切りです（引数を増やすと 2 つの実装の両方に手が要るため）。
extern int pl_iface_slots;
Node *new_var_node(Token *tok, char *name);

// AST を S 式で標準出力に表示する（--dump-ast 用）。
// テキストとして比較できる形にしておくのが重要です。
// セルフホスト版パーサの出力と diff するときに、この形式が正解になります。
void dump_ast(Node *node);

#endif  // PLC_AST_H
