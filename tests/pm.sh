#!/bin/bash
# パッケージマネージャの受け入れテスト
#
# ★ 本物の git リポジトリを tests/tmp/pm に作って、一通り動かします。
#   モックを使わないのは、**守りたい性質のほとんどが git の性質**
#   （commit が不変であること・tree が中身を表すこと）だからです。
#
# 使い方: tests/pm.sh

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# ★ 実行ファイル名は Makefile の LANG_CC / LANG_PM が決めます。
#   ここに名前を書き写すと改名のたびに直すことになるので、make に訊きます。
LANG_CC="$(make -s -C "$ROOT" print-LANG_CC)"
EXT="$(make -s -C "$ROOT" print-LANG_EXT)"
LANG_PM="$(make -s -C "$ROOT" print-LANG_PM)"
PO="$ROOT/build/$LANG_PM"
[ -x "$PO" ] || [ ! -x "$PO.exe" ] || PO="$PO.exe"
PLC="$ROOT/build/$LANG_CC"
[ -x "$PLC" ] || [ ! -x "$PLC.exe" ] || PLC="$PLC.exe"
WORK="$ROOT/tests/tmp/pm"

if [ ! -x "$PO" ]; then
    echo "$LANG_PM が見つかりません: ${PO}（先に 'make pm'）"
    exit 1
fi
if ! command -v git > /dev/null 2>&1; then
    echo "  skip  $LANG_PM のテスト（git がありません）"
    exit 0
fi

# ★ 利用者の ~/.cache とホームの設定を汚さないように、全部この下に閉じます
export PLC_CACHE="$WORK/cache"
export PLC_CC="$PLC"
export PLC_LIB_DIR="$ROOT/lib"
export PLC_RUNTIME_O="$ROOT/build/runtime.a"
export GIT_AUTHOR_NAME=test GIT_AUTHOR_EMAIL=test@example
export GIT_COMMITTER_NAME=test GIT_COMMITTER_EMAIL=test@example
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null

rm -rf "$WORK"
mkdir -p "$WORK"

pass=0
fail=0
if [ -t 1 ]; then
    C_OK=$'\033[32m'; C_NG=$'\033[31m'; C_END=$'\033[0m'
else
    C_OK=''; C_NG=''; C_END=''
fi

ok()   { pass=$((pass + 1)); printf "  %sok%s    %s\n" "$C_OK" "$C_END" "$1"; }
ng()   { fail=$((fail + 1)); printf "  %sFAIL%s  %s\n" "$C_NG" "$C_END" "$1"
         [ $# -lt 2 ] || printf "%s\n" "$2" | sed 's/^/          /'; }

# 期待どおりの結果か（want_rc=0 なら成功、1 なら失敗を期待）
expect() {
    local name="$1" want_rc="$2"; shift 2
    local out rc
    out="$("$@" 2>&1)"; rc=$?
    if [ "$want_rc" -eq 0 ] && [ "$rc" -ne 0 ]; then
        ng "$name" "失敗を期待していません（rc=${rc}）:
$out"; return 1
    fi
    if [ "$want_rc" -ne 0 ] && [ "$rc" -eq 0 ]; then
        ng "$name" "成功してしまいました（失敗を期待）:
$out"; return 1
    fi
    LAST_OUT="$out"
    ok "$name"
    return 0
}

has() {  # LAST_OUT に文字列が含まれるか
    local name="$1" want="$2"
    if printf '%s' "$LAST_OUT" | grep -qF -- "$want"; then ok "$name"
    else ng "$name" "含まれていません: $want
実際:
$LAST_OUT"; fi
}

commit() { git -C "$1" add -A && git -C "$1" commit -qm "$2"; }

# ── パッケージを 3 つ用意する ────────────────────────────────
#   cfg   1.0.0 / 1.1.0 / 1.2.0（cfg$EXT と cfg_lex$EXT を出す）
#   httpx 0.3.0（cfg >= 1.1.0 を要求する）
#   evil  1.0.0（cfg$EXT という名前をぶつけてくる＋シンボリックリンク）
mkdir -p "$WORK/cfg"
git -C "$WORK/cfg" init -q
printf 'name    cfg\nversion 1.0.0\n' > "$WORK/cfg/package.pkg"
# ★ A-32：パッケージの中でも名前は**完全に**書きます（相対 import はありません）
printf 'import cfg.cfg_lex\ndef hello() -> str:\n    return "cfg " + cfg.cfg_lex.tag()\n' \
    > "$WORK/cfg/cfg$EXT"
printf 'def tag() -> str:\n    return "1.0.0"\n' > "$WORK/cfg/cfg_lex$EXT"
# ★ ソース拡張子のシンボリックリンク。展開されないことを後で確かめます
ln -s /etc/passwd "$WORK/cfg/secrets$EXT" 2> /dev/null
commit "$WORK/cfg" v1.0.0 > /dev/null
git -C "$WORK/cfg" tag v1.0.0
for v in 1.1.0 1.2.0; do
    prev=$(git -C "$WORK/cfg" describe --tags --abbrev=0 | tr -d v)
    sed -i.bak "s/$prev/$v/" "$WORK/cfg/package.pkg" "$WORK/cfg/cfg_lex$EXT"
    rm -f "$WORK"/cfg/*.bak
    commit "$WORK/cfg" "v$v" > /dev/null
    git -C "$WORK/cfg" tag "v$v"
done

mkdir -p "$WORK/httpx"
git -C "$WORK/httpx" init -q
printf 'name    httpx\nversion 0.3.0\ndep     cfg %s 1.1.0\n' "$WORK/cfg" \
    > "$WORK/httpx/package.pkg"
printf 'import cfg.cfg\ndef get() -> str:\n    return "httpx uses " + cfg.cfg.hello()\n' \
    > "$WORK/httpx/httpx$EXT"
commit "$WORK/httpx" v0.3.0 > /dev/null
git -C "$WORK/httpx" tag v0.3.0

mkdir -p "$WORK/evil"
git -C "$WORK/evil" init -q
printf 'name    evil\nversion 1.0.0\n' > "$WORK/evil/package.pkg"
printf 'def x() -> int:\n    return 1\n' > "$WORK/evil/cfg$EXT"
printf 'def y() -> int:\n    return 2\n' > "$WORK/evil/evil$EXT"
commit "$WORK/evil" v1.0.0 > /dev/null
git -C "$WORK/evil" tag v1.0.0

# ── 利用者側 ────────────────────────────────────────────────
APP="$WORK/app"
mkdir -p "$APP"
printf 'import httpx.httpx\ndef main() -> int:\n    print(httpx.httpx.get())\n    return 0\n' \
    > "$APP/main$EXT"
cd "$APP" || exit 1

echo "  ── $LANG_PM ──────────────────────────────"

expect "init"          0 "$PO" init demo
expect "add httpx"     0 "$PO" add httpx "$WORK/cfg/../httpx"

# ① MVS：cfg は 1.2.0 があるのに、要求された下限の 1.1.0 が選ばれる
expect "list"          0 "$PO" list
has    "MVS で 1.1.0 が選ばれる" "cfg 1.1.0"

# ② 取ってきたものでコンパイルして走る
expect "build"         0 "$PO" build
if out="$("$APP/demo" 2>&1)" && [ "$out" = "httpx uses cfg 1.1.0" ]; then
    ok "実行できる"
else
    ng "実行できる" "出力: $out"
fi

# ③ シンボリックリンク（mode 120000）は持ち込まない。
#    cfg は実際にインストールされるので、これは本物の確認です。
#    ⚠️ Windows（MSYS2）では ln -s が実体のコピーになることがあります。
#      git が 120000 で持っていないなら確認の意味が無いので飛ばします。
mode="$(git -C "$WORK/cfg" ls-tree v1.1.0 secrets$EXT | awk '{print $1}')"
if [ "$mode" != "120000" ]; then
    echo "  skip  シンボリックリンクを展開しない（この環境では作れません）"
elif [ -e "$APP/deps/cfg/secrets$EXT" ]; then
    ng "シンボリックリンクを展開しない" "deps/cfg/secrets$EXT ができています"
elif grep -q secrets "$APP/package.lock"; then
    ng "シンボリックリンクを展開しない" "ロックに secrets が載っています"
else
    ok "シンボリックリンクを展開しない"
fi

# ④ ロックとの一致
expect "verify"        0 "$PO" verify
has    "3 モジュール一致" "3 モジュール"

# ⑤ 改ざんを見つける
echo "# tampered" >> "$APP/deps/cfg/cfg$EXT"
expect "改ざんを検出"   1 "$PO" verify
has    "どのファイルか言う" "deps/cfg/cfg$EXT"
expect "sync で直る"    0 "$PO" sync
expect "直った"        0 "$PO" verify

# ⑥ 同じモジュール名を出すパッケージが**共存できる**（A-32）
#
# ★ evil は cfg$EXT を出しますが、deps/evil/cfg$EXT に入るので
#   deps/cfg/cfg$EXT とぶつかりません。名前も evil.cfg と cfg.cfg で別ものです。
#   ⚠️ 0.22 まではここで断っていました（フラットに置いていたため）。
cp "$APP/package.pkg" "$WORK/pkg.before"
cp "$APP/package.lock" "$WORK/lock.before"
expect "同名モジュールのパッケージを入れられる" 0 "$PO" add evil "$WORK/evil"
if [ -f "$APP/deps/evil/cfg$EXT" ] && [ -f "$APP/deps/cfg/cfg$EXT" ]; then
    ok "パッケージごとに分かれて入る"
else
    ng "パッケージごとに分かれて入る" "$(ls -R "$APP/deps" 2>&1 | head -20)"
fi
# ★ 元に戻す（宣言とロックを戻して sync し直す。remove は無いので手で戻します）
cp "$WORK/pkg.before" "$APP/package.pkg"
cp "$WORK/lock.before" "$APP/package.lock"
expect "元に戻す"      0 "$PO" sync

# ⑦ 間接の依存を上げる（宣言に足されて 1.2.0 になる）
expect "update cfg"   0 "$PO" update cfg
expect "list（更新後）" 0 "$PO" list
has    "1.2.0 に上がる" "cfg 1.2.0"
expect "再ビルド"      0 "$PO" build
if out="$("$APP/demo" 2>&1)" && [ "$out" = "httpx uses cfg 1.2.0" ]; then
    ok "更新が効いている"
else
    ng "更新が効いている" "出力: $out"
fi

# ⑦b 標準ライブラリと同じ名前を出すパッケージは、**入れる前に**断る
mkdir -p "$WORK/shadow"
git -C "$WORK/shadow" init -q
printf 'name    shadow\nversion 1.0.0\n' > "$WORK/shadow/package.pkg"
printf 'def f() -> int:\n    return 1\n' > "$WORK/shadow/shadow$EXT"
printf 'def g() -> int:\n    return 2\n' > "$WORK/shadow/json$EXT"
commit "$WORK/shadow" v1.0.0 > /dev/null
git -C "$WORK/shadow" tag v1.0.0
# ★ A-32：標準ライブラリと同じ名前のモジュールを出すパッケージも入ります。
#   shadow.json（パッケージ）と json（標準ライブラリ）は別の名前です。
cp "$APP/package.pkg" "$WORK/pkg.before2"
cp "$APP/package.lock" "$WORK/lock.before2"
expect "標準ライブラリと同名でも入る" 0 "$PO" add shadow "$WORK/shadow"
if [ -f "$APP/deps/shadow/json$EXT" ]; then
    ok "パッケージの json は deps/shadow/ に入る"
else
    ng "パッケージの json は deps/shadow/ に入る" "$(ls -R "$APP/deps" 2>&1 | head -20)"
fi
cp "$WORK/pkg.before2" "$APP/package.pkg"
cp "$WORK/lock.before2" "$APP/package.lock"
expect "shadow を戻す"  0 "$PO" sync

# ⑧ 宣言から依存を消したら、ロックも作り直される
#    cfg は httpx が要求するので残りますが、直接の要求（1.2.0）が消えるので
#    MVS の結果は 1.1.0 に戻ります。
grep -v "^dep     cfg " "$APP/package.pkg" > "$WORK/pkg.trim"
mv "$WORK/pkg.trim" "$APP/package.pkg"
expect "依存を消して sync"   0 "$PO" sync
expect "list（削除後）"      0 "$PO" list
has    "1.1.0 に戻る"        "cfg 1.1.0"

# ⑨ タグを張り替えられても、ロックが指す commit の中身が入る
#    いま使われているのは v1.1.0 なので、それを別の commit へ動かします。
git -C "$WORK/cfg" tag -f v1.1.0 v1.0.0 > /dev/null 2>&1
rm -rf "$PLC_CACHE"
expect "タグ張り替え後も sync できる" 0 "$PO" sync
if grep -q '"1.1.0"' "$APP/deps/cfg/cfg_lex$EXT"; then
    ok "張り替えではなくロックの中身が入る"
else
    ng "張り替えではなくロックの中身が入る" "$(cat "$APP/deps/cfg/cfg_lex$EXT")"
fi

# ⑩ ロックが指す commit ごと消されたら止まる
git -C "$WORK/cfg" checkout -q v1.0.0 2> /dev/null
git -C "$WORK/cfg" branch -f master v1.0.0 > /dev/null 2>&1
git -C "$WORK/cfg" branch -f main v1.0.0 > /dev/null 2>&1
for t in $(git -C "$WORK/cfg" tag); do
    git -C "$WORK/cfg" tag -d "$t" > /dev/null
done
git -C "$WORK/cfg" tag v1.1.0
git -C "$WORK/cfg" reflog expire --expire=now --all
git -C "$WORK/cfg" gc -q --prune=now 2> /dev/null
rm -rf "$PLC_CACHE"
expect "履歴の書き換えを検出" 1 "$PO" sync
has    "commit が無いと言う" "履歴が書き換えられたか"

echo "────────────────────────────────"
if [ "$fail" -eq 0 ]; then
    echo "$LANG_PM: $pass 件すべて合格"
    exit 0
fi
echo "$LANG_PM: $fail 件失敗（$pass 件合格）"
exit 1
