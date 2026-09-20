# `ysm` リファレンス

yashirolang のパッケージマネージャです。**レジストリはありません。**
依存は git のリポジトリを直に指します。設計の理由は
[設計 §パッケージマネージャ](../design/package-manager.md) にあります。

```
ysm <コマンド> [引数…]
```

---

## はじめかた

```bash
ysm init myapp                                          # package.pkg を作る
ysm add json https://github.com/user/json-pkg 1.2.0      # 依存を足す
ysm build                                               # 実行ファイルを作る
```

`ysm add` の版を省くと、そのリポジトリでいちばん新しい `vX.Y.Z` タグを使います。

---

## コマンド

| コマンド | すること |
|---|---|
| `ysm init [名前]` | `package.pkg` を作る（既定の名前は `myapp`） |
| `ysm add <名前> <URL> [版]` | 依存を足し、解決して `deps/` に入れる |
| `ysm sync` | `package.lock` のとおりに `deps/` を作り直す |
| `ysm update [名前]` | 最新のタグまで上げてロックを書き直す |
| `ysm verify` | `deps/` の中身がロックと一致するか確かめる |
| `ysm list` | いま選ばれている版とモジュールを並べる |
| `ysm build [引数…]` | `yashirolang -I deps <entry> -o <name>` を実行する |
| `ysm clean` | `deps/` とキャッシュを消す |

余分な引数は `ysm build` からコンパイラへそのまま渡ります。

```bash
ysm build -O2                 # 所有権の検査は既定でエラーです（0.18.0〜）
ysm build --warn-own          # 古い依存を通したいときだけ
```

`ysm update` に名前を渡すと、**間接の依存でも**上げられます。そのとき
`package.pkg` に `dep` 行が足されます（MVS では「上げる」＝「下限の要求を
書き換える」なので、要求を記録する必要があるためです）。

---

## `package.pkg`

1 行 1 事実。`#` から行末はコメントです。

```
name    myapp                 パッケージ名（識別子）
version 0.1.0                 このパッケージの版
entry   main.ys               ysm build が渡す入口（ライブラリなら省略）
src     .                     .ys を置いてあるディレクトリ（既定 "."）

dep     json https://github.com/user/json-pkg 1.2.0
```

⚠️ `version` は **タグ `v1.2.3` と一致していなければなりません**。
食い違うパッケージは受け取りを断ります。

## `package.lock`

`ysm` が書きます。**git にコミットしてください。**
commit と tree の SHA で中身を固定するファイルで、中央のレジストリが
担っていた「一度公開した版は変わらない」保証をこれが肩代わりします。

```
lock 1
root httpx https://github.com/v/httpx-pkg 0.3.0   ← 宣言のうつし
pkg  httpx https://github.com/v/httpx-pkg 0.3.0 <commit> <tree>
mod  httpx                                        ← 入るモジュール
```

`root` が `package.pkg` の `dep` と食い違うと、`ysm sync` は解決し直します
（依存を消したときもこれで気づきます）。

---

## ライブラリを公開する

1. リポジトリの根に `package.pkg` を置き、`name` と `version` を書く。
2. `.ys` を置く（既定は根。別の場所なら `src`）。
3. `git tag v1.2.3` を打って push する。

これだけです。登録も申請も要りません。利用者は URL を `ysm add` に渡します。

**名前の心配は要りません**（A-32 から）。入るのは
`deps/<パッケージ名>/<モジュール>.ys` で、使う側は
`import <パッケージ名>.<モジュール>` と書きます。つまり `json` や `set` の
ような一般名でも、標準ライブラリとも他のパッケージともぶつかりません。

```python
import mytoml.toml          # deps/mytoml/toml.ys
import json                 # 標準ライブラリ。同じファイルに書けます

def main() -> int:
    t: mytoml.toml.Doc = mytoml.toml.parse("a = 1")
    return 0
```

⚠️ **パッケージの中でも、名前は完全に書きます。** `mytoml/toml.ys` から
同じパッケージの `mytoml/lex.ys` を使うときも `import mytoml.lex` です
（相対 import はありません。名前の出どころがソースから読み取れなくなるため）。

---

## 環境変数

| 変数 | 用途 |
|---|---|
| `PLC_CC` | `ysm build` が使うコンパイラ（既定 `yashirolang`） |
| `PLC_CACHE` | 取ってきたリポジトリの置き場（既定 `~/.cache/plc`） |

---

## 知っておくべきこと

- **`deps/` は `ysm` のものです。** `sync` のたびに作り直すので、手で置いた
  ファイルは消えます。自分のコードは `deps/` の外に置いてください。
- **インストール中にパッケージのコードは 1 行も実行されません。**
  `ysm` は作業ツリーを作らず、`git show` / `git ls-tree` で中身を読むだけです。
- **同じパッケージの 2 つの版は同居できません。** モジュール名が IR の
  名前修飾そのものだからです。版は MVS（要求された下限の最大）で 1 つに決まります。
- **置き場所は `deps/<パッケージ名>/<モジュール>.ys` です**（A-32）。
  パッケージごとにディレクトリが分かれるので、モジュール名の衝突は起きません。
- **タグは `vX.Y.Z` の形だけ**を見ます。`1.0.0-rc1` のようなプレリリースは
  受け付けません。
