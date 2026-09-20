# lib/ — セルフホスト製の標準ライブラリ

**yashirolang 自身で書いてあります。**

「yashirolang 自身で書けるものは yashirolang で書く」方針です。
C ランタイム（`runtime/`）に置くのは、C の機能が必要なものだけに限ります。

内容：

| ファイル | 中身 |
|---|---|
| `strings` | 文字列ヘルパ（`substr` / `find` / `split` / `join` / `strip` / `replace` …）。**C は 1 行も無い** |
| `io` | ファイル入出力。`extern` をここに閉じ込める |
| `sys` | `argv` と外部コマンド実行、`cpu_count`（注意: `cpu_count` だけベアメタルで使えません） |
| `dict` | 文字列キーの表（`str → int`。線形探索） |
| `time` | 時刻と経過時間（**ベアメタルで使えないモジュール**） |
| `complex` | 複素数（**言語を変えずに、演算子の多重定義だけで書いた例**） |
| `fft` | 高速フーリエ変換（`complex` の上に） |
| `plot` | **作図**（SVG を書き出す。matplotlib 相当） |
| `frame` | **表形式のデータ**（CSV・絞り込み・並べ替え・グループ集計。pandas 相当） |
| `net` | **TCP ソケット**（`listen` / `accept` / `connect` / `send` / `recv`）。`extern` をここに閉じ込める |
| `http` | **HTTP/1.1** の最小限（要求の解釈・応答の組み立て・受付の輪）。`net` の上に乗るだけで、C は 1 行も無い |

`import strings` と書けば、コンパイラが `lib/` から自動で見つけます
（探索場所は「入口ファイルのディレクトリ」と `lib/` の 2 つ。
両方に同じ名前があればエラーです）。

なぜ f-string を言語機能にせずヘルパ関数で済ませるのかは
[../docs/design/self-hosting.md](../docs/design/self-hosting.md) 3.7 節を参照。
