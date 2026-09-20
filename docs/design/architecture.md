# コンパイラ・アーキテクチャ

> `yashirolang`（C 言語製、stage0 コンパイラ）の内部構造です。
> 「どのファイルが何を担当し、どんなデータが流れるか」を定義します。

---

## 1. 全体のデータフロー

```
                    ┌──────────────────────────────────────────┐
  hello.ys ────────▶│  util.c :: read_file()                   │
  (テキスト)         │  ファイルを読む・改行を正規化する           │
                    └──────────────────┬───────────────────────┘
                                       │ char *src
                                       ▼
                    ┌──────────────────────────────────────────┐
                    │  lexer.c :: tokenize()                   │  ①字句解析
                    │  文字列 → トークンの配列                   │
                    └──────────────────┬───────────────────────┘
                                       │ Token *tokens  (配列)
                                       ▼
                    ┌──────────────────────────────────────────┐
                    │  parser.c :: parse()                     │  ②構文解析
                    │  トークン列 → AST                         │
                    │  （ast.c のコンストラクタを使う）           │
                    └──────────────────┬───────────────────────┘
                                       │ Node *ast
                                       ├──────────▶ --dump-ast で S 式表示
                                       ▼
                    ┌──────────────────────────────────────────┐
                    │  sema.c :: sema()     │  ③意味解析
                    │  AST を歩いて型を検査し、AST に書き込む      │
                    │  ・スコープ / シンボルテーブル              │
                    │  ・型検査                                 │
                    │  ・return 到達性検査                       │
                    └──────────────────┬───────────────────────┘
                                       │ Node *ast (型注釈つき)
                                       ▼
                    ┌──────────────────────────────────────────┐
                    │  codegen.c :: codegen()                  │  ④コード生成
                    │  AST → LLVM IR テキスト                   │
                    └──────────────────┬───────────────────────┘
                                       │ hello.ll
                                       ├──────────▶ -S で .ll を残す
                                       ▼
                    ┌──────────────────────────────────────────┐
                    │  clang hello.ll runtime.o -o hello       │  ⑤LLVM に丸投げ
                    │  （main.c が system() で呼ぶ）             │
                    └──────────────────┬───────────────────────┘
                                       ▼
                                    hello (実行ファイル)
```

**重要な性質：各段階は前の段階の出力だけを入力にとる純粋な変換です。**
この分離を守ると、段階ごとに独立してテストできます。

---

## 2. ファイル構成と責務

| ファイル | 責務 | 依存してよいもの |
|---|---|---|
| `main.c` | 引数解析、パスの起動、`clang` の呼び出し | 全部 |
| `util.c/h` | 文字列バッファ、`xmalloc`、ファイル読み込み、エラー終了 | なし（最下層） |
| `lexer.c/h` | 字句解析 | util, diag |
| `ast.c/h` | AST ノード定義とコンストラクタ、S 式ダンプ | util, lexer(Token) |
| `types.c/h` | Type 構造体と操作 | util |
| `parser.c/h` | 構文解析 | util, lexer, ast, types |
| `sema.c/h` | 型検査 | util, ast, types, diag |
| `codegen.c/h` | LLVM IR 出力 | util, ast, types, diag |
| `diag.c/h` | 診断メッセージ整形 | util, lexer（`Token` のため） |

**注意: `diag` が `lexer` に依存する点について**
診断は「ソース上の位置」を示すため `Token` を受け取ります。そのため `diag.h` は
`lexer.h` を include します（`Token` を別ヘッダに切り出す設計も可能ですが、
ヘッダが 1 枚増える割に得るものが少ないため採用していません）。

依存の向きは `lexer → diag` ではなく **`diag → lexer`** です。
`lexer.c` は `diag.h` を include して `error_at()` を呼びますが、
`lexer.h` は `diag.h` を include しません。だから循環しません。

**注意: 位置情報のないエラーは `util.c` の `error()` に残しています。**
`xmalloc()` は確保失敗時にエラー終了する必要があるので、もし `error()` を
`diag.c` に置くと `util → diag → util` の循環依存になります。
「位置を示せない低レベルなエラーは util、位置つき診断は diag」と分けることで、
`util.c` を依存グラフの最下層に保っています。

**依存の向きは常に下向き（上の表で下 → 上）にします。**
`lexer.c` が `parser.h` を include したら設計ミスです。循環依存はビルドを壊し、
理解を壊し、そして最終的にセルフホストを壊します。

### なぜファイルを分けるのか

1 ファイル 5000 行のコンパイラも書けますが（実際 chibicc はそれに近い）、
今回は**最終的に yashirolang でこれを書き直す**ことが目的です。
モジュール境界を先に決めておくと、セルフホスト版へ 1 ファイルずつ移植できます。

```
src/lexer.c    →  selfhost/lexer.ys
src/parser.c   →  selfhost/parser.ys
src/sema.c     →  selfhost/sema.ys
src/codegen.c  →  selfhost/codegen.ys
```

**この 1:1 対応が、セルフホストを「4 つの小さな移植作業」に分解します。**

---

## 3. 主要データ構造

### 3.1 Token（`lexer.h`）

```c
typedef enum {
    TK_EOF,        // 入力終端
    TK_NEWLINE,    // 論理行の終わり
    TK_INDENT,     // ブロック開始
    TK_DEDENT,     // ブロック終了
    TK_INT,        // 整数リテラル
    TK_FLOAT,      // 浮動小数リテラル
    TK_STRING,     // 文字列リテラル
    TK_IDENT,      // 識別子
    TK_KEYWORD,    // キーワード
    TK_PUNCT,      // 記号（+ - * ( ) : -> など）
} TokenKind;

typedef struct Token Token;
struct Token {
    TokenKind kind;
    // 位置情報（エラー報告用）
    const char *file;   // ファイル名
    const char *line_start; // その行の先頭へのポインタ（ソース抜粋表示用）
    int line;           // 1 起算の行番号
    int col;            // 1 起算の桁番号
    // 本体
    const char *loc;    // ソース中の開始位置
    int len;            // 長さ
    // 値（kind によって使い分ける）
    long long ival;     // TK_INT
    double fval;        // TK_FLOAT
    char *sval;         // TK_STRING（エスケープ解決済み）
    char *text;         // NUL 終端したトークン文字列（IDENT/KEYWORD/PUNCT 用）
};
```

**なぜトークンは「配列」で、リンクリストではないのか**

chibicc など多くの教材はリンクリストを使いますが、yashirolang では**配列**にします。

- パーサが `peek(1)`, `peek(2)` のような**任意の先読み**をしたい（`x : int` の判別など）
- 配列なら `toks[pos + n]` で O(1)。リンクリストだと辿る必要がある
- yashirolang 側に移植するとき、`list[Token]` として自然に書ける

### 3.2 Node（`ast.h`）

AST ノードは **1 つの構造体で全種類を表現**します（タグ付き共用体の素朴版）。

```c
typedef enum {
    // ── 式 ──
    ND_INT,        // 整数リテラル       ival
    ND_FLOAT,      // 浮動小数リテラル   fval
    ND_STR,        // 文字列リテラル     sval
    ND_BOOL,       // True / False      ival (0/1)
    ND_NONE,       // None
    ND_VAR,        // 変数参照           name, var
    ND_BINOP,      // 二項演算           op, lhs, rhs
    ND_UNARY,      // 単項演算           op, lhs
    ND_LOGICAL,    // and / or（短絡）    op, lhs, rhs
    ND_CALL,       // 関数呼び出し        name, args, nargs
    ND_INDEX,      // a[i]              lhs, rhs
    ND_FIELD,      // a.f               lhs, name
    ND_METHOD,     // a.m(...)          lhs, name, args
    ND_LIST,       // [a, b, c]         args, nargs
    ND_CAST,       // 明示的型変換        lhs, type

    // ── 文 ──
    ND_BLOCK,      // 文のリスト          body
    ND_VARDECL,    // x: T = e           name, type, rhs, var
    ND_ASSIGN,     // lhs = rhs          lhs, rhs
    ND_IF,         // cond, then, els
    ND_WHILE,      // cond, body
    ND_FOR,        // 変数名, iter, body
    ND_RETURN,     // lhs（NULL 可）
    ND_BREAK,
    ND_CONTINUE,
    ND_PASS,
    ND_EXPRSTMT,   // 式文               lhs

    // ── トップレベル ──
    ND_FUNC,       // 関数定義
    ND_CLASS,      // クラス定義
    ND_PROGRAM,    // ファイル全体
} NodeKind;

typedef struct Node Node;
struct Node {
    NodeKind kind;
    Token *tok;        // このノードの代表トークン（★エラー報告に必須）
    Type *type;        // sema が埋める。それまで NULL

    // 子ノード（kind によって使い分ける）
    Node *lhs, *rhs;
    Node *cond, *then, *els;
    Node *body;        // ND_BLOCK / ND_WHILE などの中身（next で連結）
    Node *next;        // 兄弟ノード（文のリストを繋ぐ）

    Node **args;       // 引数の配列
    int nargs;

    // 値・名前
    long long ival;
    double fval;
    char *sval;
    char *name;
    int op;            // 演算子の種類

    // sema が埋める解決結果
    struct VarEntry *var;    // ND_VAR / ND_VARDECL の解決先
    struct FuncInfo *fn;     // ND_CALL の解決先
};
```

**注意: この「全部入り構造体」は美しくありません。**
本来は各ノード種別ごとに構造体を分けるべきです。しかし：

- C で共用体を安全に扱うコードは冗長になる
- 教材としては「1 個の構造体を見れば全部わかる」ほうが読みやすい
- chibicc, 8cc, tcc など実績のある小型 C コンパイラが同じ方式をとっている

メモリの無駄（1 ノード 150 バイト程度）は、この規模では問題になりません。

**★ `Token *tok` を全ノードに持たせるのは絶対に守ってください。**
これがないと、型エラーの位置を示せません。

### 3.3 StrBuf（`util.h`）— IR 出力の要

IR を組み立てるための伸長する文字列バッファです。

```c
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

void sb_init(StrBuf *sb);
void sb_printf(StrBuf *sb, const char *fmt, ...);   // 追記
char *sb_str(StrBuf *sb);                            // NUL 終端して返す
```

**なぜ直接 `fprintf(fp, ...)` しないのか**

IR 生成では「後から前に戻って書き足したい」ことがあります。たとえば：

- 関数本体を生成し終わってから、entry ブロックの先頭にすべての `alloca` を置きたい
- 文字列リテラルを見つけたら、ファイル先頭のグローバル定義セクションに追記したい

そのため、出力先を**複数のバッファに分けて持ち、最後に連結**します。

```c
typedef struct {
    StrBuf header;    // target triple, 型定義, declare
    StrBuf globals;   // グローバル変数・文字列定数
    StrBuf body;      // 関数本体
} CodegenOut;
```

---

## 4. コマンドラインインターフェース

```
yashirolang [options] <input.ys>

  -o <file>       出力する実行ファイル名（既定: a.out）
  -S              LLVM IR (.ll) を出力してそこで停止
  --dump-tokens   トークン列を表示して終了（Lexer のデバッグ用）
  --dump-ast      AST を S 式で表示して終了（Parser のデバッグ用）
  -O0/-O1/-O2/-O3 clang に渡す最適化レベル（既定: -O0）
  --keep-ll       実行ファイルを作った後も .ll を残す
  -h, --help      使い方
```

**★ `--dump-tokens` と `--dump-ast` は最初から用意します。**
これらは「あると便利なおまけ」ではなく、**開発速度を左右する中核ツール**です。
パーサのバグを追うとき、これがないと当て推量になります。

### 実行ファイル生成の流れ（`main.c`）

```c
// 1. .ll を一時ファイルに書き出す
char *ll_path = "/tmp/yashirolang-XXXXXX.ll";
write_file(ll_path, ir_text);

// 2. clang を呼ぶ
char cmd[4096];
snprintf(cmd, sizeof(cmd), "clang %s %s -o %s %s",
         opt_level, ll_path, out_path, runtime_obj);
int rc = system(cmd);
if (rc != 0) error("clang の実行に失敗しました（生成した IR に問題があります）");

// 3. 一時ファイルを消す（--keep-ll なら残す）
```

**注意: `system()` を使うのは手抜きです**が、教材としては十分です。
`fork`/`execvp` に直すのは練習問題としておきます（シェルのメタ文字問題を避けられます）。

---

## 5. エラー処理の方針

### 5.1 コンパイル時エラー

```c
// diag.c
_Noreturn void error_at(Token *tok, const char *fmt, ...);
_Noreturn void error(const char *fmt, ...);   // 位置情報なし
```

- **最初のエラーで `exit(1)`**（第9節のエラー回復の項を参照）
- 必ず `error_at()` を使い、`error()` は位置が本当にない場合のみ
- 出力先は **stderr**

### 5.2 コンパイラ内部の想定外（バグ）

```c
#define UNREACHABLE() \
    internal_error(__FILE__, __LINE__, "到達しないはずのコード")
```

`switch` の `default:` などで使います。**ユーザーのミスとコンパイラのバグを区別**することが重要です。

```
yashirolang internal error: src/codegen.c:412: 到達しないはずのコード
  これはコンパイラのバグです。報告してください。
```

---

## 6. メモリ管理（コンパイラ自身の）

**`free()` を呼びません。**

```c
void *xmalloc(size_t n) {
    void *p = calloc(1, n);
    if (!p) error("メモリ確保に失敗しました");
    return p;
}
```

**なぜ？** コンパイラは 1 ファイルをコンパイルして終了する短命なプロセスです。
AST は全体で使われ続けるので、解放するタイミングは「プロセス終了時」しかありません。
`free` を書くと、コードが増え、二重解放バグの余地が生まれ、得るものがありません。

`calloc` を使うのは**全フィールドが 0/NULL で初期化される**ためです。
`Node` のような大きな構造体で、初期化忘れによるバグを構造的に防げます。

---

## 7. テスト戦略

### 7.1 テストの種類

| 種類 | 方法 | 例 |
|---|---|---|
| **終了コードテスト** | `.ys` をコンパイル・実行し、終了コードを検証 | `42` → 42 |
| **標準出力テスト** | 実行結果の stdout を期待値と比較 | `print("hi")` → `hi\n` |
| **エラーテスト** | コンパイルが失敗し、期待するメッセージを含むことを検証 | `1 + "a"` → `型 'int' と 'str'` |
| **IR テスト** | 生成 IR が期待パターンを含むか（`FileCheck` 相当） | `add i64` を含む |
| **セルフホストテスト** | stage2 と stage3 のバイナリ一致 | — |

### 7.2 テストランナーの形式

`tests/cases/*.ys` の先頭コメントに期待値を書く方式にします。

```python
# EXIT: 42
42
```

```python
# OUTPUT: hello
def main() -> int:
    print("hello")
    return 0
```

```python
# ERROR: 型 'int' と 'str'
def main() -> int:
    return 1 + "a"
```

**なぜこの形式か**：テストケースと期待値が同じファイルにあると、
1 ファイル追加するだけでテストが 1 個増えます。
別ファイルに期待値を置く方式（`.expected` ファイル）だと管理が 2 倍になります。

### 7.3 実行

```bash
make test          # 全テスト
make test-one CASE=tests/cases/prec.ys
```

---

## 8. ビルドシステム

`Makefile` 1 枚で完結させます（CMake は使いません）。

```makefile
CC      = clang
CFLAGS  = -std=c11 -g -O0 -Wall -Wextra -Wno-unused-parameter
SRCS    = $(wildcard src/*.c)
OBJS    = $(SRCS:src/%.c=build/%.o)
TARGET  = build/yashirolang
```

**なぜ CMake を使わないのか**：依存が 1 つ増え、生成される Makefile が読めなくなります。
ソース 10 ファイル程度のプロジェクトに CMake は過剰です。

## 9. 設計上「やらないこと」の一覧

意識的に採用しない選択肢です。判断を忘れないために記録します。

| やらないこと | 理由 |
|---|---|
| 中間表現 (IR) を自作する | LLVM IR を直接吐けば足りる。段階を増やす価値がこの規模ではない |
| SSA 形式を自分で構築する | 全変数を `alloca` にして LLVM の `mem2reg` に任せる（[ir-conventions.md](ir-conventions.md) 参照） |
| 自前の最適化パス | LLVM の `-O2` に任せる |
| インクリメンタルコンパイル | 1 ファイルが速いので不要 |
| エラー回復（複数エラー報告） | 独立した難問。v1 のスコープ外 |
| ガベージコレクション | [memory-model.md](memory-model.md) 参照 |
| プリプロセッサ | Python 風言語に不要 |
