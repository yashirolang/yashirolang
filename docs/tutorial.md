# yashirolang 入門

**yashirolang を書く人のための手引きです。**
Python と同じところは説明しません。**違うところだけ**を順に見ていきます。

> **一行でいうと** — Python の見た目のまま、型が静的に決まり、機械語にコンパイルされる言語です。
> GC の代わりに、Rust に近い所有権の仕組みで「解放し忘れ・二重解放」をコンパイル時に止めます。

| | |
|---|---|
| 拡張子 | `.ys` |
| コンパイラ | `yashirolang`（インストールは [getting-started.md](getting-started.md)） |
| 入口 | `def main() -> int:` |
| 実行時 | GC なし・アンワインドする例外なし・リフレクションなし |

**目次**

1. [まず知っておく 5 つの違い](#1-まず知っておく-5-つの違い)
2. [型と値](#2-型と値)
3. [制御構造](#3-制御構造)
4. [関数](#4-関数)
5. [クラス](#5-クラス)
6. [モジュールと標準ライブラリ](#6-モジュールと標準ライブラリ)
7. [所有権と借用 — この言語の中心](#7-所有権と借用--この言語の中心)
8. [エラー処理](#8-エラー処理)
9. [契約と範囲型](#9-契約と範囲型)
10. [並行実行](#10-並行実行)
11. [コンパイラの使い方](#11-コンパイラの使い方)
12. [まとめの例](#12-まとめの例)
13. [いま無いもの](#13-いま無いもの)

---

## 1. まず知っておく 5 つの違い

Python のつもりで書くと必ず引っかかる場所です。**どれもコンパイル時に止まります。**

| # | Python では | yashirolang では | 出るエラー |
|---|---|---|---|
| 1 | `x = 1` | **`x: int = 1`**（宣言には型注釈が要る） | `未定義の名前 'x' に代入しています` |
| 2 | `7 / 2` → `3.5` | 整数どうしは **`7 // 2`** | `整数の除算に '/' は使えません` |
| 3 | `if xs:` | **`if len(xs) > 0:`** | `if の条件には bool が必要です` |
| 4 | `1 < 2 < 3` | **`1 < 2 and 2 < 3`** | `比較演算子を連鎖させることはできません` |
| 5 | トップレベルに `print(...)` | **`main` の中に書く** | `トップレベルに実行文は書けません` |

```python
def main() -> int:
    print("hello, world")
    return 0
```

`main` の戻り値の**下位 8 ビット**がプロセスの終了コードになります。

---

## 2. 型と値

### 2.1 プリミティブ

| 型 | 例 | 備考 |
|---|---|---|
| `int` | `42`, `-7`, `0xff`, `1_000` | 64 ビット符号付き。**あふれたら実行時エラー**（§2.4） |
| `float` | `1.5`, `3.14`, `1e-3` | IEEE 754 倍精度。`int` とは**暗黙に混ざりません** |
| `bool` | `True`, `False` | 大文字始まり |
| `str` | `"abc"` | **不変**。UTF-8 の**バイト列**（§2.5） |
| `None` | `None` | 値を持たない型。戻り値の無い関数の戻り型 |

### 2.2 まとまった型

| 型 | 例 |
|---|---|
| `list[T]` | `xs: list[int] = [1, 2, 3]`（入れ子も可） |
| `(A, B)` | タプル。複数の値を返すときに使う（§4.3） |
| `T \| None` | `p: Node \| None = None`（§5.4） |
| `class` | 自分で定義する（§5） |
| `rc[T]` | 1 つの値を 2 か所から持ちたいとき（§7.5） |
| `dict.Dict[K, V]` / `set.Set[T]` | 標準ライブラリのクラス（§6） |
| `fn(A) -> B` | 関数型（§4.4） |

`list` はそのまま `print` できます。

```python
print([1, 2, 3])          # → [1, 2, 3]
print(["a", "b"])         # → ["a", "b"]   （str は " で囲まれます）
```

> 注意: 要素が `int` / `float` / `str` / `bool` のときだけです。
> `list[list[int]]` や `list[MyClass]` はコンパイルエラーになるので、`for` でまわして出してください。

`list` のメソッドは Python とほぼ同じです。

```python
xs: list[int] = [1, 2, 3]
xs.append(4)
xs.insert(0, 0)
xs.extend([5, 6])
v: int = xs.pop()         # 末尾を取り出す
i: int = xs.index(3)      # 無ければ -1（例外ではありません）
xs.remove(1)              # 注意: **添字**で消します（Python は値）
xs.reverse()
ys: list[int] = xs.copy()
xs.clear()
```

スライスは Python と同じ規則で、**新しい値を作ります**（借用ではありません）。

```python
s: str = "hello world"
print(s[0:5])             # hello
print(s[6:])              # world
print(s[3:100])           # はみ出しても落ちません（丸められます）
zs: list[int] = [1, 2, 3, 4, 5][1:4]
```

### 2.3 型注釈が要る場所

**宣言・引数・戻り値**の 3 か所だけです。再代入には書きません。

```python
def add(a: int, b: int) -> int:      # 引数と戻り値
    total: int = a + b               # 宣言
    total = total + 1                # 再代入には書かない
    return total
```

グローバル変数も同じで、**トップレベルに書けるのは宣言だけ**です。

```python
counter: int = 1                     # グローバル変数

def bump() -> None:
    counter = counter + 1
```

### 2.4 数 — `int` と `float`

| すること | 書き方 |
|---|---|
| 整数の除算 | `7 // 2` → `3`（**切り下げ**。`-7 // 2` は `-4`） |
| 剰余 | `7 % 2`（符号は Python と同じ） |
| 実数の除算 | `7.0 / 2.0` → `3.5`。注意: **`x / 0.0` は実行時エラー**（無限大は `math.inf()`） |
| 冪 | `2 ** 10`（`int` も `float` も） |
| 変換 | `float(i)` / `int(f)`（0 方向へ切り捨て）。**暗黙変換はありません** |

**整数の桁あふれは実行時エラーです。** `+` `-` `*` `**` と単項 `-`、`int(float)` / `int(str)` を
常に検査します。折り返したいときだけ `wrap_add` / `wrap_sub` / `wrap_mul` を使います。

```python
a: int = 9223372036854775807
b: int = a + 1                # ✗ 実行時エラー: 整数の加算があふれました
c: int = wrap_add(a, 1)       # ✓ 折り返す
```

`str(float)` は**元の値に戻る最短の表記**を出します（誤差を隠しません）。

```python
print(0.1 + 0.2)                     # 0.30000000000000004
print(math.round_to(0.1 + 0.2, 6))   # 0.3   （見せるために丸める）
```

### 2.5 `str` は「バイト列」です

`str` は UTF-8 の**バイト列**で、`len` と `[]` は**バイト単位**です。

```python
s: str = "あいう"
print(len(s))                     # 9（バイト数）
print(strings.char_count(s))      # 3（文字数）
for c in strings.chars(s):        # 1 文字ずつ
    print(c)
print(strings.width(s))           # 6（端末での表示幅）
```

文字列の操作は組み込みメソッドではなく `strings` モジュールです（§6）。
連結は `+`、繰り返しは `"ab" * 3` です。

### 2.6 暗黙の変換はしません

```python
x: float = 1              # ✗ 型が違います（int を float の場所へ）
y: float = 1.0            # ✓
n: int = 3
z: float = float(n) / 2.0 # ✓ 明示的に変換する
s: str = "n=" + str(n)    # ✓ 文字列化も明示的に
```

---

## 3. 制御構造

```python
if x > 0:
    print("正")
elif x == 0:
    print("零")
else:
    print("負")

while i < 10:
    i = i + 1
    if i == 5:
        continue
    if i == 8:
        break

for i in range(0, 10):        # range(stop) / range(start, stop) / range(start, stop, step)
    print(i)

for x in xs:                  # list
    print(x)

for c in "abc":               # str（1 バイトずつ）
    print(c)

for i, x in enumerate(xs):    # 添字つき
    print(str(i) + ": " + str(x))
```

**条件は必ず `bool`** です（`if xs:` や `if n:` は書けません）。

### 3.1 式として書けるもの

```python
s: str = "正" if x > 0 else "負"              # 三項演算子（選ばれない側は評価しません）
ys: list[int] = [v * 2 for v in xs if v > 0]  # 内包表記（if は省略可）
ok: bool = 3 in xs                            # in / not in
print(f"{name} は {n + 1} 個")                # f-string（中には任意の式）
assert n > 0, "n は正のはず"                  # 偽なら panic
```

> 注意: f-string に書式指定（`f"{x:>8.2f}"`）はありません。桁を揃えるときは
> `strings.lpad` / `strings.rpad`、小数の丸めは `math.round_to` を使います。

---

## 4. 関数

```python
def greet(name: str) -> None:        # 戻り値が無いときは -> None
    print("hi, " + name)

def fib(n: int) -> int:              # 再帰も前方参照もできます
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)
```

**すべての経路で `return` する必要があります**（`-> None` を除く）。
`panic` / `exit` で終わる経路は「戻らない」と見なされます。

### 4.1 引数は既定で「借用」

**ここがこの言語の中心です。** 詳しくは §7 で説明しますが、先に形だけ。

```python
def total(xs: list[int]) -> int:          # 借りる（読むだけ）
def fill(xs: mut list[int]) -> None:      # 借りて書き換える
def store(self, s: own str) -> None:      # 所有権をもらう（しまえる）
```

呼び出し側には**何も書きません**（`&` も `mut` も不要です）。

### 4.2 既定値とキーワード引数

引数に既定値を書けます。呼ぶときは `名前 = 値` で、**定義の順に関係なく**
渡せます。

```python
def pad(s: str, n: int = 3, c: str = "-", right: bool = True) -> str:
    out: str = s
    i: int = 0
    while i < n:
        if right:
            out = out + c
        else:
            out = c + out
        i = i + 1
    return out

print(pad("a"))                        # → a---
print(pad("a", 1))                     # → a-
print(pad("a", c = "*"))               # → a***
print(pad("a", right = False, n = 2))  # → --a
```

**既定値に書けるのはリテラルだけです**（数・文字列・`True` / `False` / `None`・
列挙の枝・符号つきの数）。

```python
def f(xs: list[int] = []) -> int:      # ✗ エラー
def f(xs: list[int] | None = None) -> int:   # 欲しいときはこう書いて、中で作ります
```

Python の `def f(xs=[])` は、その list が**呼び出しをまたいで共有**されます
（同じ list が次の呼び出しにも残ります）。リテラルだけにしてあるので、
この形はそもそも書けません。

覚えることは 3 つです。

- 既定値のある引数は**後ろにまとめます**
- 位置で渡す引数は、**名前で渡す引数より前**だけです
- 名前で渡せるのは、**呼び先が決まっているとき**だけです（`print` のような
  組み込み、関数の値、インタフェース越しには渡せません）

### 4.3 複数の値を返す

```python
def divmod2(a: int, b: int) -> (int, int):
    return a // b, a % b

def main() -> int:
    q, r = divmod2(17, 5)        # 分解代入は宣言も兼ねます（型注釈は書きません）
    print(f"{q} {r}")            # → 3 2
    return 0
```

### 4.4 関数を値として渡す

```python
def is_pos(x: int) -> bool:
    return x > 0

def count_if(xs: list[int], pred: fn(int) -> bool) -> int:
    n: int = 0
    for x in xs:
        if pred(x):
            n = n + 1
    return n

print(count_if([1, -2, 3], is_pos))     # → 2
```

`lambda` でその場に書くこともできます。**型は書きません**——引数と戻り値の型は
「置かれた場所」から決まります。

```python
print(count_if([1, -2, 3], lambda x: x > 0))    # → 2

p: fn(int) -> bool = lambda x: x > 0            # 型注釈のある変数でも
```

本体は**式 1 つ**だけです。文が要るなら `def` で書きます。

注意: **外の変数は捕獲できません**（クロージャはまだありません）。

```python
n: int = 5
p: fn(int) -> bool = lambda x: x > n    # ✗ lambda の中から外の変数は使えません
```

lambda はトップレベルの関数に持ち上げるので、外の名前はそこから見えません
（グローバルは使えます）。捕獲を入れるには「関数と、捕まえた値の入れ物」が要り、
この言語は借りたものを構造体に保存できないので、`fn(...)` 型の表現から決め直しに
なります。状態を持たせたいときは、いまはインタフェース（§5.6）を使います。

### 4.5 ジェネリック関数

```python
def first[T](xs: list[T], default: own T) -> T:
    if len(xs) == 0:
        return default
    return copy(xs[0])

print(first([5, 6], 0))         # 型引数は**実引数から**決まります
print(first(["a"], "z"))
```

注意: 型引数は引数の型から推論します。**戻り型にしか現れない型引数は書けません。**

---

## 5. クラス

### 5.1 定義

```python
class Point:
    x: int                      # フィールドは先に宣言する
    y: int

    def init(self, x: int, y: int) -> None:      # コンストラクタは init
        self.x = x
        self.y = y

    def dist2(self) -> int:                      # メソッド
        return self.x * self.x + self.y * self.y

    def move_by(mut self, dx: int) -> None:      # 自分を書き換えるなら mut self
        self.x = self.x + dx

p: Point = Point(3, 4)          # new は要りません
print(p.dist2())
```

- `__init__` ではなく **`init`** です。
- **継承はありません**（共有したい振る舞いはインタフェース、共有したいデータは合成で）。
- **`init` のどの経路でもフィールドを代入する**必要があります（`if` の中だけの代入は止まります）。

### 5.2 参照セマンティクス

クラスの値は**参照**です（Python と同じ）。代入や引数渡しで中身は複製されません。

```python
a: Point = Point(1, 2)
b: Point = a
b.x = 99
print(a.x)            # → 99（同じもの）
```

### 5.3 後片付け（`drop`）

`drop` という名前のメソッドを書くと、その値が捨てられるときに呼ばれます。

```python
class File:
    fd: int
    def drop(self) -> None:
        print("閉じました")
```

**解放はスコープの出口で自動的に行われます**（§7.6）。自分で呼ぶことはありません。

### 5.4 `T | None` と絞り込み

null 参照はありません。「無いかもしれない」は型に書きます。

```python
class Node:
    next: Node | None
    def init(self) -> None:
        self.next = None

def length(head: Node | None) -> int:
    n: int = 0
    cur: Node | None = head
    while cur is not None:
        n = n + 1
        cur = cur.next        # ★ is not None を通ったので Node として使えます
    return n
```

`is not None` で確かめた範囲では、`T | None` は `T` として扱えます（絞り込み）。
確かめずに触ると**コンパイルエラー**です。

### 5.5 ジェネリクス

```python
class Box[T]:
    v: T
    def init(self, v: own T) -> None:
        self.v = v
    def get(self) -> T:
        return self.v

bi: Box[int] = Box(42)
bs: Box[str] = Box("hello")
```

単相化します（`Box[int]` と `Box[str]` は別々のクラスになります）。
注意: **型引数に境界（`T: Comparable` のようなもの）は書けません。** 使えるかは実体化のときに分かります。

### 5.6 インタフェース

```python
interface Show:
    def show(self) -> str

class Point(Show):
    x: int
    y: int
    def init(self, x: int, y: int) -> None:
        self.x = x
        self.y = y
    def show(self) -> str:
        return f"({self.x}, {self.y})"

def print_all(xs: list[Show]) -> None:
    for x in xs:
        print(x.show())
```

別々のクラスを 1 つのリストに入れたいときに使います。
注意: デフォルト実装と関連型はありません。

**失敗しうるメソッドも宣言できます**（`raises`）。

```python
interface Writer:
    def write(mut self, s: str) -> int raises IOError

def write_all(w: mut Writer, parts: list[str]) -> int raises IOError:
    last: int = 0
    for s in parts:
        last = w.write(s)      # どの実装が呼ばれるかは実行時に決まります
    return last
```

投げうるエラーは**宣言で固定**します（実装にも同じ `raises` が要ります）。
呼ぶ側は宣言しか見ないので、そこが揃っていないと `try` を書く手がかりが
無くなるためです。

### 5.7 自分の型を `for` で回す

3 つのメソッドを書くと、`for` で回せます。

```python
class Bag[T]:
    xs: list[T]

    def init(self, xs: own list[T]) -> None:
        self.xs = xs

    def __first__(self) -> int:              # 最初のカーソル（無ければ -1）
        if len(self.xs) == 0:
            return -1
        return 0

    def __next__(self, cur: int) -> int:     # 次のカーソル（無ければ -1）
        if cur + 1 >= len(self.xs):
            return -1
        return cur + 1

    def __get__(self, cur: int) -> T:        # そのカーソルの要素
        return self.xs[cur]


b: Bag[str] = Bag(["あ", "い", "う"])
for s in b:
    print(s)
```

**カーソルは `int` です。** Python のようなイテレータ物体は作りません——この言語は
借りたものを構造体に保存できないので（§7）、容器を指すイテレータを作ると容器を
`rc[T]` にするか、寿命を書かせることになります。カーソルなら容器は借りたままです。

穴のある容器（ハッシュ表など）は `__next__` で空きを飛ばします。`mut self` で書けば、
行を読んでいく・受信するといった**流れてくるもの**も同じ形で書けます。

`dict` と `set` はこの規約を持っています。

```python
for k in d:                 # 鍵が順に出ます
    print(k + " -> " + str(d.get(k)))
```

### 5.8 演算子の多重定義

```python
class Vec2:
    x: float
    y: float
    def init(self, x: float, y: float) -> None:
        self.x = x
        self.y = y
    def __add__(self, o: Vec2) -> Vec2:
        return Vec2(self.x + o.x, self.y + o.y)
    def __eq__(self, o: Vec2) -> bool:
        return self.x == o.x and self.y == o.y
```

| 演算子 | メソッド |
|---|---|
| `+` `-` `*` `/` `//` `%` `**` | `__add__` `__sub__` `__mul__` `__truediv__` `__floordiv__` `__mod__` `__pow__` |
| 単項 `-` | `__neg__` |
| `==` `!=` | `__eq__` `__ne__`（`__ne__` が無ければ `not (a == b)`） |
| `<` `<=` `>` `>=` | `__lt__` `__le__` `__gt__` `__ge__` |
| `m[i]` / `m[i, j]` | `__getitem__` / `__setitem__` |
| `x in c` | `__contains__`（注意: これだけ**右辺**の型で決まります） |

注意: **反転（`__radd__`）はありません。** どのメソッドが呼ばれるか読んで分かるように、
左辺の型だけで決めます（`2.0 * m` ではなく `m * 2.0` と書きます）。

---

### 5.9 列挙と場合分け

「決まった選択肢のどれか」は `enum` で書きます。

```python
enum Color:
    Red
    Green
    Blue
```

`match` で場合分けします。

```python
def name_of(c: Color) -> str:
    match c:
        case Color.Red:
            return "あか"
        case Color.Green:
            return "みどり"
        case Color.Blue:
            return "あお"
    return "?"
```

**★ 枝を書き忘れるとコンパイルエラーになります。**

```
error: match に書いていない枝があります: Color.Blue
```

これがこの機能の値打ちです。あとから `Color` に枝を足したとき、**直す場所を
コンパイラが全部挙げてくれます**。定数の束と `if` の連なりで書いていると、
直し忘れた場所は黙って動き続けます。

`int` と `str` でも使えます。こちらは値が無限にあるので `case _` が要ります。

```python
match b:
    case 48:
        return "ゼロ"
    case _:
        return "その他"
```

**注意: 列挙は `int` と混ざりません。** `c + 1` も `c == 0` もエラーです。
番号として使いたくなったら、それは `enum` ではなく `int` の仕事です。

#### 枝は中身を持てます

```python
enum Shape:
    Circle(r: float)
    Rect(w: float, h: float)
    Empty                      # 中身なしの枝も混ぜられます


def area(s: Shape) -> float:
    match s:
        case Shape.Circle(r):          # r が束縛されます
            return 3.14 * r * r
        case Shape.Rect(w, h):
            return w * h
        case Shape.Empty:
            return 0.0
    return -1.0


a: Shape = Shape.Circle(2.0)
```

自分自身を中身に持てるので、構文木も書けます。

```python
enum Tree:
    Leaf(v: int)
    Pair(l: own Tree, r: own Tree)      # 中身は所有で受け取ります
```

**中身を持つ枝がある列挙の値は、クラスと同じ扱い**です（ヒープに置かれ、
出口で解放されます）。所有・借用・`own` の規則は §7 のままで、覚えることは
増えません。網羅の検査もそのままです。

注意: `case` の括弧に書けるのは**束縛する名前だけ**です。`case Shape.Circle(1.0)`
のように値で絞ることはできません（本体で `if` を使ってください）。

## 6. モジュールと標準ライブラリ

1 ファイル = 1 モジュールです。`import` はファイル名（拡張子なし）を指します。

```python
# geometry.ys
def area(w: int, h: int) -> int:
    return w * h
```

```python
# main.ys
import geometry

def main() -> int:
    print(geometry.area(3, 4))      # ★ 名前は必ず修飾して書きます
    return 0
```

- **`from x import y` も `as` も相対 import もありません。**
- パッケージの中は `import パッケージ.モジュール` です（`ysm` が `deps/` に置きます）。

### 6.1 よく使うモジュール

| モジュール | 中身 |
|---|---|
| `strings` | `split` / `join` / `find` / `strip` / `replace` / `substr` / `chars` / `lpad` / `width` … |
| `io` | `read_file` / `write_file` / `exists` / `listdir` / `mkdir_all` / `read_line` / `eprint` … |
| `sys` | `argv` / `getenv` / `run` / `capture` / `cpu_count` |
| `dict` | `Dict[K, V]` — ハッシュ表（`set` / `get` / `get_or` / `has` / `keys` / `remove`） |
| `set` | `Set[T]` — 集合（`add` / `has` / `items` / `union` / `intersection`） |
| `lists` | `index_of` / `contains` / `sorted_indices` / `max_of` / `count_if` … |
| `json` | 読み書き |
| `time` | 時刻と計測 |
| `math` / `linalg` / `stats` / `random` / `numeric` / `complex` / `fft` / `physics` | 数値計算（→ [reference/numerics.md](reference/numerics.md)） |
| `plot` / `frame` | 作図（SVG）と表形式データ（CSV・集計） |
| `net` / `tls` / `http` | ソケット・TLS・HTTP（→ [reference/net.md](reference/net.md)） |
| `decimal` / `bytes` | 十進固定小数点・固定幅のバイト並び |

**すべて yashirolang で書かれています**（`lib/*.ys`）。一覧は [reference/stdlib.md](reference/stdlib.md) にあります。

```python
import strings
import dict

def main() -> int:
    counts: dict.Dict[str, int] = dict.Dict()
    for word in strings.split("a b a", " "):
        counts.set(copy(word), counts.get_or(word, 0) + 1)
    for k in counts.keys():
        print(f"{k}: {counts.get(k)}")
    return 0
```

---

## 7. 所有権と借用 — この言語の中心

GC はありません。代わりに、**その値を誰が持っているか**をコンパイラが追い、
スコープの出口で自動的に解放します。書く語は **`own` と `mut` の 2 つだけ**です。

### 7.1 引数は既定で「借用」

```python
def total(xs: list[int]) -> int:       # xs は借りもの
    s: int = 0
    for x in xs:
        s = s + x
    return s

def main() -> int:
    xs: list[int] = [1, 2, 3]
    print(total(xs))                   # 呼び出し側には何も書かない
    print(len(xs))                     # ★ まだ使える（渡しても失われない）
    return 0
```

**借りものは、呼び出しより長生きできません。** この 1 つの規則があるので、
Rust のライフタイム注釈（`'a`）は要りません。

### 7.2 `own` — 所有権を受け取る

受け取った値を**しまう**（フィールドやリストに入れる）・**返す**なら `own` を書きます。

```python
class Node:
    name: str
    def init(self, name: own str) -> None:     # own を書いたので、しまえる
        self.name = name

def main() -> int:
    s: str = "abc"
    n: Node = Node(s)        # ★ ここで s の所有権が移ります
    # print(s)               # ✗ error[E-MOVE-1]: 移動済みの値 's' を使っています
    return len(n.name)
```

`own` を書かずにしまおうとすると、コンパイルが止まります。

```
error[E-BORROW-3]: 借用した値 'name' をフィールドに保存できません
  → 引数を 'name: own str' にすると、所有権を受け取れます
```

**借りたものをどうしても取っておきたい**ときは `copy(x)` で複製します。

### 7.3 `mut` — 借りたものを書き換える

```python
def fill(xs: mut list[int], n: int) -> None:
    xs.append(n)

xs: list[int] = [1]
fill(xs, 2)                  # 呼び出し側には書かない
```

`mut` の無い借用に書き込むとコンパイルエラーです。
どの実引数が書き換えられるかは `yashirolang --explain-mut app.ys` で一覧できます。

### 7.4 覚えることは 3 行です

| 書くもの | 意味 | いつ書くか |
|---|---|---|
| （何も書かない） | 借用。読める。呼び出しの間だけ生きる | ほとんどの引数 |
| `own T` | 所有権をもらう。しまえる・返せる | 値を**取っておく**とき |
| `mut T` | 借りたまま書き換える | 引数の中身を変えるとき |

注意: **`self` も同じです。** 自分のフィールドを書き換えるメソッドは `def f(mut self, …)` と書きます。

### 7.5 `rc[T]` — 1 つの値を 2 か所から持つ

「しまって、なおかつ返す」は所有権だけでは書けません。そこだけ参照カウントを使います。

```python
class Holder:
    node: rc[Node] | None
    def init(self) -> None:
        self.node = None

def store_and_return(h: mut Holder) -> rc[Node]:
    t: rc[Node] = rc(Node(7))
    h.node = t                 # しまって、
    return t                   # なおかつ返せる
```

注意: 循環参照は解放されません（弱参照はまだありません）。**必要なところだけ**に使ってください。

### 7.6 解放はいつ起きるか

- スコープを抜けるとき、そこで**所有していた**値が解放されます（`drop` があれば呼ばれます）。
- 移動した値・借りものは解放されません（持ち主が解放します）。
- 分岐で片方だけ移動した場合も、実行時の印（drop flag）で正しく処理されます。

**確かめ方:**

```bash
yashirolang --check app.ys        # 型と安全性の検査だけ
yashirolang app.ys -o app         # 既定でエラー。通ればその保証があります
```

> 注意: 逃げ道として `--warn-own`（指摘を警告に落とす）がありますが、
> **付けたコードはこの保証の外です。** 新しく書くコードでは使わないでください。

---

## 8. エラー処理

例外はありますが、**アンワインドしません**。`raises` を書いた関数は、
内部的には「値かエラーか」を返し、呼び出し側で必ず確かめます。
（だからカーネルの中でも使えます。）

```python
class IOError:
    message: str
    def init(self, message: own str) -> None:
        self.message = message

def read(path: str) -> int raises IOError:       # 失敗を型で宣言する
    if len(path) == 0:
        raise IOError("空のパス")
    return len(path)

def main() -> int:
    try:
        print("ok:" + str(read("abcde")))
    except IOError as e:
        print("caught: " + e.message)
    return 0
```

- **握りつぶせません。** `raises` のある関数を呼ぶ側は、`try` で受けるか、
  自分にも `raises` を書いて呼び出し元へ渡すかのどちらかです。
- エラー型は**ただのクラス**です（特別な基底クラスはありません）。
- `finally` はありません。後片付けは `drop` が行います。

**`panic` との使い分け** — 呼び出し側が対処できるものは `raises`、
プログラムの間違い（範囲外・不変条件の破れなど、直すべきバグ）は `panic` です。

---

## 9. 契約と範囲型

Ada から採った 2 つです。どちらも**書くのは宣言の 1 行だけ**で、外すオプションはありません。

### 9.1 範囲型（部分型）

```python
type Percent = int range(0, 100)

def half(p: Percent) -> int:
    return p // 2

def main() -> int:
    a: Percent = 50           # ✓
    # b: Percent = 200        # ✗ コンパイルエラー（定数なので実行前に分かります）
    c: int = a * 2            # 途中の計算は基底型（int）で行います
    return half(a)
```

`Percent` の場所に値を入れるたびに範囲を確かめます（変数から入れる場合は実行時）。

### 9.2 事前条件・事後条件

```python
def divide(a: int, b: int) -> int:
    requires b != 0            # 入口で確かめる
    ensures result >= 0        # return のたびに確かめる
    return a // b
```

`result` は `ensures` の中でだけ使える名前で、**戻り値そのもの**です。

注意: どちらの検査も、コンパイラが「必ず成り立つ」と示せたときは出力から消えます（§11）。

---

## 10. 並行実行

新しい構文はほとんどありません。`spawn` は**呼び出しの形のまま**です。

```python
def work(n: int) -> int:
    ...

def main() -> int:
    t: Thread[int] = spawn(work, 1000)
    u: Thread[int] = spawn(work, 2000)
    print(t.join() + u.join())
    return 0
```

**スレッドに渡せるもの**は次の 3 つだけで、それ以外はコンパイルエラー（`E-SEND-1`〜`4`）です。

| 渡せる | 例 |
|---|---|
| `own` した値 | `spawn(shout, "スレッドから")` |
| `mutex[T]` | 共有して書き換えるとき |
| `scope:` の中の借り | 下記 |

```python
scope:                                     # 出口で必ず全部 join されます
    t: Thread[int] = spawn(peek, j)        # ★ だから借りを渡せます
    u: Thread[int] = spawn(add3, xs, ys, 2)
    r = t.join()
    s = u.join()
```

書き換えを共有するときは `mutex[T]` です。**生の lock / unlock はありません**
（`lock(f)` の形しか無いので、解き忘れが起きません）。

```python
def bump(c: mut Counter) -> int:
    c.n = c.n + 1
    return c.n

m: mutex[Counter] = mutex(Counter(0))
print(m.lock(bump))
```

注意: **`Send` / `Sync` にあたるトレイトはありません。** 渡せるかどうかは
「借りか、所有か、`rc` か」で決まるので、**利用者が書く注釈は 1 つも増えません**。
注意: **`async` / `await` はありません**（理由は [design/concurrency.md](design/concurrency.md) §6）。

---

## 11. コンパイラの使い方

```bash
yashirolang [オプション] <入力.ys>
```

| オプション | 説明 |
|---|---|
| `-o <file>` | 出力する実行ファイル名（既定 `a.out`） |
| `--check` | 検査だけして、実行ファイルを作らない |
| `-O0`〜`-O3` | 最適化レベル（既定 `-O0`） |
| `-g` | デバッグ情報を出す（lldb / gdb / perf が行と変数を出せます） |
| `-I <dir>` | `import` を探す場所を足す |
| `--explain-mut` | 呼び出しで書き換えられる実引数を一覧表示 |
| `--warn-own` | 所有権の指摘を警告に落とす（注意: 保証の外に出ます） |
| `--prove-report` | 静的に消せた実行時検査の数を出す |
| `--version` / `--help` | 版番号 / 使い方 |

全オプションは [reference/cli.md](reference/cli.md) にあります。

### 検査はいつ消えるのか

範囲外・桁あふれ・契約違反は**実行時に確かめます**。ただしコンパイラが
「必ず成り立つ」と示せたときは、その検査を出しません（区間解析。**書くものは増えません**）。

```python
def total(xs: list[int]) -> int:
    s: int = 0
    i: int = 0
    while i < len(xs):
        s = s + xs[i]      # ★ 添字の検査は出ません（0 以上・長さ未満と示せる）
        i = i + 1
    return s
```

何が消えたかは `--prove-report` で見られます。示せなかったところは今までどおり実行時に確かめるので、
**安全性は変わりません**。

### デバッグ

```bash
yashirolang -g app.ys -o app
lldb ./app
```

```
(lldb) frame variable
(int) total = 9
(str) msg = "point"
(lldb) p *xs          ← list は data / len / cap が見えます
(lldb) p *p           ← クラスはフィールドの名前で見えます
```

注意: 最適化を掛けると見えない変数が出ます。デバッグは `-O0`（既定）で。

---

## 12. まとめの例

ここまでの要素だけで書ける語数カウントです（`examples/wordcount.ys`）。

```python
import io
import strings
import sys
import dict

def main() -> int:
    args: list[str] = sys.argv()
    if len(args) < 2:
        print("使い方: wordcount <ファイル>")
        return 1

    text: str = io.read_file(args[1])
    counts: dict.Dict[str, int] = dict.Dict()

    for line in strings.split(text, "\n"):
        for word in strings.split(strings.strip(line), " "):
            if len(word) > 0:
                # ★ dict は鍵を所有するので、借りている word を複製して渡す
                counts.set(copy(word), counts.get_or(word, 0) + 1)

    for k in counts.keys():
        print(k + ": " + str(counts.get(k)))
    return 0
```

```bash
$ yashirolang examples/wordcount.ys -o wc
$ ./wc examples/sample.txt
```

---

## 13. いま無いもの

| 無いもの | 代わりに |
|---|---|
| 継承 | 合成（フィールドに持つ）＋ インタフェース（§5.6） |
| クロージャ・`lambda` | トップレベルの関数を渡す（§4.4）／状態が要るならインタフェース |
| `enum` / `match` | グローバル定数と `if` / `elif` |
| 既定引数・キーワード引数 | 引数を全部書く |
| f-string の書式指定（`{x:>8.2f}`） | `strings.lpad` / `strings.rpad` / `math.round_to` |
| 型引数の境界（`T: Ord`） | そのまま書いて、実体化のときにエラーで気づく |
| 正規表現・日付の解析 | `strings` で書く |
| `async` / `await` | `spawn` / `join` / `scope:`（§10） |
| アンワインドする例外 | `raises` / `try` / `except`（§8。**設計上入れません**） |
| 弱参照（`rc` の循環） | 循環しない設計にする |

現在地とこれからは [roadmap.md](roadmap.md) にあります。

---

## 14. 次に読むもの

| 目的 | ドキュメント |
|---|---|
| 言語の正確な定義 | [spec/language-spec.md](spec/language-spec.md) |
| 所有権・エラー処理の正確な規則 | [spec/safety-spec.md](spec/safety-spec.md) |
| 標準ライブラリの一覧 | [reference/stdlib.md](reference/stdlib.md) |
| 数値計算（numpy / pandas との対応） | [reference/numerics.md](reference/numerics.md) |
| ソケット・TLS・HTTP | [reference/net.md](reference/net.md) |
| パッケージの作り方・使い方 | [reference/pkg.md](reference/pkg.md) |
| コンパイラの作り | [design/](design/) |
