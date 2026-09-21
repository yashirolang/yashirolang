# ソケット・TLS・HTTP

**サーバーもクライアントも書けます。** `yashirolang` は TCP ソケットを標準で
持っていて、その上に HTTP/1.1 の最小限が乗っています。外部のライブラリは
要りません（`curl` も要りません）。

**★ ただし TLS（`https`）だけは任意です。** 暗号を自分では書かず OpenSSL 3 に
任せているので、`make TLS=1` で建てたときに入ります。入れずに建てた処理系で
`https://` に繋ごうとすると、**平文に落ちるのではなく**断られます
（[2.5 節](#25-tls--暗号化した通信)）。

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
yashirolang server.ys -o server && ./server
```

取ってくるほうは 1 行です。

```python
res: http.Response = http.get("http://127.0.0.1:8080/")
print(str(res.status) + " " + res.body)
```

---

## 1. 層は 3 つです

| 層 | 置き場 | 役割 |
|---|---|---|
| `net` | `lib/net.ys` | TCP ソケット。C との境界（`extern`）をここに閉じ込める |
| `tls` | `lib/tls.ys` | その上で TLS を話す。**暗号は自分では書きません**（OpenSSL 3 に任せます） |
| `http` | `lib/http.ys` | 要求の解釈・応答の組み立て・受付の輪。**C は 1 行もありません** |

**★ `http` は `tls.Stream` を読み書きします。** `Stream` は「暗号化されて
いることも、されていないこともある筒」で、平文の接続も同じクラスで
表します。おかげで要求の解釈も応答の組み立ても **1 か所のまま**、
`http` と `https` の両方が通ります。

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
| `Listener.set_timeout(ms)` | `accept` そのものの待ち時間（時間切れは `timed_out`） |

**★ ポートに `0` を渡すと、OS が空いているポートを選びます。**
選ばれた番号は `l.port` で読めます。

```python
l: net.Listener = net.listen("127.0.0.1", 0)
print(l.port)      # 例: 54321
```

決め打ちのポートは、同じ機械で 2 つ立てると衝突します。
**この言語のテストがこれを使っています。**

**注意: 失敗は `raises` で返ります。落ちません。**
「ポートが使われている」「相手が切った」は**ふつうに起きること**です。

```python
try:
    c: net.Conn = net.connect("127.0.0.1", 9999)
except net.NetError as e:
    print("つながりません: " + e.message)
```

**注意: 相手が閉じたときは `recv` が `""`（空文字列）を返します。**
「もう来ない」と「失敗した」を分けるためです。失敗は `raises` 側です。

```python
while True:
    part: str = c.recv(4096)
    if len(part) == 0:
        break          # 相手が閉じた
```

**注意: 閉じるのは呼び出し側の仕事です。** クラスに「解放されるときに
呼ばれる処理」はまだ無いので、`fd` は自動では閉じません。
忘れると `fd` が尽きます。

---

## 2.5 `tls` — 暗号化した通信

**★ 既定のビルドには入っていません。** この処理系は「clang だけで建つ」を
守るので、外のライブラリを黙って要求しません。TLS が要る人だけが
こう建てます。

```bash
make TLS=1
```

入っているかは `tls.available()` で分かります。**入っていないときに
黙って平文へ落ちることはありません** — 繋ごうとしたところで断られます。

| 関数・メソッド | 意味 |
|---|---|
| `tls.available() -> bool` | TLS を組み込んで建てたか |
| `tls.connect(host, port) -> Stream` | TLS で繋ぎに行く（OS の綴じ込みを信じる） |
| `tls.listen(host, port, cert, key) -> Listener` | TLS で待ち受ける（PEM のファイル） |
| `tls.plain(c: own net.Conn) -> Stream` | 平文の接続を**そのまま通す** `Stream` にする |
| `tls.client_context() -> Context` | 取りに行く側の文脈（信じる証明書を差し替えたいとき） |
| `tls.server_context(cert, key) -> Context` | 待ち受ける側の文脈 |
| `tls.connect_with(ctx, host, port) -> Stream` | 文脈を指定して繋ぐ |
| `Context.set_ca_file(path)` | **信じる証明書をこのファイルだけにする** |
| `Listener.accept() -> Stream` | 1 本受け付けて TLS で包む |
| `Listener.accept_plain() -> net.Conn` | 1 本受け付ける（まだ平文） |
| `Listener.secure(c: own net.Conn) -> Stream` | 受け付けた接続を TLS で包む |
| `Stream.recv(max)` / `send(s)` / `set_timeout(ms)` / `close()` | `net.Conn` と同じ顔 |
| `Stream.secure() -> bool` | 暗号化されているか |
| `Stream.version() -> str` | `"TLSv1.3"` など（平文なら `""`） |
| `Stream.peer_name() -> str` | 相手の証明書に書かれている名前（記録と表示のため） |

```python
import tls

def main() -> int:
    try:
        c: tls.Stream = tls.connect("example.com", 443)
        c.set_timeout(5000)
        c.send("GET / HTTP/1.1\r\nhost: example.com\r\nconnection: close\r\n\r\n")
        print(c.version())                 # TLSv1.3
        print(c.recv(4096))
        c.close()
    except net.NetError as e:
        print("繋げません: " + e.message)
        return 1
    return 0
```

### 2.5.1 緩められない既定

**★ 利用者が下げられません。** 安全側の設定を「既定は安全、必要なら外せる」
にすると、外した状態が本番に残ります。

| 事柄 | 既定 |
|---|---|
| 最低版 | TLS 1.2（1.0 / 1.1 / SSLv3 では繋ぎません） |
| 相手の証明書 | **必ず検証します** |
| 名前の照合 | **必ず行います**（証明書が誰のものかを確かめます） |
| 部分ワイルドカード | 断ります（`w*.example.com` は通しません） |
| 圧縮 | 切ります（CRIME） |
| 再ネゴシエーション | 切ります |
| SNI（相手に伝える名前） | 住所（IP）で繋ぐときは送りません（RFC 6066） |

**注意: 検証を外す口はありません。** `insecure_skip_verify` に相当する
ものは、`lib/tls.ys` にも `runtime/tls.c` にも置いていません。範囲外
アクセスの検査に逃げ道を置いていないのと同じ考え方です。

自己署名の証明書を試したいときは、**「検証を外す」のではなく
「何を信じるか」を指定します**。

```python
ctx: tls.Context = tls.client_context()
ctx.set_ca_file("my_ca.pem")              # これだけを信じる
c: tls.Stream = tls.connect_with(ctx, "localhost", 8443)
```

**名前の照合はそのまま効きます。** 上の証明書が `localhost` のものなら、
同じ相手に `127.0.0.1` として繋いでも断られます。

### 2.5.2 失敗は `net.NetError` で返ります

`TlsError` は作っていません。作ると TLS で繋ぐ人が `except net.NetError` と
`except tls.TlsError` を 2 つ書くことになり、しかも**どちらが飛ぶかは
繋ぎ方で変わります**。同じ層の失敗は同じ型にしておくほうが、上の `http` で
包み直すのも 1 か所で済みます。`e.timed_out` も今までどおり使えます。

### 2.5.3 閉じる合図

**★ 相手が `close_notify` なしに切ったら、失敗として返します。** 「ふつうの
終わり」と同じ扱いにすると、**通信を途中で切るだけで「全部受け取った」と
思わせられます**（切り詰め）。`Content-Length` もチャンクも無い応答を
`https` で受けると、ここに当たることがあります。

---

## 3. `http` — HTTP/1.1

### 3.1 対応している範囲

| 事柄 | 対応 |
|---|---|
| 要求行・応答行 | 済 |
| ヘッダ（大小を区別しない） | 済 |
| `Content-Length` の本体 | 済 |
| クエリ文字列 `?a=1&b=2` | 済 |
| パーセント符号化の復号（`%E3%81%82` と `+`） | 済 |
| keep-alive（1 本で何度も） | 済 |
| チャンク転送 | 受けるのも、送られてくるのも |
| クライアント（`get` / `post` / `request`） | 済 |
| 待ち時間 | 済 |
| TLS（https） | 済（`make TLS=1` で建てたとき。取りに行くのも待ち受けるのも） |
| 接続を貯めておく（クライアント側の keep-alive） | まだ（毎回繋ぎます） |

**注意: 未対応のものは、黙って壊れるのではなく、はっきり断ります。**
`https://` は **平文には落ちません**。TLS を組み込んでいないビルドでも、
繋ごうとしたところで「組み込んでいません」と返ります（暗号化されて
いると思って鍵を送ってしまうのが、いちばん困るからです）。

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
| `http.serve_tls(host, port, cert, key, handler)` | **https で**待ち受けて捌く（同上） |
| `http.serve_tls_on(l, handler)` | 開いてある TLS の待ち受け口で捌く |
| `http.serve_tls_n(l, handler, n)` | **n 本だけ**捌いて戻る |

**★ https でも捌く中身は同じです。** `serve_tls_*` は受け付けたあとに
TLS で包むだけで、そこから先は `serve_conn` をそのまま呼びます。要求の
解釈も応答の組み立ても 1 か所のままです。

```python
http.serve_tls("0.0.0.0", 8443, "server.pem", "server.key", handle)
```

**注意: https の場所に http で繋いでくる相手はふつうに来ます**（港を舐めて
回る相手、`https://` を `http://` で開いた人）。`serve_tls_*` は
**その客を黙って切って次に進みます** — 1 人でサーバーが終わっては困るからです。

**注意: 永久に回る輪は `join` できません。** `spawn` で `serve_on` を回すと
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
    t: Thread[int] = spawn(worker, c)   # 注意: join は自分で管理すること
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

**注意: 1 回の要求ごとに繋いで閉じます**（`Connection: close`）。接続を
貯めておく仕組みはまだありません。何十回も叩くなら `net` で 1 本の接続を
自分で持ち回ってください。

**★ `https://` がそのまま通ります**（`make TLS=1` で建てたとき）。既定の
ポートは 443 で、`Host` ヘッダにも書きません。証明書の検証と名前の照合は
**必ず行います**（2.5 を見てください）。

```python
res: http.Response = http.get("https://example.com/")
```

**注意: 自己署名の相手には `http.get` では繋げません。** 「これを信じる」を
伝える口が `http` 側にまだ無いためです。`tls.connect_with` で筒を作って、
`http.read_response` に読ませてください（`tests/tls_probe.ys` がその形です）。

**注意: TLS を組み込んでいないビルドでは `https://` は繋がりません。**
`http://` に**落ちることはありません**。

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

**注意: これが無いと、繋いだきり何も送ってこない相手 1 つでサーバーが
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

**注意: `recv` の切れ目は、要求の切れ目ではありません。**
1 回の `recv` に要求が 2 本入っていることも、頭が途中で切れていることも
あります。`http` はそれを踏まえて「読んだぶんを貯めて、その中から
空行を探す」形で書いてあります。自分で書くときも同じにしてください。

**注意: `Connection` ヘッダを見てください。** HTTP/1.1 は既定で継続します。
`Connection: close` が来たら、応答を返してから閉じます。

**注意: `Content-Length` と `Transfer-Encoding` の両方がある要求は断ります**
（`400`）。どちらを見るかで**要求の切れ目がずれる**ため、間に挟まる機械と
解釈が食い違うと、1 本の接続に別人の要求を紛れ込ませられます
（要求の密輸。RFC 9112 §6.1）。**どちらを選んでも危ないので、断ります。**

**注意: 本体の終わりの決め方は 3 つあり、順番が決まっています。**
チャンクが先、`Content-Length` が後、どちらも無ければ相手が閉じるまで。
逆にすると、両方書いてある応答で切れ目がずれます。

---

## 4.5 送られてきたもので壊れないために

**★ ここは library 側で守ります。** 取っ手を書く人が気をつけ忘れたら
穴になる、という作りにはしていません。

| 守っていること | 守らないとどうなるか |
|---|---|
| ヘッダの値から改行を取り除く | `r.set("location", req.param("url"))` で**好きなヘッダを足せます**（応答の分割） |
| 数の桁数に上限を置く | `Content-Length: 9999…` の 1 行で**プロセスごと落とせます**（桁あふれの実行時エラー） |
| `Content-Length` と `Transfer-Encoding` の同時指定を断る | 間に挟まる機械と要求の切れ目がずれ、**別人の要求を紛れ込ませられます** |
| チャンクの合計と後付けヘッダの本数に上限を置く | 小さいものを送り続けられると**永久に付き合わされます** |
| URL の改行を断る | こちらが**送る**要求に好きなヘッダを足されます |
| `https://` を平文に落とさない | 暗号化されていると思って**鍵を平文で送ります** |
| 証明書の検証を外す口を置かない | 外した状態が**本番に残ります**（この手の旗の事故はこれが定番です） |

**注意: 待ち時間は忘れずに決めてください。** `http.serve` は自分で待ち受け口を
作るので既定（30 秒）を入れてありますが、`net.listen` してから `serve_on` を
使うときは**呼び出し側の責任**です。決めないと、繋いだきり黙っている相手
1 つでサーバーが止まります。

**注意: 本体の上限は 8 MiB です**（`http.MAX_BODY`）。頭は 64 KiB
（`http.MAX_HEAD`）。それより大きいものを扱うなら、`net` で自分で
読んでください。

**注意: ここが守るのは「HTTP として壊れていないこと」だけです。**
取っ手が受け取った `req.path` をファイル名に使うなら `..` を自分で弾く、
`req.body` を SQL に混ぜないようにする、といったことは**取っ手側の仕事**です。

---

## 5. 動く例

`examples/` にあります。

| 例 | 中身 |
|---|---|
| `webserver.ys` | 小さな Web サーバー（待ち時間つき） |
| `webclient.ys` | 取ってくるだけのクライアント（`curl` の代わり） |

---

## 6. 制限

| 制限 | 状況 |
|---|---|
| IPv6 | 入りました（名前が複数の住所を持つときは**順に試します**） |
| チャンク転送 | 入りました |
| タイムアウト | 入りました（**読み書きだけ**。下を見てください） |
| TLS（https） | 入りました（`make TLS=1`。**既定のビルドには入りません**） |
| 相互 TLS（客にも証明書を求める） | まだ |
| クライアント側の「この証明書を信じる」を `http.get` に渡す | まだ（`tls` の層で組み立ててください） |
| `connect` の待ち時間 | まだ（`SO_RCVTIMEO` は繋がったあとにしか効きません） |
| 接続を貯めておく（クライアント） | まだ（1 要求ごとに繋ぎ直します） |
| UDP | まだ |
| 多重化（`select` / `epoll`） | まだ（同時に捌くなら `spawn`） |
| ベアメタル | **できません**（`runtime/hosted.c` だけが持っています） |

**注意: `connect` だけは区切れません。** `Conn.set_timeout` が決めるのは
**繋がったあとの読み書き**です。返事の無い相手に繋ぎにいくと、OS が
あきらめるまで（数十秒）待ちます。区切るには非同期の接続と `select` が
要るので、まだ入れていません。
