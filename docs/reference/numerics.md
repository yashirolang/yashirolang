# 数値計算の手引き — numpy に相当することをどう書くか

**この文書の役目**：numpy / scipy で書いていた計算を yashirolang でどう書くかを、
対応表と実例で示します。**yashirolang の数値計算ライブラリは 100% セルフホスト製**で、
numpy のコードも API 設計も引き写していません（→ [8. ライセンスと出自](#8-ライセンスと出自)）。

関係する文書：[標準ライブラリ仕様](stdlib.md) ／
[チュートリアル](../tutorial.md) ／ [ロードマップ](../roadmap.md)

---

## 1. 3 分で全体像

```python
import linalg
import stats

def main() -> int:
    # ベクトルは list[float] そのもの
    x: list[float] = linalg.linspace(0.0, 1.0, 5)   # [0, 0.25, 0.5, 0.75, 1]
    y: list[float] = linalg.scale(x, 2.0)           # 各要素を 2 倍
    print(str(linalg.dot(x, y)))                    # 内積
    print(str(stats.mean(y)))                       # 平均

    # 行列は linalg.Matrix
    a: linalg.Matrix = linalg.from_rows([[1.0, 2.0], [3.0, 4.0]])
    b: linalg.Matrix = linalg.identity(2)
    c: linalg.Matrix = a * b                        # ★ * は行列の積
    print(linalg.mat_str(c))
    return 0
```

**要点は 3 つだけです。**

1. **ベクトルは `list[float]`** — 専用の型を作っていません。Python の list がそのまま使えます。
2. **行列は `linalg.Matrix`** — 1 本の `list[float]` を行優先で持つクラスです。
3. **`import` するだけ** — `lib/` から自動で見つかります。ビルド設定も依存解決もありません。

---

## 2. numpy → yashirolang 対応表

### 2.1 作る

| numpy | yashirolang | 備考 |
|---|---|---|
| `np.zeros(n)` | `linalg.zeros(n)` | |
| `np.ones(n)` | `linalg.ones(n)` | |
| `np.full(n, x)` | `linalg.full(n, x)` | |
| `np.linspace(a, b, n)` | `linalg.linspace(a, b, n)` | 両端を含む |
| `np.arange(a, b, s)` | `linalg.arange(a, b, s)` | 右端を含まない |
| `np.array([1.0, 2.0])` | `[1.0, 2.0]` | **リテラルがそのまま配列** |
| `a.copy()` | `linalg.vcopy(a)` | → [3.3 ビューが無い](#33-ビューが無いコピーだけ) |
| `np.zeros((r, c))` | `linalg.mzeros(r, c)` | |
| `np.eye(n)` | `linalg.identity(n)` | |
| `np.diag(d)` | `linalg.diag(d)` | |
| `np.array([[1.0,2.0],[3.0,4.0]])` | `linalg.from_rows([[1.0,2.0],[3.0,4.0]])` | |

### 2.2 要素ごとの演算（ベクトル）

| numpy | yashirolang |
|---|---|
| `a + b` | `linalg.add(a, b)` |
| `a - b` | `linalg.sub(a, b)` |
| `a * b` | `linalg.mul(a, b)` |
| `a / b` | `linalg.div(a, b)` |
| `a * k` | `linalg.scale(a, k)` |
| `a + k` | `linalg.offset(a, k)` |
| `-a` | `linalg.neg(a)` |
| `np.abs(a)` | `linalg.vabs(a)` |
| `np.sum(a)` | `linalg.vsum(a)` |
| `np.prod(a)` | `linalg.vprod(a)` |
| `a.min()` / `a.max()` | `linalg.vmin(a)` / `linalg.vmax(a)` |
| `a.argmin()` / `a.argmax()` | `linalg.argmin(a)` / `linalg.argmax(a)` |

注意: **`list[float]` に `+` は使えません。** `+` はリストの連結にも見えるため、
[§0 ②](../roadmap.md#②-安全性が下がるならその書きやすさは入れない)（取り違えるものは入れない）
から**関数名で書かせています**。行列（`Matrix`）は連結の意味を持たないので、
`+` `-` `*` を多重定義しています（→ 2.4）。

### 2.3 線形代数（ベクトル）

| numpy | yashirolang |
|---|---|
| `np.dot(a, b)` | `linalg.dot(a, b)` |
| `np.linalg.norm(a)` | `linalg.norm(a)` |
| `np.linalg.norm(a, 1)` | `linalg.norm1(a)` |
| `np.linalg.norm(a, np.inf)` | `linalg.norm_inf(a)` |
| `np.linalg.norm(a - b)` | `linalg.dist(a, b)` |
| `a / np.linalg.norm(a)` | `linalg.normalize(a)` |
| `np.cross(a, b)` | `linalg.cross(a, b)`（3 次元のみ） |
| — | `linalg.angle(a, b)`（なす角［ラジアン］） |

### 2.4 行列

| numpy | yashirolang | 備考 |
|---|---|---|
| `A @ B` | `A * B` または `linalg.matmul(A, B)` | **★ `*` は行列の積**（要素ごとではありません） |
| `A + B` / `A - B` | `A + B` / `A - B` | |
| `-A` | `-A` | |
| `A == B` | `A == B` | 完全一致（浮動小数の比較に注意） |
| `A[i, j]` | `A[i, j]` | 読み書き両方。範囲外は panic |
| `A * k` | `linalg.mscale(A, k)` | |
| `A @ v` | `linalg.matvec(A, v)` | |
| `A.T` | `linalg.transpose(A)` | |
| `np.trace(A)` | `linalg.trace(A)` | |
| `np.linalg.det(A)` | `linalg.det(A)` | LU 分解 |
| `np.linalg.solve(A, b)` | `linalg.solve(A, b)` | 部分ピボット選択つき Gauss 消去 |
| `np.linalg.inv(A)` | `linalg.inverse(A)` | |
| `np.linalg.matrix_rank(A)` | `linalg.rank(A)` | |
| `np.linalg.matrix_power(A, n)` | `linalg.mpow(A, n)` | 繰り返し二乗法 |
| `A.shape` | `A.rows` / `A.cols` | フィールドを直接読む |
| `A.size` | `A.size()` | |
| `print(A)` | `linalg.mprint(A)` / `linalg.mat_str(A)` | |

```python
import linalg

def main() -> int:
    a: linalg.Matrix = linalg.from_rows([[2.0, 1.0], [1.0, 3.0]])
    b: list[float] = [5.0, 10.0]
    x: list[float] = linalg.solve(a, b)      # A x = b を解く
    linalg.vprint(x)                         # [1.0, 3.0]
    print(str(linalg.det(a)))                # 5.0
    return 0
```

### 2.5 統計

| numpy / statistics | yashirolang |
|---|---|
| `np.mean(a)` | `stats.mean(a)` |
| `np.median(a)` | `stats.median(a)` |
| `np.var(a)`（母分散） | `stats.pvariance(a)` |
| `np.var(a, ddof=1)`（標本分散） | `stats.variance(a)` |
| `np.std(a)` | `stats.pstdev(a)` / `stats.stdev(a)` |
| `np.percentile(a, q*100)` | `stats.quantile(a, q)`（`q` は 0.0〜1.0） |
| `np.sort(a)` | `stats.sorted(a)` |
| — | `stats.sorted_by(a, less)`（比較関数を渡す） |
| `scipy.stats.gmean` / `hmean` | `stats.geometric_mean` / `harmonic_mean` |
| `statistics.mode` | `stats.mode(a)` |
| `np.cov(x, y)[0,1]` | `stats.covariance(x, y)` |
| `np.corrcoef(x, y)[0,1]` | `stats.correlation(x, y)` |
| `np.polyfit(x, y, 1)` | `stats.linear_regression(x, y)` → `[傾き, 切片]` |

### 2.6 数値解析（scipy 相当）

| scipy | yashirolang |
|---|---|
| `scipy.integrate.quad(f, a, b)` | `numeric.integrate(f, a, b, tol)`（適応 Simpson） |
| `scipy.integrate.trapezoid` | `numeric.trapezoid(f, a, b, n)` |
| `scipy.integrate.simpson` | `numeric.simpson(f, a, b, n)` |
| `scipy.optimize.bisect` | `numeric.bisect(f, a, b)` |
| `scipy.optimize.newton` | `numeric.newton(f, df, x0, tol)` / `numeric.secant(f, a, b, tol)` |
| `scipy.optimize.minimize_scalar` | `numeric.minimize(f, a, b, tol)`（黄金分割） |
| `scipy.integrate.solve_ivp` | `numeric.solve_ode(f, t0, y0, t1, n)`（RK4） |
| `np.gradient` 相当 | `numeric.derivative(f, x)` / `derivative2(f, x)` |
| `map` / `filter` / `reduce` | `numeric.map` / `filter` / `reduce`（`list[float]` 用） |
| `np.all` / `np.any` | `numeric.all(a, pred)` / `numeric.any(a, pred)` |

```python
import math
import numeric

def f(x: float) -> float:
    return math.sin(x)

def main() -> int:
    print(str(numeric.integrate(f, 0.0, math.pi(), 0.000001)))   # ≒ 2.0
    print(str(numeric.bisect(f, 3.0, 3.5)))         # ≒ π
    return 0
```

**関数を渡すのに lambda は要りません** — `fn(float) -> float` 型に `def` で定義した
関数名をそのまま渡します（クロージャは[意図的に保留](../roadmap.md)）。

### 2.7 複素数と FFT

| numpy | yashirolang |
|---|---|
| `1.0 + 2.0j` | `complex.rect(1.0, 2.0)` |
| `z1 * z2`, `z1 + z2` … | `z1 * z2`, `z1 + z2`（**演算子を多重定義済み**） |
| `abs(z)` | `complex.mag(z)` |
| `np.angle(z)` | `complex.phase(z)` |
| `z.conjugate()` | `complex.conj(z)` |
| `np.exp(z)` / `log` / `sqrt` / `sin` … | `complex.exp(z)` / `log` / `sqrt` / `sin` … |
| `np.fft.fft(a)` | `fft.fft(zs)` / `fft.fft_real(xs)` |
| `np.fft.ifft(a)` | `fft.ifft(zs)` |
| `np.abs(np.fft.fft(x))` | `fft.spectrum(xs)` |

注意: FFT の入力長は **2 の冪**に限ります（そうでなければ panic）。

### 2.8 乱数

| numpy | yashirolang |
|---|---|
| `np.random.seed(n)` | `random.seed(n)` |
| `np.random.random()` | `random.random()` |
| `np.random.uniform(a, b)` | `random.uniform(a, b)` |
| `np.random.randint(a, b)` | `random.randint(a, b)`（**両端を含む**） |
| `np.random.normal(mu, s)` | `random.gauss(mu, sigma)` |
| `np.random.exponential` | `random.expovariate(rate)` |
| `np.random.shuffle(a)` | `random.shuffle(a)`（`mut` 引数） |
| `np.random.choice(a)` | `random.choice(a)` |

注意: **暗号用途には使えません**（決定的な線形合同法です）。

### 2.9 数学関数と定数

`math` に libc 相当が揃っています（`sqrt` `exp` `log` `pow` `sin` `cos` `tan`
`asin` `acos` `atan` `atan2` `sinh` `cosh` `tanh` `floor` `ceil` `round`
`round_to` `fmod` `hypot` `cbrt` `ldexp` `gcd` `lcm` `degrees` `radians` …）。
定数は `math.pi()` `math.e()` `math.inf()` `math.nan()` のように**関数**です。

注意: **すべて自前実装です**（libc の `libm` を呼びません）。ベアメタルで動かすためです。
`physics` には光速・プランク定数などの物理定数が入っています。

### 2.10 作図（matplotlib 相当）

`lib/plot.ys` が **SVG** を書き出します。ブラウザでそのまま開けます。

| matplotlib | yashirolang |
|---|---|
| `plt.plot(x, y)` | `p.line("名前", xs, ys)` |
| `plt.scatter(x, y)` | `p.scatter("名前", xs, ys)` |
| `plt.bar(labels, h)` | `p.bar("名前", labels, hs)` |
| `plt.hist(a, bins)` | `p.hist("名前", a, bins)` |
| `plt.title` / `xlabel` / `ylabel` | `p.title(...)` / `p.xlabel(...)` / `p.ylabel(...)` |
| `plt.xlim` / `ylim` | `p.limits(x0, x1, y0, y1)` |
| `plt.legend()` | 系列に名前を付ければ自動で出ます |
| `plt.savefig("f.png")` | `p.save("f.svg")` / 文字列だけなら `p.to_svg()` |

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
    p: plot.Plot = plot.Plot(640, 400)
    p.title("正弦波")
    p.xlabel("t")
    p.line("sin", xs, ys)
    p.save("sine.svg")
    return 0
```

注意: **PNG は出しません**（zlib の実装が要るため）。画面に出す窓もありません。
軸は線形のみ（対数軸はまだ）。目盛りは 1・2・5 × 10^n から自動で選びます。

### 2.11 表形式のデータ（pandas 相当）

`lib/frame.ys` が CSV の読み書き・絞り込み・並べ替え・グループ集計を持ちます。

| pandas | yashirolang |
|---|---|
| `pd.read_csv(path)` | `frame.read_csv(path)` / `frame.parse_csv(text)` |
| `df["price"]` | `df.num("price")` / `df.text("name")`（**型を名前で選ぶ**） |
| `df[df.price > 150]` | `df.where(frame.gt(df.num("price"), 150.0))` |
| `df[(a) & (b)]` | `df.where(frame.both(a, b))` |
| `df.sort_values("price")` | `df.sort_by("price")` / `sort_by_desc` |
| `df.head(5)` | `df.head(5)` |
| `df.groupby("shop")["price"].sum()` | `frame.group_by(df, "shop", "price", "sum")`（鍵は文字列でも数値でも可） |
| `df.describe()` | `frame.describe(df)` |
| `print(df)` | `df.show()` / `df.show_head(n)` |
| `df.to_csv(path)` | `frame.write_csv(df, path)` |
| `df.shape` | `df.rows()` / `df.width()` |

注意: **列の型は取り出すときに選びます。** `df.text("price")` のように取り違えると
**その場で panic** します（pandas では黙って動いてしまう場面です）。
CSV の列の型は中身から決まります（全部数として読めれば数値、そうでなければ文字列）。

注意: 引用符（`"…"`、`""` で 1 個の `"`）とカンマ・改行を含む欄に対応しています。

**組み合わせた例**は [examples/sales_report.ys](../../examples/sales_report.ys)
にあります（CSV → 集計 → 図）。

---

## 3. numpy と違うところ（先に読んでください）

### 3.1 ブロードキャストがありません

`linalg.add(a, b)` は**長さが違えば panic** します。`a + 1.0` のような
形の違う演算は `linalg.offset(a, 1.0)` と**名前で区別**します。

> **なぜ入れないか**：ブロードキャストは「形が合わないコード」を黙って通します。
> [§0 ②](../roadmap.md#②-安全性が下がるならその書きやすさは入れない)に反します。

### 3.2 dtype がありません

要素は **`float`（64 ビット）固定**です。`list[int]` の計算は自分で書きます。
`int` と `float` は**暗黙に変換されません** — `float(n)` / `int(x)` と明示します。

```python
xs: list[float] = []
for n in range(10):
    xs.append(float(n))        # ★ float(n) が必要
```

文字列からは `float("3.14")` で作れます（**正しく丸めます**。
`float(str(x)) == x` が必ず成り立ちます）。読めない文字列は**黙って 0 にせず
panic** します — C の `atof` と違うところです。

### 3.3 ビューが無い（コピーだけ）

numpy の `a[2:5]` はビュー（元と同じメモリ）ですが、
yashirolang のスライス `a[2:5]` は**新しいリスト**です。
`linalg.transpose(A)` も `A.row(i)` も**コピー**を返します。

> **なぜ**：ビューは「知らないうちに書き換わる」原因になります。所有権の
> 検査（`own` / `mut` / 借用）とも噛み合いません。

### 3.4 `x / 0.0` は止まります（`inf` になりません）

numpy は `1.0 / 0.0` に警告を出して `inf` を返しますが、yashirolang は
**実行時エラー**です（Python の `ZeroDivisionError` と同じ考え方）。

```python
y: float = x / d              # d が 0.0 なら panic
z: float = math.inf()         # 無限大が欲しいときはこれ
```

IEEE754 のふるまいがどうしても要るときは `--no-overflow-check` を付けます
（整数の桁あふれ検査も一緒に外れます）。

### 3.5 添字は必ず検査されます

`a[i]` は範囲外なら**必ず panic** します（黙って隣を読みません）。
`A[i, j]` も同じです。この検査は**外せません**（`--no-overflow-check` で
外れるのは整数の桁あふれ検査だけで、範囲検査は残ります）。

### 3.6 速さの実情

| 計測（512×512 の行列積、`-O2`、Apple Silicon） | 時間 |
|---|---|
| C（素朴な三重ループ） | **31 ms** |
| yashirolang `list[list[float]]` | **220 ms** |
| yashirolang `linalg.Matrix` | 520 ms |

- **範囲検査の取り分は約 1/3** です（検査を実験的に外すと 220 → 145 ms）。
- 残りの差は**自動ベクトル化（SIMD）が効いていない**ことによります
  （検査を全部外しても SIMD 命令は 0 でした）。
- numpy は BLAS を呼ぶので、大きな行列積では **numpy のほうが速い**です。
  **正直に書いておきます。** 一方で、yashirolang は
  **起動が速く（インタプリタが要らない）**、**依存が無く**、
  **ベアメタルでも同じコードが動きます**。

### 3.7 書き方のコツ（速さ）

```python
# ✗ 遅い：ループ上限がグローバル変数だと、毎回読み直しになる
N: int = 512
for i in range(N):
    ...

# ✓ 速い：局所変数に写す（LLVM がループ回数を確定できる）
n: int = N
for i in range(n):
    ...
```

上の 512×512 行列積で **220 ms → 205 ms**（約 7%）でした。効き幅は小さいですが、
書き換えの手間はゼロです。

- 内側のループで `len(xs)` を呼ぶのは**問題ありません**（IR の load に展開済み）。
- `Matrix` より `list[list[float]]` のほうが今は速いです（クラス越しの添字が
  1 段深いため）。**性能が要る場所では素の list を使ってください。**

### 3.8 まだ無いもの

| 無いもの | 代わり |
|---|---|
| 3 次元以上の配列 | `list[list[list[float]]]` を自分で組む |
| 固有値・SVD・QR | `det` / `solve` / `inverse` / `rank` はあります |
| 疎行列 | 未定 |
| 対数軸・PNG 出力 | `plot` は線形軸・SVG のみ |
| `set` 型・辞書の非文字列キー | `dict`（`str` キー）を使う |
| 書式指定（`f"{x:>8.3f}"`） | `strings.lpad` / `math.round_to` |

---

## 4. 実例 — 最小二乗フィットと残差

```python
import linalg
import stats

def main() -> int:
    x: list[float] = linalg.arange(0.0, 10.0, 1.0)
    y: list[float] = []
    for i in range(len(x)):
        y.append(3.0 * x[i] + 1.0)          # 真の関係 y = 3x + 1

    ab: list[float] = stats.linear_regression(x, y)
    print("傾き=" + str(ab[0]) + " 切片=" + str(ab[1]))

    # 残差ベクトル
    pred: list[float] = linalg.offset(linalg.scale(x, ab[0]), ab[1])
    resid: list[float] = linalg.sub(y, pred)
    print("残差ノルム=" + str(linalg.norm(resid)))
    print("相関=" + str(stats.correlation(x, y)))
    return 0
```

## 5. 実例 — 信号のスペクトル

```python
import math
import fft

def main() -> int:
    n: int = 64
    xs: list[float] = []
    for i in range(n):
        t: float = float(i) / float(n)
        xs.append(math.sin(2.0 * math.pi() * 5.0 * t))   # 5 Hz

    sp: list[float] = fft.spectrum(xs)
    peak: int = 0
    for k in range(n // 2):
        if sp[k] > sp[peak]:
            peak = k
    print("ピーク周波数 = " + str(peak))                  # 5
    return 0
```

## 6. 実例 — 常微分方程式

```python
import numeric

# dy/dt = -2y（解は y = y0 e^(-2t)）
def f(t: float, y: float) -> float:
    return -2.0 * y

def main() -> int:
    ys: list[float] = numeric.solve_ode(f, 0.0, 1.0, 1.0, 100)
    print("y(1) ≒ " + str(ys[len(ys) - 1]))    # ≒ 0.1353
    return 0
```

---

## 7. マルチコアで計算する

**`spawn` で仕事を分けて、`join` で集めます。** numpy の裏で BLAS が勝手に
スレッドを使うのとは違い、**分け方は自分で書きます**（そのぶん、どこが並列に
走っているかがコードに見えます）。

```python
import sys

cores: int = sys.cpu_count()     # 使えるコア数
```

### 7.1 型付け

| 書くもの | 意味 |
|---|---|
| `spawn(f, x…)` | `f(x…)` を別スレッドで始める。返るのは `Thread[R]` |
| `t.join()` | 終わるまで待って `R` を受け取る |
| `scope:` | ブロックの出口で、始めたスレッドを全部 join する |

注意: `scope:` の外では、**渡した値は移動します**（渡したあとは触れません）。
中では借りを渡せます（下記）。

### 7.2 `scope:` で借りを共有する

**`scope:` ブロックの中では、スレッドに借りを渡せます。** 出口で必ず join
されるので、「借りは呼び出しより長生きしない」という規則が成り立ったままに
なるためです。注意: 写しを配る必要はありません。

```python
# ★ a と b は借り（読むだけ）。受け持ちぶんの結果だけを own で返す
def rows(a: list[float], b: list[float], n: int, r0: int, r1: int) -> list[float]:
    ...

scope:
    ts: list[Thread[list[float]]] = []
    p: int = 0
    while p < cores:
        ts.append(spawn(rows, a, b, n, n * p // cores, n * (p + 1) // cores))
        p = p + 1
    for th in ts:
        parts.append(th.join())
```

**動くもの全体は [examples/parallel_matmul.ys](../../examples/parallel_matmul.ys) にあります。**

注意: **書き込み先を共有することはできません**（`E-SEND-4`）。上のように
「各スレッドが自分のぶんを `own` で作って返す」形にしてください。
2 本のスレッドに同じものへの可変借用を渡すと、行が重なっていなくても
コンパイルが通りません（重なっていないことを静的に言えないためです）。

注意: `scope:` の**途中から抜けることはできません**（`return` など）。
結果を変数に受けてからブロックを出てください。

### 7.3 どのくらい速くなるか（512×512 の行列積、12 コア）

| | 時間 |
|---|---|
| 1 コア | 45 ms |
| 12 コア | **7 ms（6.4 倍）** |

全要素が 1 コアの結果と**ビット単位で一致する**ことを確かめています。

★ 1 コアの 45 ms 自体が、0.13.1 の 316 ms から **7 倍**速くなっています。
借りを共有できるようになって写しが消えたことと、
**ベクトル化が効くようになった**ことの合わせ技です
（[版の記録 0.13.2](../changelog.md)）。
素朴な三重ループなら、いま C と同じくらいの速さです。

### 7.4 共有して書き換えたいとき — `mutex[T]`

```python
def bump(c: mut Counter) -> int:
    c.n = c.n + 1
    return c.n

m: mutex[Counter] = mutex(Counter(0))
print(m.lock(bump))       # ロックを取り、bump(中身) を呼び、必ず解く
```

注意: 生の `lock` / `unlock` はありません（解き忘れが起きない形にしてあります）。
注意: **集計にしか使わないでください。** 計算の本体をロックの中に入れると、
コアを増やしても速くなりません。

### 7.5 `async` / `await` はありません

非同期は **I/O 待ちを重ねる**ための道具で、**CPU コアを増やす道具ではありません**。
数値計算に効くのは上の `spawn` のほうです。
入れていない理由と、あとから利用者のコードを変えずに非同期を得る道は
[design/concurrency.md](../design/concurrency.md) §6 にあります。

---

## 8. ライセンスと出自

**この数値計算ライブラリは全部この処理系のために書き下ろしたものです。**

- numpy / scipy / Eigen / BLAS などの**コードを写していません**。API 名も
  「Python 標準ライブラリ（`math` / `statistics` / `random`）に馴染みのある名前」を
  独自に選んだもので、numpy の API を再現する意図はありません
  （実際、上の対応表のとおり**名前も設計も違います**）。
- アルゴリズム（Gauss 消去、LU 分解、RK4、Cooley–Tukey FFT、黄金分割探索、
  Steele–White / Burger–Dybvig の桁生成）は**教科書に載っている公知の手法**で、
  著作権の対象ではありません。実装は自分で書いています。
- 外部ライブラリへのリンクはありません。**libm すら呼びません**
  （`math.ys` の `sin` / `exp` / `log` は級数と引数簡約による自前実装）。

したがって、この処理系とライブラリの配布に**第三者ライセンスの制約はありません**。
