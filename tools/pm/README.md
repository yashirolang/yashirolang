# パッケージマネージャ

> ⚠️ `<cc>` はコンパイラ、`<pm>` はパッケージマネージャのコマンド名、`<ext>` はソースの拡張子です。
> 実際の値は `make info` で分かります（言語名をここに書き写さない理由は [docs/design/naming.md](../../docs/design/naming.md)）。


本言語で書かれています。ビルドするには本言語コンパイラが要ります。

```bash
make pm         # build/<pm> ができます
make pm-test    # 本物の git リポジトリを作って一通り動かします
```

使い方は [docs/reference/pkg.md](../../docs/reference/pkg.md)、
なぜこの形なのかは [docs/design/package-manager.md](../../docs/design/package-manager.md)。

## ファイルの構成

| ファイル | 役割 |
|---|---|
| `main` | 入口。コマンドの振り分けと `deps/` への展開 |
| `manifest` | `package.pkg` の読み書き |
| `lockfile` | `package.lock` の読み書き（**この設計の中心**） |
| `resolve` | 依存の版を決める（MVS） |
| `vcs` | git から中身を読む（**作業ツリーを作らない**） |
| `semver` | `1.2.3` の解釈と比較 |
| `shell` | 外部コマンドの実行・引用・一時ファイル |
| `words` | 行を語に分ける・識別子と SHA の検査 |

## 読むときの順番

1. **`vcs`** — 安全性の中身はほぼここにあります。`git clone --bare` の
   あとは `rev-parse` / `show` / `ls-tree` しか使いません。だから
   **インストール中にパッケージのコードは 1 行も実行されません**。
2. **`lockfile`** — commit と tree の SHA で中身を固定します。
   レジストリの「不変性」をこのファイルが肩代わりします。
3. **`resolve`** — MVS。なぜソルバを持たないのかがコメントにあります。

## 直すときの約束

- 外部コマンドは `shell` 以外から呼ばないでください。「何を実行するか」を
  1 ファイルで監査できる状態を保つためです。
- シェルに渡す語は必ず `shell.q()` で包んでください。
- 取ってきた名前は `words.is_ident()` を通してから使ってください。
- 所有権の検査 3 つ（`--deny-move` / `--deny-borrow` / `--deny-mut`）を
  通した状態を保ってください（`make pm` がそう建てます）。
