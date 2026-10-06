# yashirolang

yashirolangはプログラミング言語です。
GC はありません。所有権と借用の検査でメモリ安全性を保証し、LLVM を通して機械語まで落とします。
コンパイラは C 版（`src/`）と yashirolang 版（`selfhost/`）の 2 つがあり、**セルフホストに到達しています**
（両者はバイト単位で同じ IR を出し、`make bootstrap` が stage2 == stage3 を確かめます）。

> **English:** Error messages can be shown in English — see [Use English error messages](#英語のエラーメッセージで使う--use-english-error-messages) below.
> English documentation starts at [docs/en/](docs/en/README.md).

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
断られます。詳しくは [docs/ja/design/tls.md](docs/ja/design/tls.md)。

### PATH に入れる

```bash
sudo make install                     # 既定は /usr/local
make install PREFIX=$HOME/.local      # 自分の環境だけに入れるなら

yashirolang --version                 # コンパイラ
ysm --version                         # パッケージマネージャも一緒に入ります
```

### 英語のエラーメッセージで使う / Use English error messages

エラーメッセージは**既定では日本語**です。英語の表（`msgs/en.tsv`）は配布物にも
`make install` にも入っているので、追加で入れるものはありません。**環境変数を 1 つ設定するだけ**です。

Error messages are in Japanese by default. The English messages ship with every install
(release archives and `make install` alike), so nothing extra is needed — just set one environment variable.

```bash
# 上のどれかの方法で入れたあと / after installing with any method above
echo 'export PLC_MSG_LANG=en' >> ~/.bashrc     # zsh なら ~/.zshrc / use ~/.zshrc for zsh
source ~/.bashrc

printf 'def main() -> int:\n    x: int = "a"\n    return 0\n' > bad.ys
yashirolang --check bad.ys
```

```
error: mismatched types
  --> bad.ys:2:14
   |
 2 |     x: int = "a"
   |              ^^^ expression of type 'str'
   |
note: variable 'x' is declared as type 'int'
  --> bad.ys:2:5
   |
 2 |     x: int = "a"
   |     ^
   |
   = help: there are no implicit type conversions (language spec 3.5)
```

| 設定 / Setting | 効果 / Effect |
|---|---|
| `PLC_MSG_LANG=en` | 常に英語 / always English |
| `PLC_MSG_LANG=auto` | ロケール（`LC_ALL` → `LC_MESSAGES` → `LANG`）が `ja` で始まれば日本語、それ以外は英語 / Japanese if your locale starts with `ja`, English otherwise |
| `yashirolang --lang=en …` | その 1 回だけ英語（環境変数より優先） / English for this run only (overrides the variable) |

- コンパイラの診断だけでなく、**作ったプログラムの実行時エラー**（`runtime error: index out of range: 5 (3)`）も、
  **動かすときの** `PLC_MSG_LANG` で英語になります。<br>
  Runtime errors of compiled programs follow `PLC_MSG_LANG` **at run time**, too.
- `ysm`・VS Code 拡張・IDE は同じ環境変数を受け継ぐので、設定は 1 か所で済みます。<br>
  `ysm`, the VS Code extension and the IDE inherit the same variable.
- 診断コード（`E-MOVE-1` など）は言語によらず同じです。<br>
  Diagnostic codes such as `E-MOVE-1` are the same in both languages.

★ 既定をロケールに合わせないのは、CI や他人の機械で黙って出る言語が変わらないようにするためです
（[docs/ja/design/i18n-diagnostics.md](docs/ja/design/i18n-diagnostics.md) §6.2）。

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

詳しくは [docs/ja/tutorial.md §7](docs/ja/tutorial.md#7-所有権と借用--この言語の中心) と
[docs/ja/spec/safety-spec.md](docs/ja/spec/safety-spec.md) にあります。

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

→ [docs/ja/reference/pkg.md](docs/ja/reference/pkg.md)

---

## ドキュメント

ドキュメントは言語ごとに分けてあります。**日本語が正本**で、英語は入口の文書から順に訳しています。

| | |
|---|---|
| [docs/ja/](docs/ja/README.md) | **日本語**（すべての文書） |
| [docs/en/](docs/en/README.md) | **English** — Getting started, CLI reference (more to come) |

**まずは [docs/ja/getting-started.md](docs/ja/getting-started.md)、次に [docs/ja/tutorial.md](docs/ja/tutorial.md)です。**

| | |
|---|---|
| [docs/ja/README.md](docs/ja/README.md) | ドキュメントの地図 |
| [docs/ja/getting-started.md](docs/ja/getting-started.md) | インストールから最初の 1 本まで |
| [docs/ja/tutorial.md](docs/ja/tutorial.md) | **言語ガイド** — Python との差分・所有権・並行・エラー処理 |
| [docs/ja/reference/](docs/ja/reference/) | コマンド・標準ライブラリ・パッケージマネージャ・数値計算・ネットワーク |
| [docs/ja/spec/](docs/ja/spec/) | 言語仕様（構文・型・安全性・文法） |
| [docs/ja/design/](docs/ja/design/) | 処理系の設計（使うだけなら不要） |

---

## ライセンス

**[Apache License 2.0](LICENSE)** — Copyright (c) 2026 by Contributors.

著作権表示は [NOTICE](NOTICE) にもあります。

例外が **TLS** で、`make TLS=1` で建てたときだけ **OpenSSL 3.x**
（Apache-2.0）に**リンクします**（ソースは含みません）。1.1.1 以前は旧
OpenSSL / SSLeay ライセンスで Apache-2.0 と両立しないため、ビルドの時点で
断ります。詳しくは [NOTICE](NOTICE) と
[docs/ja/design/tls.md](docs/ja/design/tls.md)。
