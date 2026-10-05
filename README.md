# yashirolang

yashirolangはプログラミング言語です。
GC はありません。所有権と借用の検査でメモリ安全性を保証し、LLVM を通して機械語まで落とします。
コンパイラは C 版（`src/`）と yashirolang 版（`selfhost/`）の 2 つがあり、**セルフホストに到達しています**
（両者はバイト単位で同じ IR を出し、`make bootstrap` が stage2 == stage3 を確かめます）。

---

## インストール

必要なのは **clang**です。

| OS | 入れるもの |
|---|---|
| Linux | `sudo apt install clang llvm make`（Debian / Ubuntu）<br>`sudo dnf install clang llvm make`（Fedora） |
| macOS | `xcode-select --install` |
| Windows | [MSYS2](https://www.msys2.org/) の MINGW64 シェルで<br>`pacman -S mingw-w64-x86_64-clang mingw-w64-x86_64-lld make diffutils grep coreutils` |

> 注意: Windows は MSYS2（または WSL）の上で使ってください。ビルドとテストが bash と make に依存しています。

### 配布物を使う（ビルド不要）

[Releases](https://github.com/yashirolang/yashirolang/releases) から OS に合う `.tar.gz` を取って展開します。
**展開した場所がどこでも動きます。**

```bash
tar xzf yashirolang-linux-x86_64.tar.gz
cd yashirolang-*

printf 'def main() -> int:\n    print("hello")\n    return 0\n' > hello.ys
./bin/yashirolang hello.ys -o hello
./hello                      # → hello
```

### ソースからビルドする

```bash
git clone https://github.com/yashirolang/yashirolang.git
cd yashirolang
make                                  # → build/yashirolang
./build/yashirolang examples/fizzbuzz.ys -o fizzbuzz
```

**★ 建てるのに要るのは clang だけです。** 外部のライブラリは使いません。

唯一の例外が **TLS（https）** で、これだけは任意です。

```bash
make TLS=1                            # OpenSSL 3.0 以上（Apache-2.0）を使います
```

既定（`make`）では入りません。入れずに建てた処理系で `https://` に
繋ごうとすると、**平文に落ちるのではなく**「TLS を組み込んでいません」と
断られます。詳しくは [docs/design/tls.md](docs/design/tls.md)。

### PATH に入れる

```bash
sudo make install                     # 既定は /usr/local
make install PREFIX=$HOME/.local      # 自分の環境だけに入れるなら

yashirolang --version                 # コンパイラ
ysm --version                         # パッケージマネージャも一緒に入ります
```

---

## 所有権と借用（覚えるのは 3 語）

**引数は既定で「借用」です。** 呼び出し側には何も書きません。

```python
def total(xs: list[int]) -> int:      # 借用。読むだけ
    s: int = 0
    for x in xs:
        s = s + x
    return s

def fill(xs: mut list[int], n: int) -> None:   # mut = 借りたものを書き換える
    xs.append(n)

def store(self, name: own str) -> None:        # own = 所有権を受け取る（持ち続ける）
    self.name = name

def main() -> int:
    xs: list[int] = [1, 2, 3]
    fill(xs, 4)                       # 呼び出し側に & も mut も書かない
    print(total(xs))                  # → 10
    return 0                          # xs はここで自動的に解放される
```

| 書くもの | 意味 | いつ書くか |
|---|---|---|
| （何も書かない） | **借用**。読める。呼び出しが終わるまでしか生きない | ほとんどの引数 |
| `own T` | **所有権をもらう**。保存しても返してもよい | 受け取った値をフィールドやリストに**しまうとき** |
| `mut T` | **借りたまま書き換える** | 引数の中身を変えるとき |

借りたものをしまおうとすると、コンパイルが止まります。

```python
class Node:
    name: str
    def init(self, name: str) -> None:
        self.name = name             # error[E-BORROW-3]: 借用した値 'name' を
                                     # フィールドに保存できません
                                     # → 'name: own str' にすると受け取れます
```

**1 つの値を 2 か所から持ちたい**ときだけ `rc[T]`（参照カウント）を使います。

```python
r: rc[Node] = rc(Node(7))
h.node = r                            # しまって、
return r                              # なおかつ返せる
```

詳しくは [docs/tutorial.md §7](docs/tutorial.md#7-所有権と借用--この言語の中心) と
[docs/spec/safety-spec.md](docs/spec/safety-spec.md) にあります。

---

## パッケージマネージャ `ysm`

**レジストリはありません。** 依存は git のリポジトリを直に指します。

```bash
ysm init myapp                                       # package.pkg を作る
ysm add toml https://github.com/user/toml-pkg 1.2.0  # 依存を足す（版を省くと最新のタグ）
ysm build                                            # yashirolang -I deps main.ys -o myapp
```

```python
import toml.parser        # deps/toml/parser.ys
import json               # 標準ライブラリ。名前はぶつかりません
```

| コマンド | すること |
|---|---|
| `ysm init [名前]` | `package.pkg` を作る |
| `ysm add <名前> <URL> [版]` | 依存を足して取ってくる |
| `ysm sync` / `ysm verify` | ロックのとおりに `deps/` を作る / 中身を確かめる |
| `ysm update [名前]` | 最新のタグまで上げる |
| `ysm build [引数…]` | コンパイルする（余分な引数はコンパイラへ渡ります） |

`package.lock` が commit と tree の SHA で中身を固定するので、タグを張り替えられても入ってくるものは変わりません。
**インストール中にパッケージのコードは 1 行も実行されません**（作業ツリーを作らず、`git show` / `git ls-tree` で読むだけです）。

→ [docs/reference/pkg.md](docs/reference/pkg.md)

---

## ドキュメント

**まずは [docs/getting-started.md](docs/getting-started.md)、次に [docs/tutorial.md](docs/tutorial.md)です。**

| | |
|---|---|
| [docs/README.md](docs/README.md) | ドキュメントの地図 |
| [docs/getting-started.md](docs/getting-started.md) | インストールから最初の 1 本まで |
| [docs/tutorial.md](docs/tutorial.md) | **言語ガイド** — Python との差分・所有権・並行・エラー処理 |
| [docs/reference/](docs/reference/) | コマンド・標準ライブラリ・パッケージマネージャ・数値計算・ネットワーク |
| [docs/spec/](docs/spec/) | 言語仕様（構文・型・安全性・文法） |
| [docs/design/](docs/design/) | 処理系の設計（使うだけなら不要） |

---

## ライセンス

**[Apache License 2.0](LICENSE)** — Copyright 2026 Shota Iwamoto.

著作権表示は [NOTICE](NOTICE) にもあります。

例外が **TLS** で、`make TLS=1` で建てたときだけ **OpenSSL 3.x**
（Apache-2.0）に**リンクします**（ソースは含みません）。1.1.1 以前は旧
OpenSSL / SSLeay ライセンスで Apache-2.0 と両立しないため、ビルドの時点で
断ります。詳しくは [NOTICE](NOTICE) と
[docs/design/tls.md](docs/design/tls.md)。
