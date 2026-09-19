# yashirolang

**Python の書きやすさのまま、Rust の安全性を手に入れる**ことを目指すプログラミング言語と、
その処理系（C 言語 + LLVM の自作コンパイラ）です。最終的に **OS を書けること**を目標にしています。

```python
# examples/fizzbuzz.ys
def main() -> int:
    for i in range(1, 16):
        if i % 15 == 0:
            print("FizzBuzz")
        elif i % 3 == 0:
            print("Fizz")
        elif i % 5 == 0:
            print("Buzz")
        else:
            print(str(i))
    return 0
```

| | |
|---|---|
| 拡張子 | `.ys` |
| コンパイラ | `yashirolang`（C 実装 → **セルフホスト済み**） |
| バックエンド | LLVM IR を直接出力（テキスト） |
| 型付け | 静的・型注釈必須・実行時型情報なし |
| 現在地 | v2（安全性・エラー処理・共有所有）実装済み／**所有権の検査は既定でエラー**（Rust と同じ強さ）／**範囲型と契約**（Ada の部分型・Pre/Post）／**RISC-V のベアメタルで動作**・600 テスト |

---

## 使ってみる（Linux / macOS / Windows）

### 必要なもの

**clang だけです。** このコンパイラは LLVM IR のテキストを出力し、
アセンブルとリンクを clang に任せる作りなので、clang があればどの OS でも同じように動きます。

| OS | 入れるもの |
|---|---|
| **Linux** | `sudo apt install clang llvm make`（Debian / Ubuntu）<br>`sudo dnf install clang llvm make`（Fedora） |
| **macOS** | `xcode-select --install`（Apple clang で足ります） |
| **Windows** | [MSYS2](https://www.msys2.org/) を入れて、MINGW64 シェルで<br>`pacman -S mingw-w64-x86_64-clang mingw-w64-x86_64-lld make diffutils grep coreutils` |

> **⚠️ Windows は MSYS2（または WSL）の上で使ってください。**
> テストとビルドが bash と make に依存しているためです。
> WSL を使う場合は「Linux」の手順がそのまま使えます。

### A. 配布物をダウンロードして使う（ビルド不要）

[Releases](https://github.com/yashirolang/yashirolang/releases) から
OS に合う `.tar.gz` を取って展開します。

| ファイル | 対象 |
|---|---|
| `yashirolang-linux-x86_64.tar.gz` | Linux（x86_64） |
| `yashirolang-macos-universal.tar.gz` | macOS（**Intel / Apple Silicon 両対応**） |
| `yashirolang-windows-x86_64.tar.gz` | Windows（MSYS2 / MINGW64） |

```bash
tar xzf yashirolang-linux-x86_64.tar.gz
cd yashirolang-*

printf 'def main() -> int:\n    print("hello")\n    return 0\n' > hello.ys
./bin/yashirolang hello.ys -o hello
./hello                      # → hello
```

中身はこの 3 つだけです。**展開した場所がどこでも動きます**
（コンパイラが実行ファイルからの相対で標準ライブラリを探すため）。

```
bin/yashirolang
lib/plc/runtime.a
lib/plc/lib/*.ys        ← 標準ライブラリ（文字列・入出力・JSON・集合・時刻・数学・線形代数・複素数・FFT・作図・表・十進小数・バイト列 ほか）
```

### B. ソースからビルドする

```bash
git clone https://github.com/yashirolang/yashirolang.git
cd yashirolang
make                         # → build/yashirolang

./build/yashirolang examples/wordcount.ys -o wc
./wc examples/sample.txt
```

### C. インストールする（PATH に置く）

```bash
sudo make install            # 既定は /usr/local
make install PREFIX=$HOME/.local   # 自分の環境だけに入れるなら

yashirolang hello.ys -o hello  # どこからでも呼べる
yashirolang --version          # → yashirolang 0.27.0 (stage0)
ysm --version                 # パッケージマネージャも一緒に入ります
```

### D. ライブラリを使う（パッケージマネージャ `ysm`）

**レジストリはありません。** 依存は git のリポジトリを直に指します。

```bash
ysm init myapp
ysm add toml https://github.com/user/toml-pkg 1.2.0
ysm build                     # yashirolang -I deps main.ys -o myapp
```

```python
import toml.parser               # deps/toml/parser.ys
import json                      # 標準ライブラリ。**名前はぶつかりません**
```

`package.lock` が commit と tree の SHA で中身を固定するので、タグを
張り替えられても入ってくるものは変わりません。**インストール中に
パッケージのコードは 1 行も実行されません**（`git clone --bare` のあとは
`rev-parse` / `show` / `ls-tree` で読むだけで、作業ツリーを作らないため）。

→ [使い方](docs/reference/pkg.md) ／ [なぜこの設計か](docs/design/package-manager.md)

### よく使うコマンド

```bash
make test                  # 全テスト + 解放の検査 + セルフホスト比較 + ysm
make pm                    # パッケージマネージャ ysm をビルド
make coverage              # C 版コンパイラのカバレッジを測る
make stdlib-usage          # 標準ライブラリの API がどれだけ呼ばれているか
make bootstrap             # 3 段ビルドと不動点の検証（stage2 == stage3）
make dist                  # 配布用のディレクトリを build/dist に作る
make qemu-test             # ベアメタル（RISC-V）の検証
make info                  # 使っている clang・triple などの現在値
```

環境変数で差し替えられます。

| 変数 | 用途 |
|---|---|
| `PLC_CLANG` | 使う clang（`clang-18` など名前が違うとき） |
| `PLC_RUNTIME_O` / `PLC_LIB_DIR` | ランタイムと標準ライブラリの場所 |
| `make CC=gcc` | コンパイラ本体のビルドに使う C コンパイラ |

---

## ドキュメント

**言語の使い方を知りたい方は [docs/tutorial.md](docs/tutorial.md)（入門）から。**
30 分で読み切れます。

> ⚠️ `docs/` のファイルは**ひな型**です。言語名を持たせないために
> `{{cc}}` `{{ext}}` `{{pm}}` という合い言葉で書いてあります。
> 実際の名前が入った読みやすい形は `make docs` で `build/docs/` に出ます。

| | |
|---|---|
| [docs/README.md](docs/README.md) | ドキュメントの入口（全体の地図） |
| [docs/tutorial.md](docs/tutorial.md) | **入門** — 言語の使い方 |
| **[docs/reference/numerics.md](docs/reference/numerics.md)** | **数値計算の手引き** — numpy / matplotlib / pandas との対応表 |
| [docs/reference/net.md](docs/reference/net.md) | **ソケットと HTTP**（サーバーとクライアントの書き方） |
| [docs/reference/cli.md](docs/reference/cli.md) | `yashirolang` のオプション |
| [docs/reference/pkg.md](docs/reference/pkg.md) | パッケージマネージャ `ysm` の使い方 |
| [docs/spec/](docs/spec/) | **言語仕様** — 構文・型・安全性・標準ライブラリ |
| [docs/design/](docs/design/) | **処理系の設計** — パス構成・IR 規約・所有権検査 |
| [docs/roadmap.md](docs/roadmap.md) | 到達点と、これから入れるもの |

---

## 安全性の考え方

Rust の保証（use-after-free / 二重解放 / データ競合 / null の排除）を入れつつ、
**Rust の記法は持ち込みません**。書かせるのは `own` / `mut` / `raises` の 3 つだけです。

```python
def total(xs: list[int]) -> int:        # 引数は既定で「借用」。&Vec<i64> とは書かない
    s: int = 0
    for x in xs:
        s = s + x
    return s

def store(self, name: own str) -> None:  # 保存するときだけ own を書く
    self.name = name

def read_config(path: str) -> Config raises IOError:   # 失敗は型で宣言する
    ...
```

- ライフタイム注釈（`'a`）は**ありません** — 借用は呼び出しより長生きしない、という規則で代用します
- **検査は既定でエラーです**（0.18.0 から）。二重解放・解放後の使用の指摘は、
  オプションを 1 つも付けなくてもコンパイルを止めます。
  ⚠️ 逃げ道は `--warn-own`（警告に落とす）ですが、**付けたコードはこの保証の外**です
- **コンパイラ自身がこの検査を通ります**（`make own-strict`）。共有が要るところは `rc[T]` です
- **クラスのフィールドは、`init` の「どの経路でも」代入されているかを見ます**
  （Ada / SPARK の definite assignment にあたる検査。`if` の中だけの代入は止まります）
- **値の範囲を型で縛れます** — `type Percent = int range(0, 100)`（Ada の部分型）。
  入れるたびに確かめ、定数はコンパイル時に断ります。**外す手段はありません**
- **事前条件・事後条件を書けます** — `requires` / `ensures`（Ada の `Pre` / `Post`）。
  `ensures` の中の `result` は戻り値そのものです
- 例外はアンワインドしません — `try` / `except` は戻り値検査に落ちるので、**カーネルでも使えます**
- **`Send` / `Sync` に相当するトレイトもありません** — スレッドに渡せるかは
  「借りか、所有か、`rc` か」で決まるので、**上の 3 語のほかに書くものは増えません**

### 並行実行

```python
def rows(a: list[float], b: list[float], n: int, r0: int, r1: int) -> list[float]:
    ...                                # ★ a と b は借り。写しを取らない

scope:                                 # 出口で必ず join される
    ts.append(spawn(rows, a, b, n, 0, 128))
    ...
```

- スレッドに渡せるのは **`own` した値・`mutex[T]`・`scope:` の中の共有の借り**です。
  それ以外の借り・`rc[T]`・可変借用はコンパイルエラーになります（`E-SEND-1`〜`4`）。
  **2 つの実装（C 版・セルフホスト版）のどちらでも同じ検査をします**
- **`Send` / `Sync` に相当するトレイトはありません** — 判定に使うのは
  「借りか、所有か、`rc` か」だけなので、書くものが増えません
- `mutex[T]` は **`m.lock(f)` の形だけ**（生の lock/unlock が無いので解き忘れが起きません）
- 512×512 の行列積が **316 ms → 7 ms**（`examples/parallel_matmul.ys`）
- ⚠️ **`async` / `await` はありません。** 理由と、あとから
  **利用者のコードを変えずに**非同期を得る道は
  [docs/design/concurrency.md](docs/design/concurrency.md) §6 に書いてあります

### C のライブラリを繋ぐ

```bash
yashirolang -O2 app.ys -framework Accelerate -o app   # macOS
yashirolang -O2 app.ys -lopenblas -o app              # Linux
```

- `list[float]` の中身は**連続した double の並び**なので、C からはそのまま
  `double*` に見えます（**写しは 1 回も起きません**）
- 512³ の行列積で、手書き（ベクトル化つき）**36 ms** → BLAS **3 ms**
- `import blas` で `dot` / `norm` / `axpy` / `matmul` が使えます

### 証明（実行時検査を消す）

```bash
yashirolang --prove-report -S app.ys > /dev/null
```

区間解析で「必ず成り立つ」と示せた実行時検査だけを落とします。
**書くものは 1 行も増えません**（ループ不変条件も SMT ソルバも要りません）。

- コンパイラ自身（10,641 行）で**桁あふれ 211 個・添字 95 個**が消えます
- ⚠️ **速さは測定誤差の中**でした。値打ちは「示せた」こと自体と、契約の検査が
  コンパイル時に片付くことです
- `--verify-prove` は消せる検査を**残して**建て、外れたら
  「証明器が誤りました」で止めます（`make prove-verify`）

### デバッグ

```bash
yashirolang -g app.ys -o app     # デバッグ情報つきで建てる
lldb ./app                     # ブレークポイントもバックトレースも行で出ます
```

- 出るのは**関数の枠と行の対応表**です（変数の中身はまだ見られません）
- **`-g` を付けないときの出力は 1 バイトも変わりません**
- macOS では `<出力>.dSYM` も一緒に作ります（`dsymutil` を自動で走らせます）

### 速さ

- **コンパイルが並列に**。コンパイラ自身のビルドが 3.58 s → **1.45 s**
  （`-j1` で逐次に戻せます。⚠️ 出来上がる実行ファイルは同一）
- **数える形のループがベクトル化されるように**。添字の範囲を
  ループの外で 1 回だけ確かめ、検査と負の添字の正規化を落とします。
  512³ の行列積（平坦な `list[float]`）が **37.6 ms** — C の平坦配列 31.3 ms の
  1.2 倍です（以前は 6.4 倍）。
  ⚠️ **診断は変わりません** — 範囲外は今までどおり、その反復で同じメッセージで止まります
  ⚠️ **0.16.0〜0.17.1 のあいだ、この最適化は既定で効いていませんでした**
  （解放が既定になったとき、版分けの入口が諦めるようになっていたため。
  同じ行列積が 357.5 ms）。0.17.2 で直し、`vz_bounds.ys` と
  `drop_vz_loop.ys` が IR の構造で見張るようにしました

---

## ディレクトリ

```
src/        C 版コンパイラ（stage0）
selfhost/   セルフホスト版コンパイラ（stage1 以降）
runtime/    C 製ランタイム（core = libc 非依存／hosted = PC 用）
lib/        セルフホスト製の標準ライブラリ（数値計算を含む。libm は使わない）
kernel/     ベアメタル（RISC-V）のカーネル
tools/pm/   パッケージマネージャ（この言語で書かれている）
tests/      テストケースとテストランナー
docs/       言語仕様・処理系の設計・入門
```

## ベアメタルで動かす（RISC-V）

```bash
brew install llvm riscv64-elf-binutils qemu   # 必要な道具
make qemu                                     # QEMU で起動（Ctrl-A X で終了）
make qemu-test                                # 出力を自動で検証
```

```
=================================
 kernel on RISC-V (virt)
=================================
1 から 10 までの合計: 55
tick 1
tick 2
tick 3
3 回割り込みが来ました
```

**カーネル本体（`kernel/kernel.ys`）に `unsafe` は 2 か所だけ**です。
`print` も `for` も `list[str]` も、PC 上とまったく同じように書けます。


---

## ライセンス

**[Apache License 2.0](LICENSE)** です。

```
Copyright 2026 The yashirolang Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```
