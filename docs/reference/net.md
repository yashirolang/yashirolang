# ソケットと HTTP

**サーバーが書けます。** `{{!cc}}` は TCP ソケットを標準で持っていて、
その上に HTTP/1.1 の最小限が乗っています。外部のライブラリは要りません。

```python
import http
import net

def handle(req: http.Request) -> http.Response:
    if req.path == "/":
        return http.html(200, "<h1>こんにちは</h1>")
    return http.text(404, "ありません")

def main() -> int:
    try:
        http.serve("127.0.0.1", 8080, handle)
    except net.NetError as e:
        print("起動できません: " + e.message)
        return 1
    return 0
```

```bash
{{!cc}} server{{!ext}} -o server && ./server
```

---

## 1. 層は 2 つです

| 層 | 置き場 | 役割 |
|---|---|---|
| `net` | `lib/net{{!ext}}` | TCP ソケット。C との境界（`extern`）をここに閉じ込める |
| `http` | `lib/http{{!ext}}` | 要求の解釈・応答の組み立て・受付の輪。**C は 1 行もありません** |

**★ `http` は `net` の上に乗るだけです。** 暗号化や別のプロトコルを足す
ときも、`net` を差し替えれば済むようにしてあります。

---

## 2. `net` — TCP ソケット

| 関数・メソッド | 意味 |
|---|---|
| `net.listen(host, port) -> Listener` | 待ち受ける |
| `net.listen_backlog(host, port, n)` | 待ち行列の長さも決める |
| `net.connect(host, port) -> Conn` | つなぎに行く |
| `Listener.accept() -> Conn` | 1 本受け付ける（来るまで待つ） |
| `Listener.port` | 実際のポート番号 |
| `Conn.recv(max) -> str` | 最大 max バイト読む |
| `Conn.recv_all() -> str` | 相手が閉じるまで読む |
| `Conn.send(s) -> int` | 全部書く |
| `Conn.close()` | 閉じる（2 回呼んでもよい） |

**★ ポートに `0` を渡すと、OS が空いているポートを選びます。**
選ばれた番号は `l.port` で読めます。

```python
l: net.Listener = net.listen("127.0.0.1", 0)
print(l.port)      # 例: 54321
```

決め打ちのポートは、同じ機械で 2 つ立てると衝突します。
**この言語のテストがこれを使っています。**

**⚠️ 失敗は `raises` で返ります。落ちません。**
「ポートが使われている」「相手が切った」は**ふつうに起きること**です。

```python
try:
    c: net.Conn = net.connect("127.0.0.1", 9999)
except net.NetError as e:
    print("つながりません: " + e.message)
```

**⚠️ 相手が閉じたときは `recv` が `""`（空文字列）を返します。**
「もう来ない」と「失敗した」を分けるためです。失敗は `raises` 側です。

```python
while True:
    part: str = c.recv(4096)
    if len(part) == 0:
        break          # 相手が閉じた
```

**⚠️ 閉じるのは呼び出し側の仕事です。** クラスに「解放されるときに
呼ばれる処理」はまだ無いので、`fd` は自動では閉じません。
忘れると `fd` が尽きます。

---

## 3. `http` — HTTP/1.1

### 3.1 対応している範囲

| 事柄 | 対応 |
|---|---|
| 要求行・応答行 | ✅ |
| ヘッダ（大小を区別しない） | ✅ |
| `Content-Length` の本体 | ✅ |
| クエリ文字列 `?a=1&b=2` | ✅ |
| パーセント符号化の復号（`%E3%81%82` と `+`） | ✅ |
| keep-alive（1 本で何度も） | ✅ |
| チャンク転送 | ⬜ 受け取ると **411** を返す |
| TLS（https） | ⬜ |

**⚠️ 未対応のものは、黙って壊れるのではなく、はっきり断ります。**
チャンク転送を受け取ったら `411 Length Required` です。

### 3.2 要求

```python
req.method            # "GET"
req.path              # "/a"（クエリを含まない。復号済み）
req.query             # "x=1&y=2"
req.param("x")        # "1"（復号済み。無ければ ""）
req.header("host")    # 大小を区別しない
req.body              # Content-Length ぶん
req.keep_alive()      # 1 本で続けてよいか
```

### 3.3 応答

```python
http.text(200, "こんにちは")            # text/plain
http.html(200, "<h1>やあ</h1>")         # text/html
http.json(200, "{\"ok\":true}")         # application/json

r: http.Response = http.text(201, "")
r.set("location", "/a")                 # ヘッダを足す
```

`Content-Length` は `encode` が必ず入れます。入れ忘れると相手は
「接続が切れるまでが本体」と解釈し、keep-alive が壊れます。

### 3.4 受付

| 関数 | 意味 |
|---|---|
| `http.serve(host, port, handler)` | 待ち受けて捌く（**戻ってきません**） |
| `http.serve_on(l, handler)` | 開いてある待ち受け口で捌く（同上） |
| `http.serve_n(l, handler, n)` | **n 本だけ**捌いて戻る |
| `http.serve_conn(c, handler)` | 1 本の接続を、閉じるまで捌く |

**⚠️ 永久に回る輪は `join` できません。** `spawn` で `serve_on` を回すと
`accept` で待ち続けるのでスレッドが終わりません。数を決めて回すか、
主スレッドで回してください。

**★ 同時接続は分けていません。** 1 本ずつ順に捌きます。同時に捌きたい
ときは `serve_conn` を `spawn` する輪を自分で書いてください——この層が
スレッドの方針を決めてしまわないようにしています。

```python
def worker(c: net.Conn) -> int:
    http.serve_conn(c, handle)
    c.close()
    return 0

while True:
    c: net.Conn = l.accept()
    t: Thread[int] = spawn(worker, c)   # ⚠️ join は自分で管理すること
```

---

## 4. 落とし穴

**⚠️ `recv` の切れ目は、要求の切れ目ではありません。**
1 回の `recv` に要求が 2 本入っていることも、頭が途中で切れていることも
あります。`http` はそれを踏まえて「読んだぶんを貯めて、その中から
空行を探す」形で書いてあります。自分で書くときも同じにしてください。

**⚠️ `Connection` ヘッダを見てください。** HTTP/1.1 は既定で継続します。
`Connection: close` が来たら、応答を返してから閉じます。

---

## 5. 動く例

`examples/` にあります。

| 例 | 中身 |
|---|---|
| `webserver{{!ext}}` | ファイルを返す小さな Web サーバー |

---

## 6. 制限

| 制限 | 状況 |
|---|---|
| IPv6 | まだ（`AF_INET` だけ） |
| TLS（https） | まだ |
| チャンク転送 | まだ（411 で断る） |
| タイムアウト | まだ（`recv` は来るまで待ちます） |
| UDP | まだ |
| ベアメタル | **できません**（`runtime/hosted.c` だけが持っています） |
