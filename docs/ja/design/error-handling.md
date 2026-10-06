# エラー処理の実装設計（`raises` / `try` / `except`）

> 仕様は [spec/safety-spec.md §8](../spec/safety-spec.md)。ここでは**どう IR に落とすか**を決めます。

---

## 1. 決定：アンワインドしない

例外の実装方式は 2 つあります。

| 方式 | 仕組み | 向き不向き |
|---|---|---|
| アンワインド（Python / C++ / Java） | 実行時にスタックを巻き戻す。`invoke` / landing pad / DWARF テーブル | 成功パスは速いが、**ランタイムと libc に強く依存**。OS では使えない |
| **戻り値検査（Go / Rust）** | 失敗を戻り値で表し、呼び出し側に分岐を挿入 | 分岐のぶん遅いが、**依存ゼロ**。OS でも動く |

**★ yashirolang は戻り値検査を採る。** 見た目の `try` / `except` は構文糖です。
理由は最終目標（OS 開発）で、アンワインド機構そのものを自分で書けないからです。

---

## 2. ABI：エラーは「出力引数」で返す

`raises` 関数は、シグネチャの末尾に**エラー出力ポインタ**が 1 本増えます。

```python
def read_config(path: str) -> Config raises IOError
```

```llvm
; ptr %err.out は { i64 tag, ptr payload } を書き込む先
define ptr @read_config(ptr %path, ptr %err.out)
```

| 状況 | `%err.out` の中身 | 戻り値 |
|---|---|---|
| 成功 | `tag = 0` | 本来の値 |
| 失敗 | `tag = <エラー型 ID>`, `payload = エラーオブジェクト` | 型ごとの既定値（0 / null。**使われない**） |

**なぜ複合戻り値（`{i64, ptr}` を返す）にしないのか**

既存の codegen は「1 つの値を返す」前提で書かれています（規約 R6）。
複合戻り値を入れると、呼び出し・戻り値の扱い・`main` の特別扱いまで全部が影響を受けます。
出力引数なら**追加は引数 1 本だけ**で、既存の生成規則をそのまま使えます。

---

## 3. 呼び出しの展開

### 3.1 `raises` 関数の中から呼ぶ（自動伝播 ＝ Rust の `?`）

```python
def load() -> Config raises IOError:
    text: str = io.read_file(path)   # ← ここ
    return parse(text)
```

```llvm
  %e = alloca { i64, ptr }
  store { i64, ptr } zeroinitializer, ptr %e
  %text = call ptr @io.read_file(ptr %path, ptr %e)
  %tag = load i64, ptr %e
  %fail = icmp ne i64 %tag, 0
  br i1 %fail, label %propagate.3, label %ok.3

propagate.3:                       ; 呼び出し元へそのまま返す
  ; ★ ここで「このスコープで生きている所有値」の drop を実行する（ownership.md §6.1）
  %ev = load { i64, ptr }, ptr %e
  store { i64, ptr } %ev, ptr %err.out
  ret ptr null                     ; 値は使われない

ok.3:
  ...
```

### 3.2 `try` / `except`

```python
try:
    cfg = read_config(p)
    use(cfg)
except IOError as e:
    print(e.message)
```

- `try` ブロック内の `raises` 呼び出しの分岐先が、`propagate` ではなく **`except` のディスパッチ**になります
- ディスパッチは `tag` の比較の連鎖（`switch i64`）
- `as e` はエラーオブジェクトを受け取る局所変数（**所有型**。ブロック末尾で drop）
- `try` ブロックの途中で抜けるので、**`try` 内で作った所有値の drop は分岐の前に実行**します

### 3.3 `main`

`main` は `raises` を宣言できません（受け取る相手がいないため）。
宣言すると `E-RAISE-4`。トップレベルで処理するか、`panic` にしてください。

---

## 4. エラー型 ID の割り当て

`tag` は **プログラム全体で一意な整数**です。`0` は「エラー無し」に予約します。

**注意: stage0（C 版）と stage1（セルフホスト版）で同じ番号にならなければ、IR がバイト単位で一致せず
`make selfhost-test` が落ちます。** そこで割り当て規則を仕様として固定します。

> **規則**：モジュールを依存順（`load_modules` が返すトポロジカル順）に走査し、
> 各モジュール内では**ソース中の出現順**に、エラーとして使われたクラスへ `1` から連番を振る。

この規則はどちらの実装でも同じ結果になります（既存の `ir_name` 修飾と同じ考え方）。

---

## 5. 検査（sema でやること）

| # | 検査 | エラーコード |
|---|---|---|
| R1 | `raises` を宣言していない関数で `raises` 関数を呼んでいる（`try` の外） | `E-RAISE-1` |
| R2 | 呼び出し先の `raises` に、自分の `raises` が含まれていない | `E-RAISE-2` |
| R3 | `except` が起こりうるエラー型を網羅していない | `E-RAISE-3` |
| R4 | `main` に `raises` がある | `E-RAISE-4` |
| R5 | `try` の中に `raises` 呼び出しが 1 つも無い（無意味な try） | 警告 |
| R6 | `except` の型が、その `try` で起こりえない | 警告 |

**★ R1 が「握りつぶせない」の本体です。** 戻り値を無視しても、
`raises` は関数シグネチャに現れるため、呼んだ時点で必ずどちらかの処理を強制されます。

---

## 6. drop との相互作用

エラー経路も**普通の早期 return と同じ**です（[ownership.md §6.1](ownership.md)）。
`propagate` ブロックでは、そのスコープで生きている所有値を逆順に drop してから返します。

**注意: エラーオブジェクト自体は drop しません**（呼び出し元に所有権が移るため）。

---

## 7. 標準ライブラリへの反映

現在 `lib/io.ys` は失敗を「空文字列」で表しています。これを `raises` に直します。

```python
# 現在
def read_file(path: str) -> str: ...          # 失敗すると ""

# v2
def read_file(path: str) -> str raises IOError: ...
```

**注意: ここで `selfhost/` の呼び出し側が全部影響を受けます。**
そのため「C 版に機能を入れる」→「lib を直す」→「selfhost を直す」→「bootstrap 確認」の順で進めます。
