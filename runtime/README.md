# runtime/ — C 製ランタイムライブラリ

**core と hosted の 2 つに分けてあります。**

| ファイル | 中身 | ベアメタルで使うか |
|---|---|---|
| `core.h` | 環境ごとに違う 4 つの操作（フック）の宣言 | — |
| `core.c` | **libc に依存しない核**（文字列・リスト・rc・解放・print の組み立て） | 使う |
| `hosted.c` | フックの libc 実装 + ファイル入出力・argv・環境変数など | 使わない |

```
  ┌─────────────┐        ┌──────────────────┐
  │  core.c      │──呼ぶ──▶│ pl_hook_alloc     │  hosted.c（PC 上）
  │              │        │ pl_hook_free      │    → calloc / free / stdout
  │              │        │ pl_hook_write     │  kernel/（ベアメタル）
  │              │        │ pl_hook_panic     │    → 自前ヒープ / UART
  └─────────────┘        └──────────────────┘
```

**なぜ分けたのか**

v1 のランタイムは `printf` / `calloc` / `fopen` を直接呼んでいました。
ベアメタルにはそのどれもありません。かといって「OS 用のランタイム」をもう 1 本書くと、
同じ処理が 2 か所に増えて必ずずれます。
**libc に触る所だけを 4 つのフックに追い出せば、核は 1 本で済みます。**

生成される LLVM IR を単純に保つため、制御フローを含む処理はここに C 関数として置き、
IR 側は `call` 1 行にします（[../docs/design/ir-conventions.md](../docs/design/ir-conventions.md) 規約 R10）。

ビルド：`make`（`build/core.o` と `build/hosted.o` を作り、`ld -r` で `build/runtime.o` にまとめます）
