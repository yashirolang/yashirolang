# ソケットと HTTP

**サーバーもクライアントも書けます。** `{{!cc}}` は TCP ソケットを標準で
持っていて、その上に HTTP/1.1 の最小限が乗っています。外部のライブラリは
要りません（`curl` も要りません）。

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

取ってくるほうは 1 行です。

```python
res: http.Response = http.get("http://127.0.0.1:8080/")
print(str(res.status) + " " + res.body)
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
| `Conn.set_timeout(ms)` | 読み書きの待ち時間（**0 なら無期限**） |
| `Conn.close()` | 閉じる（2 回呼んでもよい） |
| `Listener.set_conn_timeout(ms)` | **これから** `accept` する接続の待ち時間 |
| `Listener.set_timeout(ms)` | `accept` そのものの待ち時間 |

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
| チャンク転送 | ✅ 受けるのも、送られてくるのも |
| クライアント（`get` / `post` / `request`） | ✅ |
| 待ち時間 | ✅ |
| TLS（https） | ⬜ **はっきり断ります** |
| 接続を貯めておく（クライアント側の keep-alive） | ⬜ 毎回繋ぎます |

**⚠️ 未対応のものは、黙って壊れるのではなく、はっきり断ります。**
`https://` を `http.get` に渡すと、平文で繋ぎにいかずにエラーを返します
（暗号化されていると思って鍵を送ってしまうのが、いちばん困るからです）。

**★ `http` が投げるのは `HttpError` だけです。** 下の `net` が投げる
`NetError` も包み直すので、「HTTP を使いたいだけなのにソケットの
エラーも書かされる」ことになりません。`e.timed_out` は引き継ぎます。

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

### 3.5 クライアント

| 関数 | 意味 |
|---|---|
| `http.get(url) -> Response` | 取ってくる |
| `http.post(url, content_type, body) -> Response` | 送る |
| `http.request(method, url, headers, body, timeout_ms)` | 全部自分で決める |
| `http.parse_url(url) -> Url` | `scheme` / `host` / `port` / `target` に分ける |

```python
res: http.Response = http.get("http://example.com/")
print(str(res.status))                 # 200
print(res.header("content-type"))      # text/html
print(res.body)
```

**★ 受け取る入れ物はサーバーと同じ `Response` です。** 本体の終わりの
決め方（`Content-Length` / チャンク / 本体なし）も、読み取りの
`Reader` も、サーバー側と同じものを使っています。**送る側と受ける側で
別々に書いた解釈が食い違う**のを避けるためです。

**⚠️ 1 回の要求ごとに繋いで閉じます**（`Connection: close`）。接続を
貯めておく仕組みはまだありません。何十回も叩くなら `net` で 1 本の接続を
自分で持ち回ってください。

**⚠️ `https://` は断ります。** TLS がまだ無いので、平文で繋ぎにいくより
エラーにするほうが安全です。

ヘッダを足したいときは `request` です。

```python
h: dict.Dict[str, str] = dict.Dict()
h.set("authorization", "Bearer " + token)
res: http.Response = http.request("GET", url, h, "", 5000)
```

`host` / `content-length` / `connection` はこちらで入れるので、
渡しても無視します（二重に書かれるのを防ぐため）。

### 3.6 待ち時間

**★ サーバーには待ち時間を決めてください。**

```python
l: net.Listener = net.listen("127.0.0.1", 8080)
l.set_conn_timeout(30000)      # 受け付けた接続は 30 秒で見切る
http.serve_on(l, handle)
```

**⚠️ これが無いと、繋いだきり何も送ってこない相手 1 つでサーバーが
止まります。** `http` は 1 本ずつ順に捌くので、その間ほかの客も全員
待たされます。待ち時間を過ぎた接続には `408 Request Timeout` を返して
閉じます。

クライアントの既定は 30 秒（`http.CLIENT_TIMEOUT_MS`）です。
`http.request` の最後の引数で変えられます。

**★ 待ち時間切れは、他の失敗と区別できます。**「まだ来ていない」だけで
接続はまだ生きているので、もう一度待つこともできます。

```python
try:
    part: str = c.recv(4096)
except net.NetError as e:
    if e.timed_out:
        ...        # まだ来ていないだけ（接続は生きている）
    else:
        ...        # 本当に壊れた
```

---

## 4. 落とし穴

**⚠️ `recv` の切れ目は、要求の切れ目ではありません。**
1 回の `recv` に要求が 2 本入っていることも、頭が途中で切れていることも
あります。`http` はそれを踏まえて「読んだぶんを貯めて、その中から
空行を探す」形で書いてあります。自分で書くときも同じにしてください。

**⚠️ `Connection` ヘッダを見てください。** HTTP/1.1 は既定で継続します。
`Connection: close` が来たら、応答を返してから閉じます。

**⚠️ `Content-Length` と `Transfer-Encoding` の両方がある要求は断ります**
（`400`）。どちらを見るかで**要求の切れ目がずれる**ため、間に挟まる機械と
解釈が食い違うと、1 本の接続に別人の要求を紛れ込ませられます
（要求の密輸。RFC 9112 §6.1）。**どちらを選んでも危ないので、断ります。**

**⚠️ 本体の終わりの決め方は 3 つあり、順番が決まっています。**
チャンクが先、`Content-Length` が後、どちらも無ければ相手が閉じるまで。
逆にすると、両方書いてある応答で切れ目がずれます。

---

## 4.5 🔒 送られてきたもので壊れないために

**★ ここは library 側で守ります。** 取っ手を書く人が気をつけ忘れたら
穴になる、という作りにはしていません。

| 守っていること | 守らないとどうなるか |
|---|---|
| ヘッダの値から改行を取り除く | `r.set("location", req.param("url"))` で**好きなヘッダを足せます**（応答の分割） |
| 数の桁数に上限を置く | `Content-Length: 9999…` の 1 行で**プロセスごと落とせます**（桁あふれの実行時エラー） |
| `Content-Length` と `Transfer-Encoding` の同時指定を断る | 間に挟まる機械と要求の切れ目がずれ、**別人の要求を紛れ込ませられます** |
| チャンクの合計と後付けヘッダの本数に上限を置く | 小さいものを送り続けられると**永久に付き合わされます** |
| URL の改行を断る | こちらが**送る**要求に好きなヘッダを足されます |
| `https://` を断る | 暗号化されていると思って**鍵を平文で送ります** |

**⚠️ 待ち時間は忘れずに決めてください。** `http.serve` は自分で待ち受け口を
作るので既定（30 秒）を入れてありますが、`net.listen` してから `serve_on` を
使うときは**呼び出し側の責任**です。決めないと、繋いだきり黙っている相手
1 つでサーバーが止まります。

**⚠️ 本体の上限は 8 MiB です**（`http.MAX_BODY`）。頭は 64 KiB
（`http.MAX_HEAD`）。それより大きいものを扱うなら、`net` で自分で
読んでください。

**⚠️ ここが守るのは「HTTP として壊れていないこと」だけです。**
取っ手が受け取った `req.path` をファイル名に使うなら `..` を自分で弾く、
`req.body` を SQL に混ぜないようにする、といったことは**取っ手側の仕事**です。

---

## 5. 動く例

`examples/` にあります。

| 例 | 中身 |
|---|---|
| `webserver{{!ext}}` | 小さな Web サーバー（待ち時間つき） |
| `webclient{{!ext}}` | 取ってくるだけのクライアント（`curl` の代わり） |

---

## 6. 制限

| 制限 | 状況 |
|---|---|
| IPv6 | ✅ 入りました（名前が複数の住所を持つときは**順に試します**） |
| チャンク転送 | ✅ 入りました |
| タイムアウト | ✅ 入りました（**読み書きだけ**。下を見てください） |
| TLS（https） | まだ。**黙って平文で繋がず、断ります** |
| `connect` の待ち時間 | まだ（`SO_RCVTIMEO` は繋がったあとにしか効きません） |
| 接続を貯めておく（クライアント） | まだ（1 要求ごとに繋ぎ直します） |
| UDP | まだ |
| 多重化（`select` / `epoll`） | まだ（同時に捌くなら `spawn`） |
| ベアメタル | **できません**（`runtime/hosted.c` だけが持っています） |

**⚠️ `connect` だけは区切れません。** `Conn.set_timeout` が決めるのは
**繋がったあとの読み書き**です。返事の無い相手に繋ぎにいくと、OS が
あきらめるまで（数十秒）待ちます。区切るには非同期の接続と `select` が
要るので、まだ入れていません。
