# yashirolang ドキュメント / Documentation

| | |
|---|---|
| **[日本語](ja/README.md)** | すべての文書（言語ガイド・リファレンス・仕様・設計） |
| **[English](en/README.md)** | Getting started and the CLI reference. More documents are being translated. |

**日本語が正本です。** 英語は日本語の訳で、食い違ったときは日本語が正しいものとします。
英語の文書がまだ無いものは、英語の側から日本語の文書へリンクしています。

**Japanese is the source of truth.** The English documents are translations; if they disagree,
the Japanese version wins. Where no English version exists yet, the English index links to the Japanese one.

## 置き方 / Layout

```
docs/
├── ja/            日本語（正本）/ Japanese (source of truth)
│   ├── getting-started.md, tutorial.md, roadmap.md, changelog.md
│   ├── reference/  spec/  design/
└── en/            English（同じ構成で、訳したものから置く / same layout, filled in as translated）
    ├── getting-started.md
    └── reference/cli.md
```

英語に訳すときは、**日本語と同じパス**に置きます（`ja/reference/pkg.md` → `en/reference/pkg.md`）。
訳したら [en/README.md](en/README.md) の表のリンクを日本語版から英語版に差し替えます。

To translate a document, put it at **the same path** under `en/` and update the link in [en/README.md](en/README.md).
