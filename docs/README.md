# 本言語ドキュメント

**本言語** は、Python の文法をもつ静的型付けのコンパイル言語です。
GC を持たず、所有権と借用の検査でメモリ安全性を確保します。

```python
def main() -> int:
    for i in range(1, 4):
        print("hello " + str(i))
    return 0
```

---

## 使う

| ドキュメント | 内容 |
|---|---|
| **[tutorial.md](tutorial.md)** | **入門** — Python との差分に絞った案内。まずこれ |
| **[reference/numerics.md](reference/numerics.md)** | **数値計算の手引き** — numpy / scipy / matplotlib / pandas との対応表と実例 |
| [reference/cli.md](reference/cli.md) | `{{cc}}` のコマンドライン・リファレンス |
| [reference/pkg.md](reference/pkg.md) | パッケージマネージャ `{{pm}}` のリファレンス |
| [spec/stdlib.md](spec/stdlib.md) | 標準ライブラリ・リファレンス（文字列・入出力・**数学 / 線形代数 / 統計 / 乱数 / 物理 / 作図 / 表**） |

## 仕様

言語がどう振る舞うかの**唯一の正解**です。実装と食い違ったら、どちらかが間違いです。

| ドキュメント | 内容 |
|---|---|
| [spec/language-spec.md](spec/language-spec.md) | **言語仕様** — 字句・型・式・文・プログラム構造・組み込み関数 |
| [spec/safety-spec.md](spec/safety-spec.md) | **安全性の仕様** — 所有権・借用・可変性・解放・エラー処理・`unsafe` |
| [spec/type-system.md](spec/type-system.md) | **型システム** — 型の一覧と型付け規則 |
| [spec/grammar.md](spec/grammar.md) | **文法定義（EBNF）** |

## 設計

処理系（`{{cc}}`）がどう作られているかです。**言語を使うだけなら読む必要はありません。**

| ドキュメント | 内容 |
|---|---|
| [design/architecture.md](design/architecture.md) | パス構成とデータの流れ |
| [design/ir-conventions.md](design/ir-conventions.md) | LLVM IR の生成規約 |
| [design/memory-model.md](design/memory-model.md) | 値の表現・確保・寿命 |
| [design/ownership.md](design/ownership.md) | 所有権検査（`ownck`）の実装 |
| [design/error-handling.md](design/error-handling.md) | `raises` / `try` / `except` の実装 |
| [design/os-support.md](design/os-support.md) | freestanding・`unsafe`・ベアメタル |
| [design/self-hosting.md](design/self-hosting.md) | ブートストラップと不動点の検証 |
| [design/naming.md](design/naming.md) | 名前づけの規約 |
| [design/package-manager.md](design/package-manager.md) | パッケージマネージャ（レジストリを持たない設計・MVS・安全性） |

## これから

| ドキュメント | 内容 |
|---|---|
| **[roadmap.md](roadmap.md)** | **到達点と、これから入れるもの** — 機能ごとの優先度と設計 |
| [design/future-features.md](design/future-features.md) | 未実装機能の設計（ジェネリクス・クロージャ・多相ほか） |

## その他

| ドキュメント | 内容 |
|---|---|
| [reference/glossary.md](reference/glossary.md) | 用語集 |

---

## 実装の現在地

| | |
|---|---|
| 処理系 | C 実装（stage0）約 13,000 行 ＋ 本言語実装（stage1）約 9,000 行 |
| セルフホスト | **到達済み** — stage2 と stage3 がバイト単位で一致（不動点） |
| テスト | 531 件 ＋ 2 実装の出力比較 276 件 ＋ AddressSanitizer 12 件 ＋ `{{pm}}` 30 件 |
| 対応環境 | Linux / macOS / Windows（MSYS2）、RISC-V ベアメタル |
| CI | 4 ジョブすべて GitHub Actions で常時検証 |
