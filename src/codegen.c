#include "codegen.h"
#include "ownck.h"   // ty_is_owned（A-21e の一時値判別で使う）

#include <string.h>

#include "diag.h"
#include "langinfo.h"
#include "module.h"
#include "sema.h"

#include <stdio.h>

// ★ target triple の定義は langinfo.h に集約しました（--version でも使うため）。

// ── 出力バッファ ────────────────────────────────────────────
// IR は「後から前に戻って書き足したい」ことがあるため、
// 用途別のバッファに分けて最後に連結します。
// 例：関数本体の生成中に文字列リテラルを見つけたら globals に追記する。
typedef struct {
    StrBuf header;   // source_filename, target triple, 型定義
    StrBuf globals;  // グローバル変数・文字列定数
    StrBuf decls;    // declare（外部関数宣言）
    StrBuf body;     // 完成した関数定義

    // ── 生成中の関数用（関数ごとにリセットする）──
    //
    // ★ なぜ alloca を別バッファにするのか
    //   変数の alloca は AST を歩けば見つかりますが（collect_allocas）、
    //   短絡評価の結果を入れる %and.result.N は
    //   「ソースに現れない、コンパイラが自分で作る領域」です。
    //   生成してみて初めて必要だと分かるので、専用のバッファに溜めておき、
    //   最後に entry ブロックの先頭へまとめて差し込みます（規約 R1）。
    StrBuf allocas;  // entry ブロックに置く alloca
    StrBuf fn;       // 関数本体の命令列

    int tmp_counter;    // 一時値 %tN の連番
    int label_counter;  // ラベルの連番（同名ラベルの衝突を防ぐ）
    bool terminated;    // 現在の基本ブロックが終端命令を出力済みか（規約 R6）

    // 現在のループ（break / continue の飛び先）。
    struct LoopCtx *loop;

    // ── エラー処理 ──
    bool fn_raises;          // 生成中の関数が raises を宣言しているか
    Type *fn_ret;            // 生成中の関数の戻り型（失敗時の既定値に使う）
    bool err_slot;           // この関数で %err.slot を alloca 済みか
    bool err_type_emitted;   // %pl.err の型定義を出したか
    char prop_label[32];     // 伝播ブロックのラベル（err.propagate）
    bool prop_used;          // 伝播ブロックが使われたか（使われたときだけ出す）
    struct TryCtxG *try_ctx; // 今いる try（入れ子になるので鎖）

    // ── 整数の桁あふれ検査 ──
    bool no_ovf;            // --no-overflow-check（検査を出さない）

    // ── 解放（drop）──
    bool drop;              // --drop（解放を挿入するか）
    struct ScopeCtx *scope; // 今いるスコープ（出口で解放するものの一覧）
    StrBuf dropdefs;        // 生成した @drop.* の定義（モジュール末尾に出す）
    struct StrLit *dropfns; // 生成済みの @drop.* （型ごとに 1 つ）
    int drop_counter;       // @drop.list.N の連番

    // ── 添字の範囲をループの外で 1 回だけ確かめる ──
    bool nobc;               // 速い側の本体を生成中（境界検査を出さない）
    const char *iv_ir;       // その版分けの誘導変数（増分の桁あふれ検査も外す）
    const char *subst_ir;    // ガードの計算中だけ、この変数を…
    const char *subst_val;   //   …この値に読み替える

    // ── 並行実行（A-18）──
    StrBuf thunkdefs;        // 生成した @pl.thunk.* の定義（モジュール末尾に出す）
    struct StrLit *thunks;   // 生成済みの thunk（シグネチャごとに 1 つ）
    int thunk_counter;

    // 文字列リテラルの共有と declare の重複排除
    struct StrLit *strs;
    struct StrLit *decled;
    int str_counter;

    // ── デバッグ情報（A-30 / A-35）──
    //
    // ★ -g のときだけ、LLVM の debug metadata を出します。
    //   「行の対応表」と「関数の枠」でブレークポイント・バックトレース・
    //   perf の行表示が効き（A-30）、そこに「変数の名前と型」を足すと
    //   デバッガが中身を出せるようになります（A-35。`p x` / 変数一覧）。
    // ── 証明（A-34）──
    //
    // 注意: true なら、証明で「消せる」と判断した検査を**残します**。
    //   外れたときに専用の診断で止めるので、解析の誤りが CI で捕まります。
    bool verify_prove;
    // ★ いま出している検査が「証明では消せるはず」のものか（--verify-prove）。
    //   立っていれば、失敗したときに **証明器の誤り**として報告します。
    const char *verify_kind;

    bool dbg;               // -g
    StrBuf dbgmeta;         // metadata の定義（モジュール末尾に出す）
    StrBuf dbgdecl;         // 変数の宣言（entry ブロックの alloca の直後に置く。A-35）
    int dbg_next;           // 次に振る metadata の番号
    int dbg_file;           // !DIFile の番号
    int dbg_cu;             // !DICompileUnit の番号
    int dbg_fn;             // 生成中の関数の !DISubprogram の番号（0 なら関数の外）
    int dbg_line;           // いま生成している文の行
    int dbg_loc;            // その行の !DILocation の番号（0 なら未作成）
    int dbg_loc_line;       // dbg_loc が指している行
    struct DbgTy *dbgtys;   // 型ごとの metadata（同じ型に 2 つ作らないため。A-35）

    // ── 契約（A-29）──
    Node *fn_node;          // 生成中の関数（ensures の式を引くのに使う）
    const char *ret_val;    // いま返そうとしている値（ensures の 'result'）

    // ── モジュール ──
    Node *ast;             // 今生成しているモジュールの AST
    struct StrLit *types;  // 型定義を出済みのクラス（重複排除）
    struct StrLit *vtables;  // vtable を出済みのクラス
} Emitter;

// 出力済みの文字列リテラル / declare を覚えておくための小さなリスト。
typedef struct StrLit StrLit;
struct StrLit {
    char *bytes;
    int len;
    char *label;
    StrLit *next;
};

// 型ごとのデバッグ情報を覚えておくための小さなリスト（A-35）。
//
// ★ 同じ型の metadata を 2 つ作ると、デバッガからは **別の型**に見えます
//   （lldb が型を突き合わせるのは名前ではなく metadata の同一性です）。
//   型の名前（"list[int]" / "lexer.Token"）で引いて、1 つだけ作ります。
typedef struct DbgTy DbgTy;
struct DbgTy {
    char *key;   // 引くための名前（種類ごとに接頭辞を付ける。下の dbg_type）
    int id;      // その型の metadata 番号
    DbgTy *next;
};

// break / continue の飛び先（規約 6.5）。
//
// ★ スタック変数として持つのがポイントです。gen_while の呼び出しがネストすれば、
//   C の呼び出しスタックがそのままループのネストになります。
//   自前でスタック構造を作る必要はありません。
typedef struct LoopCtx LoopCtx;
struct LoopCtx {
    LoopCtx *outer;
    const char *break_label;     // while.end.N
    const char *continue_label;  // while.cond.N
    struct ScopeCtx *scope;      // ループに入ったときのスコープ
};

// try 1 つぶんの飛び先。
//
// ★ LoopCtx と同じ形です。失敗した呼び出しは、内側の try の振り分けへ飛びます。
//   その try が捕まえない型なら、振り分けの最後で外側の飛び先へ落ちます。
typedef struct TryCtxG TryCtxG;
struct TryCtxG {
    TryCtxG *outer;
    Node *node;              // ND_TRY
    const char *dispatch;    // try.dispatch.N（失敗したときの飛び先）
    struct ScopeCtx *scope;  // try に入ったときのスコープ（解放の巻き戻しに使う）
};

// ── スコープ ──────────────────────────────────────────
//
// ★ LoopCtx と同じで、C の呼び出しスタックにそのまま乗せます。
//   ブロックに入ったら push、出るときに **宣言と逆順**で解放します。
typedef struct DropEnt DropEnt;
struct DropEnt {
    Node *decl;      // ND_VARDECL / ND_PARAM（ir_name と type を持っている）
    DropEnt *next;   // ★ 先頭に足すので、たどると自然に「逆順」になります
};

typedef struct ScopeCtx ScopeCtx;
struct ScopeCtx {
    ScopeCtx *outer;
    DropEnt *ents;
};

// ── エラー処理の道具（実体は下のほうにあります）──
static void ensure_err_type(Emitter *e);
static const char *err_slot(Emitter *e);
static const char *fail_label(Emitter *e);
static void emit_fail_br(Emitter *e);
static const char *default_value(Type *t);
static void store_err(Emitter *e, const char *slot, int tag, const char *obj);
static char *load_tag(Emitter *e, const char *slot);
static char *load_payload(Emitter *e, const char *slot);
static void emit_drops_until(Emitter *e, struct ScopeCtx *stop);
static void emit_default_ret(Emitter *e);
static char *deref_rc(Emitter *e, Type *t, char *v);
static char *maybe_retain(Emitter *e, Node *rhs, char *val);
static char *force_retain(Emitter *e, char *val);

// 新しい一時値の名前を返す（"%t0", "%t1", ...）
//
// 注意: 規約 R4：必ず英字始まりの名前にします。
//    %0 のような数値名を自分で使うと、LLVM の暗黙採番と衝突して
//    "instruction expected to be numbered '%N'" という分かりにくい
//    エラーになります。
// 注意: 名前に '.' を入れます。
//    それまでは "%tN" でしたが、利用者が `t0` という変数を書くと
//    IR 上で衝突しました（stage1 の移植中に踏んだ実際のバグ）。
//
//        %t0 = alloca ptr      ← 利用者の変数 t0
//        %t0 = load ptr, ...   ← コンパイラの一時値
//
//    利用者の識別子に '.' は入れられないので、'.' を含む名前にすれば
//    衝突は原理的に起きません（脱糖が作る隠し変数 for.ix.0 と同じ手口）。
static char *new_tmp(Emitter *e) {
    char *buf = xmalloc(24);
    snprintf(buf, 24, "%%t.%d", e->tmp_counter++);
    return buf;
}

// 値（レジスタ）としての LLVM 型。
//
// ★ 「値の型」と「メモリの型」を別の関数にするのがここの要点です。
//   1 つの関数で済ませようとすると、呼び出し側ごとに
//   「今はどっちの意味か」を考えることになり、必ず間違えます。
static const char *llvm_type(Type *t) {
    switch (t->kind) {
        case TY_INT: return "i64";
        case TY_FLOAT: return "double";  // IEEE 754 倍精度
        case TY_FN: return "ptr";        // 関数へのポインタ
        case TY_IFACE: return "ptr";     // 実体へのポインタ
        case TY_TUPLE: return "ptr";     // 構造体へのポインタ
        case TY_BOOL: return "i1";   // レジスタ上は 1 ビット
        case TY_NONE: return "void";  // 値がない
        case TY_STR: return "ptr";    // 参照型
        case TY_LIST: return "ptr";   // PlList へのポインタ
        case TY_CLASS: return "ptr";  // インスタンスへのポインタ
        case TY_OPT: return "ptr";    // T | None。None は null
        case TY_RC: return "ptr";     // rc[T]
        case TY_PTR: return "ptr";    // ptr[T]（生ポインタ）
        case TY_THREAD: return "ptr"; // Thread[R]（PlThread への不透明な参照）
        case TY_MUTEX: return "ptr";  // mutex[T]（PlMutex への不透明な参照）
        // 列挙（A-37）。枝に 0 から番号を振っただけなので i64 です。
        // ★ ただし**中身を持つ枝**があるときは、枝の隠しクラスへの
        //   ポインタになります（A-41）。
        case TY_ENUM: return t->en && t->en->has_payload ? "ptr" : "i64";
        case TY_NULL: return "ptr";   // None リテラル
        default: UNREACHABLE();
    }
}

// メモリ（alloca / load / store）としての LLVM 型。
//
// 注意: 規約 R5：bool はメモリ上 i8。
//    alloca i1 も合法ですが、実際には 1 バイト確保され残り 7 ビットが未定義に
//    なります。C ランタイム連携で困るので、i8 に揃えておきます。
static const char *llvm_mem_type(Type *t) {
    switch (t->kind) {
        case TY_INT: return "i64";
        case TY_FLOAT: return "double";
        case TY_FN: return "ptr";
        case TY_IFACE: return "ptr";
        case TY_TUPLE: return "ptr";
        case TY_BOOL: return "i8";  // メモリ上は 1 バイト
        case TY_STR: return "ptr";  // ポインタをそのまま置く
        case TY_LIST: return "ptr";
        case TY_CLASS: return "ptr";
        case TY_OPT: return "ptr";
        case TY_RC: return "ptr";     // 数え札付きの箱へのポインタ
        case TY_PTR: return "ptr";
        case TY_THREAD: return "ptr";
        case TY_MUTEX: return "ptr";
        // ★ 列挙はただの i64 です（A-37）。枝に 0 から番号を振っただけ。
        //   中身を持つ枝があるときはポインタです（A-41）。
        case TY_ENUM: return t->en && t->en->has_payload ? "ptr" : "i64";
        // 注意: TY_NONE はメモリ上の表現を持ちません。
        //    ここに来たら「None の変数を作ろうとしている」= コンパイラのバグ。
        default: UNREACHABLE();
    }
}

// ★ 変数の IR 名は sema が「記号まで含めた完全な形」で割り当てます。
//    ローカル  : %x, %x.1
//    グローバル: @g.x
//    引数      : %n（%n.arg から alloca にコピーしたもの。規約 R8）
//    codegen 側で名前を組み立てる必要はもうありません（var_ptr は廃止）。

// 二項演算子に対応する LLVM 命令の名前を返す。
//
// 注意: int は符号付きなので、必ず 's' の付く命令を使います。
//    sdiv / srem / ashr（udiv / urem / lshr ではない）。
//    間違えると負数で誤った結果になります。
static const char *llvm_binop(Node *n) {
    // ★ float は別命令です。整数の add / sub / mul / sdiv とは
    //   ビット列の意味がまるで違うので、LLVM も別の命令を用意しています。
    //   （fadd などに 's' が付かないのは、符号が指数部と仮数部に
    //     分かれていて「符号付き/なし」の区別が要らないためです）
    if (n->lhs && n->lhs->type && n->lhs->type->kind == TY_FLOAT) {
        switch (n->op) {
            case OP_ADD: return "fadd";
            case OP_SUB: return "fsub";
            case OP_MUL: return "fmul";
            case OP_TRUEDIV: return "fdiv";
            default: UNREACHABLE();  // OP_POW はランタイム呼び出しになる
        }
    }
    switch (n->op) {
        case OP_ADD: return "add";
        case OP_SUB: return "sub";
        case OP_MUL: return "mul";
        // OP_FLOORDIV / OP_MOD / OP_POW はここに来ません。
        // ★ 「0 除算・負の指数を検査する」ためにランタイム関数
        //   （pl_floordiv / pl_mod / pl_ipow）の呼び出しに変わりました（規約 R10）。
        case OP_BITAND: return "and";
        case OP_BITOR: return "or";
        case OP_BITXOR: return "xor";
        case OP_SHL: return "shl";
        case OP_SHR: return "ashr";  // 算術シフト（符号を保つ）

        // OP_TRUEDIV はここに来ません。
        // ★ 当初はこの関数で弾いていましたが、のちに意味解析パスを
        //   作ったので、本来の担当である sema.c へ移しました。
        //   コード生成器は「検査済みの正しい AST」だけを受け取る、
        //   という役割分担がここで確立します。
        default:
            UNREACHABLE();
    }
}

// 比較演算子に対応する icmp の述語。
//
// 注意: 落とし穴：i1 の符号付き比較は逆になる
//    i1 を 2 の補数で解釈すると True(1) は -1 です。
//    icmp slt i1 0, 1 は「0 < -1」を聞くことになり False になります。
//    そのため bool の大小比較は符号なし（ult など）を使います。
//    eq / ne には符号が無いので影響しません。
static const char *icmp_pred(OpKind op, Type *operand_type) {
    bool sign = operand_type->kind == TY_INT;  // int は符号付き
    switch (op) {
        case OP_EQ: return "eq";
        case OP_NE: return "ne";
        case OP_LT: return sign ? "slt" : "ult";
        case OP_LE: return sign ? "sle" : "ule";
        case OP_GT: return sign ? "sgt" : "ugt";
        case OP_GE: return sign ? "sge" : "uge";
        default: UNREACHABLE();
    }
}

// float の比較述語（ordered。NaN が絡むと必ず False）。
static const char *fcmp_pred(OpKind op) {
    switch (op) {
        case OP_EQ: return "oeq";
        // 注意: '!=' だけ **unordered**（une）です。
        //    a != b は「a == b ではない」と定義されるので、NaN が絡むと
        //    == が False → != は True でなければなりません。
        //    ここを one（ordered）にすると nan != nan が False になり、
        //    NaN の判定（x != x）が使えなくなります。
        case OP_NE: return "une";
        case OP_LT: return "olt";
        case OP_LE: return "ole";
        case OP_GT: return "ogt";
        case OP_GE: return "oge";
        default: UNREACHABLE();
    }
}

// ── メモリとレジスタの境界（規約 R5）──────────────────────
//
// ★ zext / trunc はこの 2 つの関数の中だけに閉じ込めます。
//   他の場所には 1 つも現れません。

// ── 別名解析のタグ（TBAA） ─────────────────────────────
//
// ★ LLVM は既定で「ポインタはどれも同じ場所を指すかもしれない」と考えます。
//   そのため `c.data[i] = v` を書くと、その後の `self.cols` の読み出しが
//   **書き換わったかもしれない**と見なされ、ループの外に出せません。
//   実際、桁あふれ検査を入れたあとの行列積の内側ループには `self.cols` の
//   読み直しが残っていました。
//
// ★ 重ならない 3 種類に名前を付けて、LLVM に「別物だ」と言い切ります。
//
//   field    … クラスのフィールド
//   listhdr  … list のヘッダ（data ポインタと len）
//   listelem … list の要素
//
//   健全性：`pl_list_new` はヘッダと要素配列を**別々に確保**し
//   （runtime/core.c）、クラスのオブジェクトもまた別の確保です。
//   この 3 つは決して重なりません。
//
// 注意: **タグを付けないアクセスは「何とでも別名かもしれない」**と扱われます。
//   ローカル変数・グローバル・文字列・`unsafe:` の生ポインタ・`extern` には
//   付けません。**付けないほうが安全側**です。
#define TBAA_NONE ""
#define TBAA_FIELD ", !tbaa !4"
#define TBAA_LISTHDR ", !tbaa !5"
#define TBAA_LISTELEM ", !tbaa !6"

// ★ グローバル変数にもタグを付けます（当初の宿題②）。
//
// なぜ付けるのか
//   `while i < N:` の N がグローバルだと、LLVM は「本体の書き込みが N を
//   書き換えるかもしれない」と見て、**毎回読み直します**。すると
//   ループ回数が不定になり（uncountable loop）、ベクトル化できません。
//   実測：512³ の行列積で 109 ms 対 41 ms（上限を局所変数に写した場合）。
//
// 注意: 健全性：グローバルは .data にあり、`pl_alloc` が返すヒープの
//   オブジェクト（field / listhdr / listelem）とは**決して重なりません**。
//   だから別の兄弟にしてよいのです。注意: グローバルどうしは同じタグなので、
//   互いに別名かもしれない、と扱われます（安全側）。
//   注意: `unsafe:` の生ポインタはタグ無しのままです。タグ無しは
//   「何とでも別名かもしれない」なので、こちらも安全側です。
#define TBAA_GLOBAL ", !tbaa !8"

// メモリから読む：bool なら i8 → i1 に縮める
static char *gen_load_tb(Emitter *e, Type *ty, const char *ptr, const char *tb) {
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = load %s, ptr %s%s\n", t, llvm_mem_type(ty), ptr, tb);
    if (ty->kind != TY_BOOL) return t;

    char *t2 = new_tmp(e);
    sb_printf(&e->fn, "  %s = trunc i8 %s to i1\n", t2, t);
    return t2;
}

// ★ '@' で始まる場所はグローバルです（sema が付ける IR 名の約束）。
static const char *tbaa_for(const char *ptr) {
    return ptr && ptr[0] == '@' ? TBAA_GLOBAL : TBAA_NONE;
}

static char *gen_load(Emitter *e, Type *ty, const char *ptr) {
    return gen_load_tb(e, ty, ptr, tbaa_for(ptr));
}

// メモリへ書く：bool なら i1 → i8 に広げる
static void gen_store_tb(Emitter *e, Type *ty, const char *val, const char *ptr,
                         const char *tb) {
    if (ty->kind == TY_BOOL) {
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = zext i1 %s to i8\n", t, val);
        val = t;
    }
    sb_printf(&e->fn, "  store %s %s, ptr %s%s\n", llvm_mem_type(ty), val, ptr, tb);
}

static void gen_store(Emitter *e, Type *ty, const char *val, const char *ptr) {
    gen_store_tb(e, ty, val, ptr, tbaa_for(ptr));
}

// ── 基本ブロック（規約 R6 / R7）────────────────────────────
//
// 注意: IR にフォールスルーはありません。「次のブロックに続くだけ」でも
//    br label %next が必要です。初心者が最もよくハマる落とし穴です。
//
// ★ 「終端したか」を追跡する変数を 1 つ持つだけで、
//   「br を書き忘れた」も「終端の後に命令を置いた」も起きなくなります。
//   if / while と return は、この 3 つの関数の上に載ります。

// ラベルを出力する。直前のブロックが終端していなければ暗黙のジャンプを補う。
static void emit_label(Emitter *e, const char *label) {
    if (!e->terminated) sb_printf(&e->fn, "  br label %%%s\n", label);
    sb_printf(&e->fn, "%s:\n", label);
    e->terminated = false;
}

static void emit_br(Emitter *e, const char *label) {
    sb_printf(&e->fn, "  br label %%%s\n", label);
    e->terminated = true;
}

static void emit_cond_br(Emitter *e, const char *cond, const char *then_l,
                         const char *else_l) {
    sb_printf(&e->fn, "  br i1 %s, label %%%s, label %%%s\n", cond, then_l, else_l);
    e->terminated = true;
}

// 終端済みのブロックの後ろにコードを置く必要が出たら、
// 到達不能ブロックのラベルを作る（規約 R7）。
//
//     while True:
//         break
//         print(1)     ← 到達不能。ラベルが無いと命令を置けない
static void ensure_block(Emitter *e) {
    if (!e->terminated) return;
    char l[24];
    snprintf(l, sizeof(l), "dead.%d", e->label_counter++);
    emit_label(e, l);  // 終端済みなので br は補われない
}

// ── 文字列リテラル ───────────────────────────────────
//
// ★ 同じ内容のリテラルは 1 つにまとめます（線形探索で十分）。

// IR の文字列に 1 バイト出力する。
//
// 注意: 安全策として、ASCII 印字可能文字**以外はすべて** \XX にします。
//    「どの文字をエスケープすべきか」を考えなくて済むようにするためです。
//    UTF-8 の日本語も各バイトが \XX になるだけで、そのまま通ります。
static void emit_ir_byte(StrBuf *sb, unsigned char c) {
    if (c >= 0x20 && c < 0x7F && c != '"' && c != '\\')
        sb_printf(sb, "%c", c);
    else
        sb_printf(sb, "\\%02X", c);
}

// A-21e: 式の途中の一時値を解放する（定義は下の方。宣言だけ先に置く）
static void drop_temp(Emitter *e, Node *n, const char *val);
static bool is_owned_temp(Node *n);
static void emit_drop_value(Emitter *e, Type *t, const char *val);

// ── デバッグ情報（A-30）──────────────────────────────────
//
// ★ metadata の番号は **0〜8 を TBAA が使っている**ので 10 から振ります
//   （9 は空けておきます。番号は 2 実装で必ず同じになります）。
static int dbg_id(Emitter *e) { return e->dbg_next++; }

// ★ 下の 2 つはこの後ろで定義しています（ここでは名前だけ先に知らせます）。
static int dbg_location(Emitter *e);
static void declare_rt(Emitter *e, const char *sig);

// ── デバッグ情報：型（A-35）──────────────────────────────
//
// ★ 変数の中身を見せるには「その変数が何型か」を DWARF に書きます。
//   出す形は 3 通りだけです。
//
//     int / float / bool  → !DIBasicType（そのまま数として出ます）
//     str                 → char へのポインタ（デバッガが "..." で出します）
//     list / クラス / rc  → 構造体へのポインタ（中身を開いて見られます）
//
//   注意: **参照型はポインタのまま**にしています。`p xs` は番地を出し、
//     `p *xs` で中身が出ます。デリファレンスして見せることもできますが、
//     `T | None` が None のときに「読めません」としか出せなくなります。
//     番地が出るほうが、共有（別名）を追うときにも役に立ちます。
//
//   ★ 名前は typedef で付けます（`!DIDerivedType(tag: DW_TAG_typedef)`）。
//     ポインタ自体に名前を付けると、lldb が str を文字列として出さなく
//     なりました。typedef なら `(str) msg = 0x... "hello"` と両方出ます。

static int dbg_ty_cached(Emitter *e, const char *key) {
    for (DbgTy *d = e->dbgtys; d; d = d->next)
        if (strcmp(d->key, key) == 0) return d->id;
    return 0;
}

static void dbg_ty_put(Emitter *e, const char *key, int id) {
    DbgTy *d = xmalloc(sizeof(DbgTy));
    d->key = xstrndup(key, strlen(key));
    d->id = id;
    d->next = e->dbgtys;
    e->dbgtys = d;
}

// 数そのものの型（int / float / bool）
static int dbg_basic(Emitter *e, const char *key, const char *name, int bits,
                     const char *enc) {
    int hit = dbg_ty_cached(e, key);
    if (hit) return hit;
    int id = dbg_id(e);
    sb_printf(&e->dbgmeta,
              "!%d = !DIBasicType(name: \"%s\", size: %d, encoding: %s)\n",
              id, name, bits, enc);
    dbg_ty_put(e, key, id);
    return id;
}

// base へのポインタ（base が 0 なら「何を指すか分からないポインタ」）
static int dbg_ptr_to(Emitter *e, const char *key, int base) {
    int hit = dbg_ty_cached(e, key);
    if (hit) return hit;
    int id = dbg_id(e);
    if (base)
        sb_printf(&e->dbgmeta,
                  "!%d = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !%d, "
                  "size: 64)\n", id, base);
    else
        // 注意: baseType は省けません（LLVM が missing required field で止まります）。
        //   「何を指すか分からないポインタ」は baseType: null と書きます。
        sb_printf(&e->dbgmeta,
                  "!%d = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: null, "
                  "size: 64)\n", id);
    dbg_ty_put(e, key, id);
    return id;
}

// 構造体の 1 メンバ（名前・型・先頭からのビット位置）
static int dbg_member(Emitter *e, const char *name, int line, int base, int bits,
                      int off_bits) {
    int id = dbg_id(e);
    sb_printf(&e->dbgmeta,
              "!%d = !DIDerivedType(tag: DW_TAG_member, name: \"%s\", file: !%d, "
              "line: %d, baseType: !%d, size: %d, offset: %d)\n",
              id, name, e->dbg_file, line, base, bits, off_bits);
    return id;
}

// 型に名前を付ける（`(str) msg = ...` の "str"）
static int dbg_typedef(Emitter *e, const char *name, int base) {
    int id = dbg_id(e);
    sb_printf(&e->dbgmeta,
              "!%d = !DIDerivedType(tag: DW_TAG_typedef, name: \"%s\", "
              "baseType: !%d)\n", id, name, base);
    return id;
}

static int dbg_type(Emitter *e, Type *t);

// list の 1 要素ぶんの型。
//
// 注意: list の要素は**どの型でも 8 バイト**です（規約。runtime/core.c）。
//   bool だけは変数の箱の中では 1 バイトなので、そのまま書くと
//   `xs[1]` がとなりの要素を読みます。8 バイトの int として出します。
//
// ★ 8 バイトの bool（`DW_ATE_boolean` の 8 バイト）にはしませんでした。
//   デバッガが「C にそんな型は無い」と言って添字を断ります
//   （`error: subscript of pointer to incomplete type`）。
//   `list[bool]` の要素は **0 / 1 の int** として出ます。
static int dbg_slot_type(Emitter *e, Type *t) {
    if (t && t->kind == TY_BOOL) return dbg_type(e, ty_int);
    return dbg_type(e, t);
}

// 型 1 つぶんの metadata を作って番号を返す（同じ型なら作り直しません）。
static int dbg_type(Emitter *e, Type *t) {
    if (!t || t->kind == TY_NONE) return 0;

    // ★ `T | None` は T と同じ見え方にします（表現はどちらも同じポインタで、
    //   None は null です）。型の名前だけが違う別の metadata を作ると、
    //   デバッガの中で `Token` と `Token | None` が別の型になってしまいます。
    if (t->kind == TY_OPT) return dbg_type(e, t->elem);

    // 引くための名前。注意: クラスは**モジュール修飾した名前**で区別します
    //   （別のモジュールに同じ名前のクラスがあります）。
    const char *key = (t->kind == TY_CLASS && t->cls) ? t->cls->ir_name
                                                      : type_name(t);
    int hit = dbg_ty_cached(e, key);
    if (hit) return hit;

    switch (t->kind) {
        // ── 数そのもの ──
        //
        // 注意: **typedef で名前を付け直します。** デバッガは
        //   `!DIBasicType` の名前を見ずに、大きさと符号から自分の言葉に
        //   直してしまうためです（int が `(long)`、float が `(double)` と
        //   出ていました）。typedef の名前はそのまま出ます。
        //
        // ★ 範囲型（A-28）は「名前の付いた int」です。`(Percent) rate = 40`
        //   と、書いたとおりの名前で出ます。
        case TY_INT: {
            int b = dbg_basic(e, "b:i64", "i64", 64, "DW_ATE_signed");
            int td = dbg_typedef(e, type_name(t), b);
            dbg_ty_put(e, key, td);
            return td;
        }
        case TY_FLOAT: {
            int b = dbg_basic(e, "b:f64", "f64", 64, "DW_ATE_float");
            int td = dbg_typedef(e, "float", b);
            dbg_ty_put(e, key, td);
            return td;
        }
        case TY_BOOL: {
            int b = dbg_basic(e, "b:bool", "bool", 8, "DW_ATE_boolean");
            int td = dbg_typedef(e, "bool", b);
            dbg_ty_put(e, key, td);
            return td;
        }

        // ── str ──
        // ★ 値が指すのは「バイト列の先頭」で、そこは NUL 終端です
        //   （長さは 8 バイト手前。runtime/core.c）。char へのポインタだと
        //   書いておけば、デバッガがそのまま "hello" と出します。
        //   注意: もとの型の名前は "char" にします。デバッガが「文字列として
        //     出してよい」と判断する手がかりがここだからです（"u8" と
        //     名づけたときは番地しか出ませんでした）。
        case TY_STR: {
            int ch = dbg_basic(e, "b:char", "char", 8, "DW_ATE_signed_char");
            int p = dbg_ptr_to(e, "*char", ch);
            int td = dbg_typedef(e, "str", p);
            dbg_ty_put(e, key, td);
            return td;
        }

        // ── list[T] ──
        //   PlList { void *data; long long len; long long cap; }（runtime/core.c）
        case TY_LIST: {
            // 注意: 先に番号を取り、**中身を作る前に**表へ入れます。
            //   list[Node] のように自分を含む型で無限に回らないためです。
            int sid = dbg_id(e), pid = dbg_id(e), tid = dbg_id(e);
            dbg_ty_put(e, key, tid);

            int el = dbg_slot_type(e, t->elem);
            StrBuf ek;
            sb_init(&ek);
            sb_printf(&ek, "*slot:%s", type_name(t->elem));
            int ep = dbg_ptr_to(e, sb_str(&ek), el);
            int i64 = dbg_type(e, ty_int);

            int m0 = dbg_member(e, "data", 0, ep, 64, 0);
            int m1 = dbg_member(e, "len", 0, i64, 64, 64);
            int m2 = dbg_member(e, "cap", 0, i64, 64, 128);
            int els = dbg_id(e);
            sb_printf(&e->dbgmeta, "!%d = !{!%d, !%d, !%d}\n", els, m0, m1, m2);
            sb_printf(&e->dbgmeta,
                      "!%d = !DICompositeType(tag: DW_TAG_structure_type, "
                      "name: \"%s\", file: !%d, size: 192, elements: !%d)\n",
                      sid, key, e->dbg_file, els);
            sb_printf(&e->dbgmeta,
                      "!%d = !DIDerivedType(tag: DW_TAG_pointer_type, "
                      "baseType: !%d, size: 64)\n", pid, sid);
            sb_printf(&e->dbgmeta,
                      "!%d = !DIDerivedType(tag: DW_TAG_typedef, name: \"%s\", "
                      "baseType: !%d)\n", tid, key, pid);
            return tid;
        }

        // ── クラス ──
        //   ★ 並びは sema の layout_class が決めています（f->offset）。
        //     インタフェースを実装するクラスは**先頭に隠しフィールド**
        //     （vtable へのポインタ）があり、offset にはそれが入っています。
        case TY_CLASS: {
            Class *c = t->cls;
            if (!c) return dbg_ptr_to(e, "*void", 0);
            int sid = dbg_id(e), pid = dbg_id(e), tid = dbg_id(e);
            dbg_ty_put(e, key, tid);

            StrBuf mem;
            sb_init(&mem);
            bool first = true;
            for (Field *f = c->fields; f; f = f->next) {
                int ft = dbg_type(e, f->type);
                if (!ft) continue;
                int mid = dbg_member(e, f->name, f->tok ? f->tok->line : 0, ft,
                                     type_size(f->type) * 8, f->offset * 8);
                sb_printf(&mem, "%s!%d", first ? "" : ", ", mid);
                first = false;
            }
            int els = dbg_id(e);
            sb_printf(&e->dbgmeta, "!%d = !{%s}\n", els, sb_str(&mem));
            sb_printf(&e->dbgmeta,
                      "!%d = !DICompositeType(tag: DW_TAG_structure_type, "
                      "name: \"%s\", file: !%d, line: %d, size: %d, elements: !%d)\n",
                      sid, c->name, e->dbg_file, c->tok ? c->tok->line : 0,
                      c->size * 8, els);
            sb_printf(&e->dbgmeta,
                      "!%d = !DIDerivedType(tag: DW_TAG_pointer_type, "
                      "baseType: !%d, size: 64)\n", pid, sid);
            sb_printf(&e->dbgmeta,
                      "!%d = !DIDerivedType(tag: DW_TAG_typedef, name: \"%s\", "
                      "baseType: !%d)\n", tid, c->name, pid);
            return tid;
        }

        // ── rc[T]（共有所有）──
        //   PlRc { long long strong; long long borrow; void *value; }
        //   ★ 数え札まで見られるようにします（「なぜ解放されないのか」を
        //     追うときに、いちばん知りたいのがこの 2 つです）。
        case TY_RC: {
            int sid = dbg_id(e), pid = dbg_id(e), tid = dbg_id(e);
            dbg_ty_put(e, key, tid);

            int inner = dbg_type(e, t->elem);
            if (!inner) inner = dbg_ptr_to(e, "*void", 0);
            int i64 = dbg_type(e, ty_int);
            int m0 = dbg_member(e, "strong", 0, i64, 64, 0);
            int m1 = dbg_member(e, "borrow", 0, i64, 64, 64);
            int m2 = dbg_member(e, "value", 0, inner, 64, 128);
            int els = dbg_id(e);
            sb_printf(&e->dbgmeta, "!%d = !{!%d, !%d, !%d}\n", els, m0, m1, m2);
            sb_printf(&e->dbgmeta,
                      "!%d = !DICompositeType(tag: DW_TAG_structure_type, "
                      "name: \"%s\", file: !%d, size: 192, elements: !%d)\n",
                      sid, key, e->dbg_file, els);
            sb_printf(&e->dbgmeta,
                      "!%d = !DIDerivedType(tag: DW_TAG_pointer_type, "
                      "baseType: !%d, size: 64)\n", pid, sid);
            sb_printf(&e->dbgmeta,
                      "!%d = !DIDerivedType(tag: DW_TAG_typedef, name: \"%s\", "
                      "baseType: !%d)\n", tid, key, pid);
            return tid;
        }

        // ── ptr[T]（生ポインタ。OS 開発向け）──
        case TY_PTR: {
            int tid0 = dbg_id(e);
            dbg_ty_put(e, key, tid0);
            int inner = dbg_type(e, t->elem);
            int p;
            if (inner) {
                StrBuf pk;
                sb_init(&pk);
                sb_printf(&pk, "*%s", type_name(t->elem));
                p = dbg_ptr_to(e, sb_str(&pk), inner);
            } else {
                p = dbg_ptr_to(e, "*void", 0);
            }
            sb_printf(&e->dbgmeta,
                      "!%d = !DIDerivedType(tag: DW_TAG_typedef, name: \"%s\", "
                      "baseType: !%d)\n", tid0, key, p);
            return tid0;
        }

        // ── 中身をまだ書いていないもの ──
        //   タプル・関数・インタフェース・Thread[R]・mutex[T]。
        //   注意: 番地だけが出ます。型の名前は付くので、`p x` が
        //     「何も知らないポインタ」になることはありません。
        default: {
            int p = dbg_ptr_to(e, "*void", 0);
            int td = dbg_typedef(e, key, p);
            dbg_ty_put(e, key, td);
            return td;
        }
    }
}

// ── デバッグ情報：変数（A-35）────────────────────────────
//
// 変数 1 つを「名前・型・置いてある場所」の 3 つで書きます。
//
//   !N = !DILocalVariable(name: "x", scope: !<関数>, file: !<ファイル>, line: L, type: !T)
//   call void @llvm.dbg.declare(metadata ptr %x, metadata !N, metadata !DIExpression())
//
// ★ 置いてある場所は **alloca した箱**です（規約 R8 で引数も箱に写すので、
//   引数もローカル変数もまったく同じ扱いで済みます）。最適化を掛けると
//   LLVM が箱をレジスタに上げ、この印も一緒に付いて回ります。
//
// 注意: 脱糖が作る隠し変数（for.ix.0 / comp.res.0）は出しません。
//   利用者が書いていない名前なので、変数一覧に出ると邪魔になります。
//   見分けは「名前に '.' が入っているか」です（識別子には入りません）。
static void dbg_local(Emitter *e, const char *name, const char *ir_name, Type *t,
                      int line, int argno) {
    if (!e->dbg || !e->dbg_fn || !name || !ir_name) return;
    if (strchr(name, '.')) return;
    int ty = dbg_type(e, t);
    if (!ty) return;
    int loc = dbg_location(e);
    if (!loc) return;

    int id = dbg_id(e);
    if (argno > 0)
        sb_printf(&e->dbgmeta,
                  "!%d = !DILocalVariable(name: \"%s\", arg: %d, scope: !%d, "
                  "file: !%d, line: %d, type: !%d)\n",
                  id, name, argno, e->dbg_fn, e->dbg_file, line, ty);
    else
        sb_printf(&e->dbgmeta,
                  "!%d = !DILocalVariable(name: \"%s\", scope: !%d, file: !%d, "
                  "line: %d, type: !%d)\n",
                  id, name, e->dbg_fn, e->dbg_file, line, ty);

    declare_rt(e, "void @llvm.dbg.declare(metadata, metadata, metadata)");
    sb_printf(&e->dbgdecl,
              "  call void @llvm.dbg.declare(metadata ptr %s, metadata !%d, "
              "metadata !DIExpression()), !dbg !%d\n", ir_name, id, loc);
}

// いま生成している行の !DILocation を引く（無ければ作る）。
//
// ★ 行ごとに 1 つだけ作ります。文ごとに作ると metadata が何倍にも膨らみます。
static int dbg_location(Emitter *e) {
    if (!e->dbg || !e->dbg_fn) return 0;
    if (e->dbg_loc && e->dbg_loc_line == e->dbg_line) return e->dbg_loc;
    int id = dbg_id(e);
    sb_printf(&e->dbgmeta, "!%d = !DILocation(line: %d, column: 1, scope: !%d)\n",
              id, e->dbg_line, e->dbg_fn);
    e->dbg_loc = id;
    e->dbg_loc_line = e->dbg_line;
    return id;
}

// e->fn の start から後ろに出した**命令の行**へ、`!dbg` を付けて回る。
//
// なぜ後から付けるのか
//   命令を出す場所は数百か所あります。そのすべてに「行の印」を渡して回ると、
//   本筋（何を出すか）が印の受け渡しで埋まります。**出し終えてから、
//   その範囲の行に付ける**ほうが、読む場所が 1 つで済みます。
//
// 注意: 付けないもの：ラベル行（行頭が空白でない）・空行・すでに `!dbg` がある行
//   （内側の文が先に付けているので、外側は上書きしません）。
static void dbg_annotate(Emitter *e, size_t start) {
    if (!e->dbg || !e->dbg_fn) return;
    int loc = dbg_location(e);
    if (!loc) return;
    if (e->fn.len <= start) return;

    char *tail = xstrndup(e->fn.data + start, e->fn.len - start);
    e->fn.len = start;          // いったん切り詰めて、付け直したものを足す
    e->fn.data[start] = '\0';

    for (char *p = tail; *p;) {
        char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        bool is_insn = n > 2 && p[0] == ' ' && p[1] == ' ';
        // 注意: 1 行に 2 つ付けないこと（内側の文がすでに付けている）
        for (size_t i = 0; is_insn && i + 4 <= n; i++)
            if (memcmp(p + i, "!dbg", 4) == 0) is_insn = false;
        sb_printf(&e->fn, "%.*s", (int)n, p);
        if (is_insn) sb_printf(&e->fn, ", !dbg !%d", loc);
        if (nl) sb_printf(&e->fn, "\n");
        p = nl ? nl + 1 : p + n;
    }
}

static char *intern_str(Emitter *e, const char *bytes, int len) {
    for (StrLit *sl = e->strs; sl; sl = sl->next)
        if (sl->len == len && memcmp(sl->bytes, bytes, (size_t)len) == 0)
            return sl->label;

    StrBuf lab;
    sb_init(&lab);
    sb_printf(&lab, "@.str.%d", e->str_counter++);

    // ★ str は「長さ + バイト列」になりました（ランタイム参照）。
    //   リテラルも同じ形で出し、値としてはデータ部を指すポインタを使います。
    //
    //     @.str.0 = private unnamed_addr constant { i64, [6 x i8] }
    //                 { i64 5, [6 x i8] c"hello\00" }
    //
    // 注意: 配列の長さは「バイト数 + 1」。NUL の分を忘れない。
    StrBuf g;
    sb_init(&g);
    // ★ --drop のときは長さのヘッダに「静的」の印を立てます。
    //   リテラルは .rodata にあるので、解放しようとすると落ちるためです
    //   （runtime.c の PL_STR_STATIC を参照）。
    //
    // 注意: 印を立てるのは --drop のときだけです。既定の出力は v1 と 1 バイトも
    //    変えない、というのがこの章の約束なので（移植したら常に立てます）。
    long long hdr = e->drop ? ((long long)len | (1LL << 62)) : (long long)len;
    sb_printf(&g, "%s = private unnamed_addr constant { i64, [%d x i8] } "
                  "{ i64 %lld, [%d x i8] c\"",
              sb_str(&lab), len + 1, hdr, len + 1);
    for (int i = 0; i < len; i++) emit_ir_byte(&g, (unsigned char)bytes[i]);
    sb_printf(&g, "\\00\" }\n");
    sb_printf(&e->globals, "%s", sb_str(&g));

    // 値として使うのはデータ部のアドレス（定数式でそのまま書ける）
    StrBuf ref;
    sb_init(&ref);
    sb_printf(&ref, "getelementptr inbounds ({ i64, [%d x i8] }, ptr %s, i32 0, i32 1)",
              len + 1, sb_str(&lab));

    StrLit *sl = xmalloc(sizeof(StrLit));
    sl->bytes = (char *)bytes;
    sl->len = len;
    sl->label = sb_str(&ref);
    sl->next = e->strs;
    e->strs = sl;
    return sl->label;
}

// ランタイム関数を宣言する（1 回だけ）。
static const char *class_type(Emitter *e, Class *c);
static void declare_extern(Emitter *e, const char *ret, const char *ir_name,
                           const char *param_types);

static void declare_rt(Emitter *e, const char *sig) {
    for (StrLit *d = e->decled; d; d = d->next)
        if (strcmp(d->label, sig) == 0) return;
    sb_printf(&e->decls, "declare %s\n", sig);
    StrLit *d = xmalloc(sizeof(StrLit));
    d->label = (char *)sig;
    d->next = e->decled;
    e->decled = d;
}

// ── 式の生成 ────────────────────────────────────────────────
//
// gen_expr の約束：
//   「式を評価する命令列を body に出力し、
//     結果の値が入っている場所の名前（レジスタ名 or 即値）を返す」
//
// この 1 つの約束が、コード生成器の設計全体を決めます。
// 即値（"42"）とレジスタ（"%t0"）を同じ char * で扱えるので、
// 呼び出し側で場合分けが不要になります。
static char *gen_logical(Emitter *e, Node *n);
static char *gen_cond(Emitter *e, Node *n);      // 三項演算子
static char *gen_in(Emitter *e, Node *n);        // in / not in
static char *gen_slice(Emitter *e, Node *n);     // スライス
static char *gen_tuple(Emitter *e, Node *n);     // タプル
static bool elem_is_ptr(Type *elem);
static char *elem_to_slot_cmp(Emitter *e, Type *elem, char *v);
static char *gen_call(Emitter *e, Node *n);
static char *gen_list_lit(Emitter *e, Node *n);
static char *gen_listcomp(Emitter *e, Node *n);
// 並行実行（A-18）
static const char *thunk_for(Emitter *e, Type *fnty);
static char *pack_i64(Emitter *e, Type *t, char *v);
static char *unpack_i64(Emitter *e, Type *t, char *v);
static char *gen_index_addr(Emitter *e, char *obj, char *idx, const char *sty,
                            bool normalize, const char *ovf);
static char *gen_index(Emitter *e, Node *n);
static char *gen_method(Emitter *e, Node *n);
static char *gen_field(Emitter *e, Node *n);

// 定義は gen_index の手前。単項マイナスと二項演算のほうが
// 先に来るので、宣言だけここに置く。
static char *gen_checked_arith(Emitter *e, const char *intr, const char *l,
                               const char *r, int op);
static char *gen_checked_fdiv(Emitter *e, const char *l, const char *r);
static char *gen_range_check(Emitter *e, Type *rt, char *val);
static void gen_ensures(Emitter *e, const char *val);
static bool prove_verify_here(Emitter *e);
static void gen_prove_fail(Emitter *e);
static const char *ovf_intr(OpKind op);

static char *gen_expr(Emitter *e, Node *n) {
    switch (n->kind) {
        case ND_FLOAT:
            // ★ 字句解析器が正規化した文字列を**そのまま**出します。
            //   double を経由しないので、C 版とセルフホスト版で 1 バイトも
            //   ずれません（tests/selfhost.sh の IR 比較が効きます）。
            return n->sval;

        // ★ ensures の中の 'result'（A-29）。**いま返そうとしている値**です。
        //   注意: 箱は作りません（所有型の戻り値で解放の釣り合いが崩れるため）。
        case ND_RESULT:
            return (char *)e->ret_val;

        // ★ 範囲型の検査（A-28）。意味解析が入れたノードです。
        case ND_RANGECHK: {
            char *v = gen_expr(e, n->lhs);
            // ★ 証明（A-34）が「範囲に入る」と示したら検査を出しません
            if (n->no_range_check && !e->verify_prove) return v;
            const char *saved_vk = e->verify_kind;
            if (n->no_range_check) e->verify_kind = "range";
            char *rv = gen_range_check(e, n->type, v);
            e->verify_kind = saved_vk;
            return rv;
        }

        case ND_INT: {
            // 整数リテラルは命令を出す必要すらありません。
            // LLVM は即値をオペランドに直接書けるので（add i64 42, 1）、
            // 「42」という文字列をそのまま返します。
            char *buf = xmalloc(24);
            snprintf(buf, 24, "%lld", n->ival);
            return buf;
        }

        case ND_BINOP: {
            // ★ in / not in。注意: **左右をここで評価してはいけません。**
            //   渡す形が要素の型で変わるので、gen_in の中で作ります。
            if (n->op == OP_IN || n->op == OP_NOTIN) return gen_in(e, n);

            // ★ is / is not は「null と比べる」だけ。1 命令で済みます。
            if (n->op == OP_IS || n->op == OP_ISNOT) {
                char *v = gen_expr(e, n->lhs);
                char *t = new_tmp(e);
                sb_printf(&e->fn, "  %s = icmp %s ptr %s, null\n", t,
                          n->op == OP_IS ? "eq" : "ne", v);
                return t;
            }

            // ★ 左辺 → 右辺の順に生成する（仕様 4.5：評価順は左から右）
            char *l = gen_expr(e, n->lhs);
            char *r = gen_expr(e, n->rhs);
            char *t = new_tmp(e);

            // 注意: オペランドの型は「結果の型」ではありません。
            //    比較の結果は bool ですが、比べているのは左辺の型（int など）です。
            //    当初は両者が一致していたので llvm_type(n->type) で
            //    動いていました。比較演算子で初めてこの前提が崩れます。
            Type *ot = n->lhs->type;

            // ★ 繰り返しと list の連結（新しい値を作る）
            if (n->op == OP_MUL && ot->kind == TY_STR) {
                declare_rt(e, "ptr @pl_str_repeat(ptr, i64)");
                sb_printf(&e->fn,
                          "  %s = call ptr @pl_str_repeat(ptr %s, i64 %s)\n", t,
                          l, r);
                drop_temp(e, n->lhs, l);   // A-21e
                return t;
            }
            if (ot->kind == TY_LIST &&
                (n->op == OP_ADD || n->op == OP_MUL)) {
                if (n->op == OP_ADD) {
                    declare_rt(e, "ptr @pl_list_concat(ptr, ptr)");
                    sb_printf(&e->fn,
                              "  %s = call ptr @pl_list_concat(ptr %s, ptr %s)\n",
                              t, l, r);
                } else {
                    declare_rt(e, "ptr @pl_list_repeat(ptr, i64)");
                    sb_printf(&e->fn,
                              "  %s = call ptr @pl_list_repeat(ptr %s, i64 %s)\n",
                              t, l, r);
                }
                drop_temp(e, n->lhs, l);   // A-21e
                drop_temp(e, n->rhs, r);
                return t;
            }

            // ── 文字列 ─────────────────────────────────
            if (ot->kind == TY_STR) {
                if (n->op == OP_ADD) {
                    declare_rt(e, "ptr @pl_str_concat(ptr, ptr)");
                    sb_printf(&e->fn, "  %s = call ptr @pl_str_concat(ptr %s, ptr %s)\n",
                              t, l, r);
                    drop_temp(e, n->lhs, l);   // A-21e
                    drop_temp(e, n->rhs, r);
                    return t;
                }
                // 注意: 比較は「内容」で行う（言語仕様 4.3）。ポインタ比較ではない。
                //   pl_str_cmp が strcmp の符号を返すので、0 と比べる述語を
                //   変えるだけで 6 種類すべてに対応できます。
                declare_rt(e, "i64 @pl_str_cmp(ptr, ptr)");
                char *c = new_tmp(e);
                sb_printf(&e->fn, "  %s = call i64 @pl_str_cmp(ptr %s, ptr %s)\n", c,
                          l, r);
                drop_temp(e, n->lhs, l);   // A-21e
                drop_temp(e, n->rhs, r);
                sb_printf(&e->fn, "  %s = icmp %s i64 %s, 0\n", t,
                          icmp_pred(n->op, ty_int), c);
                return t;
            }

            // ── 検査つきの算術（規約 R10） ────────────────
            //
            // ★ 0 除算は SIGFPE でプロセスが死にます。何が起きたか分からない
            //   より、メッセージを出して死ぬほうが親切です。分岐を IR に出さず、
            //   ランタイム関数に押し込むのが R10 の実践です。
            // ★ float の '**' はランタイムの pl_fpow に落とします。
            //   注意: libc の pow は使えません（ベアメタルで動く必要があるため）。
            if (n->op == OP_POW && ot->kind == TY_FLOAT) {
                declare_rt(e, "double @pl_fpow(double, double)");
                sb_printf(&e->fn,
                          "  %s = call double @pl_fpow(double %s, double %s)\n",
                          t, l, r);
                return t;
            }

            // ★ 証明（A-34）が「0 で割らない・両方 0 以上」と示したら、
            //   呼び出しではなく命令 1 つで済みます（切り下げ＝切り捨て）。
            if ((n->op == OP_FLOORDIV || n->op == OP_MOD) && n->no_div_check &&
                !e->verify_prove) {
                sb_printf(&e->fn, "  %s = %s i64 %s, %s\n", t,
                          n->op == OP_FLOORDIV ? "sdiv" : "srem", l, r);
                return t;
            }

            if (n->op == OP_FLOORDIV || n->op == OP_MOD || n->op == OP_POW) {
                const char *fn = n->op == OP_FLOORDIV ? "pl_floordiv"
                                 : n->op == OP_MOD    ? "pl_mod"
                                                      : "pl_ipow";
                StrBuf sig;
                sb_init(&sig);
                sb_printf(&sig, "i64 @%s(i64, i64)", fn);
                declare_rt(e, sb_str(&sig));
                sb_printf(&e->fn, "  %s = call i64 @%s(i64 %s, i64 %s)\n", t, fn, l, r);
                return t;
            }

            // ★ float の比較は fcmp です。しかも述語に 'o'（ordered）を
            //   付けます。NaN が絡むと「どちらでもない」が正しい答えなので、
            //   ordered を選ぶと NaN との比較はすべて False になります
            //   （unordered の 'u' を選ぶと逆にすべて True になり、
            //     NaN != NaN が成り立たなくなります）。
            if (is_compare(n->op) && ot->kind == TY_FLOAT)
                sb_printf(&e->fn, "  %s = fcmp %s double %s, %s\n", t,
                          fcmp_pred(n->op), l, r);
            else if (is_compare(n->op))
                sb_printf(&e->fn, "  %s = icmp %s %s %s, %s\n", t,
                          icmp_pred(n->op, ot), llvm_type(ot), l, r);
            else if (n->op == OP_TRUEDIV && ot->kind == TY_FLOAT &&
                     !e->no_ovf) {
                // ★ float の 0 除算。`//` `%` と揃えます。
                return gen_checked_fdiv(e, l, r);
            } else {
                // ★ int の + - * は桁あふれを検査します。
                //   注意: 意図的に折り返したいときは wrap_add / wrap_sub /
                //     wrap_mul を使ってください（そちらは gen_call で出します）。
                // ★ 証明（A-34）が「折り返さない」と示した計算は検査を出しません
                bool proven = n->no_ovf_check && !e->verify_prove;
                const char *intr =
                    ot->kind == TY_INT && !e->no_ovf && !proven ? ovf_intr(n->op)
                                                                : NULL;
                if (intr) {
                    const char *saved_vk = e->verify_kind;
                    if (n->no_ovf_check) e->verify_kind = "overflow";
                    char *rv = gen_checked_arith(e, intr, l, r,
                                                 n->op == OP_ADD   ? 0
                                                 : n->op == OP_SUB ? 1
                                                                   : 2);
                    e->verify_kind = saved_vk;
                    return rv;
                }
                sb_printf(&e->fn, "  %s = %s %s %s, %s\n", t, llvm_binop(n),
                          llvm_type(ot), l, r);
            }
            return t;
        }

        case ND_BOOL: {
            // True / False は i1 の即値。LLVM は "true" / "false" と書けます。
            return n->ival ? "true" : "false";
        }

        case ND_NONE:
            // ★ None は「null というポインタ即値」。命令は出ません。
            return "null";

        case ND_STR:
            // ★ リテラルは .rodata の定数。ラベルをそのまま ptr として使えます
            //   （opaque pointer なので getelementptr は不要）。
            return intern_str(e, n->sval, n->slen);

        case ND_LOGICAL:
            return gen_logical(e, n);

        case ND_COND:
            return gen_cond(e, n);

        case ND_CALL:
            return gen_call(e, n);

        case ND_LIST:
            return gen_list_lit(e, n);

        case ND_INDEX:
            return gen_index(e, n);

        case ND_SLICE:
            return gen_slice(e, n);

        case ND_TUPLE:
            return gen_tuple(e, n);

        case ND_METHOD:
            return gen_method(e, n);

        case ND_FIELD:
            return gen_field(e, n);

        // ★ 中身を持つ枝のタグ（A-41）。**先頭の i64 を読むだけ**です。
        //   どの枝のクラスでもタグは先頭にあるので、見立て直しは要りません。
        case ND_ENUMTAG: {
            char *obj = gen_expr(e, n->lhs);
            char *t = new_tmp(e);
            sb_printf(&e->fn, "  %s = load i64, ptr %s\n", t, obj);
            return t;
        }

        case ND_VAR: {
            // ★ ガードの計算中は、誘導変数を「両端の値」に読み替えます。
            //   注意: 添字の式をもう一度そのまま生成するので、substitution は
            //     ここ 1 か所で足ります（式の形を分解する必要がありません）。
            if (e->subst_ir && n->ir_name &&
                strcmp(n->ir_name, e->subst_ir) == 0)
                return (char *)e->subst_val;

            // ★ 関数の名前を値として使う場合。
            //   箱から読むのではなく、**関数のラベルそのもの**が値です。
            if (n->is_func_ref) {
                char *t = xmalloc(strlen(n->ir_name) + 2);
                snprintf(t, strlen(n->ir_name) + 2, "@%s", n->ir_name);
                return t;
            }

            // 変数の読み出し（規約 R2）。bool なら i8 → i1 の変換も入る。
            // ★ n->name ではなく sema が割り当てた n->ir_name を使う
            char *v = gen_load(e, n->type, n->ir_name);

            // ★ ここで所有権が移ったなら、スロットに null を書きます。
            //   これが drop フラグの代わりです（設計 §6.3 の見直し。決定 D17）。
            if (e->drop && n->moved_out && !n->is_global)
                sb_printf(&e->fn, "  store ptr null, ptr %s\n", n->ir_name);
            return v;
        }

        case ND_UNARY: {
            char *v = gen_expr(e, n->lhs);

            // +x は何もしない（値をそのまま返す）
            if (n->op == OP_POS) return v;

            char *t = new_tmp(e);
            if (n->op == OP_NEG && n->type && n->type->kind == TY_FLOAT) {
                // ★ float には専用の否定命令 fneg があります（符号ビットを
                //   反転するだけ。0.0 - x とは -0.0 の扱いが違います）。
                sb_printf(&e->fn, "  %s = fneg double %s\n", t, v);
            } else if (n->op == OP_NEG) {
                // 注意: LLVM に整数の neg 命令はありません。0 からの減算で表現します。
                // ★ -(-9223372036854775808) は表せないので検査します。
                if (!e->no_ovf) return gen_checked_arith(e, "ssub", "0", v, 3);
                sb_printf(&e->fn, "  %s = sub i64 0, %s\n", t, v);
            } else if (n->op == OP_BITNOT) {
                // ~x は全ビット反転 = x XOR -1（-1 は全ビット 1）
                sb_printf(&e->fn, "  %s = xor i64 %s, -1\n", t, v);
            } else if (n->op == OP_NOT) {
                // not x は x XOR true（~x と同じ発想。幅が 1 ビットになっただけ）
                sb_printf(&e->fn, "  %s = xor i1 %s, true\n", t, v);
            } else {
                UNREACHABLE();
            }
            return t;
        }

        case ND_LISTCOMP:
            return gen_listcomp(e, n);

        default:
            UNREACHABLE();
    }
}

// ── 短絡評価（規約 6.6）────────────────────────────────────
//
// ★ この章で初めて基本ブロックを分岐させます。
//
//   a and b  … a が偽なら b を評価せずに偽
//   a or  b  … a が真なら b を評価せずに真
//
//   「評価しない」を実現するには命令を飛び越える必要があるので、分岐が要ります。
//
// なぜ phi を使わないのか（規約 R3）
//   教科書的には合流点で phi を使いますが、phi は「どのブロックから来たか」を
//   書く必要があり、生成側が前のブロックのラベルを覚えていなければなりません。
//   ネストすると管理が急激に面倒になります。
//   「alloca に置いて最後に読む」方式ならその面倒がゼロで、
//   mem2reg がこの alloca を phi に変換してくれます。
static char *gen_logical(Emitter *e, Node *n) {
    // 注意: 番号は最初に 1 回だけ確保する。
    //    使うたびに e->label_counter++ すると同じ and の中で番号がずれます。
    int id = e->label_counter++;
    const char *kind = n->op == OP_AND ? "and" : "or";

    char rhs_l[32], end_l[32], res[40];
    snprintf(rhs_l, sizeof(rhs_l), "%s.rhs.%d", kind, id);
    snprintf(end_l, sizeof(end_l), "%s.end.%d", kind, id);
    snprintf(res, sizeof(res), "%%%s.result.%d", kind, id);

    // 結果を入れる箱。★ alloca は entry ブロックへ（規約 R1）
    sb_printf(&e->allocas, "  %s = alloca i8\n", res);

    // ① 左辺を評価し、その値をいったん結果として置く
    char *l = gen_expr(e, n->lhs);
    gen_store(e, ty_bool, l, res);

    // ② 右辺を評価すべきか分岐する（and と or で真偽が逆）
    if (n->op == OP_AND)
        emit_cond_br(e, l, rhs_l, end_l);
    else
        emit_cond_br(e, l, end_l, rhs_l);

    // ③ 右辺（飛ばされることがあるブロック）
    emit_label(e, rhs_l);
    char *r = gen_expr(e, n->rhs);
    gen_store(e, ty_bool, r, res);
    emit_br(e, end_l);

    // ④ 合流点
    emit_label(e, end_l);
    return gen_load(e, ty_bool, res);
}

// タプルの LLVM 型を作る（{i64, ptr} のような無名の構造体）
//
// ★ 名前を付けません。タプルは「並びが同じなら同じ型」なので、
//   クラスのように定義を指す必要がないためです。
static const char *tuple_ty(Type *t) {
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "{ ");
    for (int i = 0; i < t->nparams; i++)
        sb_printf(&sb, "%s%s", i ? ", " : "", llvm_mem_type(t->params[i]));
    sb_printf(&sb, " }");
    return sb_str(&sb);
}

// タプル式 (a, b)
//
// ★ ヒープに確保して要素を書き込みます。クラスと同じ扱いなので、
//   list に入れる・引数で渡す・返す がすべてそのまま通ります。
static char *gen_tuple(Emitter *e, Node *n) {
    Type *t = n->type;
    int size = 0;
    for (int i = 0; i < t->nparams; i++) size += type_size(t->params[i]);

    declare_rt(e, "ptr @pl_alloc(i64)");
    char *obj = new_tmp(e);
    sb_printf(&e->fn, "  %s = call ptr @pl_alloc(i64 %d)\n", obj, size);

    const char *ty = tuple_ty(t);
    int k = 0;
    for (Node *x = n->body; x; x = x->next, k++) {
        char *v = gen_expr(e, x);
        char *pt = new_tmp(e);
        sb_printf(&e->fn, "  %s = getelementptr %s, ptr %s, i32 0, i32 %d\n", pt,
                  ty, obj, k);
        gen_store(e, t->params[k], v, pt);
    }
    return obj;
}

// xs[a:b] / s[a:b]
//
// ★ 省略された端は「先頭（0）」と「末尾（長さ）」に置き換えてから、
//   ランタイムの 1 つの関数に渡します。範囲の丸めもランタイム側の仕事です
//   （規約 R10：分岐を IR に出さない）。
static char *gen_slice(Emitter *e, Node *n) {
    bool is_str = n->lhs->type->kind == TY_STR;
    char *obj = gen_expr(e, n->lhs);

    char *lo = n->rhs ? gen_expr(e, n->rhs) : "0";

    char *hi;
    if (n->els) {
        hi = gen_expr(e, n->els);
    } else {
        // 終端の省略は「長さ」
        declare_rt(e, is_str ? "i64 @pl_str_len(ptr)" : "i64 @pl_list_len(ptr)");
        hi = new_tmp(e);
        sb_printf(&e->fn, "  %s = call i64 @%s(ptr %s)\n", hi,
                  is_str ? "pl_str_len" : "pl_list_len", obj);
    }

    declare_rt(e, is_str ? "ptr @pl_str_slice(ptr, i64, i64)"
                         : "ptr @pl_list_slice(ptr, i64, i64)");
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = call ptr @%s(ptr %s, i64 %s, i64 %s)\n", t,
              is_str ? "pl_str_slice" : "pl_list_slice", obj, lo, hi);
    return t;
}

// x in xs / sub in s
//
// ★ どちらも **位置を返すランタイム関数**（見つからなければ -1）に落として、
//   その結果を 0 と比べるだけにします。IR に分岐が 1 つも出ません（規約 R10）。
static char *gen_in(Emitter *e, Node *n) {
    Type *rt = n->rhs->type;
    char *pos = new_tmp(e);

    if (rt->kind == TY_STR) {
        declare_rt(e, "i64 @pl_str_find(ptr, ptr)");
        char *hay = gen_expr(e, n->rhs);
        char *nee = gen_expr(e, n->lhs);
        sb_printf(&e->fn, "  %s = call i64 @pl_str_find(ptr %s, ptr %s)\n",
                  pos, hay, nee);
    } else {
        // list[T]。要素の型で呼び分けます
        Type *el = rt->elem;
        const char *fn_name;
        const char *aty;
        if (el->kind == TY_FLOAT) {
            fn_name = "pl_list_index_f64";
            aty = "double";
        } else if (el->kind == TY_STR) {
            fn_name = "pl_list_index_str";
            aty = "ptr";
        } else if (elem_is_ptr(el)) {
            fn_name = "pl_list_index_ptr";
            aty = "ptr";
        } else {
            fn_name = "pl_list_index_i64";
            aty = "i64";
        }
        StrBuf sig;
        sb_init(&sig);
        sb_printf(&sig, "i64 @%s(ptr, %s)", fn_name, aty);
        declare_rt(e, sb_str(&sig));

        char *lst = gen_expr(e, n->rhs);
        char *v = elem_to_slot_cmp(e, el, gen_expr(e, n->lhs));
        sb_printf(&e->fn, "  %s = call i64 @%s(ptr %s, %s %s)\n", pos, fn_name,
                  lst, aty, v);
    }

    // 見つかった（>= 0）か。not in なら逆にする
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = icmp %s i64 %s, 0\n", t,
              n->op == OP_IN ? "sge" : "slt", pos);
    return t;
}

// 三項演算子 a if c else b
//
// ★ 作りは and / or の短絡評価とまったく同じです。**箱を 1 つ用意して、
//   選ばれた側だけがそこに書く**（規約 R3：phi を使わない）。
//   選ばれなかった側は **評価もされません**（Python と同じ）。
static char *gen_cond(Emitter *e, Node *n) {
    int id = e->label_counter++;
    char then_l[32], else_l[32], end_l[32], res[40];
    snprintf(then_l, sizeof(then_l), "cond.then.%d", id);
    snprintf(else_l, sizeof(else_l), "cond.else.%d", id);
    snprintf(end_l, sizeof(end_l), "cond.end.%d", id);
    snprintf(res, sizeof(res), "%%cond.result.%d", id);

    sb_printf(&e->allocas, "  %s = alloca %s\n", res, llvm_mem_type(n->type));

    char *c = gen_expr(e, n->lhs);
    emit_cond_br(e, c, then_l, else_l);

    emit_label(e, then_l);
    gen_store(e, n->type, gen_expr(e, n->rhs), res);
    emit_br(e, end_l);

    emit_label(e, else_l);
    gen_store(e, n->type, gen_expr(e, n->els), res);
    emit_br(e, end_l);

    emit_label(e, end_l);
    return gen_load(e, n->type, res);
}

// ── list[T] の生成 ────────────────────────────────────
//
// ★ 要素はすべて 8 バイト。i64 で持つか、ポインタで持つかの 2 通りだけです。
static bool elem_is_ptr(Type *elem) {
    // ★ クラスも参照（ポインタ）なので、ここに 1 語足すだけで
    //   list[Token] が動きます。この設計がそのまま効いています。
    // ★ T | None もポインタ（None は null）。
    // ★ rc[T] もポインタ（数え札付きの箱を指す）。
    // ★ インタフェースの値も「実体へのポインタ」1 個です
    // ★ Thread[R] / mutex[T] もランタイムの箱へのポインタです
    //   （list[Thread[R]] は spawn した本数を並べる、いちばん自然な形です）
    return elem->kind == TY_STR || elem->kind == TY_LIST ||
           elem->kind == TY_CLASS || elem->kind == TY_OPT ||
           elem->kind == TY_RC || elem->kind == TY_NULL ||
           elem->kind == TY_IFACE || elem->kind == TY_TUPLE ||
           elem->kind == TY_THREAD || elem->kind == TY_MUTEX;
}

// 要素の値を「ランタイムに渡す形」にする（bool は i64 に広げる。規約 R5）
// 探索関数に渡す形にする。
// 注意: elem_to_slot と違い、**float は double のまま**渡します
//   （数値として比べたいので、ビットに崩しません）。
static char *elem_to_slot_cmp(Emitter *e, Type *elem, char *v) {
    if (elem->kind != TY_BOOL) return v;
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = zext i1 %s to i64\n", t, v);
    return t;
}

static char *elem_to_slot(Emitter *e, Type *elem, char *v) {
    // ★ float はビットパターンのまま i64 のスロットに入れます。
    //   list の中身は「ポインタ 1 個か i64 1 個」という当初の作りを
    //   変えずに済みます（double も 8 バイトなので過不足なく入ります）。
    //   注意: 数値としての変換（sitofp）ではありません。bitcast です。
    if (elem->kind == TY_FLOAT) {
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = bitcast double %s to i64\n", t, v);
        return t;
    }
    if (elem->kind != TY_BOOL) return v;
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = zext i1 %s to i64\n", t, v);
    return t;
}

// ランタイムから受け取った値を「本言語の値」に戻す（bool は i1 に縮める）
static char *slot_to_elem(Emitter *e, Type *elem, char *v) {
    if (elem->kind == TY_FLOAT) {
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = bitcast i64 %s to double\n", t, v);
        return t;
    }
    if (elem->kind != TY_BOOL) return v;
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = trunc i64 %s to i1\n", t, v);
    return t;
}

static const char *slot_ty(Type *elem) { return elem_is_ptr(elem) ? "ptr" : "i64"; }

// [1, 2, 3] は「空リストを作って append を繰り返す」に脱糖する。
// ★ 複合代入や elif と同じ「脱糖」の手です。
static char *gen_list_lit(Emitter *e, Node *n) {
    Type *elem = n->type->elem;

    declare_rt(e, "ptr @pl_list_new()");
    char *l = new_tmp(e);
    sb_printf(&e->fn, "  %s = call ptr @pl_list_new()\n", l);

    const char *sty = slot_ty(elem);
    const char *push = elem_is_ptr(elem) ? "pl_list_push_ptr" : "pl_list_push_i64";
    StrBuf sig;
    sb_init(&sig);
    sb_printf(&sig, "void @%s(ptr, %s)", push, sty);
    declare_rt(e, sb_str(&sig));

    for (Node *el = n->body; el; el = el->next) {
        // 注意: **maybe_retain が要ります。** 場所（変数・フィールド・添字）から
        //    読んだ rc[T] をリストに入れると、**参照が 1 つ増えます**。
        //    増やさずに入れると、リストの解放だけが効いて早すぎる解放に
        //    なります（A-26。append は最初からこうしていました）。
        char *v = elem_to_slot(e, elem, maybe_retain(e, el, gen_expr(e, el)));
        sb_printf(&e->fn, "  call void @%s(ptr %s, %s %s)\n", push, l, sty, v);
    }
    return l;
}


// ── 内包表記 ──────────────────────────────────────────
//
//   [ E for x in xs if C ]   /   [ E for i in range(a, b) ]
//
// ★ **式の位置でループを組み立てます。** 構文解析で文に持ち上げると、
//   三項演算子や and / or の右側に書かれたときに「評価されないはずのもの」を
//   評価してしまうためです（ロードマップ §2 の A-12）。
//
// 注意: 隠し変数（結果の list / 添字 / ループ変数）の alloca は
//   collect_allocas が n->body から拾っています。ここでは名前を使うだけです。
//
// 出す形（list を回すとき）:
//
//   %res = call ptr @pl_list_new()   ; 結果
//   store 0 → %ix
//   comp.cond.N:  %i < len(%it) なら comp.body.N へ
//   comp.body.N:  x = %it[%i]、条件が真なら push、comp.next.N へ
//   comp.next.N:  %i = %i + 1、comp.cond.N へ
//   comp.end.N:   %res を返す
static char *gen_listcomp(Emitter *e, Node *n) {
    Node *lv = n->body;      // ループ変数
    Node *res = lv->next;    // 結果の list
    Node *ix = res->next;    // 添字（range のときは数える変数そのもの）
    Type *elem = n->type->elem;

    int id = e->label_counter++;
    char cond_l[40], body_l[40], next_l[40], end_l[40], keep_l[40];
    snprintf(cond_l, sizeof(cond_l), "comp.cond.%d", id);
    snprintf(body_l, sizeof(body_l), "comp.body.%d", id);
    snprintf(next_l, sizeof(next_l), "comp.next.%d", id);
    snprintf(end_l, sizeof(end_l), "comp.end.%d", id);
    snprintf(keep_l, sizeof(keep_l), "comp.keep.%d", id);

    // 結果の list を作る
    declare_rt(e, "ptr @pl_list_new()");
    char *lst = new_tmp(e);
    sb_printf(&e->fn, "  %s = call ptr @pl_list_new()\n", lst);
    sb_printf(&e->fn, "  store ptr %s, ptr %s\n", lst, res->ir_name);

    const char *sty = slot_ty(elem);
    const char *push = elem_is_ptr(elem) ? "pl_list_push_ptr" : "pl_list_push_i64";
    StrBuf sig;
    sb_init(&sig);
    sb_printf(&sig, "void @%s(ptr, %s)", push, sty);
    declare_rt(e, sb_str(&sig));

    // 注意: 対象（list）と range の端は **ループの外で 1 回だけ**評価します。
    char *it = NULL, *stop = NULL;
    if (n->args) {
        char *start = gen_expr(e, n->args);
        stop = gen_expr(e, n->args->next);
        sb_printf(&e->fn, "  store i64 %s, ptr %s\n", start, ix->ir_name);
    } else {
        it = gen_expr(e, n->rhs);
        sb_printf(&e->fn, "  store i64 0, ptr %s\n", ix->ir_name);
    }

    emit_label(e, cond_l);
    char *i = new_tmp(e);
    sb_printf(&e->fn, "  %s = load i64, ptr %s\n", i, ix->ir_name);
    char *go = new_tmp(e);
    if (n->args) {
        // 増分の符号で向きが変わります（構文解析器が定数だと確かめています）
        sb_printf(&e->fn, "  %s = icmp %s i64 %s, %s\n", go,
                  n->ival > 0 ? "slt" : "sgt", i, stop);
    } else {
        char *lenp = new_tmp(e);
        sb_printf(&e->fn, "  %s = getelementptr i8, ptr %s, i64 8\n", lenp, it);
        char *len = new_tmp(e);
        sb_printf(&e->fn, "  %s = load i64, ptr %s" TBAA_LISTHDR "\n", len, lenp);
        sb_printf(&e->fn, "  %s = icmp slt i64 %s, %s\n", go, i, len);
    }
    emit_cond_br(e, go, body_l, end_l);

    emit_label(e, body_l);
    // ループ変数に束縛する
    if (n->args) {
        sb_printf(&e->fn, "  store i64 %s, ptr %s\n", i, lv->ir_name);
    } else {
        char *ep = gen_index_addr(e, it, i, sty, false, NULL);
        char *v = new_tmp(e);
        sb_printf(&e->fn, "  %s = load %s, ptr %s" TBAA_LISTELEM "\n", v, sty, ep);
        gen_store(e, elem, slot_to_elem(e, elem, v), lv->ir_name);
    }

    // if の条件（あれば）
    if (n->els) {
        char *c = gen_expr(e, n->els);
        emit_cond_br(e, c, keep_l, next_l);
        emit_label(e, keep_l);
    }

    // 注意: リテラルと同じ理由で retain が要ります（A-26）。
    char *val = elem_to_slot(e, elem, maybe_retain(e, n->lhs, gen_expr(e, n->lhs)));
    char *lst2 = new_tmp(e);
    sb_printf(&e->fn, "  %s = load ptr, ptr %s\n", lst2, res->ir_name);
    sb_printf(&e->fn, "  call void @%s(ptr %s, %s %s)\n", push, lst2, sty, val);

    emit_label(e, next_l);
    char *i2 = new_tmp(e);
    sb_printf(&e->fn, "  %s = load i64, ptr %s\n", i2, ix->ir_name);
    char *i3 = new_tmp(e);
    // 注意: ここは桁あふれ検査を出しません（長さ／終端までしか進まないため）。
    sb_printf(&e->fn, "  %s = add i64 %s, %lld\n", i3, i2,
              n->args ? n->ival : 1);
    sb_printf(&e->fn, "  store i64 %s, ptr %s\n", i3, ix->ir_name);
    sb_printf(&e->fn, "  br label %%%s\n", cond_l);
    e->terminated = true;

    emit_label(e, end_l);
    char *out = new_tmp(e);
    sb_printf(&e->fn, "  %s = load ptr, ptr %s\n", out, res->ir_name);
    return out;
}

// None の検査を IR に展開する。
//
// 注意: クラス型のフィールドは NULL から始まります（12.6 節）。
//   NULL 参照を segfault ではなく親切なメッセージに変えるための検査です。
//
// ★ もとは pl_check_not_none への**呼び出し 1 回**でした。
//   フィールド参照はこの言語で最も回数の多い操作で、
//   linalg.Matrix.get のような小さなメソッドでは 1 回の呼び出しに
//   つき 2 回払っていました。ランタイムは別リンクなのでインライン化
//   されません。検査ごと IR に出して呼び出しを無くします。
//
// ★ 検査は消えません。null なら pl_none_fail へ飛びます。
static char *gen_not_none(Emitter *e, char *obj) {
    declare_rt(e, "void @pl_none_fail() noreturn cold");

    int id = e->label_counter++;  // ★ 番号は最初に 1 回だけ確保する
    char ok_l[32], bad_l[32];
    snprintf(ok_l, sizeof(ok_l), "none.ok.%d", id);
    snprintf(bad_l, sizeof(bad_l), "none.bad.%d", id);

    char *isn = new_tmp(e);
    sb_printf(&e->fn, "  %s = icmp eq ptr %s, null\n", isn, obj);
    emit_cond_br(e, isn, bad_l, ok_l);

    emit_label(e, bad_l);
    sb_printf(&e->fn, "  call void @pl_none_fail()\n");
    sb_printf(&e->fn, "  unreachable\n");
    e->terminated = true;

    emit_label(e, ok_l);
    return obj;
}

// ── 添字を IR に展開する ───────────────────────
//
// 注意: ここがこの言語の最大のボトルネックでした。
//   xs[i] はもともと pl_list_len / pl_norm_index / pl_list_get_* の
//   **関数呼び出し 3 回**でした。ランタイムは runtime.a として
//   別にリンクされ、LTO を使っていないので -O2 でもインライン化
//   されません。行列積の内側ループが呼び出し 5 回になり、
//   C の 61 倍遅くなっていました。
//
// ★ **検査は外しません**（方針 §0-②）。遅いのは検査ではなく
//   呼び出しなので、検査ごと IR に出します。こうすると LLVM が
//   ループの外へ持ち上げたり、範囲が自明なときに消したりできます。
//
// PlList の並びは { ptr data; i64 len; i64 cap }（runtime/core.c）。
// data がオフセット 0、len が 8 です。
//
// ★ 範囲検査は **符号なしの比較 1 回**で済みます。
//   len >= 0 なので、i を符号なしとして見れば負の i は巨大な値に
//   なり、i < 0 も i >= len も icmp ult ひとつで捕まります。
//
// normalize が true なら負の添字を末尾から数え直します。
// 戻り値は要素へのポインタ。
// ovf は「添字の計算で桁があふれた」ことを表す i1 の値（無ければ NULL）。
// ★ あふれの報告を**この範囲検査に相乗り**させます。理由は
//   gen_index_expr の説明を見てください。
static char *gen_index_addr(Emitter *e, char *obj, char *idx, const char *sty,
                            bool normalize, const char *ovf) {
    // ★ 版分けの速い側。ループの外で
    //   「全反復ぶんの添字が 0 以上・長さ未満」を確かめてあるので、
    //   ここでは検査も**負の添字の正規化も**出しません。
    //
    // 正規化（select）を消すのが要点です。検査だけ消しても
    //   アドレスが `data + idx + (idx<0 ? len : 0)` のままで、
    //   LLVM から見ると添字が 1 ずつ進むと分からず、ベクトル化できません。
    if (e->nobc) {
        char *data0 = new_tmp(e);
        sb_printf(&e->fn, "  %s = load ptr, ptr %s" TBAA_LISTHDR "\n", data0, obj);
        char *ep0 = new_tmp(e);
        sb_printf(&e->fn, "  %s = getelementptr %s, ptr %s, i64 %s\n", ep0, sty,
                  data0, idx);
        return ep0;
    }

    declare_rt(e, "void @pl_index_fail(i64, i64, i64) noreturn cold");

    char *lenp = new_tmp(e);
    sb_printf(&e->fn, "  %s = getelementptr i8, ptr %s, i64 8\n", lenp, obj);
    char *len = new_tmp(e);
    sb_printf(&e->fn, "  %s = load i64, ptr %s" TBAA_LISTHDR "\n", len, lenp);

    if (normalize) {
        char *isneg = new_tmp(e);
        sb_printf(&e->fn, "  %s = icmp slt i64 %s, 0\n", isneg, idx);
        char *plus = new_tmp(e);
        sb_printf(&e->fn, "  %s = add i64 %s, %s\n", plus, idx, len);
        char *ni = new_tmp(e);
        sb_printf(&e->fn, "  %s = select i1 %s, i64 %s, i64 %s\n", ni, isneg, plus,
                  idx);
        idx = ni;
    }

    int id = e->label_counter++;  // ★ 番号は最初に 1 回だけ確保する
    char ok_l[32], bad_l[32];
    snprintf(ok_l, sizeof(ok_l), "idx.ok.%d", id);
    snprintf(bad_l, sizeof(bad_l), "idx.bad.%d", id);

    char *inb = new_tmp(e);
    sb_printf(&e->fn, "  %s = icmp ult i64 %s, %s\n", inb, idx, len);

    // ★ 桁あふれがあったら、範囲内に見えても失敗させます。
    //   （折り返した添字がたまたま範囲内に入ることがあるため。
    //     ここを and で潰しておくのが健全性の要です。）
    const char *ovf_arg = "0";
    if (ovf) {
        char *nov = new_tmp(e);
        sb_printf(&e->fn, "  %s = xor i1 %s, true\n", nov, ovf);
        char *both = new_tmp(e);
        sb_printf(&e->fn, "  %s = and i1 %s, %s\n", both, inb, nov);
        char *z = new_tmp(e);
        sb_printf(&e->fn, "  %s = zext i1 %s to i64\n", z, ovf);
        inb = both;
        ovf_arg = z;
    }
    emit_cond_br(e, inb, ok_l, bad_l);

    emit_label(e, bad_l);
    if (prove_verify_here(e))
        gen_prove_fail(e);
    else
        sb_printf(&e->fn, "  call void @pl_index_fail(i64 %s, i64 %s, i64 %s)\n",
                  idx, len, ovf_arg);
    // 注意: pl_index_fail は戻ってきません。unreachable を置かないと
    //   LLVM は「戻るかも」と見て、検査をループ外へ出せなくなります。
    sb_printf(&e->fn, "  unreachable\n");
    e->terminated = true;

    emit_label(e, ok_l);
    char *data = new_tmp(e);
    sb_printf(&e->fn, "  %s = load ptr, ptr %s" TBAA_LISTHDR "\n", data, obj);
    char *ep = new_tmp(e);
    sb_printf(&e->fn, "  %s = getelementptr %s, ptr %s, i64 %s\n", ep, sty, data,
              idx);
    return ep;
}

// ── 整数の桁あふれを検査する ─────────────────────
//
// ★ これまでと同じ考え方で、**検査ごと IR に出します**（規約 R10 の例外）。
//   ランタイム関数を呼ぶ形にすると、+ と * はこの言語で最も回数の多い
//   演算なので、呼び出しの費用が支配的になります。
//
// ★ 当たりの経路は `llvm.*.with.overflow` 1 命令 ＋ 予測の当たる分岐だけです。
//   実測で **2.4%**（整数ループ 2000 万回が 166 → 170 ms）でした。
//
// 注意: 外れの経路の pl_overflow_fail は **noreturn cold** です。
//   cold が無いと、この分岐の費用が囲む関数のインライン化の見積りに
//   入ります（ linalg.Matrix.check が丸ごと落ちた件と同じ）。
//
// ★ ここは「値と、あふれた旗（i1）を作る」だけです。**分岐は出しません。**
//   分岐まで出す形（gen_checked_arith）と、旗だけ受け取って範囲検査に
//   相乗りさせる形（gen_index_expr）の 2 通りに使い分けます。
static char *gen_arith_ovf(Emitter *e, const char *intr, const char *l,
                           const char *r, char **bad_out) {
    StrBuf sig;
    sb_init(&sig);
    sb_printf(&sig, "{i64, i1} @llvm.%s.with.overflow.i64(i64, i64)", intr);
    declare_rt(e, sb_str(&sig));

    char *pair = new_tmp(e);
    sb_printf(&e->fn, "  %s = call {i64, i1} @llvm.%s.with.overflow.i64(i64 %s, i64 %s)\n",
              pair, intr, l, r);
    char *val = new_tmp(e);
    sb_printf(&e->fn, "  %s = extractvalue {i64, i1} %s, 0\n", val, pair);
    char *bad = new_tmp(e);
    sb_printf(&e->fn, "  %s = extractvalue {i64, i1} %s, 1\n", bad, pair);
    *bad_out = bad;
    return val;
}

// 旗を見てその場で分岐する形（添字の外の算術は全部こちら）。
// op は pl_overflow_fail に渡す番号です（0:+ 1:- 2:* 3:単項-）。
// ── 範囲型（部分型。A-28）の検査 ──────────────────────
//
// ★ 出すのは **比較 2 つと分岐 1 つ**だけです。外れの経路は cold なので、
//   インライン化の見積りにもほとんど載りません（規約 R13。A-6 と同じ形）。
//
//   %lo = icmp slt i64 %v, <下端>
//   %hi = icmp sgt i64 %v, <上端>
//   %bad = or i1 %lo, %hi
//   br i1 %bad, label %rng.bad.N, label %rng.ok.N
//
// 注意: 値はそのまま返します。範囲型の表現は int のままなので、
//   変換も詰め替えも要りません（Ada の部分型と同じ考え方）。
// 検査が外れたときの呼び先。--verify-prove で「消せるはず」と判断した
// 場所だけ、証明器の誤りとして報告します。
static bool prove_verify_here(Emitter *e) {
    return e->verify_prove && e->verify_kind != NULL;
}

static void gen_prove_fail(Emitter *e) {
    declare_rt(e, "void @pl_prove_fail(ptr) noreturn cold");
    const char *w = e->verify_kind;
    char *lab = intern_str(e, w, (int)strlen(w));
    sb_printf(&e->fn, "  call void @pl_prove_fail(ptr %s)\n", lab);
}

static char *gen_range_check(Emitter *e, Type *rt, char *val) {
    declare_rt(e, "void @pl_range_fail(ptr, i64, i64, i64) noreturn cold");

    char *lo_c = new_tmp(e);
    sb_printf(&e->fn, "  %s = icmp slt i64 %s, %lld\n", lo_c, val, rt->lo);
    char *hi_c = new_tmp(e);
    sb_printf(&e->fn, "  %s = icmp sgt i64 %s, %lld\n", hi_c, val, rt->hi);
    char *bad = new_tmp(e);
    sb_printf(&e->fn, "  %s = or i1 %s, %s\n", bad, lo_c, hi_c);

    int id = e->label_counter++;  // ★ 番号は最初に 1 回だけ確保する
    char ok_l[32], bad_l[32];
    snprintf(ok_l, sizeof(ok_l), "rng.ok.%d", id);
    snprintf(bad_l, sizeof(bad_l), "rng.bad.%d", id);
    emit_cond_br(e, bad, bad_l, ok_l);

    emit_label(e, bad_l);
    if (prove_verify_here(e)) {
        gen_prove_fail(e);
    } else {
        char *nm = intern_str(e, rt->name, (int)strlen(rt->name));
        sb_printf(&e->fn,
                  "  call void @pl_range_fail(ptr %s, i64 %s, i64 %lld, i64 %lld)\n",
                  nm, val, rt->lo, rt->hi);
    }
    sb_printf(&e->fn, "  unreachable\n");
    e->terminated = true;

    emit_label(e, ok_l);
    return val;
}

// ── 契約（事前条件・事後条件。A-29）の検査 ──────────────
//
// ★ 出るのは **条件の式と分岐 1 つ**だけです（assert と同じ形）。
//   外れの経路は cold なので、インライン化の見積りにはほとんど載りません。
//
// 注意: メッセージは**コンパイル時に組み立てて**大域定数に置きます。
//   実行時に数を文字列にする手間を、外れの経路にも置かないためです。
static void gen_contract_check(Emitter *e, Node *c, const char *fname) {
    // ★ 証明（A-34 段 2）が「常に真」と示した契約は検査を出しません
    if (c->no_contract && !e->verify_prove) return;
    // 注意: --verify-prove では残して、外れたら証明器の誤りとして報告します
    const char *saved_vk = e->verify_kind;
    if (c->no_contract) e->verify_kind = "contract";
    declare_rt(e, "void @pl_contract_fail(ptr) noreturn cold");

    char *cond = gen_expr(e, c->lhs);

    int id = e->label_counter++;
    char ok_l[32], bad_l[32];
    snprintf(ok_l, sizeof(ok_l), "ct.ok.%d", id);
    snprintf(bad_l, sizeof(bad_l), "ct.bad.%d", id);
    emit_cond_br(e, cond, ok_l, bad_l);

    emit_label(e, bad_l);
    if (prove_verify_here(e)) {
        gen_prove_fail(e);
    } else {
        StrBuf msg;
        sb_init(&msg);
        sb_printf(&msg, "%s of %s (line %d)",
                  c->kind == ND_REQUIRES ? "requires" : "ensures", fname,
                  c->tok ? c->tok->line : 0);
        const char *m = sb_str(&msg);
        char *lab = intern_str(e, m, (int)strlen(m));
        sb_printf(&e->fn, "  call void @pl_contract_fail(ptr %s)\n", lab);
    }
    sb_printf(&e->fn, "  unreachable\n");
    e->terminated = true;

    emit_label(e, ok_l);
    e->verify_kind = saved_vk;
}

// return のたびに ensures を確かめる（A-29）。
//
// ★ val は「いま返そうとしている値」です（ensures の中の 'result'）。
//   値を返さない関数では NULL を渡します。
static void gen_ensures(Emitter *e, const char *val) {
    if (!e->fn_node || !e->fn_node->body) return;
    const char *saved = e->ret_val;
    e->ret_val = val;
    for (Node *st = e->fn_node->body->body; st; st = st->next) {
        // 注意: 契約は本体の先頭に並んでいます（sema が保証）。
        //   先頭以外に出てきたら、そこで止めます。
        if (st->kind != ND_REQUIRES && st->kind != ND_ENSURES) break;
        if (st->kind == ND_ENSURES)
            gen_contract_check(e, st, e->fn_node->name);
    }
    e->ret_val = saved;
}

static char *gen_checked_arith(Emitter *e, const char *intr, const char *l,
                               const char *r, int op) {
    declare_rt(e, "void @pl_overflow_fail(i64) noreturn cold");
    char *bad;
    char *val = gen_arith_ovf(e, intr, l, r, &bad);

    int id = e->label_counter++;  // ★ 番号は最初に 1 回だけ確保する
    char ok_l[32], bad_l[32];
    snprintf(ok_l, sizeof(ok_l), "ovf.ok.%d", id);
    snprintf(bad_l, sizeof(bad_l), "ovf.bad.%d", id);
    emit_cond_br(e, bad, bad_l, ok_l);

    emit_label(e, bad_l);
    if (prove_verify_here(e))
        gen_prove_fail(e);
    else
        sb_printf(&e->fn, "  call void @pl_overflow_fail(i64 %d)\n", op);
    sb_printf(&e->fn, "  unreachable\n");
    e->terminated = true;

    emit_label(e, ok_l);
    return val;
}

// ★ float の 0 除算を検査する。
//
//   注意: 整数の `//` と `%` は 0 除算で panic するのに、`1.0 / 0.0` は
//     inf になっていました。**同じ「割る」で挙動が違う**のは穴です。
//     Python は両方 ZeroDivisionError なので、そちらに揃えます。
//
//   注意: 規約 R13：外れの経路の**呼び出し**が 1 つ増えます（約 25 点）。
//     費用は測って記録しました。
static char *gen_checked_fdiv(Emitter *e, const char *l, const char *r) {
    declare_rt(e, "void @pl_fdiv_zero_fail() noreturn cold");
    char *z = new_tmp(e);
    // 注意: oeq を使います。-0.0 も 0.0 と等しく、割れば ±inf になるためです。
    //   NaN で割るのは NaN なので、ここでは止めません（0 だけを見ます）。
    sb_printf(&e->fn, "  %s = fcmp oeq double %s, 0.000000e+00\n", z, r);

    int id = e->label_counter++;
    char ok_l[32], bad_l[32];
    snprintf(ok_l, sizeof(ok_l), "fdiv.ok.%d", id);
    snprintf(bad_l, sizeof(bad_l), "fdiv.zero.%d", id);
    emit_cond_br(e, z, bad_l, ok_l);

    emit_label(e, bad_l);
    sb_printf(&e->fn, "  call void @pl_fdiv_zero_fail()\n");
    sb_printf(&e->fn, "  unreachable\n");
    e->terminated = true;

    emit_label(e, ok_l);
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = fdiv double %s, %s\n", t, l, r);
    return t;
}

// 貯めた「あふれの旗」で 1 回だけ分岐する（相乗りできる範囲検査が無いとき）。
// op は 4 ＝「添字の計算」です。
static void gen_ovf_br(Emitter *e, const char *flag) {
    declare_rt(e, "void @pl_overflow_fail(i64) noreturn cold");
    int id = e->label_counter++;
    char ok_l[32], bad_l[32];
    snprintf(ok_l, sizeof(ok_l), "ovf.ok.%d", id);
    snprintf(bad_l, sizeof(bad_l), "ovf.bad.%d", id);
    emit_cond_br(e, flag, bad_l, ok_l);
    emit_label(e, bad_l);
    sb_printf(&e->fn, "  call void @pl_overflow_fail(i64 4)\n");
    sb_printf(&e->fn, "  unreachable\n");
    e->terminated = true;
    emit_label(e, ok_l);
}

// 整数の + - * で、検査つきにするかどうか。
// 注意: float・str・list の + * はここに来ません（呼び出し側で分けています）。
static const char *ovf_intr(OpKind op) {
    if (op == OP_ADD) return "sadd";
    if (op == OP_SUB) return "ssub";
    if (op == OP_MUL) return "smul";
    return NULL;
}

// ── 添字の式を、あふれの旗を貯めながら生成する ─────────
//
// ★ **なぜ添字だけ特別扱いするのか。**
//   `self.data[i * self.cols + j]` のような小さなメソッドが、
//   桁あふれ検査を入れた 0.6.0 で**インライン化されなくなりました**
//   （linalg.Matrix の行列積が 2.6 倍遅くなった原因）。
//   LLVM の見積りを実測すると `Matrix.get` は **cost=225 / threshold=225** で、
//   **1 点足りずに**落ちていました。内訳を数えると、外れの経路の
//   **呼び出し 1 つにつき約 25 点**です（cold を付けても引かれません）。
//   `llvm.trap` に差し替えると通ることも確かめました（＝呼び出しが費用）。
//
// ★ そこで、**あふれの報告を範囲検査に相乗りさせます。**
//   添字の計算の `+ - *` は分岐せず旗（i1）だけを立て、
//   最後に `範囲内 かつ あふれていない` を 1 回だけ分岐します。
//   失敗の呼び出しは **もともと 1 つある pl_index_fail だけ**になり、
//   分岐も呼び出しも増えません。
//
//   注意: **健全性。** 折り返した添字がたまたま範囲内に入っても、
//     旗が立っているので必ず失敗します（gen_index_addr の and）。
//     「検査を省く」案とはここが違います。
//
//   注意: **意味の違いが 1 つだけあります。** あふれた時点ではなく
//     添字の計算が終わった時点で止まるので、`xs[a * b + f()]` は
//     a * b があふれても f() が呼ばれます。診断は
//     「添字の計算があふれた」になります（演算子の種類は言いません）。
//
// ovf には、貯めた旗を or でつないだ値が入ります（無ければ NULL のまま）。
static char *gen_index_expr(Emitter *e, Node *n, char **ovf) {
    const char *intr = NULL;
    if (!e->no_ovf && n->kind == ND_BINOP && n->lhs->type &&
        n->lhs->type->kind == TY_INT)
        intr = ovf_intr(n->op);

    // 注意: 単項 - も同じ扱いにします（-INT_MIN があふれます）。
    bool neg = !e->no_ovf && n->kind == ND_UNARY && n->op == OP_NEG && n->type &&
               n->type->kind == TY_INT;

    if (!intr && !neg) return gen_expr(e, n);  // 算術以外は普通に生成する

    char *l, *r;
    if (neg) {
        l = "0";
        r = gen_index_expr(e, n->lhs, ovf);
        intr = "ssub";
    } else {
        l = gen_index_expr(e, n->lhs, ovf);
        r = gen_index_expr(e, n->rhs, ovf);
    }

    char *bad;
    char *val = gen_arith_ovf(e, intr, l, r, &bad);
    if (*ovf) {
        char *o = new_tmp(e);
        sb_printf(&e->fn, "  %s = or i1 %s, %s\n", o, *ovf, bad);
        *ovf = o;
    } else {
        *ovf = bad;
    }
    return val;
}

static char *gen_index(Emitter *e, Node *n) {
    Type *ot = n->lhs->type;
    char *obj = gen_expr(e, n->lhs);
    char *ovf = NULL;
    char *idx = gen_index_expr(e, n->rhs, &ovf);

    // str の添字は 1 文字の str を返す（型システム 5.8）。
    // ★ こちらは 1 文字の str を**新しく作る**ので、どのみち確保が
    //   入ります。展開しても利かないので従来どおり呼び出します。
    if (ot->kind == TY_STR) {
        // 注意: str の添字は pl_norm_index / pl_str_index に任せるので、
        //   相乗りさせる範囲検査がここにはありません。旗が立っていたら
        //   ここで 1 回だけ分岐して報告します。
        if (ovf) gen_ovf_br(e, ovf);
        declare_rt(e, "i64 @pl_str_len(ptr)");
        declare_rt(e, "i64 @pl_norm_index(i64, i64)");
        char *ln = new_tmp(e);
        sb_printf(&e->fn, "  %s = call i64 @pl_str_len(ptr %s)\n", ln, obj);
        char *ni = new_tmp(e);
        sb_printf(&e->fn, "  %s = call i64 @pl_norm_index(i64 %s, i64 %s)\n", ni,
                  idx, ln);
        declare_rt(e, "ptr @pl_str_index(ptr, i64)");
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = call ptr @pl_str_index(ptr %s, i64 %s)\n", t, obj,
                  ni);
        return t;
    }

    // ★ タプルの添字は「構造体の何番目か」。範囲は意味解析で確かめ済みです
    if (ot->kind == TY_TUPLE) {
        long long k = n->rhs->ival;
        char *pt = new_tmp(e);
        sb_printf(&e->fn, "  %s = getelementptr %s, ptr %s, i32 0, i32 %lld\n",
                  pt, tuple_ty(ot), obj, k);
        return gen_load(e, ot->params[k], pt);
    }

    Type *elem = ot->elem;
    const char *sty = slot_ty(elem);
    // ★ 証明（A-34）が「0 以上・長さ未満」と示した添字は、検査も
    //   負の添字の正規化も出しません（A-15c の速い側と同じ道です）。
    bool saved_nobc = e->nobc;
    const char *saved_vk = e->verify_kind;
    if (n->no_bounds_check) {
        if (e->verify_prove) e->verify_kind = "index";
        else e->nobc = true;
    }
    char *ep = gen_index_addr(e, obj, idx, sty, true, ovf);
    e->nobc = saved_nobc;
    e->verify_kind = saved_vk;
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = load %s, ptr %s" TBAA_LISTELEM "\n", t, sty, ep);
    return slot_to_elem(e, elem, t);
}

static void gen_index_store(Emitter *e, Node *target, char *val) {
    Type *elem = target->lhs->type->elem;
    char *obj = gen_expr(e, target->lhs);
    char *ovf = NULL;
    char *idx = gen_index_expr(e, target->rhs, &ovf);

    const char *sty = slot_ty(elem);
    // 注意: 代入側は負の添字を正規化しません。
    //   pl_list_set_* を呼んでいたころからそうでした（xs[-1] = v は panic）。
    //   ここで変えると意味が変わるので、振る舞いはそのままにします。
    bool saved_nobc2 = e->nobc;
    const char *saved_vk2 = e->verify_kind;
    if (target->no_bounds_check) {
        if (e->verify_prove) e->verify_kind = "index";
        else e->nobc = true;
    }
    char *ep = gen_index_addr(e, obj, idx, sty, false, ovf);
    e->nobc = saved_nobc2;
    e->verify_kind = saved_vk2;
    char *v = elem_to_slot(e, elem, val);
    sb_printf(&e->fn, "  store %s %s, ptr %s" TBAA_LISTELEM "\n", sty, v, ep);
}

static char *gen_new(Emitter *e, Node *n);

// 引数を評価して "型 値, 型 値" と "型, 型"（declare 用）を同時に作る
// 呼び出し後に解放する一時値の控え（A-21e）。
//
// ★ なぜ「呼び出し後」なのか
//   gen_args は引数の文字列を組み立てるだけで、call を出すのは emit_call です。
//   引数を作った直後に解放すると、**まだ渡していない値を解放**してしまいます。
//
// 注意: 入れるのは「借用で渡す」と**分かっている**実引数だけです。
//   own なら所有権が相手に移るので解放してはいけません。分からない経路
//   （メソッド呼び出し）も入れません。sema が arg_is_borrowed に入れます。
typedef struct TempArg {
    Node *node;
    char *val;
    struct TempArg *next;
} TempArg;

static TempArg *g_pending_temps;  // gen_args → emit_call のあいだだけ使う

static void gen_args(Emitter *e, Node *args, StrBuf *vals, StrBuf *types,
                     bool first) {
    for (Node *a = args; a; a = a->next) {
        // 注意: 内側の呼び出しに、外側の控えを触らせないこと。
        //   f(g(x)) のとき、g の emit_call が f の控えまで解放してしまい、
        //   **まだ渡していない値**が消えます。
        TempArg *saved = g_pending_temps;
        g_pending_temps = NULL;
        char *v = gen_expr(e, a);
        g_pending_temps = saved;   // 内側は自分で解放済み
        // ★ rc[T] を own の仮引数へ渡すときは、参照を 1 つ増やして渡します。
        //   rc[T] は移動しないので（ownck は共有として扱う）、増やさないと
        //   受け取った側の「出口で手放す」だけが残り、二重解放になります。
        if (e->drop && a->arg_own_rc) v = force_retain(e, v);
        sb_printf(vals, "%s%s %s", first ? "" : ", ", llvm_type(a->type), v);
        sb_printf(types, "%s%s", first ? "" : ", ", llvm_type(a->type));
        first = false;
        if (e->drop && a->arg_is_borrowed && is_owned_temp(a)) {
            TempArg *t = xmalloc(sizeof(TempArg));
            t->node = a;
            t->val = v;
            t->next = g_pending_temps;
            g_pending_temps = t;
        }
    }
}

// 控えておいた一時値を解放して、控えを空にする（A-21e）。
//
// 注意: **戻り値が借りものの呼び出しでは解放しません。**
//   `def pick(a: str) -> str: return a` のように引数をそのまま返す関数だと、
//   戻り値が実引数と**同じ実体**です。呼び出し後に実引数を解放すると、
//   受け取ったばかりの戻り値が消えます（ASan が heap-use-after-free と言う）。
//   ownck が呼び出しノードに binds_borrow を立てているので、それを見ます。
static void flush_pending_temps(Emitter *e, Node *call) {
    TempArg *t = g_pending_temps;
    g_pending_temps = NULL;
    if (call && call->binds_borrow) return;   // 漏れるが、壊れない
    for (; t; t = t->next) emit_drop_value(e, t->node->type, t->val);
}

// 呼び出しを 1 行出す（戻り値が None なら値を返さない）
//
// ★ 呼び先は**名前でもレジスタでもかまいません**（callee に "@f" か "%t.3" を
//   渡します）。インタフェース越しの呼び出し（vtable）も、失敗しうるときの
//   段取り——エラースロットを渡して、戻ったらタグを見る——が要るので、
//   同じところを通します（A-40）。
static char *emit_call_to(Emitter *e, Node *n, const char *callee,
                          const char *args) {
    // ── 失敗しうる呼び出し ──
    //
    //   ① エラースロットのタグを 0 にする
    //   ② スロットのアドレスを最後の引数として渡す
    //   ③ 戻ってきたらタグを見て、0 でなければ「失敗の飛び先」へ跳ぶ
    StrBuf full;
    sb_init(&full);
    sb_printf(&full, "%s", args);
    const char *slot = NULL;
    if (n->can_fail) {
        slot = err_slot(e);
        char *tp = new_tmp(e);
        sb_printf(&e->fn, "  %s = getelementptr %%pl.err, ptr %s, i32 0, i32 0\n", tp,
                  slot);
        sb_printf(&e->fn, "  store i64 0, ptr %s\n", tp);
        sb_printf(&full, "%sptr %s", args[0] ? ", " : "", slot);
    }

    char *t = NULL;
    if (n->type->kind == TY_NONE) {
        sb_printf(&e->fn, "  call void %s(%s)\n", callee, sb_str(&full));
    } else {
        t = new_tmp(e);
        sb_printf(&e->fn, "  %s = call %s %s(%s)\n", t, llvm_type(n->type),
                  callee, sb_str(&full));
    }

    if (n->can_fail) {
        int id = e->label_counter++;
        char ok_l[32];
        snprintf(ok_l, sizeof(ok_l), "call.ok.%d", id);
        char *tag = load_tag(e, slot);
        char *bad = new_tmp(e);
        sb_printf(&e->fn, "  %s = icmp ne i64 %s, 0\n", bad, tag);

        // 注意: 解放が有効なら、失敗の経路でも「抜けるスコープ」を解放します。
        //   分岐の辺には命令を置けないので、専用のブロックを 1 つ挟みます。
        if (e->drop) {
            char fail_l[32];
            snprintf(fail_l, sizeof(fail_l), "call.fail.%d", id);
            emit_cond_br(e, bad, fail_l, ok_l);
            emit_label(e, fail_l);
            emit_drops_until(e, e->try_ctx ? e->try_ctx->scope : NULL);
            emit_fail_br(e);
        } else {
            const char *fl = fail_label(e);
            if (fl) {
                emit_cond_br(e, bad, fl, ok_l);
            } else {
                // 到達しない経路（sema が保証）。専用ブロックに落とす。
                char un_l[32];
                snprintf(un_l, sizeof(un_l), "call.unreach.%d", id);
                emit_cond_br(e, bad, un_l, ok_l);
                emit_label(e, un_l);
                sb_printf(&e->fn, "  unreachable\n");
                e->terminated = true;
            }
        }
        emit_label(e, ok_l);
    }
    // ★ 実引数の一時値をここで解放します（A-21e）。
    //   注意: **呼び出しを出し終えてから**です。gen_args の直後に解放すると、
    //     まだ渡していない値を解放してしまいます。
    flush_pending_temps(e, n);
    return t;
}

// 名前で呼ぶ（いちばん多い形）
static char *emit_call(Emitter *e, Node *n, const char *args) {
    StrBuf callee;
    sb_init(&callee);
    sb_printf(&callee, "@%s", n->ir_name);
    return emit_call_to(e, n, sb_str(&callee), args);
}

static char *gen_method(Emitter *e, Node *n) {
    // ★ '.' の左がモジュールだった場合。sema が記録を残している。
    if (n->mod_name) {
        if (n->cls) return gen_new(e, n);  // lexer.Token(1, "x")

        StrBuf args, types;
        sb_init(&args);
        sb_init(&types);
        gen_args(e, n->args, &args, &types, true);
        if (n->is_extern) {
            if (n->can_fail) sb_printf(&types, "%sptr", sb_str(&types)[0] ? ", " : "");
            declare_extern(e, llvm_type(n->type), n->ir_name, sb_str(&types));
        }
        return emit_call(e, n, sb_str(&args));
    }

    // ★ Thread[R].join() / mutex[T].lock(f)（A-18）
    if (n->lhs && n->lhs->type) {
        Type *ot = n->lhs->type;
        if (ot->kind == TY_THREAD) {
            char *h = gen_expr(e, n->lhs);
            declare_rt(e, "i64 @pl_thread_join(ptr)");
            char *r = new_tmp(e);
            sb_printf(&e->fn, "  %s = call i64 @pl_thread_join(ptr %s)\n", r, h);
            if (ot->elem->kind == TY_NONE) return NULL;
            return unpack_i64(e, ot->elem, r);
        }
        if (ot->kind == TY_MUTEX) {
            Type *fnty = n->args->type;
            char *h = gen_expr(e, n->lhs);
            char *fv = gen_expr(e, n->args);
            const char *th = thunk_for(e, fnty);
            char *fi = new_tmp(e);
            sb_printf(&e->fn, "  %s = ptrtoint ptr %s to i64\n", fi, fv);
            declare_rt(e, "i64 @pl_mutex_with(ptr, ptr, i64)");
            char *r = new_tmp(e);
            sb_printf(&e->fn,
                      "  %s = call i64 @pl_mutex_with(ptr %s, ptr %s, i64 %s)\n",
                      r, h, th, fi);
            if (n->type->kind == TY_NONE) return NULL;
            return unpack_i64(e, n->type, r);
        }
    }

    // ★ インタフェース越しの呼び出し。
    //   ① 実体の先頭から vtable を読む ② スロットの関数ポインタを読む ③ 呼ぶ
    //   注意: 呼び先はコンパイル時には決まりません（実行時の型で決まります）。
    if (n->is_iface_call) {
        char *obj = gen_expr(e, n->lhs);
        char *ok = gen_not_none(e, obj);

        char *vt = new_tmp(e);
        sb_printf(&e->fn, "  %s = load ptr, ptr %s\n", vt, ok);
        char *slotp = new_tmp(e);
        sb_printf(&e->fn,
                  "  %s = getelementptr [%d x ptr], ptr %s, i32 0, i32 %d\n",
                  slotp, pl_iface_slots, vt, n->iface_slot);
        char *fp = new_tmp(e);
        sb_printf(&e->fn, "  %s = load ptr, ptr %s\n", fp, slotp);

        StrBuf ia, it;
        sb_init(&ia);
        sb_init(&it);
        sb_printf(&ia, "ptr %s", ok);
        gen_args(e, n->args, &ia, &it, false);

        // ★ 失敗しうるメソッドなら、エラースロットも渡します（A-40）。
        //   **直接呼び出しと同じ段取り**です（呼び先がレジスタなだけ）。
        return emit_call_to(e, n, fp, sb_str(&ia));
    }

    // クラスのメソッド。self を第 1 引数に渡すだけ。
    // ★ 呼ぶ関数名は sema が修飾済み（n->ir_name = "lexer.Token.show"）。
    // ★ rc[T] の中身のメソッドも同じように呼べます（自動デリファレンス）。
    if (n->lhs->type->kind == TY_CLASS || n->lhs->type->kind == TY_RC) {
        char *obj = deref_rc(e, n->lhs->type, gen_expr(e, n->lhs));

        StrBuf args, types;
        sb_init(&args);
        sb_init(&types);
        sb_printf(&args, "ptr %s", obj);
        sb_printf(&types, "ptr");
        gen_args(e, n->args, &args, &types, false);

        // ★ import したクラスのメソッドは、このモジュールには
        //   定義がないので declare する（sema が is_extern を立てている）。
        if (n->is_extern) {
            if (n->can_fail) sb_printf(&types, ", ptr");
            declare_extern(e, llvm_type(n->type), n->ir_name, sb_str(&types));
        }

        return emit_call(e, n, sb_str(&args));
    }

    // ── list のメソッド（append とその他）──
    //
    // ★ ランタイム側は **すべて i64 のスロット**で扱います。list の中身が
    //   「ポインタ 1 個か i64 1 個」という当初の作りのままなので、
    //   pop / insert / remove は 1 つの関数で全部の要素型に効きます。
    //   要素の型を意識するのは、値を出し入れするときの変換だけです。
    Type *elem = n->lhs->type->elem;
    char *obj = gen_expr(e, n->lhs);

    if (strcmp(n->name, "pop") == 0 || strcmp(n->name, "remove") == 0) {
        const char *fn = strcmp(n->name, "pop") == 0 ? "pl_list_pop"
                                                     : "pl_list_remove_at";
        char *t = new_tmp(e);
        if (strcmp(n->name, "pop") == 0) {
            declare_rt(e, "i64 @pl_list_pop(ptr)");
            sb_printf(&e->fn, "  %s = call i64 @%s(ptr %s)\n", t, fn, obj);
        } else {
            declare_rt(e, "i64 @pl_list_remove_at(ptr, i64)");
            char *i = gen_expr(e, n->args);
            sb_printf(&e->fn, "  %s = call i64 @%s(ptr %s, i64 %s)\n", t, fn,
                      obj, i);
        }
        // i64 のスロットから要素の型へ戻す
        if (elem_is_ptr(elem)) {
            char *pt = new_tmp(e);
            sb_printf(&e->fn, "  %s = inttoptr i64 %s to ptr\n", pt, t);
            return pt;
        }
        return slot_to_elem(e, elem, t);
    }

    if (strcmp(n->name, "insert") == 0) {
        declare_rt(e, "void @pl_list_insert(ptr, i64, i64)");
        char *i = gen_expr(e, n->args);
        char *v = elem_to_slot(e, elem,
                               maybe_retain(e, n->args->next,
                                            gen_expr(e, n->args->next)));
        char *iv = v;
        if (elem_is_ptr(elem)) {
            iv = new_tmp(e);
            sb_printf(&e->fn, "  %s = ptrtoint ptr %s to i64\n", iv, v);
        }
        sb_printf(&e->fn,
                  "  call void @pl_list_insert(ptr %s, i64 %s, i64 %s)\n", obj,
                  i, iv);
        return NULL;
    }

    if (strcmp(n->name, "index") == 0) {
        Type *el = elem;
        const char *fn_name;
        const char *aty;
        if (el->kind == TY_FLOAT)      { fn_name = "pl_list_index_f64"; aty = "double"; }
        else if (el->kind == TY_STR)   { fn_name = "pl_list_index_str"; aty = "ptr"; }
        else if (elem_is_ptr(el))      { fn_name = "pl_list_index_ptr"; aty = "ptr"; }
        else                           { fn_name = "pl_list_index_i64"; aty = "i64"; }
        StrBuf sig;
        sb_init(&sig);
        sb_printf(&sig, "i64 @%s(ptr, %s)", fn_name, aty);
        declare_rt(e, sb_str(&sig));
        char *v = elem_to_slot_cmp(e, el, gen_expr(e, n->args));
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = call i64 @%s(ptr %s, %s %s)\n", t, fn_name,
                  obj, aty, v);
        return t;
    }

    if (strcmp(n->name, "reverse") == 0) {
        declare_rt(e, "void @pl_list_reverse(ptr)");
        sb_printf(&e->fn, "  call void @pl_list_reverse(ptr %s)\n", obj);
        return NULL;
    }
    if (strcmp(n->name, "clear") == 0) {
        declare_rt(e, "void @pl_list_clear(ptr)");
        sb_printf(&e->fn, "  call void @pl_list_clear(ptr %s)\n", obj);
        return NULL;
    }
    if (strcmp(n->name, "copy") == 0) {
        declare_rt(e, "ptr @pl_list_copy(ptr)");
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = call ptr @pl_list_copy(ptr %s)\n", t, obj);
        return t;
    }
    if (strcmp(n->name, "extend") == 0) {
        declare_rt(e, "void @pl_list_extend(ptr, ptr)");
        char *o = gen_expr(e, n->args);
        sb_printf(&e->fn, "  call void @pl_list_extend(ptr %s, ptr %s)\n", obj, o);
        return NULL;
    }

    const char *sty = slot_ty(elem);
    const char *push = elem_is_ptr(elem) ? "pl_list_push_ptr" : "pl_list_push_i64";
    StrBuf sig;
    sb_init(&sig);
    sb_printf(&sig, "void @%s(ptr, %s)", push, sty);
    declare_rt(e, sb_str(&sig));

    char *v = elem_to_slot(e, elem, maybe_retain(e, n->args, gen_expr(e, n->args)));
    sb_printf(&e->fn, "  call void @%s(ptr %s, %s %s)\n", push, obj, sty, v);
    return NULL;
}

// ── class の生成 ──────────────────────────────────────
//
// ★ 使う道具は getelementptr ひとつだけです。
//   「オブジェクトの何番目のフィールドか」を渡すと、
//   バイト数への変換（パディング込み）は LLVM がやってくれます。

// フィールドのアドレスを求める。読み出しにも代入にも使います。
// rc[T] の参照を 1 つ増やす。
//
// ★ 増やすのは「場所から読んだ参照を、別の場所に置く」ときだけです。
//   rc(...) や関数の戻り値は**新しい参照**なので、そのまま置けます。
// 注意: retain と release は対です。解放を挿さない（--drop 無し）ときは、
//    どちらも出しません（数が合わなくなるより、何もしないほうが安全）。
// 注意: **`rc[T] | None` も数えます。** ここを TY_RC だけで見ていたので、
//   nullable な rc を共有すると数が足りず、`--drop` で早すぎる解放になりました。
static bool ty_is_rc_shared(Type *t) {
    if (!t) return false;
    if (t->kind == TY_RC) return true;
    return t->kind == TY_OPT && t->elem && t->elem->kind == TY_RC;
}

// 参照を 1 つ増やす（場所かどうかを問わない）。
static char *force_retain(Emitter *e, char *val) {
    declare_rt(e, "ptr @pl_rc_retain(ptr)");
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = call ptr @pl_rc_retain(ptr %s)\n", t, val);
    return t;
}

static char *maybe_retain(Emitter *e, Node *rhs, char *val) {
    if (!e->drop || !rhs || !ty_is_rc_shared(rhs->type)) return val;
    if (rhs->kind != ND_VAR && rhs->kind != ND_FIELD && rhs->kind != ND_INDEX)
        return val;
    declare_rt(e, "ptr @pl_rc_retain(ptr)");
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = call ptr @pl_rc_retain(ptr %s)\n", t, val);
    return t;
}

// rc[T] なら中身を取り出す（自動デリファレンス）
//
// ★ **呼び出しではなく IR に展開します**（同じ手）。
//   コンパイラ自身を rc[T] へ移したら、フィールド参照とメソッド
//   呼び出しのたびに `call @pl_rc_get` が出るようになり、stage1 が
//   5〜10% 遅くなりました。中身は**オフセット 16 の load 1 つ**です
//   （`PlRc { i64 strong; i64 borrow; ptr value; }`。runtime/core.c）。
//
// 注意: **null の検査は外しません**（方針 §0-②）。移動済みのスロットは
//   null になるので（決定 D17）、黙って隣を読むわけにはいきません。
//   外れの経路は `pl_rc_none_fail`（noreturn cold）です。
static char *deref_rc(Emitter *e, Type *t, char *v) {
    if (!t || t->kind != TY_RC) return v;
    declare_rt(e, "void @pl_rc_none_fail() noreturn cold");

    int id = e->label_counter++;
    char ok_l[32], bad_l[32];
    snprintf(ok_l, sizeof(ok_l), "rc.ok.%d", id);
    snprintf(bad_l, sizeof(bad_l), "rc.bad.%d", id);

    char *isnull = new_tmp(e);
    sb_printf(&e->fn, "  %s = icmp eq ptr %s, null\n", isnull, v);
    emit_cond_br(e, isnull, bad_l, ok_l);

    emit_label(e, bad_l);
    sb_printf(&e->fn, "  call void @pl_rc_none_fail()\n");
    sb_printf(&e->fn, "  unreachable\n");
    e->terminated = true;

    emit_label(e, ok_l);
    char *vp = new_tmp(e);
    sb_printf(&e->fn, "  %s = getelementptr i8, ptr %s, i64 16\n", vp, v);
    char *g = new_tmp(e);
    sb_printf(&e->fn, "  %s = load ptr, ptr %s\n", g, vp);
    return g;
}

// ── 「空の値」を作る ────────────────────────────────────────
//
// ★ ゼロ初期化では足りない型に入れる、有効で中身が無い値です。
//     str    → ""（静的なので --drop でも解放されません）
//     list[T] → 新しい空のリスト
//
//   使うのは 2 か所です：クラスを作るとき（init の書き忘れ対策）と、
//   move_out(場所) が値を持っていったあとの書き戻しです。
static char *gen_empty_value(Emitter *e, Type *ty) {
    if (ty->kind == TY_STR) return intern_str(e, "", 0);
    declare_rt(e, "ptr @pl_list_new()");
    char *val = new_tmp(e);
    sb_printf(&e->fn, "  %s = call ptr @pl_list_new()\n", val);
    return val;
}

static char *gen_field_ptr(Emitter *e, Node *n) {
    Type *ot = n->lhs->type;
    Class *c = ot->kind == TY_RC ? ot->elem->cls : ot->cls;
    char *obj = deref_rc(e, ot, gen_expr(e, n->lhs));

    // 注意: クラス型のフィールドは NULL から始まります（12.6 節）。
    //    NULL 参照を segfault ではなく親切なメッセージに変えます。
    // ★ 検査を IR に展開します（規約 R10 の例外。理由は gen_not_none）。
    char *ok = gen_not_none(e, obj);

    // 注意: 第 1 インデックスは常に 0（「Token の配列の何個目か」）。
    //    ここを 1 にすると隣のオブジェクトがある場所を読みます。
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = getelementptr %%%s.type, ptr %s, i32 0, i32 %d\n", t,
              class_type(e, c), ok, n->field->index);
    return t;
}

// 他モジュールのグローバル変数は、使う側の .ll に external で宣言する。
//   @g.lexer.MAX_KIND = external global i64
static void declare_extern_global(Emitter *e, Node *n) {
    StrBuf sig;
    sb_init(&sig);
    sb_printf(&sig, "%s = external global %s", n->ir_name, llvm_mem_type(n->type));

    for (StrLit *d = e->decled; d; d = d->next)
        if (strcmp(d->label, sb_str(&sig)) == 0) return;

    sb_printf(&e->globals, "%s\n", sb_str(&sig));
    StrLit *d = xmalloc(sizeof(StrLit));
    d->label = sb_str(&sig);
    d->next = e->decled;
    e->decled = d;
}

static char *gen_field(Emitter *e, Node *n) {
    // ★ math.sin のように、他モジュールの関数を値として使う場合。
    //   注意: 呼ぶわけではないので **declare だけ**出して、ラベルを値にします。
    if (n->is_func_ref) {
        StrBuf types;
        sb_init(&types);
        Type *ft = n->type;
        for (int i = 0; i < ft->nparams; i++)
            sb_printf(&types, "%s%s", i ? ", " : "", llvm_type(ft->params[i]));
        declare_extern(e, llvm_type(ft->elem), n->ir_name, sb_str(&types));
        char *t = xmalloc(strlen(n->ir_name) + 2);
        snprintf(t, strlen(n->ir_name) + 2, "@%s", n->ir_name);
        return t;
    }

    // ★ '.' の左がモジュールなら、これはグローバル変数の読み出し。
    //   ND_FIELD のままだが、sema が ir_name を入れているので変数と同じ扱い。
    if (n->mod_name) {
        if (n->is_extern) declare_extern_global(e, n);
        return gen_load(e, n->type, n->ir_name);
    }

    // ★ 読み書きは gen_load / gen_store をそのまま使います。
    //   bool フィールドの i8 ↔ i1 変換（規約 R5）は、何も書かずに手に入ります。
    // ★ ここは「クラスのフィールド」だと LLVM に伝えます（TBAA）。
    return gen_load_tb(e, n->type, gen_field_ptr(e, n), TBAA_FIELD);
}

// インスタンス生成 Token(1, "x")。
//
//   ① ヒープに確保する（pl_alloc は calloc なので必ずゼロ初期化される）
//   ② 参照型フィールドに既定値を入れる（12.6 節）
//   ③ init があれば呼ぶ
// ── vtable ──────────────────────────────────────────────
//
// ★ クラス 1 つにつき **1 本**の表を出します。中身は「プログラム全体の
//   インタフェース・メソッドのスロット数」ぶんの関数ポインタの配列で、
//   そのクラスが実装しているスロットだけが埋まり、残りは null です。
//
// なぜスロットを全体で一意にするのか
//   1 つのクラスが複数のインタフェースを実装できるようにするためです。
//   インタフェースごとに 0 から番号を振ると、呼ぶ側（インタフェースしか
//   知らない）がどの表のどこを見ればよいか決められません。
//   注意: 代わりに表は疎になります。インタフェースが増えると全クラスの表が
//     伸びるので、数が多くなったら別の方式（実行時に探す）が要ります。
static void gen_vtable(Emitter *e, Class *c) {
    if (!c->impls) return;
    for (StrLit *t = e->vtables; t; t = t->next)
        if (strcmp(t->label, c->ir_name) == 0) return;  // 出済み

    StrLit *rec = xmalloc(sizeof(StrLit));
    rec->label = c->ir_name;
    rec->next = e->vtables;
    e->vtables = rec;

    int n = pl_iface_slots;
    sb_printf(&e->globals, "@vt.%s = private constant [%d x ptr] [", c->ir_name,
              n);
    for (int i = 0; i < n; i++) {
        const char *fn = NULL;
        for (IfaceList *l = c->impls; l && !fn; l = l->next)
            for (IMethod *im = l->iface->methods; im; im = im->next)
                if (im->slot == i) {
                    // ★ そのクラスの実装（名前は "Class.method" で修飾済み）
                    static char buf[256];
                    snprintf(buf, sizeof(buf), "%s.%s", c->ir_name, im->name);
                    fn = buf;
                    break;
                }
        if (fn)
            sb_printf(&e->globals, "%sptr @%s", i ? ", " : "", fn);
        else
            sb_printf(&e->globals, "%sptr null", i ? ", " : "");
    }
    sb_printf(&e->globals, "]\n");
}

static char *gen_new(Emitter *e, Node *n) {
    Class *c = n->cls;

    declare_rt(e, "ptr @pl_alloc(i64)");
    char *obj = new_tmp(e);
    sb_printf(&e->fn, "  %s = call ptr @pl_alloc(i64 %d)\n", obj, c->size);

    // ★ インタフェースを実装するなら、先頭に vtable を入れます。
    //   注意: ここを忘れると、インタフェース越しの呼び出しが null を呼びます。
    if (c->impls) {
        gen_vtable(e, c);
        char *vp = new_tmp(e);
        sb_printf(&e->fn,
                  "  %s = getelementptr %%%s.type, ptr %s, i32 0, i32 0\n", vp,
                  c->ir_name, obj);
        sb_printf(&e->fn, "  store ptr @vt.%s, ptr %s\n", c->ir_name, vp);
    }

    // ★ ゼロ初期化では足りない型に、有効な値を入れておきます。
    //   str → ""、list[T] → 空のリスト。
    //   これで「init を書き忘れたら壊れる」がほぼ無くなります。
    for (Field *f = c->fields; f; f = f->next) {
        if (f->type->kind != TY_STR && f->type->kind != TY_LIST) continue;

        char *val = gen_empty_value(e, f->type);
        char *p = new_tmp(e);
        sb_printf(&e->fn, "  %s = getelementptr %%%s.type, ptr %s, i32 0, i32 %d\n",
                  p, class_type(e, c), obj, f->index);
        sb_printf(&e->fn, "  store ptr %s, ptr %s\n", val, p);
    }

    if (!c->has_init) return obj;

    // init は「self を第 1 引数に取るふつうの関数」（規約 R8 がそのまま働く）
    StrBuf args, ptypes;
    sb_init(&args);
    sb_init(&ptypes);
    sb_printf(&args, "ptr %s", obj);
    sb_printf(&ptypes, "ptr");
    for (Node *a = n->args; a; a = a->next) {
        char *v = gen_expr(e, a);
        // ★ gen_args と同じ理由で、own の仮引数へ渡す rc[T] は参照 +1。
        //   （生成は init を直に呼ぶので、gen_args を通りません）
        if (e->drop && a->arg_own_rc) v = force_retain(e, v);
        sb_printf(&args, ", %s %s", llvm_type(a->type), v);
        sb_printf(&ptypes, ", %s", llvm_type(a->type));
    }

    StrBuf init;
    sb_init(&init);
    sb_printf(&init, "%s.init", c->ir_name);

    // ★ 別モジュールのクラスなら declare が要る
    if (n->is_extern) declare_extern(e, "void", sb_str(&init), sb_str(&ptypes));

    sb_printf(&e->fn, "  call void @%s(%s)\n", sb_str(&init), sb_str(&args));
    return obj;
}

// クラスの型定義を出す：%lexer.Token.type = type { i64, ptr }
//
// ★ フィールドの LLVM 型は「メモリ上の型」（bool は i8。規約 R5）。
// 注意: LLVM の型定義はモジュールローカルです。import したクラスを
//    使うモジュールにも、同じ定義を書き直す必要があります。レイアウトは
//    コンパイラのプロセス内で 1 回だけ計算した Class * を共有しているので、
//    2 つの .ll が食い違うことはありません（13.7 節）。
static void gen_class_type(Emitter *e, Class *c) {
    for (StrLit *t = e->types; t; t = t->next)
        if (strcmp(t->label, c->ir_name) == 0) return;  // 出済み

    StrLit *t = xmalloc(sizeof(StrLit));
    t->label = c->ir_name;
    t->next = e->types;
    e->types = t;

    sb_printf(&e->header, "%%%s.type = type { ", c->ir_name);
    bool first = true;
    // ★ インタフェースを実装するなら、先頭に vtable へのポインタ。
    //   注意: sema の layout_class と **並びを揃えること**（ずれると全部壊れます）。
    if (c->impls) {
        sb_printf(&e->header, "ptr");
        first = false;
    }
    for (Field *f = c->fields; f; f = f->next) {
        sb_printf(&e->header, "%s%s", first ? "" : ", ", llvm_mem_type(f->type));
        first = false;
    }
    // 注意: フィールドが 0 個でも空の構造体は書けます（サイズ 0）。
    //    pl_alloc(0) は calloc(1, 0) になり、有効なポインタが返ります。
    sb_printf(&e->header, " }\n");
}

// クラスの型名を返す（まだ出していなければ定義も出す）。
//
// ★ 「使ったものだけ出す」ので、import したクラスの型定義も自動で付いてきます。
static const char *class_type(Emitter *e, Class *c) {
    gen_class_type(e, c);
    return c->ir_name;
}

// 別モジュールの関数を declare する（引数の型は呼び出しから作る）。
//
// ★ declare_rt と同じ仕組み（使ったものだけ宣言する）。
static void declare_extern(Emitter *e, const char *ret, const char *ir_name,
                           const char *param_types) {
    StrBuf sig;
    sb_init(&sig);
    sb_printf(&sig, "%s @%s(%s)", ret, ir_name, param_types);
    declare_rt(e, sb_str(&sig));
}



// ── エラー処理の生成 ───────────────────────────────────
//
// 設計は docs/design/error-handling.md。**アンワインドはしません。**
// 失敗しうる関数は、末尾に「エラー出力ポインタ」を 1 本余分に取ります。
//
//   define ptr @read(ptr %path, ptr %err.out)
//
//   %pl.err = type { i64, ptr }     ; { タグ, エラーオブジェクト }
//   タグ 0 は「エラー無し」に予約。

// %pl.err の型定義を 1 回だけ出す
static void ensure_err_type(Emitter *e) {
    if (e->err_type_emitted) return;
    e->err_type_emitted = true;
    sb_printf(&e->header, "%%pl.err = type { i64, ptr }\n");
}

// この関数のエラースロット（呼び出しの結果を受け取る場所）を用意する
static const char *err_slot(Emitter *e) {
    ensure_err_type(e);
    if (!e->err_slot) {
        e->err_slot = true;
        sb_printf(&e->allocas, "  %%err.slot = alloca %%pl.err\n");
    }
    return "%err.slot";
}

// 失敗して戻るときの ret（値は使われない。設計 §2 の表）
static void emit_default_ret(Emitter *e) {
    const char *dv = default_value(e->fn_ret);
    if (!dv) sb_printf(&e->fn, "  ret void\n");
    else sb_printf(&e->fn, "  ret %s %s\n", llvm_type(e->fn_ret), dv);
    e->terminated = true;
}

// 型ごとの「使われない戻り値」（設計 §2 の表）
static const char *default_value(Type *t) {
    switch (t->kind) {
        case TY_NONE: return NULL;  // void（値を返さない）
        case TY_INT: return "0";
        case TY_BOOL: return "false";
        default: return "null";  // str / list / class / T | None
    }
}

// 失敗したときの飛び先（内側の try があればその振り分け、無ければ伝播）。
//
// 注意: どちらでもない場合（この関数は raises を宣言していない）は **到達しません**。
//    sema が「捕まえるか宣言するか」を強制しているからです（E-RAISE-1）。
//    その場合は NULL を返し、呼ぶ側が unreachable を出します。
static const char *fail_label(Emitter *e) {
    if (e->try_ctx) return e->try_ctx->dispatch;
    if (!e->fn_raises) return NULL;
    e->prop_used = true;
    return e->prop_label;
}

// 失敗の経路へ飛ぶ（飛び先が無ければ unreachable）
static void emit_fail_br(Emitter *e) {
    const char *l = fail_label(e);
    if (l) {
        emit_br(e, l);
    } else {
        sb_printf(&e->fn, "  unreachable\n");
        e->terminated = true;
    }
}

// エラー（タグと値）をスロットへ書く
static void store_err(Emitter *e, const char *slot, int tag, const char *obj) {
    ensure_err_type(e);
    char *tp = new_tmp(e);
    sb_printf(&e->fn, "  %s = getelementptr %%pl.err, ptr %s, i32 0, i32 0\n", tp, slot);
    sb_printf(&e->fn, "  store i64 %d, ptr %s\n", tag, tp);
    char *pp = new_tmp(e);
    sb_printf(&e->fn, "  %s = getelementptr %%pl.err, ptr %s, i32 0, i32 1\n", pp, slot);
    sb_printf(&e->fn, "  store ptr %s, ptr %s\n", obj, pp);
}

// スロットからタグを読む
static char *load_tag(Emitter *e, const char *slot) {
    char *tp = new_tmp(e);
    sb_printf(&e->fn, "  %s = getelementptr %%pl.err, ptr %s, i32 0, i32 0\n", tp, slot);
    char *tv = new_tmp(e);
    sb_printf(&e->fn, "  %s = load i64, ptr %s\n", tv, tp);
    return tv;
}

// スロットからエラーオブジェクトを読む
static char *load_payload(Emitter *e, const char *slot) {
    char *pp = new_tmp(e);
    sb_printf(&e->fn, "  %s = getelementptr %%pl.err, ptr %s, i32 0, i32 1\n", pp, slot);
    char *pv = new_tmp(e);
    sb_printf(&e->fn, "  %s = load ptr, ptr %s\n", pv, pp);
    return pv;
}

// ── 解放（drop）の生成 ─────────────────────────────────
//
// 設計は docs/design/ownership.md §6。仕様は safety-spec.md §6。
//
// ★ この章のいちばん大きな判断：**drop フラグを持たない。**
//
//   設計 §6.3 は Rust に倣って「MaybeMoved の場所は alloca i1 のフラグを持つ」
//   としていました。しかし本言語の所有型（str / list / class）は
//   **すべてポインタ**なので、**移動したときにスロットへ null を書けば**
//   同じことができます。解放関数はどれも null を受け取れるので、
//   フラグの alloca も分岐も要りません（決定 D17）。
//
//     xs: list[int] = [1, 2]      %xs = alloca ptr        …… 所有している
//     ys: list[int] = xs          store ptr null, ptr %xs …… 移動した印
//     （スコープ終端）              %v = load ptr, ptr %xs
//                                 call void @drop.list.0(ptr %v)  …… null なら何もしない

static const char *drop_fn_for(Emitter *e, Type *t);
static const char *gen_rc_drop(Emitter *e, Type *t);

// 生成済みの @drop.* を覚えておく（型名を鍵にする）
static const char *drop_fn_cached(Emitter *e, const char *key) {
    for (StrLit *d = e->dropfns; d; d = d->next)
        if (strcmp(d->bytes, key) == 0) return d->label;
    return NULL;
}

static void drop_fn_remember(Emitter *e, const char *key, const char *name) {
    StrLit *d = xmalloc(sizeof(StrLit));
    d->bytes = (char *)key;
    d->label = (char *)name;
    d->next = e->dropfns;
    e->dropfns = d;
}

// クラス C を解放する関数 @drop.<C> を生成する。
//
//   ① drop メソッドがあれば先に呼ぶ（デストラクタ。仕様 §6.2）
//   ② 所有型のフィールドを宣言順に解放する
//   ③ インスタンス自身を解放する
//
// 注意: 自分自身を含むクラス（連結リストなど）では再帰します。
//    長いリストではスタックを使い切る可能性があります（見直します）。
static const char *gen_class_drop(Emitter *e, Class *c) {
    StrBuf key;
    sb_init(&key);
    sb_printf(&key, "class:%s", c->ir_name);
    const char *hit = drop_fn_cached(e, sb_str(&key));
    if (hit) return hit;

    StrBuf name;
    sb_init(&name);
    sb_printf(&name, "@drop.%s", c->ir_name);
    // ★ 先に登録します。フィールドが自分自身の型でも無限再帰しないため。
    drop_fn_remember(e, sb_str(&key), sb_str(&name));

    // ユーザー定義の drop メソッド（デストラクタ）を探す
    Node *dtor = NULL;
    if (c->node)
        for (Node *m = c->node->body; m; m = m->next)
            if (m->kind == ND_FUNC && strcmp(m->name, "drop") == 0) dtor = m;

    // ★ フィールドの解放関数を先に作ります（本体を書き始める前に）。
    //   drop_fn_for が e->dropdefs に書き足すので、書きかけの本体と混ざらないように。
    const char *ftype = class_type(e, c);
    const char **fdrops = xmalloc(sizeof(char *) * (size_t)(c->nfields + 1));
    int nf = 0;
    for (Field *f = c->fields; f; f = f->next) fdrops[nf++] = drop_fn_for(e, f->type);

    StrBuf b;
    sb_init(&b);
    sb_printf(&b, "\ndefine internal void %s(ptr %%p) {\nentry:\n", sb_str(&name));
    sb_printf(&b, "  %%isnull = icmp eq ptr %%p, null\n");
    sb_printf(&b, "  br i1 %%isnull, label %%done, label %%body\nbody:\n");

    if (dtor) {
        // 注意: 別モジュールのクラスなら declare が要ります。自分のモジュールで
        //    定義しているクラスに declare を出すと「再定義」で落ちます。
        bool local = false;
        for (Node *d = e->ast->body; d; d = d->next)
            if (d->kind == ND_CLASS && !d->targs && d->cls == c) local = true;
        if (!local) declare_extern(e, "void", dtor->ir_name, "ptr");
        sb_printf(&b, "  call void @%s(ptr %%p)\n", dtor->ir_name);
    }

    int i = 0;
    for (Field *f = c->fields; f; f = f->next, i++) {
        if (!fdrops[i]) continue;  // コピー型のフィールドは何もしない
        sb_printf(&b, "  %%f%d = getelementptr %%%s.type, ptr %%p, i32 0, i32 %d\n", i,
                  ftype, f->index);
        sb_printf(&b, "  %%v%d = load ptr, ptr %%f%d\n", i, i);
        sb_printf(&b, "  call void %s(ptr %%v%d)\n", fdrops[i], i);
    }

    declare_rt(e, "void @pl_drop_obj(ptr)");
    sb_printf(&b, "  call void @pl_drop_obj(ptr %%p)\n");
    sb_printf(&b, "  br label %%done\ndone:\n  ret void\n}\n");
    sb_printf(&e->dropdefs, "%s", sb_str(&b));

    return sb_str(&name);
}

// list[T] を解放する関数を生成する。
//
// ★ pl_drop_list は「要素を解放する関数」を受け取ります（要素がコピー型なら null）。
//   引数が 2 つあるので、そのままでは「ptr を 1 つ取る解放関数」の形に合いません。
//   包む関数を 1 つ作れば、あとはどの型でも同じ形で扱えます。
static const char *gen_list_drop(Emitter *e, Type *t) {
    StrBuf key;
    sb_init(&key);
    sb_printf(&key, "list:%s", type_name(t));
    const char *hit = drop_fn_cached(e, sb_str(&key));
    if (hit) return hit;

    StrBuf name;
    sb_init(&name);
    sb_printf(&name, "@drop.list.%d", e->drop_counter++);
    drop_fn_remember(e, sb_str(&key), sb_str(&name));

    const char *elem = drop_fn_for(e, t->elem);

    declare_rt(e, "void @pl_drop_list(ptr, ptr)");
    sb_printf(&e->dropdefs,
              "\ndefine internal void %s(ptr %%l) {\nentry:\n"
              "  call void @pl_drop_list(ptr %%l, ptr %s)\n"
              "  ret void\n}\n",
              sb_str(&name), elem ? elem : "null");
    return sb_str(&name);
}

// rc[T] を手放す関数を生成する（カウントを 1 減らす）
static const char *gen_rc_drop(Emitter *e, Type *t) {
    StrBuf key;
    sb_init(&key);
    sb_printf(&key, "rc:%s", type_name(t));
    const char *hit = drop_fn_cached(e, sb_str(&key));
    if (hit) return hit;

    StrBuf name;
    sb_init(&name);
    sb_printf(&name, "@drop.rc.%d", e->drop_counter++);
    drop_fn_remember(e, sb_str(&key), sb_str(&name));

    const char *inner = drop_fn_for(e, t->elem);
    declare_rt(e, "void @pl_rc_release(ptr, ptr)");
    sb_printf(&e->dropdefs,
              "\ndefine internal void %s(ptr %%p) {\nentry:\n"
              "  call void @pl_rc_release(ptr %%p, ptr %s)\n"
              "  ret void\n}\n",
              sb_str(&name), inner ? inner : "null");
    return sb_str(&name);
}

// 型 t の値 1 つを解放する関数の名前。コピー型なら NULL。
// 中身を持つ列挙を解放する関数を作る（A-41）。
//
// ★ **どの枝かは実行時にしか分かりません。** 先頭のタグを読んで、
//   その枝のクラスの解放関数へ振り分けます（枝ごとにフィールドが違うので、
//   1 つの解放関数では済みません）。
//
//   注意: 名前だけの列挙（A-37）はただの i64 なので、ここには来ません。
static const char *gen_enum_drop(Emitter *e, Type *t) {
    EnumDef *en = t->en;
    StrBuf key;
    sb_init(&key);
    sb_printf(&key, "enum:%s.%s", en->owner ? "m" : "", en->name);
    const char *hit = drop_fn_cached(e, sb_str(&key));
    if (hit) return hit;

    StrBuf name;
    sb_init(&name);
    sb_printf(&name, "@drop.enum.%d", e->drop_counter++);
    // ★ 先に登録します。枝が自分自身を持つ列挙（木）でも無限再帰しないため。
    drop_fn_remember(e, sb_str(&key), sb_str(&name));

    // 枝ごとの解放関数を先に作ります（本体を書き始める前に）。
    int nv = en->nvals;
    const char **bd = xmalloc(sizeof(char *) * (size_t)(nv ? nv : 1));
    int i = 0;
    for (EnumVal *v = en->vals; v; v = v->next, i++)
        bd[i] = v->cls ? gen_class_drop(e, v->cls) : NULL;

    StrBuf b;
    sb_init(&b);
    sb_printf(&b, "\ndefine internal void %s(ptr %%p) {\nentry:\n", sb_str(&name));
    sb_printf(&b, "  %%isnull = icmp eq ptr %%p, null\n");
    sb_printf(&b, "  br i1 %%isnull, label %%done, label %%body\nbody:\n");
    sb_printf(&b, "  %%tag = load i64, ptr %%p\n");
    sb_printf(&b, "  switch i64 %%tag, label %%done [\n");
    i = 0;
    for (EnumVal *v = en->vals; v; v = v->next, i++)
        sb_printf(&b, "    i64 %lld, label %%b%d\n", v->val, i);
    sb_printf(&b, "  ]\n");
    i = 0;
    for (EnumVal *v = en->vals; v; v = v->next, i++) {
        sb_printf(&b, "b%d:\n", i);
        if (bd[i]) sb_printf(&b, "  call void %s(ptr %%p)\n", bd[i]);
        sb_printf(&b, "  br label %%done\n");
    }
    sb_printf(&b, "done:\n  ret void\n}\n");
    sb_printf(&e->dropdefs, "%s", sb_str(&b));

    return sb_str(&name);
}

static const char *drop_fn_for(Emitter *e, Type *t) {
    if (!t) return NULL;
    switch (t->kind) {
        case TY_STR:
            declare_rt(e, "void @pl_drop_str(ptr)");
            return "@pl_drop_str";
        case TY_LIST: return gen_list_drop(e, t);
        case TY_RC: return gen_rc_drop(e, t);
        case TY_CLASS: return gen_class_drop(e, t->cls);
        // ★ 中身を持つ列挙（A-41）。名前だけの列挙は i64 なので何もしません。
        case TY_ENUM: return t->en && t->en->has_payload ? gen_enum_drop(e, t) : NULL;
        // T | None は中身と同じ扱い（解放関数はどれも null を受け取れる）
        case TY_OPT: return drop_fn_for(e, t->elem);
        default: return NULL;  // int / bool / None
    }
}

// 変数 1 つを解放する（スロットを読んで、解放関数に渡すだけ）
static void emit_drop_slot(Emitter *e, Node *decl) {
    const char *fn = drop_fn_for(e, decl->type);
    if (!fn) return;
    char *v = new_tmp(e);
    sb_printf(&e->fn, "  %s = load ptr, ptr %s\n", v, decl->ir_name);
    sb_printf(&e->fn, "  call void %s(ptr %s)\n", fn, v);
}

// 一時的な値を解放する（式文の結果など）
static void emit_drop_value(Emitter *e, Type *t, const char *val);

// ── 式の途中に現れる「一時値」の解放（A-21e）────────────────
//
// ★ 何が問題だったか（docs/roadmap.md A-21e）
//   `s: str = "x" + str(i)` の **str(i) の結果**は、どこにも束縛されないまま
//   pl_str_concat に渡され、**誰も解放しません**。束縛すれば解放されるので、
//   式の途中に現れる一時値だけが漏れていました。
//
// 注意: **所有権が移る場所では呼んではいけません。** xs.append(str(i)) は
//   リストが所有権を受け取るので、ここで解放すると二重解放になります。
//   呼んでよいのは「借りて読むだけ」の場所です:
//     ・二項演算のオペランド（連結・比較）
//     ・組み込みの引数（len / str / print …）
//     ・own でない仮引数へ渡した実引数
static bool is_owned_temp(Node *n) {
    if (!n || !n->type || !ty_is_owned(n->type)) return false;
    if (n->binds_borrow) return false;  // 借用を返す関数の戻り値（仕様 §4.5）
    switch (n->kind) {
        // 新しく作られる値。ほかに持ち主がいない。
        case ND_CALL:
        case ND_METHOD:
        case ND_BINOP:
        case ND_LIST:
            return true;
        // 変数・フィールド・添字の読みは借用。リテラルは静的。
        default:
            return false;
    }
}

// 一時値なら解放する（--drop のときだけ）。
static void drop_temp(Emitter *e, Node *n, const char *val) {
    if (!e->drop || !val || !is_owned_temp(n)) return;
    emit_drop_value(e, n->type, val);
}

static void emit_drop_value(Emitter *e, Type *t, const char *val) {
    const char *fn = drop_fn_for(e, t);
    if (!fn) return;
    sb_printf(&e->fn, "  call void %s(ptr %s)\n", fn, val);
}

// この変数はスコープ終端で解放する対象か。
//
// 注意: 借りものを束縛している変数（`t = xs[i]` や for のループ変数）は
//    **所有していない**ので解放しません。ownck が印を付けています。
static bool is_droppable(Node *decl) {
    // ★ 中身を持つ列挙（A-41）はヒープの物体です。名前だけの列挙は i64 なので
    //   解放しません（en->has_payload で分かれます）。
    bool payload_enum = decl->type && decl->type->kind == TY_ENUM &&
                        decl->type->en && decl->type->en->has_payload;
    return decl->type && !decl->is_global && !decl->binds_borrow &&
           (decl->type->kind == TY_STR || decl->type->kind == TY_LIST ||
            decl->type->kind == TY_CLASS || decl->type->kind == TY_OPT ||
            decl->type->kind == TY_RC ||   // rc[T] はカウントを減らす
            payload_enum);
}

static void scope_add(Emitter *e, Node *decl) {
    if (!e->drop || !e->scope || !is_droppable(decl)) return;
    DropEnt *d = xmalloc(sizeof(DropEnt));
    d->decl = decl;
    d->next = e->scope->ents;  // 先頭に足す＝たどると宣言の逆順
    e->scope->ents = d;
}

// スコープ 1 つぶんの解放を出す（宣言と逆順。設計 §6.1）
static void emit_scope_drops(Emitter *e, ScopeCtx *sc) {
    for (DropEnt *d = sc->ents; d; d = d->next) emit_drop_slot(e, d->decl);
}

// 今のスコープから stop（含まない）まで、抜けるスコープすべてを解放する。
//   return  … stop = NULL（関数の外まで抜ける）
//   break   … stop = ループの外側のスコープ
static void emit_drops_until(Emitter *e, struct ScopeCtx *stop) {
    if (!e->drop) return;
    for (ScopeCtx *sc = e->scope; sc && sc != stop; sc = sc->outer)
        emit_scope_drops(e, sc);
}

// ── 制御構文の生成（規約 6.3 / 6.4 / 6.5）──────────────────

static char *gen_stmt(Emitter *e, Node *n);

static void gen_if(Emitter *e, Node *n) {
    int id = e->label_counter++;  // ★ 番号は最初に 1 回だけ確保する

    char then_l[32], else_l[32], end_l[32];
    snprintf(then_l, sizeof(then_l), "if.then.%d", id);
    snprintf(else_l, sizeof(else_l), "if.else.%d", id);
    snprintf(end_l, sizeof(end_l), "if.end.%d", id);

    char *cond = gen_expr(e, n->lhs);
    // else が無ければ else ブロックを作らず、直接 end へ分岐する
    emit_cond_br(e, cond, then_l, n->els ? else_l : end_l);

    emit_label(e, then_l);
    gen_stmt(e, n->body);
    // 注意: then 節が break / continue で終わっていたら、そこは既に終端済み。
    //    もう 1 つ br を出すと「1 ブロックに終端命令が 2 つ」になり LLVM が怒ります。
    if (!e->terminated) emit_br(e, end_l);

    if (n->els) {
        emit_label(e, else_l);
        gen_stmt(e, n->els);
        if (!e->terminated) emit_br(e, end_l);
    }

    emit_label(e, end_l);
}


// ── 場合分け（A-37）──────────────────────────────────────
//
// ★ **比べる連なりに落とすだけです。** IR に新しい形は増やしません
//   （elif が if の連なりに落ちるのと同じ考え）。
//
// ★ **調べる式は 1 回しか評価しません。** `match f(x):` で f を case の数
//   だけ呼ぶと、副作用のある関数で答えが変わります。
//
// 注意: 箱（alloca）は作りません。値を SSA のレジスタに 1 つ置くだけです。
//   箱にすると、str を調べたときに「その箱は誰のものか」という所有権の
//   話が要るようになります——ここはただ比べるだけの場所です。
static void gen_match(Emitter *e, Node *n) {
    int id = e->label_counter++;

    // ★ 先に 1 回だけ評価します。
    char *subj = gen_expr(e, n->lhs);
    bool is_str = n->lhs->type && n->lhs->type->kind == TY_STR;

    char end_l[40];
    snprintf(end_l, sizeof(end_l), "match.end.%d", id);

    int k = 0;
    for (Node *c = n->body; c; c = c->next, k++) {
        if (!c->lhs) {
            // `case _`（それ以外）。sema が「最後にしか置けない」ことを
            // 確かめてあるので、ここは素通しで本体を出します。
            gen_stmt(e, c->body);
            if (!e->terminated) emit_br(e, end_l);
            break;
        }

        char body_l[40], next_l[40];
        snprintf(body_l, sizeof(body_l), "match.case.%d.%d", id, k);
        snprintf(next_l, sizeof(next_l), "match.next.%d.%d", id, k);

        char *want = gen_expr(e, c->lhs);
        char *cond = new_tmp(e);
        if (is_str) {
            // 注意: 文字列は**内容**で比べます（言語仕様 4.3）。
            declare_rt(e, "i64 @pl_str_cmp(ptr, ptr)");
            char *cmp = new_tmp(e);
            sb_printf(&e->fn, "  %s = call i64 @pl_str_cmp(ptr %s, ptr %s)\n",
                      cmp, subj, want);
            sb_printf(&e->fn, "  %s = icmp eq i64 %s, 0\n", cond, cmp);
        } else {
            sb_printf(&e->fn, "  %s = icmp eq i64 %s, %s\n", cond, subj, want);
        }
        emit_cond_br(e, cond, body_l, next_l);

        emit_label(e, body_l);
        gen_stmt(e, c->body);
        if (!e->terminated) emit_br(e, end_l);

        emit_label(e, next_l);
    }

    // ★ どれにも当たらなかったときは、ここに落ちて終わります。
    //   列挙なら sema が網羅を確かめてあり、int / str なら case _ が
    //   あるので、**実際には到達しません**。それでもブロックは要ります
    //   （最後の match.next が終端命令を持たなければならないため）。
    if (!e->terminated) emit_br(e, end_l);
    emit_label(e, end_l);
}

// ── 添字の範囲をループの外で 1 回だけ確かめる（loop versioning）──
//
// なぜこれを入れたか
//   平坦な list[float] の三重ループ（512×512 の行列積）を測ったら、
//   **境界検査が 4 倍**の費用でした（365 ms → 89 ms）。当初の
//   「外しても 1.5 倍」は list[list[float]] での数字で、平坦な配列では
//   桁が違います。さらに検査を外すだけでは足りず、**負の添字の正規化
//   （select）も消さないと SIMD が 1 つも出ません**。
//   どちらも「添字が全反復で 0 以上・長さ未満」と分かれば消せます。
//
// ★ やること：数える形のループを 2 つに分け、入口で 1 回だけ確かめます。
//
//     if 全部の添字が両端で範囲内:  検査なしの版（ここがベクトル化される）
//     else:                        今までどおりの版（診断もそのまま）
//
// 注意: **診断は変わりません。** 範囲外になるループは今までどおり、
//    ちょうどその反復で、同じメッセージで止まります。
//
// ★ 両端だけ見れば足りるのは、添字が誘導変数について**アフィン**
//   （A*v + B）だからです。アフィンなら単調なので、途中の値は必ず
//   両端のあいだに入ります。だから v が 1 回しか現れない形に限ります。

// 添字の式に使ってよい形か（整数リテラル・局所の int 変数・+ - * だけ）。
static bool vz_pure_index(Node *n) {
    if (!n) return false;
    switch (n->kind) {
        case ND_INT: return true;
        case ND_VAR:
            // ★ グローバルも通します。本体に呼び出しが無いので、**この本体の
            //   中の代入以外に書き換える経路がありません**（スレッドからの
            //   グローバル書き換えは E-SEND-3 が止めています）。
            //   実際に書き換えていないかは vz_invariant が確かめます。
            return !n->is_func_ref && n->type && n->type->kind == TY_INT;
        case ND_BINOP:
            if (n->op != OP_ADD && n->op != OP_SUB && n->op != OP_MUL) return false;
            return vz_pure_index(n->lhs) && vz_pure_index(n->rhs);
        default: return false;
    }
}

// 添字の対象に使ってよい形か（x / x.f / x.f.g のような「場所」だけ）。
static bool vz_place(Node *n) {
    if (!n) return false;
    if (n->kind == ND_VAR) return !n->is_func_ref && !n->is_global;
    if (n->kind == ND_FIELD) return !n->mod_name && vz_place(n->lhs);
    return false;
}

// 式・文の木を歩いて、ir_name への代入があるか。
static bool vz_assigns(Node *n, const char *ir);

static bool vz_assigns_list(Node *n, const char *ir) {
    for (; n; n = n->next)
        if (vz_assigns(n, ir)) return true;
    return false;
}

static bool vz_assigns(Node *n, const char *ir) {
    if (!n) return false;
    if ((n->kind == ND_ASSIGN || n->kind == ND_VARDECL) && n->ir_name &&
        strcmp(n->ir_name, ir) == 0)
        return true;
    if (n->kind == ND_ASSIGN && n->lhs && n->lhs->kind == ND_VAR &&
        n->lhs->ir_name && strcmp(n->lhs->ir_name, ir) == 0)
        return true;
    Node *kids[] = {n->lhs, n->rhs, n->els, n->incr};
    for (unsigned i = 0; i < sizeof(kids) / sizeof(kids[0]); i++)
        if (vz_assigns(kids[i], ir)) return true;
    return vz_assigns_list(n->body, ir) || vz_assigns_list(n->args, ir);
}

// 式に出てくる変数（誘導変数を除く）が、この本体で書き換わらないか。
//
// 注意: **これが無いと健全性が壊れます。** 例えば
//
//     while j < n:
//         s = s + xs[k]
//         k = k + 1        ← k がループ内で変わる
//         j = j + 1
//
//   の添字 `k` は「j についてアフィン」ではありますが（j が現れない＝定数項）、
//   **両端で確かめた値と実際の値が違います**。範囲外を読んで通ってしまいます。
//   だから「誘導変数以外は不変」を要求します。
static bool vz_invariant(Node *e, const char *iv, Node *body) {
    if (!e) return true;
    if (e->kind == ND_VAR && e->ir_name && strcmp(e->ir_name, iv) != 0 &&
        vz_assigns(body, e->ir_name))
        return false;
    return vz_invariant(e->lhs, iv, body) && vz_invariant(e->rhs, iv, body);
}

// 変数 ir が式の中に何回現れるか（アフィンかどうかの判定に使う）。
static int vz_count(Node *n, const char *ir) {
    if (!n) return 0;
    if (n->kind == ND_VAR && n->ir_name && strcmp(n->ir_name, ir) == 0) return 1;
    return vz_count(n->lhs, ir) + vz_count(n->rhs, ir);
}

// 版分けしてよい本体か。
//
// 注意: **呼び出しがあってはいけません。** 呼んだ先が list に append すれば
//    長さが変わり、入口で確かめた前提が崩れます。
// 注意: 入れ子のループ・break / continue / return も外します（速い側から
//    抜ける道が増えると、どのみちベクトル化できません）。
static bool vz_body_ok(Node *n);

static bool vz_body_ok_list(Node *n) {
    for (; n; n = n->next)
        if (!vz_body_ok(n)) return false;
    return true;
}

static bool vz_body_ok(Node *n) {
    if (!n) return true;
    switch (n->kind) {
        // 通すもの
        case ND_INT: case ND_FLOAT: case ND_BOOL: case ND_STR: case ND_NONE:
        case ND_PASS:
            return true;
        case ND_VAR:
            return !n->is_func_ref;
        case ND_FIELD:
            return !n->mod_name && vz_body_ok(n->lhs);
        case ND_BLOCK:
            return vz_body_ok_list(n->body);
        case ND_IF:
            return vz_body_ok(n->lhs) && vz_body_ok(n->body) && vz_body_ok(n->els);
        case ND_COND:
            return vz_body_ok(n->lhs) && vz_body_ok(n->rhs) && vz_body_ok(n->els);
        case ND_LOGICAL:
            return vz_body_ok(n->lhs) && vz_body_ok(n->rhs);
        case ND_UNARY:
            // 注意: 単項マイナスは int だと桁あふれ検査（分岐）が出ますが、
            //    正しさには関わりません（速い側でも検査は残ります）。
            return vz_body_ok(n->lhs);
        case ND_BINOP:
            // 注意: ** // % は実体がランタイム関数の呼び出しです。
            //    str / list の + * も同じ（連結・繰り返し）。
            if (n->op == OP_IN || n->op == OP_NOTIN || n->op == OP_POW ||
                n->op == OP_FLOORDIV || n->op == OP_MOD)
                return false;
            if (n->lhs->type && (n->lhs->type->kind == TY_STR ||
                                 n->lhs->type->kind == TY_LIST))
                return false;
            return vz_body_ok(n->lhs) && vz_body_ok(n->rhs);
        case ND_INDEX:
            // 注意: str の添字はランタイム関数を呼びます（pl_str_index）。
            if (!n->lhs->type || n->lhs->type->kind == TY_STR) return false;
            return vz_body_ok(n->lhs) && vz_body_ok(n->rhs);
        case ND_ASSIGN:
            // 注意: フィールドやリスト変数への代入は外します。代入されると
            //    「入口で見た list と同じものか」が言えなくなります。
            if (n->lhs->kind == ND_FIELD) return false;
            if (n->lhs->kind == ND_VAR && n->type &&
                (n->type->kind == TY_LIST || n->type->kind == TY_STR))
                return false;
            return vz_body_ok(n->lhs) && vz_body_ok(n->rhs);
        case ND_VARDECL:
            if (n->type && (n->type->kind == TY_LIST || n->type->kind == TY_STR))
                return false;
            return vz_body_ok(n->rhs);
        default:
            // 呼び出し・入れ子ループ・break / continue / return・try・
            // unsafe・内包表記・スライス・タプル…… すべて外します。
            return false;
    }
}

// 本体の中の「list への添字」を集める。1 つでも証明できなければ false。
//
// ★ 集めた添字は、ガードで**両端の値**を計算するのに使います。
static bool vz_sites(Node *n, const char *iv, Node *body, Node **out, int *cnt,
                     int max);

static bool vz_sites_list(Node *n, const char *iv, Node *body, Node **out,
                          int *cnt, int max) {
    for (; n; n = n->next)
        if (!vz_sites(n, iv, body, out, cnt, max)) return false;
    return true;
}

static bool vz_sites(Node *n, const char *iv, Node *body, Node **out, int *cnt,
                     int max) {
    if (!n) return true;
    if (n->kind == ND_INDEX && n->lhs->type && n->lhs->type->kind == TY_LIST) {
        // 対象は「場所」で、添字は誘導変数についてアフィンで、
        // 誘導変数以外は不変であること。
        if (!vz_place(n->lhs)) return false;
        if (!vz_pure_index(n->rhs)) return false;
        if (vz_count(n->rhs, iv) > 1) return false;  // アフィンでない（v*v など）
        if (!vz_invariant(n->rhs, iv, body)) return false;
        if (*cnt >= max) return false;
        out[(*cnt)++] = n;
    }
    Node *kids[] = {n->lhs, n->rhs, n->els, n->incr};
    for (unsigned i = 0; i < sizeof(kids) / sizeof(kids[0]); i++)
        if (!vz_sites(kids[i], iv, body, out, cnt, max)) return false;
    return vz_sites_list(n->body, iv, body, out, cnt, max) &&
           vz_sites_list(n->args, iv, body, out, cnt, max);
}

// 「v = v + 1」の形か。
static bool vz_is_step1(Node *st, const char *iv) {
    if (!st || st->kind != ND_ASSIGN) return false;
    if (!st->lhs || st->lhs->kind != ND_VAR || !st->lhs->ir_name) return false;
    if (strcmp(st->lhs->ir_name, iv) != 0) return false;
    Node *r = st->rhs;
    if (!r || r->kind != ND_BINOP || r->op != OP_ADD) return false;
    if (!r->lhs || r->lhs->kind != ND_VAR || !r->lhs->ir_name) return false;
    if (strcmp(r->lhs->ir_name, iv) != 0) return false;
    return r->rhs && r->rhs->kind == ND_INT && r->rhs->ival == 1;
}

// ループの本体の最後の文を返す（ND_BLOCK の末尾）。
static Node *vz_last_stmt(Node *body) {
    if (!body || body->kind != ND_BLOCK) return NULL;
    Node *last = NULL;
    for (Node *st = body->body; st; st = st->next) last = st;
    return last;
}

static void gen_while_plain(Emitter *e, Node *n) {
    int id = e->label_counter++;

    char cond_l[32], body_l[32], end_l[32];
    snprintf(cond_l, sizeof(cond_l), "while.cond.%d", id);
    snprintf(body_l, sizeof(body_l), "while.body.%d", id);
    snprintf(end_l, sizeof(end_l), "while.end.%d", id);

    // 注意: 条件ブロックに「入る」ための br が必要（規約 6.4）。
    //    条件を独立したブロックにしないと 1 回目の判定が飛ばされ、
    //    do-while になってしまいます。
    emit_br(e, cond_l);

    emit_label(e, cond_l);
    char *cond = gen_expr(e, n->lhs);  // ★ 条件は反復のたびに評価される
    emit_cond_br(e, cond, body_l, end_l);

    // ★ 増分があるなら continue の飛び先は「増分ブロック」。
    //   無ければ従来どおり「条件ブロック」。
    //
    // 注意: ここを間違えると、for の中の continue が増分を飛ばして無限ループになります。
    char incr_l[32];
    const char *cont_l = cond_l;
    if (n->incr) {
        snprintf(incr_l, sizeof(incr_l), "for.incr.%d", id);
        cont_l = incr_l;
    }

    // break / continue の飛び先を積む
    LoopCtx ctx = {.outer = e->loop,
                   .break_label = end_l,
                   .continue_label = cont_l,
                   .scope = e->scope};
    e->loop = &ctx;

    emit_label(e, body_l);
    gen_stmt(e, n->body);

    if (n->incr) {
        // ★ emit_label が「終端していなければ br を補う」ので、
        //   本体から増分ブロックへは自動的に繋がります（その関数）。
        emit_label(e, incr_l);
        gen_stmt(e, n->incr);
    }
    if (!e->terminated) emit_br(e, cond_l);  // ループバック

    e->loop = ctx.outer;  // ★ 対で戻す

    emit_label(e, end_l);
}

#define VZ_MAX_SITES 16

// ループが版分けできるなら、誘導変数の IR 名を返す（できなければ NULL）。
//
// 注意: **かつてここは `if (e->drop) return NULL;` で始まっていました**
//    （理由は「本体を 2 回出すと解放の記録が二重になる」）。0.16.0 で
//    `--drop` が既定になった結果、**版分けが既定で丸ごと死んでいました**。
//    512³ の行列積（平坦な list[float]）が 36.6 ms → 357.5 ms です。
//
//    測り直したところ、その心配は起きません。理由は 2 つあります。
//
//    ① **速い側と遅い側は「並ぶ」のではなく「どちらか」です。**
//       解放は各ブロックに 1 つずつ出ますが、実行時に通るのは片方だけです。
//    ② **スコープの記帳は共有されていません。** ND_BLOCK を生成するたびに
//       ScopeCtx をその場の局所変数として作るので（gen_stmt の ND_BLOCK）、
//       2 回目の本体は 1 回目とは別の一覧に積みます。
//
//    そもそも版分けできる本体には、解放すべきものがほとんど生まれません
//    （vz_body_ok が呼び出し・list と str の宣言と代入・内包表記・連結を
//    すべて外しているため）。残るのは rc[T] や T | None の束縛ぐらいで、
//    それも ① と ② のとおり各ブロックで釣り合います。
//    ★ 回帰テスト: tests/cases/vz_drop（拡張子は make info）が ASan で釣り合いを見張ります。
static const char *vz_analyze(Emitter *e, Node *n, Node **sites, int *nsites) {
    if (n->kind != ND_WHILE) return NULL;

    // ① 形：while v < L:
    Node *c = n->lhs;
    if (!c || c->kind != ND_BINOP || c->op != OP_LT) return NULL;
    Node *v = c->lhs;
    if (!v || v->kind != ND_VAR || v->is_global || !v->ir_name) return NULL;
    if (!v->type || v->type->kind != TY_INT) return NULL;
    const char *iv = v->ir_name;

    // ② 上限 L は副作用の無い式で、本体で書き換わらないこと。
    Node *L = c->rhs;
    bool L_ok = vz_pure_index(L);
    if (!L_ok && L && L->kind == ND_CALL && L->name && strcmp(L->name, "len") == 0 &&
        L->args && !L->args->next && vz_place(L->args))
        L_ok = true;  // ★ while i < len(xs): は非常に多いので通します
    if (!L_ok) return NULL;
    if (vz_count(L, iv) != 0) return NULL;
    if (!vz_invariant(L, iv, n->body)) return NULL;   // 上限も不変であること

    // ③ 本体：呼び出し・入れ子ループ・脱出が無いこと。
    if (!vz_body_ok(n->body)) return NULL;
    if (n->incr && !vz_body_ok(n->incr)) return NULL;

    // ④ 増分は v = v + 1 ただ 1 つ。
    Node *step = n->incr ? n->incr : vz_last_stmt(n->body);
    if (n->incr && n->incr->kind == ND_BLOCK) step = vz_last_stmt(n->incr);
    if (!vz_is_step1(step, iv)) return NULL;

    // ⑤ v への代入はその 1 本だけ（増分を数から引く）。
    {
        Node *saved = step->rhs;
        step->rhs = NULL;                       // 増分を一時的に隠して数える
        bool other = vz_assigns(n->body, iv) || (n->incr && vz_assigns(n->incr, iv));
        step->rhs = saved;
        // 注意: step 自身も n->body の中にいるので、隠しても ND_ASSIGN の
        //    lhs は残ります。ここは「本体の中の代入が step だけか」を
        //    別に数えます。
        (void)other;
    }
    {
        int nassign = 0;
        Node *stack[64];
        int sp = 0;
        stack[sp++] = n->body;
        if (n->incr) stack[sp++] = n->incr;
        while (sp > 0) {
            Node *q = stack[--sp];
            for (; q; q = q->next) {
                if (q->kind == ND_ASSIGN && q->lhs && q->lhs->kind == ND_VAR &&
                    q->lhs->ir_name && strcmp(q->lhs->ir_name, iv) == 0)
                    nassign++;
                if (q->kind == ND_VARDECL && q->ir_name &&
                    strcmp(q->ir_name, iv) == 0)
                    nassign++;
                Node *kids[] = {q->body, q->els};
                for (unsigned i = 0; i < 2; i++)
                    if (kids[i] && sp < 62) stack[sp++] = kids[i];
            }
        }
        if (nassign != 1) return NULL;
    }

    // ⑥ 添字を集める。1 つも無ければ版分けする意味がありません。
    *nsites = 0;
    if (!vz_sites(n->body, iv, n->body, sites, nsites, VZ_MAX_SITES)) return NULL;
    if (n->incr &&
        !vz_sites(n->incr, iv, n->body, sites, nsites, VZ_MAX_SITES))
        return NULL;
    if (*nsites == 0) return NULL;

    // ⑦ 添字の対象になっている list が本体で書き換わらないこと。
    for (int i = 0; i < *nsites; i++) {
        Node *root = sites[i]->lhs;
        while (root->kind == ND_FIELD) root = root->lhs;
        if (!root->ir_name || vz_assigns(n->body, root->ir_name)) return NULL;
    }
    // 注意: 上限に len(xs) を使っているなら、その xs も不変であること。
    if (L->kind == ND_CALL) {
        Node *lr = L->args;
        while (lr && lr->kind == ND_FIELD) lr = lr->lhs;
        if (!lr || !lr->ir_name || vz_assigns(n->body, lr->ir_name)) return NULL;
    }
    return iv;
}

// 添字 1 つぶんの「両端が範囲内か」を計算して、ガードに and する。
static char *vz_check_at(Emitter *e, Node *site, const char *iv, const char *val,
                         const char *len, char *acc) {
    e->subst_ir = iv;
    e->subst_val = val;
    char *ovf = NULL;
    char *idx = gen_index_expr(e, site->rhs, &ovf);
    e->subst_ir = NULL;
    e->subst_val = NULL;

    // ★ 符号なしで比べると「0 以上」と「長さ未満」が 1 回で済みます
    //   （負の数は符号なしでは巨大な値になるため）。
    char *in = new_tmp(e);
    sb_printf(&e->fn, "  %s = icmp ult i64 %s, %s\n", in, idx, len);

    // 注意: 両端の計算で桁あふれしたら、ガードを落として遅い側へ行かせます
    //    （そちらが正しい位置で正しく panic します）。
    if (ovf) {
        char *nov = new_tmp(e);
        sb_printf(&e->fn, "  %s = xor i1 %s, true\n", nov, ovf);
        char *both = new_tmp(e);
        sb_printf(&e->fn, "  %s = and i1 %s, %s\n", both, in, nov);
        in = both;
    }
    if (!acc) return in;
    char *a = new_tmp(e);
    sb_printf(&e->fn, "  %s = and i1 %s, %s\n", a, acc, in);
    return a;
}

static void gen_while(Emitter *e, Node *n) {
    Node *sites[VZ_MAX_SITES];
    int nsites = 0;
    const char *iv = vz_analyze(e, n, sites, &nsites);
    if (!iv) {
        gen_while_plain(e, n);
        return;
    }

    int id = e->label_counter++;
    char fast_l[32], slow_l[32], done_l[32];
    snprintf(fast_l, sizeof(fast_l), "vz.fast.%d", id);
    snprintf(slow_l, sizeof(slow_l), "vz.slow.%d", id);
    snprintf(done_l, sizeof(done_l), "vz.done.%d", id);

    // ── ガード：入口で 1 回だけ確かめる ──
    //
    // ★ 見るのは**両端だけ**です。添字は誘導変数についてアフィンなので、
    //   途中の値は必ず両端のあいだに入ります。
    char *v0 = gen_load(e, ty_int, n->lhs->lhs->ir_name);  // 入口の v
    char *Lv = gen_expr(e, n->lhs->rhs);                   // 上限 L
    char *Lm1 = new_tmp(e);
    sb_printf(&e->fn, "  %s = sub i64 %s, 1\n", Lm1, Lv);  // 最後に回る v

    char *acc = NULL;
    for (int i = 0; i < nsites; i++) {
        char *obj = gen_expr(e, sites[i]->lhs);
        char *lenp = new_tmp(e);
        sb_printf(&e->fn, "  %s = getelementptr i8, ptr %s, i64 8\n", lenp, obj);
        char *len = new_tmp(e);
        sb_printf(&e->fn, "  %s = load i64, ptr %s" TBAA_LISTHDR "\n", len, lenp);
        acc = vz_check_at(e, sites[i], iv, v0, len, acc);
        acc = vz_check_at(e, sites[i], iv, Lm1, len, acc);
    }
    emit_cond_br(e, acc, fast_l, slow_l);

    // ── 速い側：検査も正規化も出さない（ここがベクトル化される）──
    emit_label(e, fast_l);
    e->nobc = true;
    e->iv_ir = iv;
    gen_while_plain(e, n);
    e->nobc = false;
    e->iv_ir = NULL;
    if (!e->terminated) emit_br(e, done_l);

    // ── 遅い側：今までどおり（診断もそのまま）──
    emit_label(e, slow_l);
    gen_while_plain(e, n);
    if (!e->terminated) emit_br(e, done_l);

    emit_label(e, done_l);
}

// print(e) の暫定実装。C の printf を借ります。
//
// ★ 用意しておいた globals / decls バッファが、ここで初めて使われます。
// 組み込み関数の呼び出し。
//
// ★ sema が選んだ候補（n->builtin）に従って、対応するランタイム関数を呼ぶだけ。
//   「どの実装を呼ぶか」の判断は sema 側で終わっています。
// ── len は呼び出しではなく load 1 つ ─────────────────
//
// ★ **`for x in xs:` の条件が毎回の関数呼び出しでした。**
//   脱糖した形は `for.ix.N < len(for.it.N)` です。ここが呼び出しだと
//   LLVM は「メモリを書くかもしれない」と見て、ループの外へ出せません。
//   さらに悪いことに、**本体の境界検査は同じ長さを `load` で読みます**。
//   呼び出しと load は「同じ値」だと分からないので、**明らかに範囲内なのに
//   検査が残ります**（規約 R10 の例外に当たる場面。同じ理由）。
//
//   load に揃えると、LLVM が
//     ① 長さの読み出しをループの外へ持ち上げ
//     ② 条件 `i < len` から `i <u len` が常に真だと導いて**検査を消す**
//   ようになります。
//
// 注意: list の長さはヘッダのオフセット 8（`PlList { ptr data; i64 len; i64 cap }`）。
// 注意: str の長さは**ポインタの 8 バイト手前**にあり、最上位側の 1 ビットが
//   「静的な文字列か」の印なので落とします（`runtime/core.c` の PL_STR_STATIC）。
static char *gen_len(Emitter *e, Node *n, Type *at) {
    char *obj = gen_expr(e, n->args);
    if (at->kind == TY_LIST) {
        char *p = new_tmp(e);
        sb_printf(&e->fn, "  %s = getelementptr i8, ptr %s, i64 8\n", p, obj);
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = load i64, ptr %s" TBAA_LISTHDR "\n", t, p);
        drop_temp(e, n->args, obj);   // A-21e: len(作ったばかりのリスト)
        return t;
    }
    // str：ptr[-8] を読んで、PL_STR_STATIC（1 << 62）を落とす
    //
    // 注意: 解放は**長さを読み終えてから**です。先に解放すると読めません。
    char *p = new_tmp(e);
    sb_printf(&e->fn, "  %s = getelementptr i8, ptr %s, i64 -8\n", p, obj);
    char *raw = new_tmp(e);
    sb_printf(&e->fn, "  %s = load i64, ptr %s\n", raw, p);
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = and i64 %s, -4611686018427387905\n", t, raw);
    drop_temp(e, n->args, obj);   // A-21e: len(作ったばかりの文字列)
    return t;
}

static char *gen_builtin_call(Emitter *e, Node *n) {
    const Builtin *b = n->builtin;
    // 注意: 引数の型は表からではなく実引数から取ります。
    //    list[T] にはシングルトンが無いので type_from_kind では引けません。
    Type *at = n->args->type;
    Type *rt = type_from_kind(b->ret);

    // ★ len は IR に展開します（呼び出しにしません）
    if (strcmp(b->name, "len") == 0 &&
        (at->kind == TY_LIST || at->kind == TY_STR))
        return gen_len(e, n, at);

    // ★ 値型の copy は「そのまま」です。呼び出しを出しません
    //   （int の複製に関数呼び出しを 1 つ払うのは筋が通りません）。
    if (strcmp(b->impl, "pl_copy_id") == 0) return gen_expr(e, n->args);

    // 注意: bool は本言語のレジスタ上では i1 ですが、C 側は long long で
    //    受け取ります。境界で i64 に広げます（規約 R5 と同じ考え方）。
    const char *argty = at->kind == TY_BOOL ? "i64" : llvm_type(at);

    // declare を 1 回だけ出す
    //
    // ★ **cold** も付けます。noreturn は「戻らない」ことしか言わず、
    //   インライン化の見積りには効きません。panic の手前に並ぶ
    //   メッセージ組み立て（連結・数値→文字列）が呼び出し 6 回ぶんの
    //   費用として数えられ、それを含む小さな検査メソッドが
    //   **丸ごとインライン化を諦められて**いました。
    //   cold を付けると、その経路は「まず通らない」ものとして扱われます。
    //
    // ★ **戻らない**組み込み（panic / exit）には noreturn を付けます。
    //   付けないと LLVM は「戻ってくるかもしれない」と見なし、
    //     ① panic の後ろの経路を生かしたままにする
    //     ② panic までに並ぶ文字列連結を「メモリを書くかもしれない呼び出し」
    //        として扱い、**ループ内の load を外に持ち上げられなくなる**
    //   の 2 つが起きます。
    //   注意: ②「linalg.Matrix.check の self.rows / self.cols / self.data が
    //     ループ外に出せない」と当初は書いていましたが、**間違いでした**。
    //     測ったところ、あの遅さの原因はインライン化のほうです。
    //   注意: sema の never_returns_call と対になっています。片方だけ変えないこと。
    const char *nr = (strcmp(b->impl, "pl_panic") == 0 ||
                      strcmp(b->impl, "pl_exit") == 0)
                         ? " noreturn cold"
                         : "";
    StrBuf sig;
    sb_init(&sig);
    sb_printf(&sig, "%s @%s(%s)%s", llvm_type(rt), b->impl, argty, nr);
    declare_rt(e, sb_str(&sig));

    char *v = gen_expr(e, n->args);
    if (at->kind == TY_BOOL) {
        char *z = new_tmp(e);
        sb_printf(&e->fn, "  %s = zext i1 %s to i64\n", z, v);
        v = z;
    }

    if (rt->kind == TY_NONE) {
        sb_printf(&e->fn, "  call void @%s(%s %s)\n", b->impl, argty, v);
        drop_temp(e, n->args, v);   // A-21e: print(作ったばかりの文字列) など
        return NULL;
    }
    char *t = new_tmp(e);
    sb_printf(&e->fn, "  %s = call %s @%s(%s %s)\n", t, llvm_type(rt), b->impl,
              argty, v);
    // 注意: 組み込みはどれも**借りて読むだけ**です（所有権を受け取りません）。
    //   だから引数が一時値なら、呼び終わったところで解放できます（A-21e）。
    drop_temp(e, n->args, v);
    return t;
}

// 関数呼び出し。
//
// 注意: void の呼び出しに結果を代入してはいけません。
//      %t0 = call void @f()   ✗
//      call void @f()         // ── 低レベルの生成 ──────────────────────────────────────
//
// ★ どれも命令 1〜2 個です。ランタイム関数は要りません（OS では呼べないので）。
//   注意: 読み書きは volatile にします。MMIO（装置のレジスタ）は
//     「同じ番地を読んでも値が変わる」ので、最適化で消されると困ります。
static char *gen_lowlevel(Emitter *e, Node *n) {
    Node *a0 = n->args;

    // ── インラインアセンブリ ──
    //
    // ★ sideeffect を付けます。「値を返さないから消してよい」と
    //   最適化に判断されると、csrw も wfi も消えてしまうためです。
    if (strncmp(n->name, "asm", 3) == 0) {
        // 注意: 利用者は C と同じ %0 で書きますが、LLVM IR のインライン
        //    アセンブリでは $0 が「1 番目のオペランド」です。ここで直します
        //    （RISC-V では % は %hi(...) のような再配置指定に使われるため、
        //     そのままだとアセンブラが別物として読んでしまいます）。
        StrBuf asmbuf;
        sb_init(&asmbuf);
        for (const char *c = a0->sval; *c; c++) {
            if (*c == '%' && c[1] >= '0' && c[1] <= '9') {
                sb_printf(&asmbuf, "$%c", c[1]);
                c++;
            } else if (*c == '$') {
                sb_printf(&asmbuf, "$$");
            } else {
                sb_printf(&asmbuf, "%c", *c);
            }
        }
        const char *text = sb_str(&asmbuf);
        if (strcmp(n->name, "asm") == 0) {
            sb_printf(&e->fn, "  call void asm sideeffect \"%s\", \"\"()\n", text);
            return NULL;
        }
        if (strcmp(n->name, "asm_in") == 0) {
            char *v = gen_expr(e, a0->next);
            sb_printf(&e->fn,
                      "  call void asm sideeffect \"%s\", \"r\"(i64 %s)\n", text, v);
            return NULL;
        }
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = call i64 asm sideeffect \"%s\", \"=r\"()\n", t, text);
        return t;
    }

    Node *a1 = a0 ? a0->next : NULL;
    Node *a2 = a1 ? a1->next : NULL;

    if (strcmp(n->name, "ptr_at") == 0) {
        char *v = gen_expr(e, a0);
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = inttoptr i64 %s to ptr\n", t, v);
        return t;
    }
    if (strcmp(n->name, "addr_of") == 0) {
        char *v = gen_expr(e, a0);
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = ptrtoint ptr %s to i64\n", t, v);
        return t;
    }

    bool is8 = strstr(n->name, "8") != NULL;
    const char *ity = is8 ? "i8" : "i64";
    char *p = gen_expr(e, a0);
    char *off = gen_expr(e, a1);
    char *addr = new_tmp(e);
    sb_printf(&e->fn, "  %s = getelementptr %s, ptr %s, i64 %s\n", addr, ity, p, off);

    if (strncmp(n->name, "peek", 4) == 0) {
        char *v = new_tmp(e);
        sb_printf(&e->fn, "  %s = load volatile %s, ptr %s\n", v, ity, addr);
        if (!is8) return v;
        char *z = new_tmp(e);
        sb_printf(&e->fn, "  %s = zext i8 %s to i64\n", z, v);
        return z;
    }

    // poke8 / poke64
    char *val = gen_expr(e, a2);
    if (is8) {
        char *tr = new_tmp(e);
        sb_printf(&e->fn, "  %s = trunc i64 %s to i8\n", tr, val);
        val = tr;
    }
    sb_printf(&e->fn, "  store volatile %s %s, ptr %s\n", ity, val, addr);
    return NULL;
}

static char *gen_call(Emitter *e, Node *n) {
    // ★ 低レベルの組み込み（sema が ir_name を付けない目印）
    if (!n->ir_name && !n->cls && !n->builtin && is_lowlevel_name(n->name))
        return gen_lowlevel(e, n);

    // ★ min / max。**分岐を作らず select 1 命令**で済ませます。
    //   両辺とも必ず評価されますが、どちらも副作用のない値なので問題ありません。
    if (n->is_minmax) {
        char *a = gen_expr(e, n->args);
        char *b = gen_expr(e, n->args->next);
        bool isf = n->type->kind == TY_FLOAT;
        bool is_min = strcmp(n->name, "min") == 0;
        char *c = new_tmp(e);
        if (isf)
            sb_printf(&e->fn, "  %s = fcmp %s double %s, %s\n", c,
                      is_min ? "olt" : "ogt", a, b);
        else
            sb_printf(&e->fn, "  %s = icmp %s i64 %s, %s\n", c,
                      is_min ? "slt" : "sgt", a, b);
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = select i1 %s, %s %s, %s %s\n", t, c,
                  llvm_type(n->type), a, llvm_type(n->type), b);
        return t;
    }

    // ★ wrap_add / wrap_sub / wrap_mul。
    //   **桁あふれを検査せず 2 の補数で折り返します。**
    //   法 2⁶⁴ の計算（線形合同法など）を書くための逃げ道です。
    //   注意: min / max と同じく 2 引数なので、組み込みの表では表せません。
    if (n->is_wrap) {
        char *a = gen_expr(e, n->args);
        char *b = gen_expr(e, n->args->next);
        const char *ins = strcmp(n->name, "wrap_add") == 0   ? "add"
                          : strcmp(n->name, "wrap_sub") == 0 ? "sub"
                                                             : "mul";
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = %s i64 %s, %s\n", t, ins, a, b);
        return t;
    }

    // ── move_out(場所) — 所有権を取り出して、その場所は空にする ──────
    //
    // ★ 出す形は 3 行です（docs/roadmap.md A-21d）:
    //     ① いまの値を読む      … これが戻り値（所有権ごともらう）
    //     ② 空の値を作る        … str なら ""、list なら新しい空リスト
    //     ③ その場所へ書き戻す  … 場所は有効なまま残る
    //
    // 注意: **retain も release も出しません。** 参照の数は動いていません
    //   （持ち主が「場所」から「呼び出し側」へ移っただけです）。
    //   だから move_out は --drop の有無で意味が変わりません。
    if (n->is_move_out) {
        Node *a = n->args;
        char *v = gen_expr(e, a);                     // ①
        char *empty = gen_empty_value(e, a->type);    // ②
        if (a->kind == ND_INDEX) {                    // ③
            gen_index_store(e, a, empty);
        } else if (a->kind == ND_FIELD && !a->mod_name) {
            gen_store_tb(e, a->type, empty, gen_field_ptr(e, a), TBAA_FIELD);
        } else {
            // 他モジュールのグローバル、または自分のグローバル
            if (a->is_extern) declare_extern_global(e, a);
            gen_store(e, a->type, empty, a->ir_name);
        }
        return v;
    }

    // ★ print(xs) / str(xs)
    if (n->is_list_str) {
        Type *el = n->args->type->elem;
        int kind = el->kind == TY_FLOAT ? 1
                 : el->kind == TY_STR   ? 2
                 : el->kind == TY_BOOL  ? 3
                                        : 0;
        char *v = gen_expr(e, n->args);
        bool is_print = strcmp(n->name, "print") == 0;
        declare_rt(e, is_print ? "void @pl_print_list(ptr, i64)"
                               : "ptr @pl_list_str(ptr, i64)");
        if (is_print) {
            sb_printf(&e->fn, "  call void @pl_print_list(ptr %s, i64 %d)\n", v,
                      kind);
            return NULL;
        }
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = call ptr @pl_list_str(ptr %s, i64 %d)\n", t, v,
                  kind);
        return t;
    }

    if (n->builtin) return gen_builtin_call(e, n);
    if (n->cls) return gen_new(e, n);  // ★ インスタンス生成

    // ★ 関数ポインタ越しの呼び出し。
    //   注意: LLVM では **呼び先がラベルでもレジスタでも同じ書き方**です
    //     （call <戻り型> <呼び先>(引数)）。だから箱から読んだ値を
    //     そのまま置くだけで間接呼び出しになります。declare も要りません。
    if (n->is_indirect) {
        // 注意: **ptr として読むこと。** ty_int を渡すと i64 で読んでしまい、
        //    call の呼び先が整数になって LLVM に弾かれます。
        char *fp = new_tmp(e);
        sb_printf(&e->fn, "  %s = load ptr, ptr %s\n", fp, n->ir_name);
        StrBuf a2, t2;
        sb_init(&a2);
        sb_init(&t2);
        gen_args(e, n->args, &a2, &t2, true);
        if (n->type->kind == TY_NONE) {
            sb_printf(&e->fn, "  call void %s(%s)\n", fp, sb_str(&a2));
            return NULL;
        }
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = call %s %s(%s)\n", t, llvm_type(n->type), fp,
                  sb_str(&a2));
        return t;
    }

    // ★ spawn(f, a) — 別スレッドで f(a) を始める（A-18）
    if (!n->ir_name && n->type && n->type->kind == TY_THREAD) {
        Type *fnty = n->args->type;
        char *fv = gen_expr(e, n->args);            // 関数ポインタ
        int na = fnty->nparams;

        // ★ 引数は i64 の配列にして渡します。注意: この箱は entry ブロックの
        //   alloca です（規約 R1）。ランタイムが**写しを取ってから**
        //   スレッドを作るので、呼び出しのあいだだけ生きていれば足ります。
        char *buf = new_tmp(e);
        sb_printf(&e->allocas, "  %s = alloca [%d x i64]\n", buf, na < 1 ? 1 : na);

        int k = 0;
        for (Node *a = n->args->next; a; a = a->next, k++) {
            char *av = gen_expr(e, a);
            char *ai = pack_i64(e, fnty->params[k], av);
            char *sp = new_tmp(e);
            sb_printf(&e->fn,
                      "  %s = getelementptr [%d x i64], ptr %s, i64 0, i64 %d\n",
                      sp, na < 1 ? 1 : na, buf, k);
            sb_printf(&e->fn, "  store i64 %s, ptr %s\n", ai, sp);
        }

        const char *th = thunk_for(e, fnty);
        char *fi = new_tmp(e);
        sb_printf(&e->fn, "  %s = ptrtoint ptr %s to i64\n", fi, fv);
        declare_rt(e, "ptr @pl_thread_spawn(ptr, i64, ptr, i64)");
        char *t = new_tmp(e);
        sb_printf(&e->fn,
                  "  %s = call ptr @pl_thread_spawn(ptr %s, i64 %s, ptr %s, i64 %d)\n",
                  t, th, fi, buf, na);
        return t;
    }

    // ★ mutex(x) — 中身をロックの内側に閉じる
    if (!n->ir_name && n->type && n->type->kind == TY_MUTEX) {
        char *v = gen_expr(e, n->args);
        char *vi = pack_i64(e, n->args->type, v);
        declare_rt(e, "ptr @pl_mutex_new(i64)");
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = call ptr @pl_mutex_new(i64 %s)\n", t, vi);
        return t;
    }

    // ★ rc(x) — 数え札を付けてくるむ（sema が ir_name を付けない目印）
    if (!n->ir_name && strcmp(n->name, "rc") == 0) {
        char *v = gen_expr(e, n->args);
        declare_rt(e, "ptr @pl_rc_new(ptr)");
        char *t = new_tmp(e);
        sb_printf(&e->fn, "  %s = call ptr @pl_rc_new(ptr %s)\n", t, v);
        return t;
    }

    // 引数を左から順に評価する（言語仕様 4.5）
    StrBuf args, types;
    sb_init(&args);
    sb_init(&types);
    gen_args(e, n->args, &args, &types, true);

    // ★ 呼ぶ名前は sema が修飾済み（n->ir_name = "lexer.make"）
    if (n->is_extern) {
        // ★ 失敗しうる関数は、エラー出力の ptr が 1 本増えています
        if (n->can_fail) sb_printf(&types, "%sptr", sb_str(&types)[0] ? ", " : "");
        declare_extern(e, llvm_type(n->type), n->ir_name, sb_str(&types));
    }
    return emit_call(e, n, sb_str(&args));
}


// ── 並行実行（A-18）の道具 ──────────────────────────────
//
// ランタイム（runtime/hosted.c）との ABI は「i64 を 2 つ取って i64 を返す」
// の 1 種類だけです。型のある世界とその ABI の橋渡しをここでやります。

// 値（型 t）を i64 に詰める。
static char *pack_i64(Emitter *e, Type *t, char *v) {
    // 注意: 一時値は**必要になってから**取ります。先に new_tmp すると、
    //    int のときに番号が 1 つ飛び、セルフホスト版と IR がずれます。
    if (t->kind == TY_INT) return v;  // そのまま
    char *r = new_tmp(e);
    switch (t->kind) {
        case TY_BOOL:
            sb_printf(&e->fn, "  %s = zext i1 %s to i64\n", r, v);
            return r;
        case TY_FLOAT:
            // 注意: fptosi ではありません。**ビットをそのまま**運びます。
            sb_printf(&e->fn, "  %s = bitcast double %s to i64\n", r, v);
            return r;
        default:  // ポインタで表される型はすべてここ
            sb_printf(&e->fn, "  %s = ptrtoint ptr %s to i64\n", r, v);
            return r;
    }
}

// i64 から値（型 t）を取り出す。
static char *unpack_i64(Emitter *e, Type *t, char *v) {
    if (t->kind == TY_INT) return v;
    char *r = new_tmp(e);
    switch (t->kind) {
        case TY_BOOL:
            sb_printf(&e->fn, "  %s = trunc i64 %s to i1\n", r, v);
            return r;
        case TY_FLOAT:
            sb_printf(&e->fn, "  %s = bitcast i64 %s to double\n", r, v);
            return r;
        default:
            sb_printf(&e->fn, "  %s = inttoptr i64 %s to ptr\n", r, v);
            return r;
    }
}

// fn(A) -> R の関数を「i64 2 つを取って i64 を返す」形に見せる中継関数を出す。
//
// ★ シグネチャごとに 1 つだけ作ります（同じ形の関数が何本 spawn されても
//   thunk は 1 つ）。名前は @pl.thunk.N。
//
// なぜ thunk が要るのか
//   「全部 i64 として直接呼べばいい」ように見えますが、**呼び出し規約は
//   型で決まります**。float は整数と別のレジスタで渡され、None を返す
//   関数には戻り値レジスタがありません。型を偽って呼ぶと、その 2 つで
//   静かに壊れます。型どおりに呼ぶ場所を 1 か所に閉じ込めます。
static const char *thunk_for(Emitter *e, Type *fnty) {
    const char *key = type_name(fnty);
    for (StrLit *q = e->thunks; q; q = q->next)
        if (strcmp(q->label, key) == 0) return q->bytes;

    StrBuf name;
    sb_init(&name);
    sb_printf(&name, "@pl.thunk.%d", e->thunk_counter++);

    Type *rt = fnty->elem;

    StrBuf b;
    sb_init(&b);
    sb_printf(&b, "\ndefine internal i64 %s(i64 %%f, ptr %%a) {\n", sb_str(&name));
    sb_printf(&b, "entry:\n");
    sb_printf(&b, "  %%fp = inttoptr i64 %%f to ptr\n");

    // 引数を i64 の配列から本来の型へ 1 つずつ取り出す
    StrBuf argl;
    sb_init(&argl);
    for (int i = 0; i < fnty->nparams; i++) {
        Type *at = fnty->params[i];
        sb_printf(&b, "  %%p%d = getelementptr i64, ptr %%a, i64 %d\n", i, i);
        sb_printf(&b, "  %%w%d = load i64, ptr %%p%d\n", i, i);
        const char *use;
        StrBuf ub;
        sb_init(&ub);
        switch (at->kind) {
            case TY_INT:
                sb_printf(&ub, "%%w%d", i);
                break;
            case TY_BOOL:
                sb_printf(&b, "  %%x%d = trunc i64 %%w%d to i1\n", i, i);
                sb_printf(&ub, "%%x%d", i);
                break;
            case TY_FLOAT:
                sb_printf(&b, "  %%x%d = bitcast i64 %%w%d to double\n", i, i);
                sb_printf(&ub, "%%x%d", i);
                break;
            default:
                sb_printf(&b, "  %%x%d = inttoptr i64 %%w%d to ptr\n", i, i);
                sb_printf(&ub, "%%x%d", i);
                break;
        }
        use = sb_str(&ub);
        sb_printf(&argl, "%s%s %s", i ? ", " : "", llvm_type(at), use);
    }

    // 本来の型で呼び、戻り値を i64 に詰め直す
    if (rt->kind == TY_NONE) {
        sb_printf(&b, "  call void %%fp(%s)\n", sb_str(&argl));
        sb_printf(&b, "  ret i64 0\n");
    } else {
        sb_printf(&b, "  %%r = call %s %%fp(%s)\n", llvm_type(rt), sb_str(&argl));
        switch (rt->kind) {
            case TY_INT:
                sb_printf(&b, "  ret i64 %%r\n");
                break;
            case TY_BOOL:
                sb_printf(&b, "  %%ri = zext i1 %%r to i64\n  ret i64 %%ri\n");
                break;
            case TY_FLOAT:
                sb_printf(&b, "  %%ri = bitcast double %%r to i64\n  ret i64 %%ri\n");
                break;
            default:
                sb_printf(&b, "  %%ri = ptrtoint ptr %%r to i64\n  ret i64 %%ri\n");
                break;
        }
    }
    sb_printf(&b, "}\n");
    sb_printf(&e->thunkdefs, "%s", sb_str(&b));

    StrLit *q = xmalloc(sizeof(StrLit));
    q->label = (char *)key;
    q->bytes = sb_str(&name);
    q->next = e->thunks;
    e->thunks = q;
    return q->bytes;
}

// ── 文の生成 ────────────────────────────────────────────────
//
// 文は「値を返さない」ので、gen_expr とは別の関数にします。
// ただし式文だけは値を持つので、その値を返します
// （プログラムの値＝最後の式文の値、という暫定仕様のため）。
// 文 1 つを出す（デバッグ情報を付ける入口。実体は gen_stmt_inner）
static char *gen_stmt_inner(Emitter *e, Node *n);

static char *gen_stmt(Emitter *e, Node *n) {
    if (!e->dbg || !e->dbg_fn || !n->tok) return gen_stmt_inner(e, n);

    // ★ 文の行を覚えてから出し、出し終わった範囲に !dbg を付けます（A-30）
    int saved_line = e->dbg_line;
    e->dbg_line = n->tok->line;
    size_t start = e->fn.len;
    char *r = gen_stmt_inner(e, n);
    dbg_annotate(e, start);
    e->dbg_line = saved_line;
    return r;
}

static char *gen_stmt_inner(Emitter *e, Node *n) {
    // 終端済みブロックの後ろに来たら、到達不能ブロックを開く（規約 R7）
    ensure_block(e);

    switch (n->kind) {
        case ND_BLOCK: {
            // ★ ブロック＝スコープ。抜けるときに宣言の逆順で解放します。
            ScopeCtx sc = {e->scope, NULL};
            e->scope = &sc;

            char *last = NULL;
            for (Node *st = n->body; st; st = st->next) {
                char *v = gen_stmt(e, st);
                if (v) last = v;
            }
            // 終端済み（return / break で抜けた）なら、そちらで解放済み
            if (e->drop && !e->terminated) emit_scope_drops(e, &sc);

            e->scope = sc.outer;
            return last;
        }

        // ★ unsafe: は「検査のための印」なので、生成は中身そのまま
        // ★ scope: ブロック（A-18 の scoped spawn）。
        //   入口で枠を開き、出口で「まだ待っていないスレッド」を全部 join します。
        //   注意: 途中から抜ける道（return / break / continue / 失敗の伝播）は
        //     sema が禁じているので、出口はここ 1 か所だけです。
        case ND_SCOPE: {
            declare_rt(e, "void @pl_scope_begin()");
            declare_rt(e, "void @pl_scope_end()");
            sb_printf(&e->fn, "  call void @pl_scope_begin()\n");

            // 注意: ここは ND_BLOCK を呼ばずに自分で展開します。**join を
            //    解放より先に**行う必要があるためです（スレッドがこの
            //    ブロックの局所変数を借りていることがあります）。
            ScopeCtx sc68 = {e->scope, NULL};
            e->scope = &sc68;
            Node *blk = n->body;
            for (Node *st = blk->body; st; st = st->next) gen_stmt(e, st);

            ensure_block(e);
            sb_printf(&e->fn, "  call void @pl_scope_end()\n");

            if (e->drop) emit_scope_drops(e, &sc68);
            e->scope = sc68.outer;
            return NULL;
        }

        case ND_UNSAFE: return gen_stmt(e, n->body);

        case ND_IF: gen_if(e, n); return NULL;
        case ND_MATCH: gen_match(e, n); return NULL;
        case ND_WHILE: gen_while(e, n); return NULL;
        // ★ 列挙の宣言は実行時に何も出しません（枝はただの定数。A-37）
        case ND_ENUM: return NULL;

        // ── try / except ──
        //
        //   try の本体で失敗したら「振り分け」へ飛び、タグを見て except を選びます。
        //   どの except にも当たらなければ、外側の飛び先（外の try か伝播）へ。
        case ND_TRY: {
            int id = e->label_counter++;
            char disp_l[32], end_l[32];
            snprintf(disp_l, sizeof(disp_l), "try.dispatch.%d", id);
            snprintf(end_l, sizeof(end_l), "try.end.%d", id);

            TryCtxG ctx = {e->try_ctx, n, NULL, e->scope};
            ctx.dispatch = xstrndup(disp_l, strlen(disp_l));
            e->try_ctx = &ctx;
            gen_stmt(e, n->body);
            e->try_ctx = ctx.outer;
            if (!e->terminated) emit_br(e, end_l);

            // ── 振り分け ──
            emit_label(e, disp_l);
            const char *slot = err_slot(e);
            char *tag = load_tag(e, slot);

            int k = 0;
            for (Node *ex = n->els; ex; ex = ex->next, k++) {
                char hit_l[40], next_l[40];
                snprintf(hit_l, sizeof(hit_l), "except.%d.%d", id, k);
                snprintf(next_l, sizeof(next_l), "try.next.%d.%d", id, k);
                char *c = new_tmp(e);
                sb_printf(&e->fn, "  %s = icmp eq i64 %s, %d\n", c, tag, ex->err_tag);
                emit_cond_br(e, c, hit_l, next_l);
                emit_label(e, next_l);
            }
            // どの except にも当たらなかった → 外側へ渡す（無ければ到達しない）
            emit_fail_br(e);

            // ── 各 except の本体 ──
            k = 0;
            for (Node *ex = n->els; ex; ex = ex->next, k++) {
                char hit_l[40];
                snprintf(hit_l, sizeof(hit_l), "except.%d.%d", id, k);
                emit_label(e, hit_l);
                if (ex->ir_name) {
                    // as e : エラーオブジェクトを局所変数に入れる
                    char *pv = load_payload(e, slot);
                    sb_printf(&e->fn, "  store ptr %s, ptr %s\n", pv, ex->ir_name);
                }
                gen_stmt(e, ex->body);
                if (!e->terminated) emit_br(e, end_l);
            }

            emit_label(e, end_l);
            return NULL;
        }

        // ── raise ──
        case ND_RAISE: {
            char *obj = gen_expr(e, n->lhs);

            // ★ 同じ関数の中の try が捕まえるなら、そこへ飛びます（Python と同じ）。
            //   捕まえる try が無ければ、呼び出し元へ伝播します。
            TryCtxG *catcher = NULL;
            for (TryCtxG *t = e->try_ctx; t && !catcher; t = t->outer)
                for (Node *ex = t->node->els; ex; ex = ex->next)
                    if (ex->err_tag == n->err_tag) {
                        catcher = t;
                        break;
                    }

            if (catcher) {
                store_err(e, err_slot(e), n->err_tag, obj);
                if (e->drop) emit_drops_until(e, catcher->scope);
                emit_br(e, catcher->dispatch);
            } else {
                store_err(e, "%err.out", n->err_tag, obj);
                if (e->drop) emit_drops_until(e, NULL);
                emit_default_ret(e);
            }
            return NULL;
        }
        case ND_RETURN: {
            if (!n->lhs) {
                // ★ 値を返さない関数でも ensures は確かめます（A-29）
                gen_ensures(e, NULL);
                emit_drops_until(e, NULL);  // 抜けるスコープを全部解放
                sb_printf(&e->fn, "  ret void\n");  // 規約 R9
            } else {
                // ★ 戻り値を先に評価します。移動した変数はスロットが null に
                //   なっているので、この後の解放は何もしません（設計 §6.1）。
                //   注意: rc[T] を返すときは参照を 1 つ増やします（呼び出し側のぶん）。
                char *v = maybe_retain(e, n->lhs, gen_expr(e, n->lhs));
                // ★ 契約（A-29）：**解放の前に**確かめます。
                //   注意: 順序が逆だと、ensures の式が解放済みの値を読みます。
                gen_ensures(e, v);
                emit_drops_until(e, NULL);
                sb_printf(&e->fn, "  ret %s %s\n", llvm_type(n->lhs->type), v);
            }
            e->terminated = true;
            return NULL;
        }
        case ND_PASS: return NULL;  // 本当に何も出さない

        // ★ 契約（A-29）は入口と出口で出します。並びの位置では何も出しません。
        case ND_REQUIRES:
        case ND_ENSURES:
            return NULL;

        // 飛び先は sema が保証している（ループの外なら検査で弾かれる）
        // ★ ループから抜ける経路でも、抜けるスコープぶんだけ解放します。
        case ND_BREAK:
            emit_drops_until(e, e->loop->scope);
            emit_br(e, e->loop->break_label);
            return NULL;
        case ND_CONTINUE:
            emit_drops_until(e, e->loop->scope);
            emit_br(e, e->loop->continue_label);
            return NULL;

        case ND_VARDECL: {
            // alloca は entry ブロックに出済み（規約 R1）。ここでは store だけ。
            char *val = maybe_retain(e, n->rhs, gen_expr(e, n->rhs));
            gen_store(e, n->type, val, n->ir_name);
            scope_add(e, n);  // このスコープの解放対象に加える
            return NULL;
        }

        // ★ 分解代入 q, r = f()
        //   右辺を **1 回だけ**評価し、各要素を取り出してそれぞれの箱に入れます。
        case ND_UNPACK: {
            char *obj = gen_expr(e, n->rhs);
            const char *ty = tuple_ty(n->type);
            int k = 0;
            for (Node *v = n->params; v; v = v->next, k++) {
                char *pt = new_tmp(e);
                sb_printf(&e->fn,
                          "  %s = getelementptr %s, ptr %s, i32 0, i32 %d\n", pt,
                          ty, obj, k);
                char *val = gen_load(e, v->type, pt);
                gen_store(e, v->type, val, v->ir_name);
                scope_add(e, v);
            }
            return NULL;
        }

        case ND_ASSIGN: {
            // ★ 版分けの速い側では、誘導変数の増分の桁あふれ検査を
            //   外します。`v < L` のあいだ回るので、v が最後に取る値は L で、
            //   L は i64 に収まっています（＝あふれません）。
            //   注意: 外すのは**この 1 本だけ**です。本体の他の足し算
            //     （s = s + x など）は今までどおり検査します。
            bool saved_ovf67 = e->no_ovf;
            if (e->nobc && e->iv_ir && n->lhs->kind == ND_VAR && n->lhs->ir_name &&
                strcmp(n->lhs->ir_name, e->iv_ir) == 0)
                e->no_ovf = true;
            char *val = maybe_retain(e, n->rhs, gen_expr(e, n->rhs));
            e->no_ovf = saved_ovf67;

            // ★ 入れ替える前に、古い値を解放します。
            //   `s = s + "!"` のように、上書きは v1 では黙って捨てていました。
            //   注意: 借りものを束縛している変数（ownck が印を付けた）は所有者では
            //      ないので触りません。
            if (e->drop && !n->binds_borrow) {
                // 注意: グローバル（@g.x）は「プログラムが終わるまで生きる場所」
                //    なので、ここでは触りません（解放するのは所有者だけ）。
                if (n->lhs->kind == ND_VAR && n->lhs->ir_name[0] == '%')
                    emit_drop_value(e, n->type, gen_load(e, n->type, n->lhs->ir_name));
                // 注意: 対象を 2 回評価しないこと。古い値を読むために
                //    gen_field_ptr をもう一度通すので、対象が単純な変数の
                //    ときだけに限ります（xs[f()].g = v で f が 2 回走るのを防ぐ）。
                else if (n->lhs->kind == ND_FIELD && !n->lhs->mod_name &&
                         n->lhs->lhs->kind == ND_VAR)
                    emit_drop_value(
                        e, n->type,
                        gen_load_tb(e, n->type, gen_field_ptr(e, n->lhs),
                                    TBAA_FIELD));
            }
            // 添字への代入 xs[i] = v
            if (n->lhs->kind == ND_INDEX) {
                gen_index_store(e, n->lhs, val);
                return NULL;
            }
            // フィールドへの代入 t.kind = v
            if (n->lhs->kind == ND_FIELD) {
                // lexer.counter = v は「他モジュールのグローバル」への代入
                if (n->lhs->mod_name) {
                    if (n->lhs->is_extern) declare_extern_global(e, n->lhs);
                    gen_store(e, n->type, val, n->lhs->ir_name);
                    return NULL;
                }
                gen_store_tb(e, n->type, val, gen_field_ptr(e, n->lhs),
                             TBAA_FIELD);
                return NULL;
            }
            gen_store(e, n->type, val, n->lhs->ir_name);
            return NULL;
        }

        default: {
            char *v = gen_expr(e, n);  // 式文
            // ★ 捨てられる一時値（呼び出しの戻り値）を解放します。
            //   注意: 借用を返す関数（仕様 §4.5）の戻り値は所有していません。
            if (e->drop && !n->binds_borrow && n->type) emit_drop_value(e, n->type, v);
            return v;
        }
    }
}

// ── alloca の収集（規約 R1）────────────────────────────────
//
// ★ すべてのローカル変数は entry ブロックで alloca します。
//
//   関数本体の途中に alloca を書いても動きますが、ループの中にあると
//   反復のたびにスタックを消費します。entry にまとめるのが LLVM の作法で、
//   mem2reg が最適化しやすい形でもあります。
//
//   本体を生成する「前」に AST を歩いて、宣言されている変数を全部集めます。
//   if / while のブロックが入っても、再帰で辿れば同じように動きます。
static void collect_allocas(Emitter *e, Node *n) {
    if (!n) return;

    // 注意: グローバル変数は alloca しない（@g.x をそのまま読み書きする）
    if (n->kind == ND_VARDECL && !n->is_global) {
        sb_printf(&e->allocas, "  %s = alloca %s\n", n->ir_name,
                  llvm_mem_type(n->type));
        // ★ デバッグ情報（A-35）：箱を作ったついでに「名前と型」を書きます。
        //   注意: 箱を作る場所と印を付ける場所を分けると、必ずどちらかが
        //     先に増えて食い違います。**同じ 1 か所**に置きます。
        dbg_local(e, n->name, n->ir_name, n->type, n->tok ? n->tok->line : 0, 0);
    }

    // ★ 分解代入で受け取る名前も、ふつうの局所変数と同じ箱が要ります
    if (n->kind == ND_UNPACK)
        for (Node *v = n->params; v; v = v->next) {
            sb_printf(&e->allocas, "  %s = alloca %s\n", v->ir_name,
                      llvm_mem_type(v->type));
            dbg_local(e, v->name, v->ir_name, v->type, v->tok ? v->tok->line : 0, 0);
        }

    // ★ except ... as e で束縛する変数も、ふつうの局所変数と同じ箱が要ります。
    if (n->kind == ND_EXCEPT && n->ir_name) {
        sb_printf(&e->allocas, "  %s = alloca %s\n", n->ir_name,
                  llvm_mem_type(n->type));
        dbg_local(e, n->name, n->ir_name, n->type, n->tok ? n->tok->line : 0, 0);
    }

    // 注意: except の並びは next で繋がっています（if の else と違って複数あります）。
    //    2 番目以降はここでたどります（先頭は下の collect_allocas(n->els) が拾う）。
    if (n->kind == ND_TRY && n->els)
        for (Node *ex = n->els->next; ex; ex = ex->next) collect_allocas(e, ex);  // ★ bool は i8（規約 R5）

    // 子と兄弟をたどる。
    // ★ 「再帰なのでブロックが入っても勝手に見つかる」と
    //   書いたとおりになりました。else 節の分だけ 1 行足せば済みます。
    collect_allocas(e, n->lhs);
    collect_allocas(e, n->rhs);
    collect_allocas(e, n->els);
    // ★ 実引数もたどります。内包表記は**式**なので、
    //   f([x for x in xs]) のように呼び出しの引数の中に現れます。
    //   その隠し変数の箱を見落とすと、生成した IR が壊れます
    //   （"use of undefined value" で clang に叱られて気づきました）。
    for (Node *a = n->args; a; a = a->next) collect_allocas(e, a);
    for (Node *s = n->body; s; s = s->next) collect_allocas(e, s);
}

// ── 関数の生成 ──────────────────────────────────────────────

// 1 つの関数を出力する。
//
// ★ 当初は「暗黙の main」でしたが、ここで普通の関数になりました。
static void gen_func(Emitter *e, Node *n) {
    // 関数ごとに状態をリセットする（最初から決めてあった規約）
    e->tmp_counter = 0;
    e->label_counter = 0;
    e->terminated = false;
    e->loop = NULL;

    // ── デバッグ情報（A-30 / A-35）：この関数の枠を 1 つ作る ──
    //
    // ★ 引数と戻り値の型も書きます（A-35）。バックトレースが
    //   `add(a=2, b=3)` の形で出るのは、この型と、下で出す
    //   !DILocalVariable(arg: N) が揃っているからです。
    //
    // 注意: types の**先頭は戻り値**で、None（void）は `null` と書きます。
    //   失敗しうる関数が余分に取るエラー出力（%err.out）は書いていません。
    //   利用者が書いた引数だけを並べます。
    e->dbg_fn = 0;
    e->dbg_loc = 0;
    e->dbg_line = n->tok ? n->tok->line : 1;
    if (e->dbg) {
        int ret = dbg_type(e, n->type);
        StrBuf pl;
        sb_init(&pl);
        if (ret) sb_printf(&pl, "!%d", ret);
        else sb_printf(&pl, "null");
        for (Node *pm = n->params; pm; pm = pm->next) {
            int pt = dbg_type(e, pm->type);
            if (pt) sb_printf(&pl, ", !%d", pt);
            else sb_printf(&pl, ", null");
        }
        int types = dbg_id(e);
        sb_printf(&e->dbgmeta, "!%d = !{%s}\n", types, sb_str(&pl));
        int subty = dbg_id(e);
        sb_printf(&e->dbgmeta, "!%d = !DISubroutineType(types: !%d)\n", subty,
                  types);

        int id = dbg_id(e);
        sb_printf(&e->dbgmeta,
                  "!%d = distinct !DISubprogram(name: \"%s\", linkageName: \"%s\", "
                  "scope: !%d, file: !%d, line: %d, type: !%d, scopeLine: %d, "
                  "spFlags: DISPFlagDefinition, unit: !%d)\n",
                  id, n->name, n->ir_name, e->dbg_file, e->dbg_file, e->dbg_line,
                  subty, e->dbg_line, e->dbg_cu);
        e->dbg_fn = id;
    }

    // ── エラー処理の状態 ──
    e->fn_raises = n->raises != NULL;
    e->fn_ret = n->type;
    e->err_slot = false;
    e->try_ctx = NULL;
    e->prop_used = false;
    snprintf(e->prop_label, sizeof(e->prop_label), "err.propagate");
    if (e->fn_raises) ensure_err_type(e);
    sb_init(&e->allocas);
    sb_init(&e->dbgdecl);
    sb_init(&e->fn);

    // ★ 関数もメソッドも、sema がモジュール修飾済みの名前を入れています
    //   （@lexer.make / @lexer.Token.show）。main も例外ではありません。
    //   「main だけ @pl_main」という当初からの特別扱いは、
    //   モジュール修飾がその役目を引き取ったので無くなりました。
    const char *ir_name = n->ir_name;

    // ① 引数を alloca にコピーする（規約 R8）。
    //
    // なぜコピーするのか
    //   %n.arg は SSA レジスタなので代入できません。本言語では引数に代入
    //   できる（a = a + 1）ので、ローカル変数と同じ「箱」にしてしまいます。
    //   mem2reg がこの余分なコピーを消してくれます。
    int argno = 0;
    for (Node *pm = n->params; pm; pm = pm->next) {
        sb_printf(&e->allocas, "  %s = alloca %s\n", pm->ir_name,
                  llvm_mem_type(pm->type));
        StrBuf arg;
        sb_init(&arg);
        sb_printf(&arg, "%%%s.arg", pm->name);
        gen_store(e, pm->type, sb_str(&arg), pm->ir_name);
        // ★ デバッグ情報（A-35）：引数も箱に写してあるので、ローカル変数と
        //   同じ形で書けます。何番目の引数かだけを足します。
        argno++;
        dbg_local(e, pm->name, pm->ir_name, pm->type,
                  pm->tok ? pm->tok->line : e->dbg_line, argno);
    }

    // ★ 範囲型の仮引数は、**関数の入口で 1 回だけ**確かめます（A-28）。
    //
    // なぜ呼び出し側ではないのか
    //   呼び出しの経路は 5 通り（ふつうの呼び出し・メソッド・生成・
    //   関数ポインタ越し・spawn）あり、どれか 1 つ書き忘れると
    //   **そこだけ検査が外れます**。入口なら 1 か所で全部を守れます。
    for (Node *pm = n->params; pm; pm = pm->next)
        if (ty_is_range(pm->type)) {
            char *v = new_tmp(e);
            sb_printf(&e->fn, "  %s = load i64, ptr %s\n", v, pm->ir_name);
            gen_range_check(e, pm->type, v);
        }

    // ★ 契約（A-29）：requires は**関数の入口**で確かめます。
    //   注意: 範囲型の引数の検査（A-28）より後です。引数が範囲型なら、
    //     まず「その型に入るか」、次に「契約を満たすか」の順になります。
    e->fn_node = n;
    for (Node *st = n->body->body; st; st = st->next) {
        if (st->kind != ND_REQUIRES && st->kind != ND_ENSURES) break;
        if (st->kind == ND_REQUIRES) gen_contract_check(e, st, n->name);
    }

    // ② ローカル変数の alloca
    collect_allocas(e, n->body);

    // ★ 引数のスコープ。own で受け取った引数は、この関数が所有者なので
    //   出口で解放します（借用の引数には ownck が「借りもの」の印を付けています）。
    ScopeCtx params = {NULL, NULL};
    e->scope = &params;
    for (Node *pm = n->params; pm; pm = pm->next) scope_add(e, pm);

    // ③ 本体
    gen_stmt(e, n->body);

    // ── 伝播ブロック（呼び出し元へエラーをそのまま返す）──
    //
    // ★ 使われたときだけ出します（使わないブロックがあると LLVM が警告します）。
    if (e->prop_used) {
        emit_label(e, e->prop_label);
        if (e->drop) emit_drops_until(e, NULL);
        ensure_err_type(e);
        char *ev = new_tmp(e);
        sb_printf(&e->fn, "  %s = load %%pl.err, ptr %%err.slot\n", ev);
        sb_printf(&e->fn, "  store %%pl.err %s, ptr %%err.out\n", ev);
        emit_default_ret(e);
    }

    // ④ 終端されていなければ終端する（規約 R6）
    if (!e->terminated) {
        // ★ 契約（A-29）：**最後まで落ちてくる出口**でも ensures を確かめます。
        //   注意: ここを忘れると、`return` を書かない None の関数だけ
        //     事後条件がすり抜けます（出口は return だけではありません）。
        if (n->type->kind == TY_NONE) gen_ensures(e, NULL);
        if (e->drop) emit_scope_drops(e, &params);
        if (n->type->kind == TY_NONE) {
            sb_printf(&e->fn, "  ret void\n");  // 規約 R9
        } else {
            // 全経路 return は sema が保証済み。ここに来るのは
            // 「if/else の両方が return して合流点が到達不能」の場合。
            sb_printf(&e->fn, "  unreachable\n");
        }
    }
    e->scope = NULL;

    // ⑤ 組み立て
    sb_printf(&e->body, "\ndefine %s @%s(", llvm_type(n->type), ir_name);
    bool first = true;
    for (Node *pm = n->params; pm; pm = pm->next) {
        sb_printf(&e->body, "%s%s %%%s.arg", first ? "" : ", ",
                  llvm_type(pm->type), pm->name);
        first = false;
    }
    // ★ 失敗しうる関数は、エラー出力ポインタを 1 本余分に取ります
    if (e->fn_raises) sb_printf(&e->body, "%sptr %%err.out", first ? "" : ", ");
    // ★ デバッグ情報（A-30）：この関数の枠を metadata に結びます
    if (e->dbg && e->dbg_fn)
        sb_printf(&e->body, ") !dbg !%d {\nentry:\n", e->dbg_fn);
    else
        sb_printf(&e->body, ") {\nentry:\n");
    sb_printf(&e->body, "%s", sb_str(&e->allocas));
    // ★ 変数の印は alloca の**すべて後ろ**に置きます（A-35）。
    //   途中に挟むと、後ろの alloca が entry の先頭から離れます（規約 R1）。
    sb_printf(&e->body, "%s", sb_str(&e->dbgdecl));
    sb_printf(&e->body, "%s", sb_str(&e->fn));
    sb_printf(&e->body, "}\n");
    e->dbg_fn = 0;
    e->dbg_loc = 0;
}

// グローバル変数を出力する（言語仕様 6.2）
static void gen_global(Emitter *e, Node *n) {
    // 初期化式がリテラルであることは sema が保証している
    if (n->type->kind == TY_STR) {
        char *lab = intern_str(e, n->rhs->sval, n->rhs->slen);
        sb_printf(&e->globals, "%s = global ptr %s\n", n->ir_name, lab);
        return;
    }
    // ★ float は値ではなく、字句解析器が正規化した文字列を出します
    //   （src/lexer.c の read_number 参照）。
    if (n->type->kind == TY_FLOAT) {
        sb_printf(&e->globals, "%s = global double %s\n", n->ir_name,
                  n->rhs->sval);
        return;
    }
    sb_printf(&e->globals, "%s = global %s %lld\n", n->ir_name,
              llvm_mem_type(n->type), n->rhs->ival);
}

// C の main を出力する。
//
// 本言語の main は int（= i64）を返しますが、C の main は i32 を返します。
// そこで「ユーザーの main を @<モジュール>.main として出し、@main は
// それを呼んで trunc するラッパにする」方式をとります。
// （ir-conventions.md 第7節の方式 A）
//
// なぜラッパ方式か：main を他の関数と同じ規則で生成できるので、
//    コード生成器に「main だけ特別」という分岐が入りません。
//
// 注意: これを出すのは入口モジュールだけです。
//    全モジュールが出すと、リンク時に @main が重複します。
static void gen_c_main(Emitter *e, const char *main_ir_name) {
    e->tmp_counter = 0;

    // ★ argc / argv を受け取り、ランタイムに預けます。
    //   sys.argv() はこれを list[str] にして返します。
    //   ラッパ方式にしておいたので、利用者の main も AST も無変更です。
    declare_rt(e, "void @pl_set_args(i64, ptr)");

    sb_printf(&e->body, "\n");
    sb_printf(&e->body, "define i32 @main(i32 %%argc, ptr %%argv) {\n");
    sb_printf(&e->body, "entry:\n");

    char *a = new_tmp(e);
    sb_printf(&e->body, "  %s = sext i32 %%argc to i64\n", a);
    sb_printf(&e->body, "  call void @pl_set_args(i64 %s, ptr %%argv)\n", a);

    char *t0 = new_tmp(e);
    sb_printf(&e->body, "  %s = call i64 @%s()\n", t0, main_ir_name);

    char *t1 = new_tmp(e);
    sb_printf(&e->body, "  %s = trunc i64 %s to i32\n", t1, t0);

    // ★ 下位 8 ビットに切り詰めます（言語仕様 6.1）。
    //   POSIX は終了コードを勝手に & 0xFF しますが、Windows は 32 ビットを
    //   そのまま返すので、-3 が 0xFFFFFFFD になって「異常終了」に見えます。
    //   ここで揃えておけば、**どの OS でも同じ終了コード**になります
    //   （CI の Windows ジョブが見つけた差）。
    char *t2 = new_tmp(e);
    sb_printf(&e->body, "  %s = and i32 %s, 255\n", t2, t1);

    sb_printf(&e->body, "  ret i32 %s\n", t2);
    sb_printf(&e->body, "}\n");
}

// ── 入口 ───────────────────────────────────────────────────

// モジュール 1 つぶんの IR を作る。
//
// ★ 「1 ファイル = 1 モジュール = 1 つの .ll」（13.2 節）。
//   import したモジュールのものは、使ったぶんだけ declare / 型定義の複製が
//   自動で付いてきます（class_type / declare_extern が「出済みか」を見るため）。
char *codegen(Module *mod, const char *main_ir_name, bool drop, bool no_ovf,
              const char *triple, bool debug, bool verify_prove) {
    Node *ast = mod->ast;

    Emitter e = {0};
    e.ast = ast;
    e.drop = drop;      // 解放を挿入するか
    e.no_ovf = no_ovf;  // 桁あふれの検査を出さないか
    e.dbg = debug;      // -g（デバッグ情報を出すか）
    e.verify_prove = verify_prove;   // --verify-prove（消さずに残す。A-34）
    e.dbg_next = 10;    // ★ 0〜8 は TBAA が使っています（9 は空け）
    sb_init(&e.dbgmeta);
    sb_init(&e.header);
    sb_init(&e.globals);
    sb_init(&e.decls);
    sb_init(&e.body);
    sb_init(&e.dropdefs);
    sb_init(&e.thunkdefs);
    // ① ヘッダ
    sb_printf(&e.header, "; Generated by " PLC_LANG_CC "\n");
    sb_printf(&e.header, "source_filename = \"%s\"\n", mod->path);

    // 注意: 規約 R11：target triple は必ず出力する。
    //    書かないと clang が -Woverride-module 警告を出します。
    // ★ triple は外から渡します（--target / pragma target）。
    //   NULL ならビルド時に埋め込んだ既定値（＝この機械のもの）。
    if (!triple) triple = PLC_TARGET_TRIPLE;
    if (triple[0]) sb_printf(&e.header, "target triple = \"%s\"\n", triple);

    // ★ デバッグ情報（A-30）：モジュールに 1 組だけ要るものを先に作ります。
    //   注意: 関数より**先**に作ります。番号の振り方が 2 実装で同じになるためです。
    //   注意: 共有の !DISubroutineType は A-35 で無くなりました（関数ごとに
    //     本物の型を出すようになったためです）。
    if (e.dbg) {
        e.dbg_file = dbg_id(&e);
        e.dbg_cu = dbg_id(&e);
        sb_printf(&e.dbgmeta, "!%d = !DIFile(filename: \"%s\", directory: \".\")\n",
                  e.dbg_file, mod->path);
        sb_printf(&e.dbgmeta,
                  "!%d = distinct !DICompileUnit(language: DW_LANG_C99, file: !%d, "
                  "producer: \"" PLC_LANG_CC " " PLC_LANG_VERSION "\", isOptimized: false, "
                  "runtimeVersion: 0, emissionKind: FullDebug)\n",
                  e.dbg_cu, e.dbg_file);
    }

    // ② クラスの型定義（★ 使う側より先に、モジュールの先頭に出す）
    for (Node *d = ast->body; d; d = d->next) {
        // 注意: ジェネリックなテンプレートは出しません。
        //   K や V が何なのか決まっていないので、構造体の形が作れません。
        //   出すのは実体（Box$int など）だけです。
        if (d->kind == ND_CLASS && !d->targs) gen_class_type(&e, d->cls);
    }

    // ⑤ グローバル変数と関数定義
    for (Node *d = ast->body; d; d = d->next) {
        if (d->kind == ND_VARDECL) gen_global(&e, d);
        // ★ pragma は生成に何も出しません（main.c が読むだけ）
    }
    for (Node *d = ast->body; d; d = d->next) {
        // ★ extern は宣言だけを出す（定義は C 側にある）
        if (d->kind == ND_FUNC && !d->body) {
            StrBuf types;
            sb_init(&types);
            bool first = true;
            for (Node *pm = d->params; pm; pm = pm->next) {
                sb_printf(&types, "%s%s", first ? "" : ", ", llvm_type(pm->type));
                first = false;
            }
            declare_extern(&e, llvm_type(d->type), d->name, sb_str(&types));
            continue;
        }
        // 注意: ジェネリックなテンプレートは出しません
        if (d->kind == ND_FUNC && !d->targs) gen_func(&e, d);
        // メソッドも、ふつうの関数とまったく同じ関数で出します。
        // 違うのは名前（@lexer.Token.show）と、第 1 引数が self であることだけ。
        if (d->kind == ND_CLASS && !d->targs)
            for (Node *m = d->body; m; m = m->next)
                if (m->kind == ND_FUNC) gen_func(&e, m);
    }

    // 注意: C の main を出すのは入口モジュールだけ（重複定義になるため）
    if (main_ir_name) gen_c_main(&e, main_ir_name);

    // バッファを規定の順に連結する
    //
    // ★ 生成した @drop.* は最後に置きます（関数定義と同じ扱い）。
    StrBuf out;
    sb_init(&out);
    sb_printf(&out, "%s", sb_str(&e.header));
    sb_printf(&out, "%s", sb_str(&e.globals));
    sb_printf(&out, "%s", sb_str(&e.decls));
    sb_printf(&out, "%s", sb_str(&e.body));
    sb_printf(&out, "%s", sb_str(&e.dropdefs));
    sb_printf(&out, "%s", sb_str(&e.thunkdefs));
    // ⑥ 別名解析（TBAA）の型タグ。
    //   注意: 番号は固定です。**2 実装が同じ IR を出す**ため、
    //     使っていなくても常にこの 7 行を出します。
    sb_printf(&out, "\n!0 = !{!\"" PLC_LANG_NAME "\"}\n");
    sb_printf(&out, "!1 = !{!\"field\", !0}\n");
    sb_printf(&out, "!2 = !{!\"listhdr\", !0}\n");
    sb_printf(&out, "!3 = !{!\"listelem\", !0}\n");
    sb_printf(&out, "!4 = !{!1, !1, i64 0}\n");
    sb_printf(&out, "!5 = !{!2, !2, i64 0}\n");
    sb_printf(&out, "!6 = !{!3, !3, i64 0}\n");
    sb_printf(&out, "!7 = !{!\"global\", !0}\n");
    sb_printf(&out, "!8 = !{!7, !7, i64 0}\n");

    // ★ デバッグ情報（A-30）。-g のときだけ出ます。
    //   注意: 「Debug Info Version」を書かないと、clang が metadata を丸ごと
    //     捨てます（黙って消えるので、気づくのに時間がかかります）。
    if (e.dbg) {
        int flags1 = dbg_id(&e);
        int flags2 = dbg_id(&e);
        sb_printf(&out, "!%d = !{i32 7, !\"Dwarf Version\", i32 4}\n", flags1);
        sb_printf(&out, "!%d = !{i32 2, !\"Debug Info Version\", i32 3}\n", flags2);
        sb_printf(&out, "!llvm.module.flags = !{!%d, !%d}\n", flags1, flags2);
        sb_printf(&out, "!llvm.dbg.cu = !{!%d}\n", e.dbg_cu);
        sb_printf(&out, "%s", sb_str(&e.dbgmeta));
    }
    return sb_str(&out);
}
