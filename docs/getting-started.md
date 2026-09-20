# はじめの 5 分

yashirolang を入れて、最初のプログラムを動かすところまでです。
言語そのものの説明は [tutorial.md](tutorial.md) にあります。

---

## 1. 入れる

必要なのは **clang だけ**です。yashirolang は LLVM IR のテキストを出力し、
アセンブルとリンクを clang に任せるので、clang がある環境ならどこでも同じように動きます。

| OS | 入れるもの |
|---|---|
| Linux | `sudo apt install clang llvm make`（Debian / Ubuntu）<br>`sudo dnf install clang llvm make`（Fedora） |
| macOS | `xcode-select --install`（Apple clang で足ります） |
| Windows | [MSYS2](https://www.msys2.org/) の MINGW64 シェルで<br>`pacman -S mingw-w64-x86_64-clang mingw-w64-x86_64-lld make diffutils grep coreutils` |

> ⚠️ Windows は MSYS2（または WSL）の上で使ってください。ビルドとテストが bash と make に依存しています。
> WSL なら Linux の手順がそのまま使えます。

そのうえで、次のどちらかを選びます。

**A. 配布物を展開する（ビルド不要）**

[Releases](https://github.com/yashirolang/yashirolang/releases) から OS に合う `.tar.gz` を取ります。

```bash
tar xzf yashirolang-linux-x86_64.tar.gz
cd yashirolang-*
./bin/yashirolang --version
```

**展開した場所がどこでも動きます**（コンパイラが実行ファイルからの相対で標準ライブラリを探します）。

**B. ソースから建てる**

```bash
git clone https://github.com/yashirolang/yashirolang.git
cd yashirolang
make                      # → build/yashirolang
make test                 # 通ることを確かめる（任意）
```

**PATH に置くなら:**

```bash
sudo make install                    # 既定は /usr/local
make install PREFIX=$HOME/.local     # 自分の環境だけに入れるなら
```

コンパイラ `yashirolang` とパッケージマネージャ `ysm` の 2 つが入ります。

---

## 2. 動かす

```python
# hello.ys
def main() -> int:
    print("hello, world")
    return 0
```

```bash
$ yashirolang hello.ys -o hello
$ ./hello
hello, world
```

- **`main` は必須です。** Python のようにトップレベルへ処理は書けません。
- `main` の戻り値の**下位 8 ビット**がプロセスの終了コードになります。
- 出力ファイル名を省くと `a.out` になります。

よく使うオプションはこれだけです（残りは [reference/cli.md](reference/cli.md)）。

| | |
|---|---|
| `-o <file>` | 出力する実行ファイル名 |
| `--check` | 型と安全性の検査だけ行う（実行ファイルを作らない） |
| `-O2` | 最適化して建てる |
| `-g` | デバッガ（lldb / gdb）で追えるようにする |

---

## 3. 少し大きいものを動かす

リポジトリの `examples/` がそのまま動きます。

```bash
$ yashirolang examples/fizzbuzz.ys -o fizzbuzz && ./fizzbuzz
$ yashirolang examples/wordcount.ys -o wc && ./wc examples/sample.txt
$ yashirolang -O2 examples/parallel_matmul.ys -o matmul && ./matmul
```

| ファイル | 中身 |
|---|---|
| `examples/fizzbuzz.ys` | いちばん小さい例 |
| `examples/wordcount.ys` | ファイル入出力・`dict`・文字列操作 |
| `examples/sales_report.ys` | CSV を読んで集計し、SVG の図を書く |
| `examples/parallel_matmul.ys` | `spawn` / `join` による並列化 |
| `examples/webserver.ys` / `webclient.ys` | HTTP サーバーとクライアント |

---

## 4. プロジェクトにする（`ysm`）

ファイルが増えてきたら、パッケージマネージャを使います。**レジストリはありません** —
依存は git のリポジトリを直に指します。

```bash
$ ysm init myapp                # package.pkg を作る（入口は main.ys）
$ printf 'def main() -> int:\n    print("hi")\n    return 0\n' > main.ys
$ ysm build                     # → ./myapp
$ ./myapp
hi
```

他人のライブラリを足すとき:

```bash
$ ysm add toml https://github.com/user/toml-pkg 1.2.0   # 版を省くと最新のタグ
$ ysm build
```

```python
import toml.parser        # deps/toml/parser.ys が入ります
import json               # 標準ライブラリ。名前はぶつかりません
```

`package.lock`（`ysm` が書きます）を git にコミットしてください。commit と tree の SHA で
中身を固定するので、タグを張り替えられても入ってくるものは変わりません。

→ 詳しくは [reference/pkg.md](reference/pkg.md)

---

## 5. つまずいたら

| 症状 | 見るところ |
|---|---|
| `clang が見つかりません` | clang を入れるか、`PLC_CLANG=clang-18` のように使う clang を指定します |
| 標準ライブラリが見つからない | `yashirolang --print-lib-dir` で探し先を確認します（`PLC_LIB_DIR` で上書きできます） |
| `error[E-BORROW-…]` / `error[E-MOVE-…]` | 所有権の検査です。[tutorial.md §7](tutorial.md#7-所有権と借用--この言語の中心) を読んでください |
| 型エラーの意味がわからない | [spec/language-spec.md](spec/language-spec.md) に規則があります |

---

**次は [tutorial.md](tutorial.md) です**（30 分で読み切れます）。
