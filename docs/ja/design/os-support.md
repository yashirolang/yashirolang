# OS 開発に向けた設計（仕様先行・実装は後半）

> 最終目標は **yashirolang で書かれた OS がベアメタルで動く**ことです。
> **今の設計判断が将来の手戻りになる**ため、
> 「何が必要か」と「今から守ること」を先に決めます。

---

## 1. ゴールの定義

| 段階 | 到達点 |
|---|---|
| G1 | `unsafe:` と `ptr[T]` で、生メモリを触るコードが書ける |
| G2 | libc・ホスト OS に依存しない実行ファイルが作れる（`pragma no_runtime`） |
| G3 | QEMU 上で「シリアルに文字を出す」ベアメタルバイナリが動く |
| G4 | 割り込み・ページング・キーボード入力を持つ最小カーネル |

---

## 2. いま何が邪魔をしているか

| 依存 | 現状 | G2 で必要な形 |
|---|---|---|
| メモリ確保 | `pl_alloc` が `calloc` を呼ぶ | 確保器を差し替え可能にする（後述 §4） |
| 出力 | `pl_print_*` が `printf` | `print` は使用不可。シリアル出力は自分で書く |
| 終了 | `pl_exit` が `exit` | `panic` フックを差し替え可能にする |
| 文字列・リスト | ヒープ前提 | 確保器の差し替えで動く（**言語仕様は変えない**） |
| リンク | `clang out.ll runtime.o` | リンカスクリプト + `-nostdlib` + カスタム entry |
| ターゲット | ホストの triple 固定 | `pragma target "x86_64-unknown-none"` |
| エラー処理 | — | アンワインドしない設計なので**そのまま使える**（[error-handling.md](error-handling.md)） |
| 所有権・drop | — | ランタイム非依存なので**そのまま使える** |

**★ 安全機能（所有権・借用・drop・raises）を先に作るのが正しい順序です。**
どれもランタイムに依存せず、むしろ OS を書くために必要なものだからです。

---

## 3. プロファイル

```python
pragma profile freestanding
pragma target "x86_64-unknown-none"
```

| プロファイル | 既定 | ランタイム | 禁止されるもの |
|---|---|---|---|
| `hosted` | 済 | `libpl_hosted`（libc あり） | なし |
| `freestanding` | | `libpl_core`（libc なし） | `print`, `io.*`, `sys.*`, 既定の確保器 |

`freestanding` でコンパイルすると、禁止された組み込みを使った時点で
`E-UNSAFE-10: このプロファイルでは 'print' は使えません` になります。

---

## 4. ランタイムの 2 分割（★ 今から効いてくる決定）

```
  runtime/
    core.c      ← libc に依存しない部分（memcpy 相当・list/str の操作・panic フック）
    hosted.c    ← libc を使う部分（print・ファイル・環境変数・既定の確保器）
```

- `core.c` は `malloc` / `printf` / `exit` を**呼ばない**。必要な機能は関数ポインタで外から与える

```c
// core.c が要求する 3 つのフック（hosted.c か、OS 自身が実装する）
extern void *pl_hook_alloc(long long size);
extern void  pl_hook_free(void *p);
extern _Noreturn void pl_hook_panic(const char *msg);
```

**注意: `runtime/runtime.c` に新しい関数を足すときは
「これは core か hosted か」を意識して書きます**（drop の実装は core 側）。

---

## 5. 低レベル機能の仕様スケッチ

### 5.1 `unsafe:`

```python
unsafe:
    <文の並び>
```

中でだけ許されること：`ptr[T]` の生成・参照外し・演算、`volatile` アクセス、`asm`、
型の再解釈（`transmute`）、`extern` 関数の宣言と呼び出し。

**借用検査・ムーブ検査は `unsafe:` の中でも働きます。** 止まるのは「ポインタ操作の禁止」だけです。

### 5.2 `ptr[T]`

| 操作 | 書き方 |
|---|---|
| 参照外し（読み） | `p[0]` |
| 参照外し（書き） | `p[0] = v` |
| 加算 | `p + 1`（要素単位。C と同じ） |
| 整数化 | `int(p)` / `ptr[int](addr)` |
| volatile | `volatile_load(p)` / `volatile_store(p, v)` |

`ptr[T]` は自動解放されません（drop 対象外）。

### 5.3 アセンブリと属性

```python
@naked                       # プロローグ/エピローグを出さない
def _start() -> None:
    unsafe:
        asm("mov $stack_top, %rsp")
        asm("call kmain")

@interrupt                   # 割り込みハンドラの ABI で生成する
def timer(frame: ptr[int]) -> None: ...

@section(".mb_header")       # 出力セクションの指定
GRUB_MAGIC: int = 0x1BADB002
```

### 5.4 静的メモリ

OS の初期化では、ヒープが使えるようになる前に固定領域が要ります。

```python
@static(4096)
STACK: ptr[int] = ...        # .bss に 4096 バイト確保して先頭を指す
```

---

## 6. ビルドとテスト

```
$ yashirolang kernel.ys --target x86_64-unknown-none -c -o kernel.o
$ ld.lld -T linker.ld -nostdlib kernel.o core.o -o kernel.elf
$ qemu-system-x86_64 -kernel kernel.elf -serial stdio -display none
```

**テストの形**：QEMU をシリアル出力付きで起動し、期待する文字列が出たら合格。
既存の `tests/run_tests.sh` と同じ「期待値をファイル先頭のコメントに書く」方式を踏襲します。

```python
# QEMU: Hello from yashirolang
# EXIT: 0
pragma profile freestanding
```

---

## 7. 今から守る 6 つの約束（手戻り防止）

1. **生成 IR に libc を前提とした構造を埋め込まない**（`printf` 直呼びなど）
2. **新しいランタイム関数は core / hosted のどちらかを決めてから書く**
3. **target triple は `PLC_TARGET_TRIPLE` 経由で外から差し替えられる状態を保つ**（すでにそうなっている）
4. **`panic` はランタイム関数 1 本（`pl_panic`）に集約する**（差し替え点を増やさない）
5. **アンワインドを必要とする仕様を入れない**（エラー処理は戻り値検査。§ [error-handling.md](error-handling.md)）
6. **`rc[T]` や `list` の内部で確保器を直接呼ばない**（必ず `pl_alloc` 経由。差し替え 1 点に集約）

---

## 時計

`lib/time.ys` の実体は `runtime/hosted.c` にあります（**ベアメタルには
ありません**。時計は OS の持ち物です）。

| 環境 | 使うもの |
|---|---|
| macOS / Linux | `clock_gettime(CLOCK_MONOTONIC)` |
| MSYS2（mingw-w64） | 同上（mingw-w64 が持っています） |
| どれも無いとき | `timespec_get`（あれば）→ `time(NULL)`（秒の精度） |

> **注意: 「C11 の標準だからどこにでもある」は成り立ちません。**
> `timespec_get` / `TIME_UTC` は C11 で標準になりましたが、**MSYS2 の clang は
> `-std=c11` でも持っていません**（CI の Windows ジョブが
> 「call to undeclared function 'timespec_get'」で見つけました）。
> **`#if defined(TIME_UTC)` で確かめてから使います。**
> 同じ理由で `clock_gettime` も `#if defined(CLOCK_MONOTONIC)` で守ります
> （strict C11 では POSIX の宣言が隠れる環境があります）。
