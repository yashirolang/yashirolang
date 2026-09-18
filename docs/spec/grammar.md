# 本言語文法定義（EBNF）

> パーサを書くときは、このファイルを見ながら書きます。
> **文法規則 1 つ = パーサ関数 1 つ** に対応させるのがこの教材の方針です。

---

## 0. 記法の説明

| 記法 | 意味 |
|---|---|
| `A ::= B` | A は B と定義される |
| `A B` | A の後に B（連結） |
| `A \| B` | A または B（選択） |
| `[ A ]` | A は 0 回か 1 回（省略可） |
| `{ A }` | A は 0 回以上の繰り返し |
| `( A )` | グループ化 |
| `"if"` | その文字列そのもの（終端記号） |
| `NEWLINE` | 大文字は字句解析器が作るトークン |

---

## 1. トークン（字句解析器が作るもの）

```ebnf
(* ── 数値 ── *)
INT        ::= DEC | HEX | OCT | BIN
DEC        ::= digit { digit | "_" }
HEX        ::= "0" ("x"|"X") hexdigit { hexdigit | "_" }
OCT        ::= "0" ("o"|"O") octdigit { octdigit | "_" }
BIN        ::= "0" ("b"|"B") bindigit { bindigit | "_" }
(* ⚠️ 小数点の後には**必ず数字が要ります**（`1.` は FLOAT ではない）。
   `1.foo` のような書き方と食い違わせないためです。 *)
FLOAT      ::= digit { digit | "_" } "." digit { digit | "_" } [ exponent ]
             | digit { digit | "_" } exponent
exponent   ::= ("e"|"E") [ "+" | "-" ] digit { digit }

digit      ::= "0".."9"
hexdigit   ::= digit | "a".."f" | "A".."F"
octdigit   ::= "0".."7"
bindigit   ::= "0" | "1"

(* ── 識別子 ── *)
IDENT      ::= ident_start { ident_cont }        (* ただしキーワードを除く *)
ident_start::= "A".."Z" | "a".."z" | "_"
ident_cont ::= ident_start | digit

(* ── 文字列 ── *)
STRING     ::= '"' { dq_char } '"' | "'" { sq_char } "'"
dq_char    ::= <'"' と '\' と改行 以外の任意のバイト> | escape
sq_char    ::= <"'" と '\' と改行 以外の任意のバイト> | escape
escape     ::= "\" ( "n" | "t" | "r" | "0" | "\" | '"' | "'" | "x" hexdigit hexdigit )

(* ── 仮想トークン（字句解析器が合成する） ── *)
NEWLINE    (* 論理行の終わり *)
INDENT     (* ブロックの始まり *)
DEDENT     (* ブロックの終わり *)
EOF        (* 入力の終わり *)
```

### 仮想トークンについて

`NEWLINE` / `INDENT` / `DEDENT` はソースコード中に対応する文字がありません。
字句解析器がインデント幅を追跡して**合成**します。

```python
def f() -> int:
    return 1
```

は、こうトークン化されます。

```
DEF IDENT(f) LPAREN RPAREN ARROW IDENT(int) COLON NEWLINE
INDENT
    RETURN INT(1) NEWLINE
DEDENT
EOF
```

こうすると、パーサ側では `INDENT` … `DEDENT` を「`{` … `}`」と同じように扱えます。
**インデント言語の構文解析が、波括弧言語とまったく同じ難しさに落ちる**のがこの技法の狙いです。

---

## 2. プログラム全体

```ebnf
program    ::= { NEWLINE } { top_level } EOF

top_level  ::= func_def
             | class_def
             | extern_def
             | import_stmt
             | range_decl
             | global_var
             | NEWLINE
```

⚠️ **`range_decl` は `global_var` より先に試します。** `type` は予約語では
ないので、`type: int = 0`（`type` という名前のグローバル変数）と区別するには
3 つ先まで見る必要があります。

---

## 3. 宣言

```ebnf
(* ── 関数定義 ── *)
func_def   ::= "def" IDENT "(" [ param_list ] ")" "->" type ":" block

param_list ::= param { "," param }
param      ::= IDENT ":" [ "own" | "mut" ] type      (* v2 *)
             | [ "mut" ] "self"                       (* v2。self は型注釈なし *)

(* ── extern 宣言 ── *)
extern_def ::= "extern" "def" IDENT "(" [ param_list ] ")" "->" type NEWLINE

(* ── クラス定義 ── *)
class_def  ::= "class" IDENT ":" NEWLINE INDENT class_body DEDENT

class_body ::= { field_decl } { func_def | NEWLINE }
field_decl ::= IDENT ":" type NEWLINE

(* ── 範囲型（部分型。A-28）── *)
(* ⚠️ "type" も "range" も予約語ではありません。トップレベルで
   「IDENT("type") IDENT "="」と並んだときだけこの規則に入ります。 *)
range_decl ::= "type" IDENT "=" "int" "range" "(" int_lit "," int_lit ")" NEWLINE
int_lit    ::= [ "-" ] INT

(* ── グローバル変数 ── *)
global_var ::= IDENT ":" type "=" expr NEWLINE

(* ── import ── *)
import_stmt::= "import" IDENT NEWLINE

(* ⚠️ "from X import Y" は採用しません。名前の出どころが
   ソースから読み取れなくなるためです。 *)
```

**⚠️ 注意**：`class_body` はフィールド宣言をすべてメソッドより先に置くことを要求します。
これは「フィールドのレイアウトを確定してからメソッドを型検査したい」という
実装上の都合を、文法レベルで保証させるためです。

---

## 4. ブロックと文

```ebnf
block      ::= NEWLINE INDENT stmt { stmt } DEDENT

stmt       ::= simple_stmt NEWLINE
             | if_stmt
             | while_stmt
             | for_stmt
             | contract_stmt

(* ── 契約（A-29）──
   ⚠️ "requires" / "ensures" は予約語ではありません。次のトークンが
   "=" "+=" "-=" "*=" "//=" "%=" ":" "." "(" "[" "," と改行のどれでもない
   ときだけ、この規則に入ります（`requires = 3` は今までどおりの代入）。
   ⚠️ 置ける場所は**関数の本体の先頭**だけです（意味解析が確かめます）。 *)
contract_stmt ::= ( "requires" | "ensures" ) expr NEWLINE

simple_stmt::= var_decl
             | assign_stmt
             | return_stmt
             | "break"
             | "continue"
             | "pass"
             | expr_stmt

(* ── 変数宣言と代入 ── *)
var_decl   ::= IDENT ":" type "=" expr
assign_stmt::= target ( "=" | augop ) expr
augop      ::= "+=" | "-=" | "*=" | "//=" | "%="
target     ::= IDENT
             | postfix "[" expr "]"
             | postfix "." IDENT

return_stmt::= "return" [ expr ]
expr_stmt  ::= expr

(* ── if ── *)
if_stmt    ::= "if" expr ":" block
               { "elif" expr ":" block }
               [ "else" ":" block ]

(* ── while ── *)
while_stmt ::= "while" expr ":" block

(* ── for ── *)
for_stmt   ::= "for" IDENT "in" expr ":" block
```

### 曖昧性の解消：`var_decl` と `assign_stmt`

どちらも `IDENT` で始まります。パーサは **2 トークン先読み**して区別します。

```
IDENT ":"  → var_decl    （x: int = 1）
それ以外   → assign_stmt （x = 1、xs[0] = 1、p.f = 1）
```

`assign_stmt` の左辺はさらに厄介です。`xs[0] = 1` の `xs[0]` を読み終わるまで、
代入文なのか式文なのかわかりません。実装では次の方針をとります。

1. まず `expr` として読む
2. 次のトークンが `=` か `augop` なら、読んだ式を代入先（target）として再解釈する
3. `target` になれない式（例：`f() = 1`）ならエラー

これは **式を先に読んで後から役割を決める** よくある実装テクニックです。

---

## 5. 式（優先順位の階層）

**ここが再帰下降パーサの核心です。**
優先順位の低い規則が、優先順位の高い規則を呼ぶ形に階層化します。

```ebnf
expr       ::= or_expr

or_expr    ::= and_expr   { "or"  and_expr }
and_expr   ::= not_expr   { "and" not_expr }
not_expr   ::= "not" not_expr
             | comparison

comparison ::= bitor_expr [ compop bitor_expr ]        (* 連鎖不可：1回だけ *)
compop     ::= "==" | "!=" | "<" | "<=" | ">" | ">=" | "in" | "is" | "is" "not" | "not" "in"

bitor_expr ::= bitxor_expr { "|" bitxor_expr }
bitxor_expr::= bitand_expr { "^" bitand_expr }
bitand_expr::= shift_expr  { "&" shift_expr }
shift_expr ::= add_expr    { ("<<" | ">>") add_expr }
add_expr   ::= mul_expr    { ("+" | "-") mul_expr }
mul_expr   ::= unary       { ("*" | "/" | "//" | "%") unary }

unary      ::= ("-" | "+" | "~") unary
             | power

power      ::= postfix [ "**" unary ]                  (* 右結合 *)

postfix    ::= primary { call_suffix | index_suffix | attr_suffix }
call_suffix ::= "(" [ arg_list ] ")"
index_suffix::= "[" expr "]"
attr_suffix ::= "." IDENT
arg_list   ::= expr { "," expr }

primary    ::= INT | FLOAT | STRING
             | "True" | "False" | "None"
             | IDENT
             | "(" expr ")"
             | list_display

list_display ::= "[" [ expr { "," expr } [ "," ] ] "]"
```

### 📖 この階層がなぜ優先順位を実現するのか

`1 + 2 * 3` を `add_expr` から読む様子を追ってみます。

```
add_expr:
  mul_expr を呼ぶ
    unary → power → postfix → primary → INT(1)     ← 1 を読んだ
    次は "+"。mul_expr のループ条件（* / // %）に合わないので抜ける
  → mul_expr は 1 を返す
  次のトークンは "+" → add_expr のループに入る
    mul_expr を呼ぶ
      unary → ... → INT(2)                          ← 2 を読んだ
      次は "*" → mul_expr のループに入る
        unary → ... → INT(3)                        ← 3 を読んだ
      → BinOp(*, 2, 3) を作る
    → mul_expr は (2*3) を返す
  → BinOp(+, 1, (2*3)) を作る
```

結果の木：

```
      +
     / \
    1   *
       / \
      2   3
```

**`*` のほうが木の深い位置（＝先に計算される位置）に来ています。**
「優先順位の高い演算子を、深い（後から呼ばれる）関数に置く」
これだけで優先順位が実現します。

### 📖 左結合と右結合の書き分け

```ebnf
(* 左結合：ループで書く → ((1-2)-3) *)
add_expr ::= mul_expr { ("+" | "-") mul_expr }

(* 右結合：再帰で書く → (2**(3**4)) *)
power    ::= postfix [ "**" unary ]
                            ↑ 自分より上（弱い方）を呼び戻すのがポイント
```

C のコードにすると、この差が明確になります。

```c
// 左結合：while ループ
Node *add_expr(Parser *p) {
    Node *lhs = mul_expr(p);
    while (match(p, TK_PLUS) || match(p, TK_MINUS)) {
        ...
        lhs = new_binop(op, lhs, mul_expr(p));   // ← lhs を上書きし続ける
    }
    return lhs;
}

// 右結合：再帰呼び出し
Node *power(Parser *p) {
    Node *base = postfix(p);
    if (match(p, TK_POW)) {
        return new_binop(OP_POW, base, unary(p)); // ← 自分と同格を再帰で呼ぶ
    }
    return base;
}
```

---

## 6. 型の文法

```ebnf
type       ::= [ IDENT "." ] IDENT [ "[" type { "," type } "]" ]   (* 修飾は 1 段 *)
             | type "|" "None"                    (* Nullable *)
```

例：

```
int
str
list[int]
list[list[str]]
dict[str, int]
Token
lexer.Token          (* 他モジュールの型 *)
list[lexer.Token]
Token | None
```

**⚠️ モジュール修飾は 1 段だけ**です（`a.b.Token` は書けません）。
パッケージ（階層モジュール）を採用しないためです。

**⚠️ 曖昧性**：`type` の `|` と、式の `|`（ビット OR）は同じ記号です。
型が現れる文脈（`:` の後、`->` の後、`[` `]` の中）でのみ型パーサを呼ぶことで区別します。
**型と式を別のパーサ関数で処理する**のが解決策です。

---

## 7. 文法の検証方法

文法を変えたら、次のテストケースが期待どおりに parse できるか確認します。
AST ダンプ（S 式）で確認するのが確実です。

```
$ ./build/{{cc}} --dump-ast tests/cases/prec{{ext}}
(binop + (int 1) (binop * (int 2) (int 3)))
```

`--dump-ast` オプションは**パーサのデバッグに必須の道具**です。
