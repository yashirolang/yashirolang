# 本言語入門

**本言語を書く人のための手引きです。** コンパイラの作り方（このリポジトリの本編）ではなく、
**言語そのものの使い方**を扱います。

Python を書いたことがあれば、最初の 30 分で読み切れる分量にしてあります。
Python との違いに絞って説明するので、同じところは説明しません。

> **一行でいうと** — Python の見た目のまま、型が静的に決まり、機械語にコンパイルされる言語です。
> さらに、Rust の所有権に近い仕組みで「解放し忘れ・二重解放」を検査します。

| | |
|---|---|
| 拡張子 | `{{ext}}` |
| コンパイラ | `{{cc}}` |
| 入口 | `def main() -> int:` |
| 実行時 | GC なし・例外なし・リフレクションなし |

---

## 1. 5 分で動かす

インストールは [README](../README.md#使ってみるlinux--macos--windows) を見てください。**clang があれば動きます。**

```python
# hello{{ext}}
def main() -> int:
    print("hello, world")
    return 0
```

```bash
$ {{cc}} hello{{ext}} -o hello
$ ./hello
hello, world
```

**`main` は必須です。** Python のようにトップレベルへ処理を書くことはできません。
`main` の戻り値の**下位 8 ビット**がプロセスの終了コードになります。

```bash
$ {{cc}} --version
{{cc}} 0.4.0 (stage0)
target: x86_64-apple-macosx26.0.0
```

---

## 2. まず知っておく 6 つの違い

Python のつもりで書くと最初に必ず引っかかる場所です。
**どれもコンパイル時に止まります**（実行してから気づくことはありません）。

| # | Python では | 本言語では | 出るエラー |
|---|---|---|---|
| 1 | `x = 1` | **`x: int = 1`**（宣言に型注釈が要る） | `未定義の名前 'x' に代入しています` |
| 2 | `7 / 2` → `3.5` | **`7 // 2`** を使う | `整数の除算に '/' は使えません` |
| 3 | `if xs:` | **`if len(xs) > 0:`** | `if の条件には bool が必要です` |
| 4 | `1 < 2 < 3` | **`1 < 2 and 2 < 3`** | `比較演算子を連鎖させることはできません` |
| 5 | トップレベルに `print(...)` | **`main` の中に書く** | `トップレベルに実行文は書けません` |

そのほか **無いもの**：継承・例外（後述の `raises` が代わり）・クロージャ・`lambda`・
集合・内包表記・変数のシャドーイング。
（タプルは 0.5.0 で入りましたが、**複数戻り値と分解代入だけ**です。）

---

## 3. 型と値

### 3.1 プリミティブ

| 型 | 例 | 備考 |
|---|---|---|
| `int` | `42`, `-7` | 64 ビット符号付き。あふれたら 2 の補数で回り込む |
| `float` | `1.5`, `3.14`, `1e-3` | IEEE 754 倍精度。`int` とは**暗黙に混ざりません** |
| `bool` | `True`, `False` | 大文字始まり（Python と同じ） |
| `str` | `"abc"` | **不変**。UTF-8 の**バイト列**（§3.6） |
| `None` | `None` | 値を持たない型。戻り値の無い関数の戻り型 |

### 3.2 まとまった型

| 型 | 例 |
|---|---|
| `list[T]` | `xs: list[int] = [1, 2, 3]` |
| `dict.Dict` | 標準ライブラリのクラス（§7） |
| `class` | 自分で定義する（§6） |
| `T \| None` | `p: Node \| None = None`（§6.3） |
| `rc[T]` | 共有したいときの逃げ道（§9.4） |
| `(A, B)` | タプル。複数の値を返すときに使う（§5.2） |

`list` は**入れ子にできます**。

```python
def main() -> int:
    grid: list[list[int]] = [[1, 2], [3]]
    print(len(grid[0]))       # → 2
    return 0
```

`list` は**そのまま `print` できます**。

```python
print([1, 2, 3])          # → [1, 2, 3]
print(["a", "b"])         # → ["a", "b"]   （str は " で囲まれます）
```

> **⚠️ 要素が `int` / `float` / `str` / `bool` のときだけです。**
> `list[list[int]]` や `list[MyClass]` はコンパイルエラーになるので、
> `for` でまわして出してください。

### 3.3 型注釈が要る場所

**宣言・引数・戻り値**の 3 か所です。それ以外（再代入など）には書きません。

```python
def add(a: int, b: int) -> int:      # 引数と戻り値
    total: int = a + b               # 宣言
    total = total + 0                # 再代入には書かない
    return total
```

### 3.4 数値 — `int` と `float`

`int` は 64 ビット整数、`float` は IEEE 754 の倍精度です。**この 2 つは自動で混ざりません。**

```python
def main() -> int:
    a: float = 1.5
    print(a * 2.0)          # → 3.0
    print(a / 4.0)          # → 0.375
    print(a + 1)            # × 型 'float' と 'int' に演算子 '+' は適用できません
    print(a + float(1))     # ○ 明示的に変換する
    print(int(2.9))         # → 2（0 方向へ切り捨て。round ではない）
    return 0
```

演算子は **`int` と `float` でちょうど裏返し**です。

| | `+ - *` | `/` | `// %` | `**` | ビット演算 |
|---|---|---|---|---|---|
| `int` | ✅ | ✗ | ✅ | ✅ | ✅ |
| `float` | ✅ | ✅ | ✗ | ✅ | ✗ |

```python
print(2 ** 10)        # → 1024      （int。繰り返し二乗法）
print(2.0 ** 0.5)     # → 1.414214  （float。平方根に使える）
print(2 ** -1)        # × 実行時エラー（int では表せない）
print(2.0 ** -1.0)    # → 0.5
```

> **`// %` は負の数でも Python と同じです**（0.5.1 から）。`-7 // 2` は `-4`、
> `-7 % 2` は `1` になります。`//` は負の無限大の方向へ丸め、`%` の符号は
> 除数と同じです。⚠️ **C の `/` `%` とは違う**ので、C から移すときは注意してください。

**表示は最大 6 桁**で、末尾の 0 は落ちますが最低 1 桁は残ります。

```python
print(1.0)          # 1.0        （1 ではない）
print(0.1 + 0.2)    # 0.3        （6 桁で丸めるので誤差が見えない）
print(1.0e-9)       # 1.0e-9     （とても大きい/小さい値は指数表記）
```

### 3.5 文字列は「バイト列」です

**ここは必ず読んでください。** Python と最も違うところです。

```python
s: str = "あいう"
print(len(s))          # 9   ← 文字数ではなく **バイト数**
print(s[0])            # 壊れた 1 バイト（「あ」の 1 バイト目）
for c in s:            # 9 回まわる（3 回ではない）
    ...
```

`str` は UTF-8 の**バイト列**です。添字・スライス・`find` はすべて
**バイト位置**で、多バイト文字の途中で切ると壊れます。

文字として扱いたいときは `strings` を使ってください。

| したいこと | 使うもの |
|---|---|
| 文字数を数える | `strings.char_count(s)` |
| 表示幅を数える | `strings.width(s)`（全角を 2 と数える） |
| 1 文字ずつまわす | `for c in strings.chars(s):` |
| 桁を揃える | `strings.lpad` / `rpad`（**表示幅**で数えます） |

```python
import strings

s: str = "あいう"
print(len(s))                    # 9
print(strings.char_count(s))     # 3
print(strings.width(s))          # 6
for c in strings.chars(s):       # 3 回
    print(c)
```

> **🤔 なぜバイト列なのか**
> C の関数（`strlen` / `fopen`）にそのまま渡せるからです。文字単位の型に
> すると、境界のたびに変換が要ります。**必要なときだけ `strings` で
> 文字として見る**、という分け方にしています。

### 3.6 暗黙の変換はしない

`int` と `str` は自動で混ざりません。**明示的に変換します。**

```python
n: int = 5
print("答え: " + str(n))     # ○
print("答え: " + n)          # × 型 'str' と 'int' に演算子 '+' は適用できません
```

変換に使う組み込み関数はこれだけです。

| 関数 | 説明 |
|---|---|
| `str(int)` / `str(bool)` / `str(float)` / `str(str)` | 文字列にする（`str` は複製） |
| `int(str)` / `int(float)` | 整数にする（float からは 0 方向へ切り捨て） |
| `float(int)` | 浮動小数点数にする |
| `ord(str)` | 先頭 1 バイトのコード |
| `chr(int)` | 1 バイトの文字列を作る |
| `len(str)` / `len(list[T])` | 長さ |
| `print(...)` | `int` / `float` / `str` / `bool` を受け取る |
| `panic(str)` | メッセージを stderr に出して異常終了 |
| `exit(int)` | 終了コードを指定して終了 |
| `abs(int)` / `abs(float)` | 絶対値 |
| `min(a, b)` / `max(a, b)` | 2 つのうち小さい/大きいほう（`int` と `float`） |
| `sum(list[int])` | 総和（`list[float]` は `linalg.vsum`） |
| `hash(x)` | ハッシュ値（`int` / `float` / `str` / `bool`）。`dict` が使っています |
| `copy(str)` | 文字列を複製する（§9.3） |

---

## 4. 制御構造

Python と同じ見た目です。**条件は必ず `bool`** である点だけ違います。

```python
def classify(n: int) -> str:
    if n < 0:
        return "負"
    elif n == 0:
        return "ゼロ"
    else:
        return "正"

def main() -> int:
    for i in range(0, 6, 2):          # 0, 2, 4
        print(str(i) + ": " + classify(i - 2))

    xs: list[str] = ["a", "b"]
    for s in xs:                      # リストを直接まわす
        print(s)

    for i, x in enumerate(xs):        # 添字も一緒に受け取る
        print(f"{i}: {x}")

    i: int = 0
    while True:
        i = i + 1
        if i < 3:
            continue
        break                         # break / continue は使える
    print(i)                          # → 3
    return 0
```

`range` は `range(n)` / `range(a, b)` / `range(a, b, step)` の 3 通りです。
インデントは**スペースのみ**（タブはエラー）。

> **⚠️ ループ変数を 2 つ書けるのは `enumerate` のときだけ**です。
> タプルが無いので、一般の分解代入（`for a, b in pairs:`）はできません。

### 4.1 f-string・三項演算子・`in`・スライス・`assert`

Python と同じように書けます。

```python
def main() -> int:
    name: str = "world"
    n: int = 42
    xs: list[int] = [1, 2, 3]

    print(f"{name} は {n} 文字目 {len(name)}")   # f-string（式も書ける）
    print("正" if n > 0 else "負")               # 三項演算子
    print(2 in xs)                                # in / not in
    print(name[0:4])                              # スライス
    print(len(xs[1:]))
    assert n > 0, "n は正のはず"                  # assert
    return 0
```

| | 備考 |
|---|---|
| f-string | `{{` `}}` で波括弧そのもの。**書式指定（`{x:>8}`）はありません** |
| 三項演算子 | 右結合。選ばれなかった側は**評価されません** |
| `in` | `list[T]` と `str` に使えます。`dict` は `d.has(k)` |
| スライス | **新しい値を作ります**。範囲外でも落ちません（Python と同じ規則で丸めます） |
| 負の添字 | `xs[-1]` は末尾。**範囲外なら落ちます**（`xs[-100]` は実行時エラー） |
| 連結・繰り返し | `"ab" * 3` / `[1] + [2]` / `[0] * 4`。⚠️ `3 * "ab"` は書けません |
| `assert` | 破れたら `panic`。**Python の `-O` のような無効化はありません** |

### 4.2 `list` のメソッド

```python
xs: list[int] = [1, 2, 3]
xs.append(4)
xs.insert(0, 0)      # 位置を指定して差し込む
print(xs.pop())      # 末尾を取り出す
print(xs.remove(0))  # 位置で取り除いて、その値を返す
print(xs.index(2))   # 見つからなければ -1
xs.reverse()
ys: list[int] = xs.copy()
xs.extend(ys)
xs.clear()
```

---

## 5. 関数

```python
def greet(name: str) -> str:
    return "hello, " + name

def show(n: int) -> None:        # 値を返さないなら -> None
    print(str(n))
```

**オーバーロードはできません**（`print` だけコンパイラが特別扱いしています）。
デフォルト引数・キーワード引数・可変長引数もありません。

#### 複数の値を返す

戻り型に `(A, B)` と書くと、カンマで並べて返せます。受け取るほうは
**そのまま分解できます**。

```python
def divmod2(a: int, b: int) -> (int, int):
    return a // b, a % b

def main() -> int:
    q, r = divmod2(17, 5)
    print(f"{q} 余り {r}")       # → 3 余り 2
    return 0
```

`q, r = ...` は**宣言も兼ねています**。型は右辺から決まるので書きません。

> **⚠️ 1 要素のタプルは書けません。** `(int)` は「括弧で囲んだ `int`」と
> 区別がつかないためで、Python の `(1,)` にあたる記法はありません。
> また `a, b = b, a`（入れ替え）もまだ書けません。

### 5.1 関数を値として渡す

型は `fn(引数の型, ...) -> 戻り型` です。**関数の名前をそのまま値として書けます。**

```python
import math

def integrate(f: fn(float) -> float, a: float, b: float, n: int) -> float:
    h: float = (b - a) / float(n)
    s: float = 0.0
    i: int = 0
    while i < n:
        s = s + f(a + (float(i) + 0.5) * h)
        i = i + 1
    return s * h

def sq(x: float) -> float:
    return x * x

def main() -> int:
    print(integrate(sq, 0.0, 1.0, 1000))         # 自分の関数
    print(integrate(math.sin, 0.0, 3.0, 1000))   # 他モジュールの関数も渡せる
    g: fn(float) -> float = math.cos             # 変数に入れられる
    print(g(0.0))
    return 0
```

**⚠️ クロージャはまだありません。** 渡せるのは「トップレベルの関数」だけで、
外側の変数を捕まえることはできません。`raises` する関数も渡せません
（関数型がエラーの受け渡しを表さないため）。

---

## 6. クラス

### 6.1 定義

`__init__` ではなく **`init`** です。ダンダーは導入していません。

```python
class Point:
    x: int                      # フィールドは先に宣言する
    y: int

    def init(self, x: int, y: int) -> None:
        self.x = x
        self.y = y

    def norm2(self) -> int:
        return self.x * self.x + self.y * self.y

def main() -> int:
    p: Point = Point(3, 4)      # Point(...) で生成（new は無い）
    print(p.norm2())            # → 25
    return 0
```

**継承はありません。** フィールドは宣言したものだけで、後から生やせません（静的レイアウトのため）。

### 6.2 参照セマンティクス

`class` と `list` は**参照**です。代入しても複製されません。

```python
a: Point = Point(1, 2)
b: Point = a
b.x = 99
print(a.x)                      # → 99（同じものを指している）
```

### 6.3 `T | None` と絞り込み

「無いかもしれない」は `T | None` で表します。**中身を触る前に必ず確認が要ります。**

```python
class Node:
    n: int
    def init(self, n: int) -> None:
        self.n = n

def find(ok: bool) -> Node | None:
    if ok:
        return Node(7)
    return None

def main() -> int:
    p: Node | None = find(True)
    print(p.n)                  # × 'Node | None' のまま触ることはできません
    if p is not None:
        print(p.n)              # ○ ここでは Node に絞り込まれている
    return 0
```

これが **None 安全**です。`if p is not None:` の中では、コンパイラが `p` を `Node` として扱います。

### 6.4 ジェネリクス

クラスは**型引数**を取れます。

```python
class Box[T]:
    v: T

    def init(self, v: own T) -> None:
        self.v = v

    def get(self) -> T:
        return self.v

class Pair[A, B]:
    first: A
    second: B
    def init(self, a: own A, b: own B) -> None:
        self.first = a
        self.second = b

def main() -> int:
    bi: Box[int] = Box(42)
    bs: Box[str] = Box("hello")
    p: Pair[str, int] = Pair("age", 30)
    print(f"{bi.get()} {bs.get()} {p.first}={p.second}")
    return 0
```

**`Box[int]` と `Box[str]` は別々のクラスになります**（単相化）。
生成される IR を見ると、それぞれ別の構造体が出ています。

```llvm
%Box$int.type = type { i64 }
%Box$str.type = type { ptr }
```

> **⚠️ 型引数は「代入される先の型」から決まります。**
> `Box(1)` だけでは何の `Box` か決められないので、必ず変数の型を書いてください。
>
> ```python
> b: Box[int] = Box(1)        # ○
> print(f(Box(1)))            # × 'Box' のどの実体を作るのか決められません
> ```

> **⚠️ 型制約はありません。** `T` に「比較できること」などを要求できないので、
> `==` が使えるかどうかは実体化してみるまで分かりません。

**関数も型引数を取れます。**

```python
def first[T](xs: list[T], default: own T) -> T:
    if len(xs) == 0:
        return default
    return xs[0]

first([5, 6], 0)          # 5
first(["a", "b"], "z")    # "a"
```

> **⚠️ クラスとは決め方が違います。**
> クラスは「代入される先の型」から、**関数は「実引数の型」から**決めます
> （`f(xs)` の左辺には戻り値の型しかないためです）。
> 戻り型にしか現れない型引数は書けません。

### 6.5 インタフェース

**共通の振る舞い**をまとめます。継承ではありません。

```python
interface Show:
    def show(self) -> str

interface Sized:
    def size(self) -> int

class Point(Show, Sized):        # 2 つ実装できる
    x: int
    y: int
    def init(self, x: int, y: int) -> None:
        self.x = x
        self.y = y
    def show(self) -> str:
        return f"({self.x}, {self.y})"
    def size(self) -> int:
        return 2

class Label(Show):
    text: str
    def init(self, text: own str) -> None:
        self.text = text
    def show(self) -> str:
        return "[" + self.text + "]"

def print_all(xs: list[Show]) -> None:
    for x in xs:
        print(x.show())            # 実行時にどちらの実装かが決まる

def main() -> int:
    xs: list[Show] = []
    xs.append(Point(1, 2))
    xs.append(Label("hi"))
    print_all(xs)                  # (1, 2) / [hi]
    return 0
```

| | |
|---|---|
| 中身 | **メソッドの宣言だけ**。フィールドは持てません（持てると継承になります） |
| 本体 | 書けません（`def show(self) -> str` で改行） |
| 実装 | `class C(I1, I2):`。**宣言どおりのメソッドが無いとコンパイルエラー** |
| 代入 | クラス → インタフェースは自動。**値の変換は起きません**（同じポインタ） |
| 呼べるもの | インタフェースに**宣言したものだけ**。実装側の他のメソッドは呼べません |

> **⚠️ 継承はありません。** `class D(C):` と書いても、`C` がクラスなら
> 「インタフェースが見つかりません」になります。振る舞いの共有はインタフェース、
> 実装の共有は合成（フィールドに持つ）で行います。
>
> **⚠️ `raises` するメソッドは宣言できません。** インタフェース越しの呼び出しは
> エラーの受け渡しを表せないためです。

---

### 6.5.2 内包表記

```python
ys: list[int] = [x * 2 for x in xs]
odd: list[int] = [x for x in xs if x % 2 == 1]
sq: list[int] = [i * i for i in range(5)]
```

対象にできるのは `list[T]` と `range(...)` です。`for` は 1 つだけで、
入れ子にしたいときは内側を式として書きます
（`[v + 1 for v in [y * 10 for y in xs]]`）。

> **⚠️ ループ変数は要素を借りています。** `[s for s in strs]` のように
> 借りたものをそのまま新しいリストへ入れるとエラーになります（所有権検査）。
> 要るのが値そのものなら `[copy(s) for s in strs]` です。

### 6.6 演算子の多重定義

**クラスに `__add__` などを書くと、演算子で書けます**（Python と同じ名前です）。

```python
class Money:
    yen: int

    def init(self, yen: int) -> None:
        self.yen = yen

    def __add__(self, o: Money) -> Money:
        return Money(self.yen + o.yen)

    def __eq__(self, o: Money) -> bool:
        return self.yen == o.yen

def main() -> int:
    a: Money = Money(300)
    b: Money = Money(120)
    print(str((a + b).yen))     # → 420
    print(str(a != b))          # → True（__eq__ から導かれます）
    return 0
```

使えるのは `+ - * / // % **`・単項 `-`・`== != < <= > >=`、添字、そして `in` です。

```python
m[i, j]          # → __getitem__(i, j)
m[i, j] = v      # → __setitem__(i, j, v)
x in c           # → c.__contains__(x)   ⚠️ これだけは右辺で決まります
```

`dict.Dict` はこれを使っているので、`if "a" in d:` と書けます。

`linalg.Matrix` はこれを使っているので、行列の式がそのまま書けます。

```python
c = a * b + d        # ⚠️ '*' は行列の積です（要素ごとの積は linalg.mmul）
print(str(c[0, 0]))
```

> **⚠️ 左辺の型だけで決まります。** `2.0 * m` は書けません（`m * 2.0`）。
> 反転（Python の `__rmul__`）はありません。

## 7. 標準ライブラリ

`import` するだけで使えます。**中身はすべて本言語で書かれています**（`lib/*{{ext}}`）。
コンパイラは特別扱いしていません。

### strings

```python
import strings

def main() -> int:
    for w in strings.split("a,b,c", ","):
        print(w)
    print(strings.join(["x", "y"], "-"))     # → x-y
    print(strings.strip("  hi  "))           # → hi
    print(str(strings.find("hello", "ll")))  # → 2
    return 0
```

ほかに `substr` / `contains` / `startswith` / `endswith` / `replace` /
`repeat` / `lpad` / `rpad` / `byte_at` / `matches_at` / `is_space` があります。

### io

`read_file` / `write_file` / `remove` / `exists` / `print_raw`（改行なし）/ `eprint`（stderr）、
および標準入力の `read_line` / `read_all`。

```python
import io

def main() -> int:
    io.write_file("memo.txt", "hello\n")
    print(io.read_file("memo.txt"))
    io.remove("memo.txt")
    return 0
```

標準入力は `read_line` を **`None` が返るまで**繰り返します（それが EOF です）。
戻り値に改行は含まれません。

```python
import io

def main() -> int:
    line: str | None = io.read_line()
    while line is not None:
        print("> " + line)
        line = io.read_line()
    return 0
```

対話的に 1 つ聞くだけなら、**組み込みの `input()`** のほうが簡単です
（`import` が要りません）。

```python
def main() -> int:
    n: int = int(input("数: "))
    print(str(n * 2))
    return 0
```

> **⚠️ `input()` は EOF で落ちます。** 静かに空文字を返すと、読み続ける
> `while` が**止まらなくなる**ためです。パイプで流し込むような、
> 入力が尽きうる場面では `io.read_line()`（`None` が返る）を使ってください。

### sys

`argv()` / `argc()` / `getenv(name)` / `run(cmd)`。

```python
import sys

def main() -> int:
    args: list[str] = sys.argv()
    if len(args) < 2:
        print("使い方: prog <名前>")
        return 1
    print("hello, " + args[1])
    return 0
```

### dict

**ジェネリック**です。鍵と値の型を書きます。

```python
import dict

def main() -> int:
    d: dict.Dict[str, int] = dict.Dict()
    d.set("apple", 3)
    print(str(d.get_or("apple", 0)))    # → 3
    print(str(d.get_or("none", -1)))    # → -1
    for k in d.keys():
        print(k)
    return 0
```

**クラスも値にできます**（ジェネリクスが入るまでは書けませんでした）。

```python
import dict

class Symbol:
    name: str
    value: int
    def init(self, name: own str, value: int) -> None:   # own は §9.2
        self.name = name
        self.value = value

def main() -> int:
    syms: dict.Dict[str, Symbol] = dict.Dict()
    syms.set("x", Symbol("x", 10))
    print(str(syms.get("x").value))     # → 10
    return 0
```

⚠️ 鍵の比較は `==` です。`int` / `float` / `str` は**中身**で、
クラスと `list` は**同一性**（同じものを指しているか）で比べます。

### time

**自分のプログラムの速さを測るためのモジュールです。**

```python
import time

def main() -> int:
    t0: int = time.monotonic_ns()
    total: int = 0
    for i in range(10000000):
        total = total + i
    print(str(time.since_ms(t0)) + " ms")   # → 例: 12.481 ms
    print(str(total))
    return 0
```

`monotonic_ns()` の起点に意味はありません。**2 つ取って引き算**して使います
（`since_ms` / `since_sec` はそれを 1 行でやるものです）。
「いつ動いたか」を記録したいときは `now()` / `now_ns()`（実時刻）を使います。

> **⚠️ 速さを測るのに実時刻を使わないでください。** NTP の補正などで
> **時間が戻ることがあり**、経過時間が負になります。

> **⚠️ このモジュールだけはベアメタルで使えません**（時計は OS の持ち物です）。

### 数値計算

`math` / `linalg` / `stats` / `random` / `numeric` / `physics` /
**`complex`** / **`fft`** があります。
**どれも本言語で書かれていて、`libm` を呼びません**（ベアメタルでも動きます）。

```python
import math
import linalg
import physics

def main() -> int:
    print(math.sqrt(2.0))                          # 1.414214
    print(math.degrees(math.atan2(1.0, 1.0)))      # 45.0

    m: linalg.Matrix = linalg.from_rows([[1.0, 2.0], [3.0, 4.0]])
    print(str(linalg.det(m)))                      # -2.0
    linalg.vprint(linalg.solve(m, [5.0, 11.0]))    # [1.0, 2.0]

    print(physics.escape_velocity(5.972e24, 6.371e6))   # 11185.977892
    return 0
```

複素数とフーリエ変換もあります（`complex` / `fft`）。
**言語の組み込み型ではなく、演算子の多重定義（§6.6）で書かれた
ふつうのライブラリです。**

```python
import complex
import fft

def main() -> int:
    z: complex.Complex = complex.rect(3.0, 4.0)
    print((z * z).show())            # → -7.0+24.0i
    print(str(complex.mag(z)))       # → 5.0

    xs: list[float] = [1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0]
    print(str(fft.spectrum(xs)[0]))  # → 8.0
    return 0
```

> **⚠️ スカラー倍は `complex.scale(z, 2.0)`** です（`z * 2.0` は書けません）。
> **⚠️ `fft` の長さは 2 のべき乗だけ**です。

ベクトルは専用の型ではなく **`list[float]` をそのまま**使います。
一覧は [spec/stdlib.md](spec/stdlib.md) を見てください。

---

## 8. モジュール

ファイルがそのままモジュールです。`import foo` は
**同じディレクトリの `foo{{ext}}` → 標準ライブラリ**の順に探します。

```python
# geometry{{ext}}
class Point:
    x: int
    y: int
    def init(self, x: int, y: int) -> None:
        self.x = x
        self.y = y

def origin() -> Point:
    return Point(0, 0)
```

```python
# main{{ext}}
import geometry

def main() -> int:
    p: geometry.Point = geometry.origin()
    print(str(p.x))
    return 0
```

**必ず `モジュール名.` を付けて呼びます**（`from ... import ...` はありません）。
コンパイルするのは入口のファイルだけで、`import` は自動でたどられます。

```bash
$ {{cc}} main{{ext}} -o main
```

### 8.1 よく使う標準ライブラリ

```python
import json
import set
import io
import sys

def main() -> int:
    # JSON を読む
    r: json.Result = json.parse("{\"name\": \"world\", \"stars\": 3}")
    if r.ok:
        print(r.get().field("name").as_str())

    # 集合
    tags: set.Set[str] = set.of(["a", "b", "a"])
    print(str(tags.len()))          # 2

    # ディレクトリ
    io.mkdir_all("out/reports")
    for name in io.listdir("out"):
        print(name)
    io.remove_all("out")

    # コマンドの出力を受け取る
    print(sys.capture("git rev-parse --short HEAD").line())
    return 0
```

一覧は[標準ライブラリ](spec/stdlib.md)にあります。

### 8.2 他の人が書いたライブラリを使う

`-I` で探す場所を足せます。**レジストリはありません。** 依存は git の
リポジトリを直に指し、パッケージマネージャ `{{pm}}` が `deps/` に展開します。

```bash
$ {{pm}} init myapp
$ {{pm}} add json https://github.com/user/json-pkg 1.2.0
$ {{pm}} build          # {{cc}} -I deps main{{ext}} -o myapp と同じ
```

`package.lock` が commit と tree の SHA で中身を固定するので、タグを
張り替えられても入ってくるものは変わりません。インストール中に
パッケージのコードは 1 行も実行されません。

⚠️ **モジュール名はプログラム全体で一意です。** 2 つのパッケージが同じ
`json{{ext}}` を出すと使えません（`{{pm}}` がインストール時に止めます）。
公開するライブラリのファイル名にはパッケージ名を付けてください（`json` や `set` は
標準ライブラリが使っています）。

→ [reference/pkg.md](reference/pkg.md)

---

## 9. 安全性 — 本言語の中心

ここからが Python にも C にも無い部分です。**GC を使わずに、解放し忘れと二重解放を防ぎます。**

> **★ ここの検査は既定でエラーです**（0.18.0 から）。オプションは要りません。
> ⚠️ 古いコードを持ち込むときだけ `--warn-own` で警告に落とせます。
> そのうえで `--warn-own --deny-move` のように、通ったものから順に
> エラーへ戻していくのが移行の順序です（**後に書いたほうが勝ち**）。

### 9.1 引数は既定で「借用」

関数に値を渡しても、**所有権は移りません**。呼び出しの後も自由に使えます。

```python
def total(xs: list[int]) -> int:     # 借りるだけ
    s: int = 0
    for x in xs:
        s = s + x
    return s

def main() -> int:
    xs: list[int] = [1, 2, 3]
    print(str(total(xs)))
    print(str(len(xs)))              # ○ まだ使える
    return 0
```

**呼び出し側には何も書きません。** `&` も `.clone()` も要りません。

### 9.2 `own` — 所有権を受け取る

「この関数が値を引き取る」と言いたいときだけ `own` を書きます。
渡した側は**それ以降その変数を使えません**。

```python
def take(xs: own list[int]) -> int:
    return len(xs)

def main() -> int:
    xs: list[int] = [1, 2, 3]
    print(str(take(xs)))             # ここで xs は take のものになった
    print(str(len(xs)))              # ⚠️ 移動済みの値 'xs' を使っています
    return 0
```

```
warning[E-MOVE-1]: 移動済みの値 'xs' を使っています
  --> mv{{ext}}:7:15
   |
 7 |     print(len(xs))
   |               ^^ ここで使われています
   |
note: 'xs' はここで移動しました
  --> mv{{ext}}:6:16
   |
 6 |     print(take(xs))
   |                ^^
```

### 9.3 借用したものは保存できない

借りた値を**自分より長生きする場所**（フィールド・グローバル・戻り値）に置くことはできません。
貸してくれた相手が先に消えると、壊れたポインタが残るためです。

直し方は 2 つです。

| やりたいこと | 書き方 |
|---|---|
| 呼び出し側から所有権をもらう | 引数を `own` にする |
| 中身だけ複製して持つ | `copy(x)`（いまは `str` のみ） |

```python
import dict

def count(words: list[str]) -> None:
    d: dict.Dict = dict.Dict()
    for w in words:
        d.set(copy(w), d.get_or(w, 0) + 1)   # 鍵は Dict が所有するので複製
```

### 9.4 `mut` — 借りたものを書き換える

借用は既定で**読み取り専用**です。書き換えるなら `mut` を書きます。

```python
def fill(xs: mut list[int], n: int) -> None:
    xs.append(n)

class Counter:
    n: int
    def init(self) -> None:
        self.n = 0
    def bump(mut self) -> None:      # self を書き換えるメソッドにも mut
        self.n = self.n + 1
    def get(self) -> int:            # 読むだけなら要らない
        return self.n
```

**呼び出し側は何も変わりません。**

```python
c: Counter = Counter()
c.bump()                             # mut と書く必要はない
```

どの引数が書き換えられるのかを確かめたいときは `--explain-mut` を使います。

> 局所変数の再代入（`i = i + 1`）と `init` の中の `self.x = ...` に `mut` は要りません。

### 9.5 `rc[T]` — 共有したいとき

「1 つの値を複数の場所が持ち続ける」構造は、所有権だけでは書けません。
そのための逃げ道が `rc[T]`（参照カウント）です。**最後の 1 つが手放したときに解放されます。**

```python
class Node:
    name: str
    def init(self, name: own str) -> None:
        self.name = name

def keep(dst: mut list[rc[Node]], n: rc[Node]) -> None:
    dst.append(n)

def main() -> int:
    a: rc[Node] = rc(Node("a"))
    b: rc[Node] = a                  # 共有。a はまだ使える
    box: list[rc[Node]] = []
    keep(box, a)
    print(a.name)                    # → a（`.` は自動で中身に届く）
    print(b.name)                    # → a
    return 0
```

`rc` は**最後の手段**です。まず借用で書けないか試してください。

### 9.6 解放

スコープを出るときに解放が入ります。クラスに `drop` を書けば、そのときに呼ばれます。

```python
class File:
    name: str
    def init(self, name: own str) -> None:
        self.name = name
    def drop(self) -> None:
        print("閉じる: " + self.name)
```

**解放は既定で入ります**（0.16.0 から）。入れたくないときだけ `--no-drop` を付けます。

### 9.7 範囲型 — 値の範囲を型で言う

**`int` に名前と両端を付けた型**を作れます（Ada の部分型にあたります）。

```python
type Percent = int range(0, 100)      # ★ 宣言はこの 1 行だけ

def apply(p: Percent) -> int:
    return p * 2                       # 計算の途中は int

def main() -> int:
    a: Percent = 50                    # ✅
    b: Percent = 101                   # ❌ コンパイル時に断られます
    n: int = int(input())
    c: Percent = n                     # ✅ 実行時に確かめます（外れたら止まる）
    return 0
```

確かめるのは**その型の場所に入れるとき**です——変数・フィールド・`list` の要素・
引数・`return`。途中の計算は `int` で行うので、`p * 2` のたびに止まることは
ありません。

⚠️ **検査を外すオプションはありません。** 「型で 0〜100 と言ったのに 101 が
入る」状態を作れるなら、型で言った意味がないからです。

### 9.8 契約 — 満たすべき条件を書く

関数の先頭に、**入口と出口で満たすべき条件**を書けます（Ada の `Pre` / `Post`）。

```python
def divide(a: int, b: int) -> int:
    requires b != 0          # ★ 入口で確かめる
    ensures result >= 0      # ★ return のたびに確かめる
    return a // b
```

`ensures` の中の `result` は**戻り値そのもの**です。破ると止まります。

```
runtime error: contract violated: requires of divide (line 2)
```

⚠️ `result` が特別なのは `ensures` の式の中だけです。`result` という名前の
変数は今までどおり使えます。

⚠️ `ensures` は `return` の**直前**に評価されます（入口の値と比べたいときは、
自分で写しておいてください）。

---

## 10. エラー処理

例外はありません。`raises` / `try` / `except` は**見た目だけ例外で、実体は戻り値の検査**です。
スタックの巻き戻し（アンワインド）は起きません。

```python
class IOError:
    message: str
    def init(self, message: own str) -> None:
        self.message = message

def read(path: str) -> int raises IOError:     # 起こしうるエラーを宣言する
    if len(path) == 0:
        raise IOError("空のパス")
    return len(path)

def main() -> int:
    try:
        print("ok:" + str(read("abcde")))
    except IOError as e:
        print("caught: " + e.message)

    try:
        print("ok:" + str(read("")))
    except IOError as e2:
        print("caught: " + e2.message)
    return 0
```

```
ok:5
caught: 空のパス
```

3 つの規則があります。

1. **宣言しないと投げられない** — `raises` に書いていない型は `raise` できません。
2. **握りつぶせない** — `raises` する関数を呼ぶ側は、`try` で受けるか自分も `raises` を付けるか、どちらかが必要です。
3. **エラー型は `class`** — `int` や `str` は使えません。

複数書くときは `raises A | B` とします。

**`panic` との使い分け** — 呼び出し側が対処できるものは `raises`、
プログラムの間違い（配列の範囲外など、直すべきバグ）は `panic` です。

---

## 11. コンパイラの使い方

```bash
{{cc}} [オプション] <入力{{ext}}>
```

| オプション | 説明 |
|---|---|
| `-o <file>` | 出力する実行ファイル名（既定 `a.out`） |
| `--check` | 型検査までで止める。問題が無ければ何も出さない |
| `-O0`〜`-O3` | clang に渡す最適化レベル（既定 `-O0`） |
| `--version` | 版番号と target triple |
| `-S` | LLVM IR を標準出力に出して終了 |

安全性の検査：

| オプション | 説明 |
|---|---|
| （既定） | 移動済みの値・借用の保存／返却・読み取り専用への書き換えは**すべてエラー** |
| `--warn-own` | 上の 4 つを**警告に落とす**（0.17 以前の既定。⚠️ 保証の外に出ます） |
| `--deny-move` / `--deny-borrow` / `--deny-mut` / `--deny-store-borrow` | `--warn-own` の後に書くと、その検査だけエラーへ戻す |
| `--drop` / `--no-drop` | スコープの出口に解放を挿入する（`--drop` が既定） |
| `--explain-mut` | 呼び出しで書き換えられる実引数を一覧表示 |

新しく書くコードでは、**何も付けないのがいちばん安全**です。

```bash
{{cc}} app{{ext}} -o app                        # 4 つともエラー（既定）
{{cc}} --warn-own old{{ext}} -o old              # 古いコードを通したいときだけ
```

コンパイラ自身をいじる人向けのオプション（`--dump-tokens` / `--dump-ast` /
`--keep-ll` / `--target=` / `-c`）は `{{cc}} --help` を見てください。

---

## 12. まとめの例

ここまでの要素だけで書ける、語数カウントです。

```python
# wordcount{{ext}}
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
    counts: dict.Dict = dict.Dict
    for line in strings.split(text, "\n"):
        for word in strings.split(strings.strip(line), " "):
            if len(word) > 0:
                counts.set(copy(word), counts.get_or(word, 0) + 1)

    for k in counts.keys():
        print(k + ": " + str(counts.get(k)))
    return 0
```

```bash
$ {{cc}} examples/wordcount{{ext}} -o wc
$ ./wc examples/sample.txt
```

---

## 13. 次に読むもの

| 目的 | ドキュメント |
|---|---|
| 言語の正確な定義を知りたい | [spec/language-spec.md](spec/language-spec.md) |
| 所有権・エラー処理の正確な規則 | [spec/safety-spec.md](spec/safety-spec.md) |
| 文法（EBNF） | [spec/grammar.md](spec/grammar.md) |
| **コンパイラの作り方を学びたい** | [docs/README.md](README.md) — 設計ドキュメント一式 |
| 用語がわからない | [reference/glossary.md](reference/glossary.md) |

---

## 付録: いま無いもの

正直に書いておきます。**これらは未実装です。**（`{{cc}} {{version}}` で確認）

いまどのくらいの言語なのか（Python / Rust / Ada との比較）と、実践に足りない
ものの優先順は [ロードマップ §1.5](roadmap.md) にまとめてあります。

| 無いもの | 代わりに |
|---|---|
| **継承** | 合成（フィールドに持つ）。振る舞いの共有は**インタフェース**（§6.5） |
| **クロージャ・`lambda`** | トップレベルの関数を渡す（関数型はあります。§5.1） |
| **タプル・複数戻り値** | `list` かクラスを返す |
| 集合 | `dict.Dict` を「値は使わない表」として使う |
| 内包表記 | `for` と `append`、または `numeric.map` / `numeric.filter` |
| `dict` の `in` | `d.has(k)` |
| f-string の書式指定 `{x:>8}` | `strings.lpad` / `strings.rpad` |
| 例外（アンワインド） | `raises` / `try` / `except`（§10。**設計上入れません**） |
| モジュールの名前空間（`import a.b`） | ファイル名を一意にする（パッケージ名を接頭辞に。§8.2） |
| `enum` / `match` | グローバル定数と `if`/`elif` |
| 既定引数・キーワード引数 | 引数を全部書く |
| **既定でメモリを解放しない** | 短命なプログラムなら気にしなくて構いません。`--drop` で解放を挿入できます（[ロードマップ §1.5.1](roadmap.md)） |

**上の 2 つが実用上いちばん効きます。**

言語の現在地と今後は [roadmap.md](roadmap.md) と
[design/future-features.md](design/future-features.md) にあります。
