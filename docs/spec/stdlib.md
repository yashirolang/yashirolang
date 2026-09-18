# 標準ライブラリ

`import` するだけで使えます。**中身はすべて本言語で書かれています**（`lib/*{{ext}}`）。
コンパイラは一切特別扱いしていません — ユーザーが書けるものだけで出来ています。

```python
import strings

def main() -> int:
    print(strings.join(strings.split("a,b,c", ","), "-"))   # → a-b-c
    return 0
```

**必ず `モジュール名.` を付けて呼びます**（`from ... import ...` はありません）。

| モジュール | 内容 |
|---|---|
| [`strings`](#strings) | 文字列の操作 |
| [`io`](#io) | ファイル・標準出力・標準エラー・標準入力 |
| [`sys`](#sys) | コマンドライン引数・環境変数・外部コマンド |
| [`dict`](#dict) | ハッシュ表 |
| [`set`](#set) | **集合** — 重複しない入れもの |
| [`json`](#json) | **JSON の読み書き** |
| [`pair`](#pair) | 2 つ・3 つの値をまとめる（タプルの代わり） |
| [`lists`](#lists) | **型を問わない**リスト操作 |
| [`math`](#math) | **数学関数** — Python の `math` 相当 |
| [`linalg`](#linalg) | **ベクトルと行列** — 線形代数 |
| [`stats`](#stats) | **統計** — Python の `statistics` 相当 |
| [`random`](#random) | **疑似乱数** — Python の `random` 相当 |
| [`numeric`](#numeric) | **数値解析** — 積分・求根・微分・常微分方程式・最適化 |
| [`physics`](#physics) | **物理定数と単位換算** |
| [`time`](#time) | **時刻と経過時間** — 自分のプログラムの速さを測る |
| [`complex`](#complex) | **複素数** — Python の `cmath` 相当 |
| [`fft`](#fft) | **高速フーリエ変換** |
| [`plot`](#plot) | **作図** — SVG を書き出す（matplotlib 相当） |
| [`frame`](#frame) | **表形式のデータ** — CSV・絞り込み・並べ替え・集計（pandas 相当） |
| [`decimal`](#decimal) | **十進の固定小数点** — 金額のための正確な数 |
| [`bytes`](#bytes) | **固定幅のバイト並び** — 通信フレーム・バイナリ形式 |
| [`blas`](#blas) | **BLAS を呼ぶ** — 行列積・内積（⚠️ リンクの指定が要ります） |

> **numpy / scipy / matplotlib / pandas との対応表**は
> [数値計算の手引き](../reference/numerics.md)にあります。

> **⚠️ 数値計算のモジュールも、すべて本言語で書かれています。**
> `libm` を呼びません。ランタイムが libc に依存しない約束（ベアメタルで動く）
> を守るためで、`math.sqrt` はニュートン法、`math.exp` は引数を縮小してから
> テイラー展開、というように中身まで `lib/math{{ext}}` にあります。

---

## strings

`str` は**不変**なので、どの関数も**新しい文字列を返します**。

### 取り出す

| 関数 | 説明 |
|---|---|
| `byte_at(s: str, i: int) -> int` | `i` バイト目のバイト値。範囲外は実行時エラー |
| `substr(s: str, start: int, count: int) -> str` | `start` から `count` バイト |

### 探す

| 関数 | 説明 |
|---|---|
| `find(s: str, sub: str) -> int` | `sub` が最初に現れる位置。**無ければ `-1`** |
| `contains(s: str, sub: str) -> bool` | 含むか |
| `startswith(s: str, prefix: str) -> bool` | で始まるか |
| `endswith(s: str, suffix: str) -> bool` | で終わるか |
| `matches_at(s: str, sub: str, at: int) -> bool` | 位置 `at` に `sub` があるか |

### 分ける・つなぐ

| 関数 | 説明 |
|---|---|
| `split(s: str, sep: str) -> list[str]` | `sep` で分ける。`sep` が空なら 1 要素 |
| `join(xs: list[str], sep: str) -> str` | `sep` を挟んで連結 |

### 整える

| 関数 | 説明 |
|---|---|
| `strip(s: str) -> str` | 前後の空白を落とす |
| `from_code_point(cp: int) -> str` | コードポイントを UTF-8 の文字にする（Python の `chr` 相当）。⚠️ **組み込みの `chr()` はバイト**（0〜255）を作ります |
| `lpad(s: str, w: int) -> str` | 左に空白を詰めて表示幅 `w` にする |
| `rpad(s: str, w: int) -> str` | 右に空白を詰める |
| `repeat(s: str, times: int) -> str` | `times` 回くり返す |
| `replace(s: own str, old: str, new: str) -> str` | 全て置換。**`s` の所有権を取ります**（`own`） |

### 数える

| 関数 | 説明 |
|---|---|
| `char_count(s: str) -> int` | **文字数**（`len` はバイト数） |
| `width(s: str) -> int` | **表示幅**（全角を 2 と数える） |
| `chars(s: str) -> list[str]` | 1 文字ずつに切る。**`for c in s` はバイト単位です** |
| `char_len_at(s: str, i: int) -> int` | 位置 `i` の文字が何バイトか |

```python
len("あいう")                  # 9  ← バイト数
strings.char_count("あいう")   # 3  ← 文字数
strings.width("あいう")        # 6  ← 表示幅
```

⚠️ `width` は**目安**です。端末や字体によって幅は変わります。
「よく全角で表示される範囲」を 2 桁として数えています。

> **⚠️ 桁を揃えるときは `len` ではなく `lpad` / `rpad` を使ってください。**
> `lpad` / `rpad` は表示幅で数えるので、日本語が混ざっても崩れません。

### 判定

| 関数 | 説明 |
|---|---|
| `is_space(c: str) -> bool` | 空白文字か（先頭 1 バイトを見る） |
| `is_space_byte(b: int) -> bool` | バイト値が空白か |

> **⚠️ 探索や切り出しはバイト単位です。** `str` は UTF-8 のバイト列で、
> `find` / `substr` / スライスの添字はバイト位置です
> （多バイト文字の途中で切ると壊れます）。
>
> ```python
> s: str = "あいう"
> len(s)                     # 9   ← バイト数
> s[0]                       # 壊れた 1 バイト
> for c in s:                # 9 回まわる
> for c in strings.chars(s): # 3 回まわる ← こちらを使う
> ```

---

## io

| 関数 | 説明 |
|---|---|
| `read_file(path: str) -> str` | 全部読む。開けなければ実行時エラー |
| `write_file(path: str, text: str) -> None` | 上書きで書く |
| `exists(path: str) -> bool` | あるか |
| `remove(path: str) -> None` | 消す |
| `is_dir(path: str) -> bool` | ディレクトリか |
| `mkdir(path: str) -> bool` | 作る。**既にあれば True**。親が無ければ失敗 |
| `mkdir_all(path: str) -> bool` | 途中の親ごと作る（`mkdir -p` 相当） |
| `listdir(path: str) -> list[str]` | 中身の名前（`.` と `..` は含まない）。⚠️ **並びは OS 任せ** |
| `rmdir(path: str) -> bool` | **空の**ディレクトリを消す |
| `remove_all(path: str) -> bool` | 中身ごと消す（`rm -rf` 相当。⚠️ リンクの先はたどらない） |
| `rename(from_: str, to: str) -> bool` | 名前を変える／移す |
| `size(path: str) -> int` | 大きさ（バイト）。無ければ `-1` |
| `print_raw(s: str) -> None` | 標準出力へ**改行を足さずに**書く |
| `eprint(s: str) -> None` | 標準エラーへ書く |
| `read_line() -> str \| None` | 標準入力から 1 行。**改行は含まない**。読めなければ `None` |
| `read_all() -> str` | 標準入力を最後まで。何も無ければ空文字列 |

```python
import io

def main() -> int:
    io.write_file("memo.txt", "hello\n")
    print(io.read_file("memo.txt"))
    io.remove("memo.txt")
    return 0
```

### ディレクトリ

```python
import io

def main() -> int:
    io.mkdir_all("out/reports")
    io.write_file("out/reports/a.txt", "hi")
    for name in io.listdir("out/reports"):
        print(name + " " + str(io.size("out/reports/" + name)))
    io.remove_all("out")
    return 0
```

⚠️ **区切りは `/` だけ**を見ます（Windows でも `/` で書きます）。
⚠️ `listdir` の並びは OS 任せです。**同じ結果が欲しいときは自分で整列**してください。
そうしないと、同じプログラムが環境によって違う順で動きます。

### 標準入力

`read_line` は 1 行ずつ読み、**EOF で `None` を返します**。これが繰り返しの終わり方です。

```python
import io

def main() -> int:
    n: int = 0
    line: str | None = io.read_line()
    while line is not None:
        n = n + 1
        print(str(n) + ": " + line)
        line = io.read_line()
    return 0
```

```
$ printf 'hello\nworld\n' | ./count
1: hello
2: world
```

| | |
|---|---|
| 改行 | `read_line` の戻り値に**含まれません** |
| CRLF | 行末の `\r` は落とします（Windows で作った入力でも同じ結果） |
| 最終行 | 改行で終わっていなくても 1 行として読めます |
| 空の入力 | `read_line` は `None`、`read_all` は `""` |

> **⚠️ ベアメタルでは使えません。** 標準入力は `runtime/hosted.c`（PC 用）にしかなく、
> カーネル側（`runtime/core.c` の 4 フック）には含めていません。
> 入力の無い環境に「使わないのに実装させる」ことを避けるためです。

### `input()` との使い分け

`input()` は **import なしで使える組み込み関数**です（Python と同じ形）。

```python
def main() -> int:
    name: str = input("名前: ")     # プロンプトは省略できます
    print("hello, " + name)
    return 0
```

| | `input()` | `io.read_line()` |
|---|---|---|
| import | 要りません | `import io` |
| プロンプト | 出せます | 出せません |
| EOF | **`panic` します** | `None` を返します |
| 向く場面 | 対話的に 1 つ聞く | 入力を最後まで読む |

> **⚠️ EOF で落ちるのは意図的です。** 静かに空文字を返すと、
> `while` で読み続ける書き方が**止まらなくなります**。
> 読めないかもしれない場面では `io.read_line()` を使ってください
> （Python の `input()` が `EOFError` を投げるのと同じ考え方です）。

---

## sys

| 関数 | 説明 |
|---|---|
| `argv() -> list[str]` | コマンドライン引数。**`argv()[0]` はプログラム名** |
| `argc() -> int` | その個数 |
| `getenv(name: str) -> str` | 環境変数。**無ければ空文字列**（`None` ではない） |
| `run(cmd: str) -> int` | シェルでコマンドを実行し、終了コードを返す |
| `cpu_count() -> int` | 使える CPU コアの数（`spawn` の本数を決めるのに使う）。**0 は返りません**。⚠️ ベアメタルでは使えません |
| `start_shell(cmd: str) -> int` | コマンドを**起こして戻る**。返るのはプロセス番号。起こせなければ `-1` |
| `wait_for(pid: int) -> int` | その番号の終了を待って、終了コードを返す |
| `capture(cmd: str) -> Output` | 実行して、**標準出力**と終了コードを受け取る |

### `capture` — コマンドの出力を受け取る

```python
import sys

def main() -> int:
    r: sys.Output = sys.capture("git rev-parse HEAD")
    if r.ok():
        print(r.line())          # 末尾の改行を落とした 1 行
    for f in sys.capture("ls").lines():   # 空でない行に分ける
        print(f)
    return 0
```

| `Output` | |
|---|---|
| `code: int` | 終了コード（起動できなければ `-1`） |
| `out: str` | 標準出力（**そのまま**。改行も落としません） |
| `ok() -> bool` | `code == 0` か |
| `line() -> str` | 前後の空白を落とした文字列 |
| `lines() -> list[str]` | 空でない行の並び |

⚠️ **標準エラーは含みません。** 混ぜたいときはコマンドに `2>&1` を書いてください
（混ぜるかどうかは呼ぶ側が決めることなので、既定は分けます）。
⚠️ 中身は**シェルが解釈します**。外から来た文字列をそのまま繋がないでください。
⚠️ **どのシェルかは OS が決めます。** `capture` と [`run`](#sys) は必ず**同じシェル**を
使いますが、`;` やパイプの意味は OS によって変わります。**複数のコマンドを 1 つの
文字列に詰め込まないでください**（Windows で 1 行にまとめられて CI が落ちました）。

⚠️ **複数のコマンドを同時に走らせたいときは、`spawn` ではなく
`start_shell` / `wait_for` を使ってください。** macOS の `system()` は
グローバルなロックを取るので、**スレッドから `run()` を呼んでも 1 本ずつしか
走りません**（コンパイラのドライバを並列化したときに実測で気づきました）。

⚠️ `start_shell` は Windows では `-1` を返します。呼ぶ側は `run()` に
落としてください。

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

---

## dict

**ジェネリック**です。鍵と値の型を書きます。

| メソッド | 説明 |
|---|---|
| `Dict[K, V]()` | 空の表を作る |
| `has(k: K) -> bool` | 鍵があるか |
| `get(k: K) -> V` | 値。**無ければ実行時エラー** |
| `get_or(k: K, default: own V) -> V` | 値。無ければ `default` |
| `set(mut self, k: own K, v: own V) -> None` | 入れる（あれば上書き）。**鍵と値の所有権を取ります** |
| `remove(k: K) -> bool` | 消す。あったら `True`。**O(1)**（墓石方式） |
| `key_at(i) -> K` / `val_at(i) -> V` | `i` 番目。**順に見るときに使います** |
| `len() -> int` | 個数 |
| `keys() -> list[K]` / `values() -> list[V]` | 一覧。**入れた順**に返します |

⚠️ **鍵の比較は `==` です。** `int` / `float` / `str` は**中身**で、
クラスと `list` は**同一性**（同じものを指しているか）で比べます。
クラスを鍵にするときはこの違いに注意してください。

**ハッシュ表です**。それまでは線形探索で、4000 件の出し入れに
0.19 秒かかっていました。いまは 50,000 件でも 0.04 秒です。

⚠️ **鍵に使えるのは `hash` が定義された型**（`int` / `float` / `str` / `bool`）
だけです。クラスを鍵にすると、**実体化したときに**エラーになります
（型制約が無いので、テンプレートの時点では分かりません）。

```
error: hash は 'Node' 型を受け取れません
   --> lib/dict{{ext}}:50:21
note: この実体化（Dict$Node$int）で使われました
   --> main{{ext}}:7:13          ← 発端の行も示します
```

⚠️ ハッシュは FNV-1a です。**暗号用ではありません。** 敵が鍵を選べる場面
（外部入力を鍵にするサーバなど）では、衝突を狙われて線形探索に落とされます。

⚠️ **`items()` のように「組のリスト」を返すことはできません。** 中身は表が
持っているので、組にして返すと「借りたものを保存する」ことになります
（`E-BORROW-1` / `E-BORROW-3`）。順に見るときは `key_at` / `val_at` を使います。

```python
i: int = 0
while i < d.len():
    print(f"{d.key_at(i)} -> {d.val_at(i)}")
    i = i + 1
```

```python
import dict

def main() -> int:
    d: dict.Dict[str, int] = dict.Dict()
    d.set("apple", 3)
    print(str(d.get_or("apple", 0)))     # → 3
    print(str(d.get_or("none", -1)))     # → -1
    return 0
```

### クラスを値にする

```python
import dict

class Symbol:
    name: str
    value: int
    def init(self, name: own str, value: int) -> None:
        self.name = name
        self.value = value

def main() -> int:
    syms: dict.Dict[str, Symbol] = dict.Dict()
    syms.set("x", Symbol("x", 10))
    print(str(syms.get("x").value))      # → 10
    return 0
```

> **ジェネリクスが入るまでは書けませんでした。** 値が `int` に固定されていたので、
> 「`list` に置いて、その添字を値に入れる」回り道が必要でした
> （`tests/cases/lib_dict_handle{{ext}}` にその形が残っています）。

### 鍵の所有権に注意

`set` は鍵の**所有権を取ります**（`k: own str`）。借りている文字列を渡すときは
`copy` が要ります。

```python
def count(words: list[str]) -> None:
    d: dict.Dict[str, int] = dict.Dict()
    for w in words:                       # w は借用
        d.set(copy(w), d.get_or(w, 0) + 1)
```

`copy` を書かないと `E-BORROW-1` の警告が出ます。詳しくは
[safety-spec.md](safety-spec.md) を参照してください。

---

### `in`

```python
if "a" in d:        # → d.__contains__("a")
    print(str(d.get("a")))
if "z" not in d:
    ...
```

`d.has(k)` と同じものです。⚠️ `in` は**右辺（入れ物）の型**で決まります。

## math

**Python の `math` に相当します。** すべて `float` を受け取り `float` を返します。

> **⚠️ `libm` は呼びません。** ランタイムは libc に依存しない約束なので
> （ベアメタルで動かすため）、`lib/math{{ext}}` にアルゴリズムごと書いてあります。
> 精度の目標は**倍精度の有効桁のうち下位 1〜2 桁を除いて合う**ことです。
> libm のような最終ビットまでの正確さは目指していません
> （`tests/cases/lib_math_libm{{ext}}` が libm の値 580 件と突き合わせています）。

### 定数

`pi()` / `tau()` / `e()` / `ln2()` / `ln10()` / `inf()` / `nan()`

⚠️ **関数です。** グローバル変数の初期化式はリテラルしか書けないためです。

### 判定・符号

| 関数 | 説明 |
|---|---|
| `isnan(x)` / `isinf(x)` / `isfinite(x)` | 種別の判定 |
| `fabs(x)` | 絶対値 |
| `copysign(x, y)` | `y` の符号を `x` の大きさに付ける |
| `sign(x)` | `-1.0` / `0.0` / `1.0` |
| `isclose(a, b, rel, abs_tol)` | ほぼ等しいか（**`==` で比べる代わりに使う**） |

### 丸め

`trunc(x)` / `floor(x)` / `ceil(x)` / `round(x)` / `fmod(x, y)`

⚠️ `round` は **0 から遠いほうへ**丸めます（C の `round` と同じ。
Python の偶数丸めではありません）。

| `round_to(x, n)` | 小数点以下 `n` 桁に丸める（**表示のため**） |

> **⚠️ `str(float)` は「元に戻る最短の表記」です**（`0.1 + 0.2` は
> `0.30000000000000004`）。桁を落として見せたいときに `round_to` を使います。

### 冪根・指数・対数

| 関数 | 備考 |
|---|---|
| `sqrt(x)` / `cbrt(x)` | 負の入力で `sqrt` は `NaN`、`cbrt` は符号を保つ |
| `exp(x)` / `log(x)` / `log2(x)` / `log10(x)` / `log1p(x)` | `log(0)` は `-inf`、`log(負)` は `NaN` |
| `pow(x, y)` | 指数が整数なら繰り返し二乗法。負の底の非整数乗は `NaN` |
| `hypot(x, y)` | `√(x²+y²)`。途中で溢れない |
| `ldexp(x, n)` | `x · 2ⁿ` |

⚠️ **`float ** float` は演算子としては使えません**（`**` は `int` 専用）。
`math.pow` を使ってください。

### 三角関数・双曲線関数

`sin` / `cos` / `tan` / `asin` / `acos` / `atan` / `atan2(y, x)`
`sinh` / `cosh` / `tanh` / `asinh` / `acosh` / `atanh`
`degrees(rad)` / `radians(deg)`

### 整数の関数

`iabs` / `gcd` / `lcm` / `factorial`（20 まで）/ `imin` / `imax` / `fmin` / `fmax`

```python
import math

def main() -> int:
    print(math.sqrt(2.0))                 # 1.414214
    print(math.degrees(math.pi()))        # 180.0
    print(math.atan2(1.0, 1.0))           # 0.785398
    print(str(math.gcd(48, 18)))          # 6
    return 0
```

---

## linalg

**ベクトルと行列。** ベクトルは専用の型を作らず `list[float]` をそのまま使います。

### ベクトルを作る

`zeros(n)` / `ones(n)` / `full(n, x)` / `linspace(a, b, n)` / `arange(a, b, step)` / `vcopy(a)`

### 要素ごと

`add` / `sub` / `mul`（アダマール積）/ `div` / `scale(a, k)` / `offset(a, k)` / `neg` / `vabs`

⚠️ **`div` は 0 の要素があると実行時エラーです**（`x / 0.0` が
止まるようになったため）。`inf` が欲しいときは `--no-overflow-check`。

### まとめる

`vsum` / `vprod` / `vmin` / `vmax` / `argmin` / `argmax`

### 内積・ノルム

| 関数 | 説明 |
|---|---|
| `dot(a, b)` | 内積 |
| `norm(a)` | ユークリッドノルム。**途中で溢れません** |
| `norm1(a)` / `norm_inf(a)` | L1 / L∞ |
| `dist(a, b)` | 距離 |
| `normalize(a)` | 長さ 1 に揃える |
| `cross(a, b)` | 外積（3 次元のみ） |
| `angle(a, b)` | なす角（ラジアン） |

### 行列

`Matrix` クラス。中身は 1 本の `list[float]`（**行優先**）です。

| 作る | |
|---|---|
| `mzeros(r, c)` / `mfull(r, c, x)` | 埋める |
| `identity(n)` | 単位行列 |
| `from_rows([[...], [...]])` | 行の並びから |
| `diag([...])` | 対角行列 |
| `mcopy(a)` | 複製 |

| 使う | |
|---|---|
| `a.get(i, j)` / `a.set(i, j, v)` | 要素 |
| `a.row(i)` / `a.col(j)` | 行・列を取り出す（複製） |
| `a.rows` / `a.cols` / `a.size()` / `a.is_square()` | 形 |

| 計算する | |
|---|---|
| `madd` / `msub` / `mmul`（要素ごと）/ `mscale` / `mneg` | 要素ごと |
| `matmul(a, b)` | **行列の積** |
| `matvec(a, v)` | 行列 × ベクトル |
| `transpose(a)` / `trace(a)` / `mpow(a, n)` | |
| `det(a)` | 行列式 |
| `solve(a, b)` | **`A·x = b` を解く** |
| `inverse(a)` | 逆行列 |
| `rank(a)` | 階数 |

`det` / `solve` / `inverse` / `rank` は**部分ピボット選択つきのガウス消去法**です。
各段で「その列で絶対値が最大の行」を選ぶので、素朴な消去法よりずっと安定します
（条件数の悪い 5×5 ヒルベルト行列でも `solve` が誤差 10⁻⁹ 以内で解けることを
テストで確かめています）。

> **⚠️ 連立方程式を解くだけなら `solve` を使ってください。**
> `inverse` を作ってから掛けるのは、遅いうえに誤差も大きくなります。

| 表示 | |
|---|---|
| `vec_str(v)` / `mat_str(a)` | 文字列にする |
| `vprint(v)` / `mprint(a)` | そのまま表示 |

```python
import linalg

def main() -> int:
    m: linalg.Matrix = linalg.from_rows([[1.0, 2.0], [3.0, 4.0]])
    print(str(linalg.det(m)))              # -2.0
    linalg.vprint(linalg.solve(m, [5.0, 11.0]))   # [1.0, 2.0]
    linalg.mprint(linalg.inverse(m))
    return 0
```

**形が合わなければ `panic` します。** 行列の積で列数と行数が食い違うのは
書いた人の間違いであって、実行時に回復できる状況ではないためです。

---

### 演算子

**`Matrix` は演算子で書けます。**

```python
c = a * b + d          # 行列の積 → 足し算
print(str(m[i, j]))    # 添字
m[i, j] += 1.0
```

| 書き方 | 意味 |
|---|---|
| `a + b` / `a - b` | `madd` / `msub`（形が違えば panic） |
| **`a * b`** | **行列の積**（`matmul`） |
| `-a` | `mneg` |
| `a == b` / `a != b` | 形と全要素が等しいか（⚠️ 誤差は考慮しません） |
| `m[i, j]` / `m[i, j] = v` | `get` / `set`（範囲検査つき） |

> **⚠️ numpy とは違います。** numpy の `a * b` は要素ごとの積、`a @ b` が
> 行列の積です。本言語に `@` は無いので、数学の書き方に合わせて
> **`*` を行列の積**にしました。**要素ごとの積は `mmul(a, b)`** です。

> **⚠️ スカラー倍（`a * 2.0`）は書けません。** 多重定義は左辺の型だけで
> 決まり、引数の型では選び分けないためです。`mscale(a, k)` を使ってください。

## stats

**Python の `statistics` に相当します。**

| 代表値 | |
|---|---|
| `mean` / `median` / `mode` | |
| `geometric_mean` / `harmonic_mean` | 正の数のみ |

| ばらつき | |
|---|---|
| `variance` / `stdev` | **標本**（`n-1` で割る） |
| `pvariance` / `pstdev` | **母集団**（`n` で割る） |

⚠️ **標本と母集団のどちらを使うかは、データの性質で決まります。**
手元のデータが「全体からの抜き取り」なら `variance`、「全体そのもの」なら
`pvariance` です。既定はありません。

| その他 | |
|---|---|
| `sorted(a)` | 昇順の**複製**を返す（元は壊しません）。マージソート |
| `sorted_by(a, less)` | 比較関数つき。`less(a, b)` が真なら `a` が先。**安定** |
| `quantile(a, q)` | 分位点（線形補間） |
| `covariance(x, y)` / `correlation(x, y)` | 2 変数 |
| `linear_regression(x, y)` | 最小二乗法。`[傾き, 切片]` を返す |

---

## random

**Python の `random` に相当します。** 自前の線形合同法です。

> **⚠️ 暗号には使えません。** 状態が見えれば次の値が完全に予測できます。
> シミュレーション・サンプリング・テストデータ用です。

| 関数 | 説明 |
|---|---|
| `seed(n)` | 種を決める。**同じ種なら同じ系列** |
| `random()` | `[0.0, 1.0)` の一様乱数 |
| `uniform(a, b)` | `[a, b)` の一様乱数 |
| `randint(a, b)` | `[a, b]` の一様な整数（**両端を含む**） |
| `chance(p)` | 確率 `p` で `True` |
| `gauss(mu, sigma)` | 正規分布（ボックス＝ミュラー法） |
| `expovariate(rate)` | 指数分布 |
| `shuffle(a)` | その場で並べ替え（フィッシャー＝イェーツ法） |
| `choice(a)` | 1 つ選ぶ |
| `sample_with_replacement(a, n)` | 復元ありで `n` 個 |

`randint` は**剰余による偏りを取り除いてあります**（範囲外に当たったら引き直す）。

---

## physics

**物理定数と単位換算。** 値は **CODATA 2018** の推奨値です。

⚠️ 2019 年の SI 改定で、光速・プランク定数・電気素量・ボルツマン定数・
アボガドロ定数は**定義値**になりました。

| 基礎定数 | |
|---|---|
| `c()` | 光速 [m/s]（厳密） |
| `h()` / `hbar()` | プランク定数 [J·s] |
| `elementary_charge()` | 電気素量 [C]（厳密） |
| `k_B()` / `N_A()` / `R()` | ボルツマン・アボガドロ・気体定数 |
| `G()` | 万有引力定数。⚠️ **測定値**で不確かさが大きい（2.2×10⁻⁵） |
| `mu_0()` / `epsilon_0()` / `coulomb()` | 電磁気 |
| `g_0()` / `atm()` | 標準重力加速度・標準大気圧（どちらも定義値） |
| `sigma_SB()` / `alpha()` / `rydberg()` | シュテファン＝ボルツマン・微細構造・リュードベリ |
| `electron_mass()` / `proton_mass()` / `neutron_mass()` / `atomic_mass()` | 質量 [kg] |

| 換算 | |
|---|---|
| `celsius_to_kelvin` / `kelvin_to_celsius` | 温度 |
| `fahrenheit_to_celsius` / `celsius_to_fahrenheit` | |
| `ev_to_joule` / `joule_to_ev` | エネルギー |
| `mass_to_energy` / `energy_to_mass` | `E = mc²` |
| `light_year()` / `au()` / `parsec()` | 長さ [m] |

| 関係式 | |
|---|---|
| `lorentz_factor(v)` / `relativistic_ke(m, v)` | 相対論 |
| `de_broglie(p)` / `photon_energy(wavelength)` | 量子 |
| `blackbody_flux(t)` | 黒体放射 `σT⁴` |
| `gravity(m1, m2, r)` / `orbital_velocity(m, r)` / `escape_velocity(m, r)` | 重力 |
| `schwarzschild_radius(m)` | |

```python
import physics

def main() -> int:
    print(physics.escape_velocity(5.972e24, 6.371e6))   # 11186.0（地球）
    print(physics.joule_to_ev(physics.photon_energy(550.0e-9)))  # 2.254（緑の光）
    return 0
```

**単位はすべて SI です。** 混ざると事故になるので、換算は必ず関数を通してください。

---

## numeric

**関数を受け取る数値計算。** 関数型（`fn(...) -> T`）が入って初めて書けるようになった
ものです。

| 積分 | |
|---|---|
| `trapezoid(f, a, b, n)` | 台形則。誤差は h² |
| `simpson(f, a, b, n)` | シンプソン則。誤差は h⁴。⚠️ `n` は偶数 |
| `integrate(f, a, b, tol)` | 分割を自動で増やす。⚠️ **収束は保証しません** |

| 求根 | |
|---|---|
| `bisect(f, a, b)` | 二分法。遅いが**必ず収束**。⚠️ 両端で符号が違うこと |
| `newton(f, df, x0, tol)` | ニュートン法。速いが収束しないことがある |
| `secant(f, a, b, tol)` | 割線法（導関数を渡さない版） |

| 微分 | |
|---|---|
| `derivative(f, x)` | 中心差分。**刻み幅は小さすぎると桁落ちします**（自動で選びます） |
| `derivative2(f, x)` | 2 階微分 |

| その他 | |
|---|---|
| `solve_ode(f, t0, y0, t1, n)` | `dy/dt = f(t, y)` を 4 次ルンゲ＝クッタ法で。各段の `y` を返す |
| `minimize(f, a, b, tol)` | 黄金分割探索。⚠️ 区間内の極小が 1 つだけであること |
| `map` / `filter` / `reduce` / `all` / `any` | 関数を渡す列の操作 |

```python
import numeric
import math

def sq(x: float) -> float:
    return x * x

def main() -> int:
    print(numeric.simpson(sq, 0.0, 1.0, 100))          # 0.333333
    print(numeric.bisect(math.cos, 0.0, 3.0))          # 1.570796（π/2）
    print(numeric.derivative(math.sin, 1.0))           # 0.540302（cos 1）
    return 0
```

⚠️ 渡す関数は **`raises` しないもの**に限ります（関数型はエラーの受け渡しを表しません）。

---

## set

**集合**（重複しない入れもの）です。中身は `dict.Dict[T, bool]` で、
**言語は 1 つも変えていません**。

```python
import set

def main() -> int:
    a: set.Set[int] = set.of([1, 2, 3, 3])
    b: set.Set[int] = set.of([3, 4])
    print(str(a.len()))                      # 3
    print(str(2 in a))                       # True
    print(str(set.union(a, b).len()))        # 4
    print(str(set.difference(a, b).len()))   # 2

    s: set.Set[str] = set.Set()      # ⚠️ 型引数は左辺の注釈が渡します
    s.add("x")
    return 0
```

| メソッド | 説明 |
|---|---|
| `len() -> int` | 個数 |
| `has(x: T) -> bool` / `x in s` | あるか |
| `add(x: own T) -> bool` | 入れる。返るのは「**新しく入ったか**」 |
| `remove(x: T) -> bool` | 消す。返るのは「あったか」 |
| `items() -> list[T]` | 中身。⚠️ **並びは入れた順ではありません** |

| 関数 | 説明 |
|---|---|
| `of(xs: list[T]) -> Set[T]` | リストから作る |
| `union` / `intersection` / `difference` | 和・積・差。**新しい集合を返します** |
| `is_subset(a, b) -> bool` / `equals(a, b) -> bool` | 包含・等しさ |
| `dedup(xs: list[T]) -> list[T]` | 重複を落とす。**元の並びのまま** |

⚠️ リテラル `{1, 2}` は書けません。⚠️ `|` `&` `-` の多重定義は入れていません
（演算子の意味は左辺の型で決まるので、別の型で違う意味になるのを避けました）。

---

## json

**JSON の読み書き**です。RFC 8259 の範囲だけを読みます
（末尾のカンマもコメントも読みません）。

```python
import json

def main() -> int:
    r: json.Result = json.parse("{\"n\": 1, \"xs\": [true, null]}")
    if not r.ok:
        print("読めません: " + r.error)
        return 1
    v: rc[json.Value] = r.get()
    print(str(v.field("n").as_int()))          # 1
    print(str(v.field("xs").at(0).as_bool()))  # True

    b: rc[json.Value] = json.object()
    b.put("name", json.of_str("world"))
    print(json.dumps(b))                       # {"name":"world"}
    return 0
```

| 読む | 説明 |
|---|---|
| `parse(text: str) -> Result` | 読む。**失敗しても panic しません** |
| `Result.ok: bool` / `error: str` / `at: int` | 結果・理由・何バイト目か |
| `Result.get() -> rc[Value]` | 読めた値（読めていなければ panic） |

| `Value` | 説明 |
|---|---|
| `is_null()` / `is_bool()` / `is_number()` / `is_string()` / `is_array()` / `is_object()` | 種類 |
| `as_bool()` / `as_float()` / `as_int()` / `as_str()` | 取り出す。⚠️ **種類が違えば panic** |
| `len()` / `at(i)` | 配列 |
| `get(key) -> rc[Value] \| None` / `field(key)` / `has(key)` | オブジェクト。`field` は無ければ panic |
| `push(x)` / `put(key, x)` | 組み立てる |

| 書く | 説明 |
|---|---|
| `dumps(v) -> str` | 1 行で書く |
| `dumps_pretty(v) -> str` | 2 文字字下げで整形して書く |
| `null()` / `of_bool` / `of_int` / `of_float` / `of_str` / `array()` / `object()` | 値を作る |

⚠️ **数はすべて `float` で持ちます**（JSON の数に整数・小数の区別が無いため）。
整数として読むときは `as_int()` を使ってください。書くときは、整数なら
`1.0` ではなく `1` と出します。

---

## pair

**タプルの代わり**です。タプル（`(int, str)`）はありません — ジェネリクスで
足りるからで、タプルが足すのは構文の短さだけです。

| 型 | 説明 |
|---|---|
| `Pair[A, B]` | `first` / `second` |
| `Triple[A, B, C]` | `first` / `second` / `third` |

```python
import pair

def divmod2(a: int, b: int) -> pair.Pair[int, int]:
    return pair.Pair(a // b, a % b)

def main() -> int:
    r: pair.Pair[int, int] = divmod2(17, 5)
    print(f"{r.first} 余り {r.second}")     # 3 余り 2
    return 0
```

⚠️ **分解代入（`a, b = f()`）は書けません。** `p.first` / `p.second` を使います。

⚠️ **所有権を受け取ります**（`own`）。入れ物の中身を組にして返すことは
できません（借りたものを渡せないため）。

---

## lists

**型を問わない**リスト操作です（ジェネリック関数）。`list[int]` も
`list[str]` も同じ関数で扱えます。

| 調べる | |
|---|---|
| `index_of(xs, x) -> int` | 最初の位置。無ければ `-1` |
| `contains(xs, x) -> bool` | 含むか |
| `count_if(xs, pred) -> int` | 条件を満たす数 |
| `all_of(xs, pred)` / `any_of(xs, pred)` | すべて／どれか |

| 取り出す | |
|---|---|
| `first_or(xs, default)` / `last_or(xs, default)` | 空なら `default` |
| `max_of(xs)` / `min_of(xs)` | ⚠️ 空なら `panic` |

| 順番を得る | |
|---|---|
| `sorted_indices(xs) -> list[int]` | 昇順に見るための**添字の並び**（安定） |
| `indices_where(xs, keep) -> list[int]` | 条件を満たす要素の**添字** |
| `reversed_indices(xs) -> list[int]` | 逆順の**添字** |

```python
import lists

def main() -> int:
    xs: list[str] = ["b", "a", "c"]
    for i in lists.sorted_indices(xs):
        print(xs[i])                     # a / b / c
    return 0
```

### ⚠️ なぜ「添字」を返すのか

**要素を別のリストに移すものは、汎用には書けません。**

```python
def filter[T](xs: list[T], keep: fn(T) -> bool) -> list[T]:
    out: list[T] = []
    for x in xs:
        if keep(x):
            out.append(x)     # ← E-BORROW-3
    return out
```

借りた要素を別の入れ物に保存することになり、`T` が所有型（`str` / `list` /
クラス）のときに壊れます。**添字なら `int`（コピー型）なので、中身は元の
リストが持ったまま**でいられます。

`list[float]` を実際に並べ替えたいときは `stats.sorted` を使ってください
（`float` はコピー型なので移せます）。

### ⚠️ 比較を伴うものは、比較できる型にしか使えません

`max_of` / `sorted_indices` / `index_of` は `<` や `==` を使います。
クラスを渡すと、実体化したときに「演算子が適用できません」と言われます
（型制約が無いためです）。

---

## time

**自分のプログラムの速さを測るためのモジュールです**。

```python
import time

def main() -> int:
    t0: int = time.monotonic_ns()
    heavy()
    print(str(time.since_ms(t0)) + " ms")
    return 0
```

| 関数 | 戻り値 | 内容 |
|---|---|---|
| `monotonic_ns()` | `int` | 単調時計（ナノ秒）。**起点に意味はありません** |
| `monotonic()` | `float` | 同じものを秒で |
| `now_ns()` | `int` | 実時刻（1970-01-01 UTC からのナノ秒） |
| `now()` | `float` | 同じものを秒で（Python の `time.time()` 相当） |
| `since_ns(start)` | `int` | `monotonic_ns()` の値からの経過ナノ秒 |
| `since_ms(start)` | `float` | 同じものをミリ秒で |
| `since_sec(start)` | `float` | 同じものを秒で |
| `ns_to_ms(ns)` / `ns_to_sec(ns)` | `float` | 単位の換算 |

> **⚠️ 速さを測るときは `monotonic_ns` を使ってください。**
> `now_ns`（実時刻）は **戻ることがあります**（NTP の補正・夏時間・
> 利用者が時計を直す）。測っている最中に戻ると経過時間が負になります。

> **⚠️ ベアメタル（`kernel/`）では使えません。** 時計は OS の持ち物なので、
> 実体は `runtime/hosted.c` にあります。他のモジュールと違い、
> このモジュールだけは `extern` の先が PC 用ランタイムに限られます。

**眠る手立て（`sleep`）はまだありません。** 並行実行と一緒に設計します。

---

## complex

**複素数です**。`lib/complex{{ext}}` は **本言語だけで書かれていて、
言語は 1 行も変えていません** — 0.7.0 の[演算子の多重定義](language-spec.md)で
`z1 * z2 + z3` がそのまま書けるようになったためです。

```python
import complex

def main() -> int:
    a: complex.Complex = complex.rect(3.0, 4.0)
    b: complex.Complex = complex.rect(1.0, -2.0)
    print((a * b).show())             # → 11.0-2.0i
    print(str(complex.mag(a)))        # → 5.0
    print(complex.sqrt(complex.rect(-1.0, 0.0)).show())   # → 0.0+1.0i
    return 0
```

| 作る | |
|---|---|
| `rect(re, im)` | 実部と虚部から |
| `polar(r, theta)` | 大きさと偏角から |
| `real(x)` / `zero()` / `one()` / `i()` | 実数から / 0 / 1 / 虚数単位 |
| `from_reals(xs)` | `list[float]` → `list[Complex]` |

| 演算子 | 意味 |
|---|---|
| `+` `-` `*` `/` | 複素数どうしの四則（⚠️ `/` は `mag2(o)` が 0 だと**実行時エラー**） |
| 単項 `-` | 符号反転 |
| `==` `!=` | 実部・虚部の一致（⚠️ 誤差は考慮しません。`isclose` を使ってください） |

| 関数 | |
|---|---|
| `mag(z)` / `mag2(z)` | 大きさ `\|z\|`（`hypot` なので途中であふれません）/ `\|z\|²` |
| `phase(z)` / `conj(z)` | 偏角（-π〜π）/ 共役 |
| `scale(z, k)` | **スカラー倍**（下の注意を見てください） |
| `exp` `log` `sqrt` `pow` `powi` `sin` `cos` `tan` | 初等関数（名前は Python の `cmath` に合わせています） |
| `reals` / `imags` / `mags` | `list[Complex]` から実部 / 虚部 / 大きさを取り出す |
| `isclose(a, b, tol)` | `\|a - b\| <= tol` |

> **⚠️ スカラー倍は `scale(z, k)` です。** `z * 2.0` とは書けません。
> 演算子の多重定義は**左辺の型だけ**で決まり、引数の型では選び分けないためです。

> **⚠️ 複素数はオブジェクトです**（`int` / `float` のような「値」ではありません）。
> 演算のたびに 1 つ確保します。内側ループで何万回も回すところは、
> 実部と虚部を `list[float]` 2 本で持つほうが速くなります。

---

## fft

**高速フーリエ変換**。`complex` の上に、やはり本言語だけで
書かれています。

```python
import fft

def main() -> int:
    xs: list[float] = [1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0]
    for v in fft.spectrum(xs):
        print(str(v))          # → 8.0, 0.0, 0.0, ...
    return 0
```

| 関数 | |
|---|---|
| `fft(zs)` / `ifft(zs)` | 順変換 / 逆変換（`ifft` が 1/N を掛けます） |
| `fft_real(xs)` | `list[float]` をそのまま変換 |
| `spectrum(xs)` | 振幅スペクトル（`\|X[k]\|` の並び） |
| `is_pow2(n)` | 長さが 2 のべき乗か |

> **⚠️ 長さは 2 のべき乗だけです**（基数 2 の Cooley–Tukey）。
> ほかの長さは `panic` します。0 埋めしてから渡してください。

> **⚠️ 速さより読みやすさを取っています。** 蝶形演算のたびに複素数を
> 確保します。大きな N を何度も変換するなら、実部と虚部を
> `list[float]` 2 本で持つ書き方のほうが速くなります。

---

## plot

**作図。SVG のテキストを書き出します**。ブラウザでそのまま開けます。

⚠️ **matplotlib のコードは見ていません。** SVG は W3C の公開仕様で、
関数名もこの言語の書き味に合わせて選んだものです。

```python
import math
import plot

def main() -> int:
    xs: list[float] = []
    ys: list[float] = []
    for i in range(64):
        t: float = float(i) / 8.0
        xs.append(t)
        ys.append(math.sin(t))

    p: plot.Plot = plot.Plot(640, 400)     # 幅・高さ（120 以上）
    p.title("正弦波")
    p.xlabel("t")
    p.ylabel("sin(t)")
    p.line("sin", xs, ys)                  # 名前を付けると凡例に出ます
    p.save("sine.svg")
    return 0
```

| 呼び出し | 中身 |
|---|---|
| `plot.Plot(w, h)` | 図を作る（`w`・`h` は 120 以上。⚠️ 小さすぎると panic） |
| `p.title(s)` / `p.xlabel(s)` / `p.ylabel(s)` | 見出しと軸の名前 |
| `p.line(name, xs, ys)` | 折れ線 |
| `p.scatter(name, xs, ys)` | 散布図 |
| `p.bar(name, labels, hs)` | 棒グラフ（`labels` が横軸の目盛り） |
| `p.hist(name, a, bins)` | ヒストグラム（度数を数えて棒にします） |
| `p.limits(x0, x1, y0, y1)` | 軸の範囲を固定（呼ばなければ自動） |
| `p.to_svg()` | SVG を文字列で返す |
| `p.save(path)` | ファイルに書く（⚠️ `io` を使うのでベアメタル不可） |

**決まりごと**

- 系列の名前が空でなければ**凡例が自動で出ます**。色は 6 色を順に使い回します。
- 目盛りは **1・2・5 × 10^n** から自動で選びます（軸は線形のみ。対数軸はまだ）。
- 棒グラフは **0 を必ず含みます**（高さの比較が狂わないように）。負の値も描けます。
- `xs` と `ys` の長さが違えば panic します。
- 見出しの `&` `<` `>` は自動で escape します。

---

## frame

**表形式のデータ**。CSV の読み書き・絞り込み・並べ替え・グループ集計。

⚠️ **pandas の設計は真似していません。** pandas の列は「何でも入る」ものですが、
この言語には暗黙の型変換がありません。**列は数値か文字列**で、
取り出すときに型を選びます。取り違えたら**その場で panic** します。

```python
import frame

def main() -> int:
    df: frame.Frame = frame.read_csv("sales.csv")
    print(frame.describe(df))
    print(df.show_head(5))

    hi: frame.Frame = df.where(frame.gt(df.num("price"), 150.0))
    g: frame.Frame = frame.group_by(df, "shop", "price", "sum")
    print(g.show())
    return 0
```

### 読む・書く

| 呼び出し | 中身 |
|---|---|
| `frame.read_csv(path)` | ファイルから読む |
| `frame.parse_csv(text)` | 文字列から読む |
| `frame.write_csv(df, path)` / `frame.to_csv(df)` | 書く |

**1 行目が見出し**です。列の型は**中身を見て決めます** — その列の値が全部
数として読めれば数値の列、そうでなければ文字列の列です（空の欄があれば文字列）。
引用符（`"…"`、`""` で `"` 1 個）とカンマ・改行を含む欄に対応しています。

### 形を見る・取り出す

| 呼び出し | 中身 |
|---|---|
| `df.rows()` / `df.width()` | 行数・列数 |
| `df.names()` | 列名の一覧 |
| `df.has(name)` / `df.index_of(name)` | 列があるか |
| `df.num(name)` | 数値の列を `list[float]` で（⚠️ 文字列の列なら panic） |
| `df.text(name)` | 文字列の列を `list[str]` で（⚠️ 数値の列なら panic） |
| `df.show()` / `df.show_head(n)` | 桁を揃えた表（**表示幅**で揃えます） |
| `frame.describe(df)` | 列ごとの平均・最小・最大・標準偏差 |

### 組み立てる・選ぶ

| 呼び出し | 中身 |
|---|---|
| `df.add_num(name, xs)` / `df.add_text(name, xs)` | 列を足す（⚠️ 行数が合わないと panic） |
| `df.where(keep)` | `keep: list[bool]` が `True` の行だけ |
| `df.take(idx)` | 行番号の並びのとおりに取り出す |
| `df.head(n)` | 先頭 n 行 |
| `df.sort_by(name)` / `sort_by_desc(name)` | 数値の列で並べ替え（**安定**） |

**印（`list[bool]`）を作る関数**：`frame.gt` / `ge` / `lt` / `le` / `eq_num`
（数値の列と定数）、`frame.eq_text`（文字列の列と定数）、
`frame.both` / `either` / `negate`（印どうしの論理演算）。

### まとめる

`frame.group_by(df, key, value, how)` — `key` は**文字列でも数値の列でも**よく、
`value` は数値の列です（返る表の鍵の列は元と同じ種類になります）。
`how` は `"mean"` / `"sum"` / `"count"` / `"min"` / `"max"`。
返るのは 2 列の新しい表で、**`key` の初出の順**に並びます。

**組み合わせた例**は [examples/sales_report{{ext}}](../../examples/sales_report{{ext}})
にあります（CSV → 集計 → 図）。

---

## decimal

**金額を `float` で持ってはいけません。** 二進の浮動小数点は `0.1` を
持てないので、足すたびに誤差が積もります（`0.1` を 10 回足すと
`0.9999999999999999`）。`decimal` は「整数の**単位数**と小数点以下の
**桁数**」で持つので**正確**です。

```python
import decimal

a: decimal.Decimal = decimal.Decimal(1045, 2)      # 10.45
b: decimal.Decimal = decimal.Decimal(299, 2)       # 2.99
print((a + b).to_str())                             # 13.44
print((a * b).to_str())                             # 31.2455（桁は足し算）
print(a.div_to(b, 4, decimal.HALF_EVEN).to_str())   # 3.4950
```

| 呼び方 | 意味 |
|---|---|
| `decimal.Decimal(units, scale)` | `units × 10^-scale`（`Decimal(1045, 2)` は `10.45`） |
| `decimal.from_int(n, scale)` / `decimal.zero(scale)` | 整数から / 0 |
| `decimal.parse(s)` | 文字列から（**読めなければ `None`**。float を経由しません） |
| `d.to_str()` | 文字にする |
| `d.rescale(scale, mode)` | 桁数を変える（減らすときは丸める） |
| `d.div_to(other, scale, mode)` | **割り算**（桁と丸め方を必ず書きます） |
| `d.cmp(other)` / `==` `!=` `<` `<=` `>` `>=` | 比べる（⚠️ `1.5 == 1.50`） |
| `+` `-` `*` `-`（単項） | 足す・引く・掛ける・符号を変える |
| `d.sign()` / `d.is_zero()` / `d.abs_of()` | 符号・0 か・絶対値 |
| `d.to_float()` | ⚠️ **誤差が入ります**（図を描くなど表示のためだけに） |

**丸め方**：`decimal.HALF_UP`（日常の四捨五入）/ `HALF_EVEN`（銀行家の丸め。
足し合わせても偏りません）/ `DOWN`（0 の方向へ）/ `UP`（0 から遠い方へ）。

⚠️ **既定の丸め方はありません。** 毎回選びます
（「気づかないうちに丸められていた」が金額のいちばん多い事故だからです）。

⚠️ 単位数は `int`（64 ビット）です。桁があふれたら**止まります**。
小数点以下は 18 桁までです。

---

## bytes

通信のフレーム・バイナリ形式・レジスタの写しのための、**固定幅の並べ方**です。
バイト列は `list[int]`（各要素 0..255）で持ちます。

```python
import bytes

buf: list[int] = []
bytes.put_u16_be(buf, 513)          # ネットワーク並び → 02 01
bytes.put_i32_le(buf, -2)           # 2 の補数 → fe ff ff ff
print(bytes.hex(buf))               # "02 01 fe ff ff ff"
print(str(bytes.get_i32_le(buf, 2)))  # -2
```

| 呼び方 | 意味 |
|---|---|
| `bytes.put_u8` / `put_u16_le` / `put_u16_be` / `put_u32_le` / `put_u32_be` | 符号なしを書く |
| `bytes.put_i8` / `put_i16_le` / `put_i32_le` / `put_i64_le` / `put_i64_be` | 符号つきを書く（2 の補数） |
| `bytes.get_u8` / `get_u16_le` / `get_u16_be` / `get_u32_le` / `get_u32_be` | 符号なしを読む |
| `bytes.get_i8` / `get_i16_le` / `get_i16_be` / `get_i32_le` / `get_i32_be` / `get_i64_le` / `get_i64_be` | 符号つきを読む |
| `bytes.from_str(s)` / `bytes.hex(buf)` | 文字列の中身をバイトに / 16 進で見る |

**幅の型**：`bytes.U8` / `U16` / `U32` / `I8` / `I16` / `I32` は
[範囲型](type-system.md#45-範囲型部分型a-28)です。

⚠️ **幅に合わない値は、入れる時点で止まります**（黙って切り詰めません）。
検査は library に 1 行も書いていません——引数の型が範囲型なので、
関数の入口で確かめられます。

```
runtime error: value out of range: U8 accepts 0..255 but got 300
```

⚠️ 64 ビットだけは符号つきのまま扱います（`int` がそれだからです）。
2^63 以上は、ビットの並びが同じ**負の数**として渡してください。

---

## blas

**BLAS**（数値線形代数の標準的な C ライブラリ）を呼びます。行列積のような
計算は、手で書いた版が敵う相手ではありません（512³ で **36 ms → 3 ms**）。

⚠️ **リンクの指定が要ります**（標準ライブラリには入っていません）。

```bash
{{cc}} -O2 app{{ext}} -framework Accelerate -o app   # macOS
{{cc}} -O2 app{{ext}} -lopenblas -o app              # Linux（または -lblas）
```

```python
import blas

def main() -> int:
    x: list[float] = [1.0, 2.0, 3.0]
    y: list[float] = [4.0, 5.0, 6.0]
    print(str(blas.dot(x, y)))                  # 32.0

    a: list[float] = [1.0, 2.0, 3.0, 4.0]       # 2x2（行優先の平たい list）
    b: list[float] = [5.0, 6.0, 7.0, 8.0]
    c: list[float] = blas.matmul_new(a, b, 2, 2, 2)
    return 0
```

| 呼び方 | 意味 |
|---|---|
| `blas.dot(x, y)` | 内積 |
| `blas.norm(x)` | ノルム（‖x‖₂） |
| `blas.axpy(a, x, y)` | `y ← a·x + y`（y を書き換えます） |
| `blas.scal(a, x)` | `x ← a·x`（x を書き換えます） |
| `blas.matmul(a, b, c, m, n, k)` | `C ← A·B`（c は呼ぶ側が用意します） |
| `blas.matmul_new(a, b, m, n, k)` | 同上。結果を新しく作って返します |

**★ 写しは 1 回も起きません。** `list[float]` の中身は 8 バイトの値が連続して
並んでいるので、C から見ればそのまま `double*` です。先頭の番地だけ渡します。

⚠️ **渡しているあいだ、その list を変えないでください。** `append` で伸びると
別の場所へ移ることがあり、C 側が見ている番地が古くなります。

⚠️ 行列は**行優先の平たい `list[float]`** です（`linalg.Matrix` からは
中身を取り出してから渡します）。列優先（Fortran 並び）は扱いません。
