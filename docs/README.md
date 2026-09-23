# yashirolang ドキュメント

**yashirolang** は、Python の文法をもつ静的型付けのコンパイル言語です。
GC を持たず、所有権と借用の検査でメモリ安全性を確保します。

```python
def main() -> int:
    for i in range(1, 4):
        print("hello " + str(i))
    return 0
```

```bash
yashirolang hello.ys -o hello && ./hello
```

---

## 読む順番

| | ドキュメント | 中身 |
|---|---|---|
| 1 | **[getting-started.md](getting-started.md)** | 入れる・動かす・プロジェクトにする |
| 2 | **[tutorial.md](tutorial.md)** | 言語のひととおり（Python との差分・所有権・並行・エラー処理） |
| 3 | [reference/](reference/) | 困ったときに引く（下記） |

---

## 引く（リファレンス）

| ドキュメント | 中身 |
|---|---|
| [reference/cli.md](reference/cli.md) | コンパイラ `yashirolang` のオプション |
| [reference/pkg.md](reference/pkg.md) | パッケージマネージャ `ysm` と `package.pkg` |
| [reference/stdlib.md](reference/stdlib.md) | 標準ライブラリ（文字列・入出力・`dict` / `set` / `json`・数値計算・作図・表 …） |
| [reference/numerics.md](reference/numerics.md) | 数値計算の手引き（numpy / scipy / matplotlib / pandas との対応表） |
| [reference/net.md](reference/net.md) | ソケット・TLS・HTTP（サーバーとクライアント） |

## 決まりごと（仕様）

言語がどう振る舞うかの**唯一の正解**です。実装と食い違ったら、どちらかが間違いです。

| ドキュメント | 中身 |
|---|---|
| [spec/language-spec.md](spec/language-spec.md) | 字句・型・式・文・プログラム構造・組み込み関数 |
| [spec/safety-spec.md](spec/safety-spec.md) | 所有権・借用・可変性・解放・エラー処理・`unsafe` |
| [spec/type-system.md](spec/type-system.md) | 型の一覧と型付け規則 |
| [spec/grammar.md](spec/grammar.md) | 文法定義（EBNF） |

## 処理系の設計

**言語を使うだけなら読む必要はありません。** コンパイラに手を入れる人向けです。

| ドキュメント | 中身 |
|---|---|
| [design/architecture.md](design/architecture.md) | パス構成とデータの流れ |
| [design/ir-conventions.md](design/ir-conventions.md) | LLVM IR の生成規約 |
| [design/memory-model.md](design/memory-model.md) | 値の表現・確保・寿命 |
| [design/ownership.md](design/ownership.md) | 所有権検査（`ownck`）の実装 |
| [design/generics-and-interfaces.md](design/generics-and-interfaces.md) | ジェネリクス（単相化）・インタフェース（vtable）・演算子の多重定義 |
| [design/error-handling.md](design/error-handling.md) | `raises` / `try` / `except` の実装 |
| [design/closures.md](design/closures.md) | 捕獲（クロージャ）を入れるなら、どの表現にするか（まだ入れていません） |
| [design/enum-payload.md](design/enum-payload.md) | 中身を持つ枝の表現（枝ごとの隠しクラス＋先頭のタグ） |
| [design/concurrency.md](design/concurrency.md) | スレッド・`mutex[T]`・送出可能性の検査 |
| [design/package-manager.md](design/package-manager.md) | レジストリを持たない設計・MVS・安全性 |
| [design/tls.md](design/tls.md) | 暗号を自分で書かない理由・任意ビルド・ライセンスの整理 |
| [design/os-support.md](design/os-support.md) | freestanding・`unsafe`・ベアメタル |
| [design/self-hosting.md](design/self-hosting.md) | ブートストラップと不動点の検証 |
| [design/naming.md](design/naming.md) | 言語名の扱いと改名の手順 |
| [design/ci.md](design/ci.md) | CI の作りと、速くするために入れた工夫 |

## これまでとこれから

| ドキュメント | 中身 |
|---|---|
| [roadmap.md](roadmap.md) | 現在地・これから入れるもの・入れないと決めたもの |
| [changelog.md](changelog.md) | 版ごとの変更 |
