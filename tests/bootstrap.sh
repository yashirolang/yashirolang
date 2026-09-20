#!/bin/bash
# ブートストラップと不動点の検証
#
#   stage1 … C 版（build/<LANG_CC>）がビルドしたセルフホスト版コンパイラ
#   stage2 … stage1 が自分自身のソースをビルドしたもの
#   stage3 … stage2 が自分自身のソースをビルドしたもの
#
# ★ stage2 と stage3 が一致すれば「不動点」に到達したことになります。
#   stage1 は C 版が作ったので中身が違ってもよいのですが、
#   stage2 以降は「セルフホスト版コンパイラが作ったセルフホスト版コンパイラ」なので、
#   出力が変わる理由がありません。変わるなら、どこかに
#   「誰がコンパイルしたかによって変わる何か」が残っています。

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# ★ 実行ファイル名は Makefile の LANG_CC / LANG_PM が決めます。
#   ここに名前を書き写すと改名のたびに直すことになるので、make に訊きます。
LANG_CC="$(make -s -C "$ROOT" print-LANG_CC)"
EXT="$(make -s -C "$ROOT" print-LANG_EXT)"
LANG_PM="$(make -s -C "$ROOT" print-LANG_PM)"
PLC_CC="$ROOT/build/$LANG_CC"
BOOT="$ROOT/build/boot"

# ★ C 版がビルド時に埋め込む値を、stage1 には環境変数で渡します
#   両者とも環境変数を先に見る規則。
export PLC_LIB_DIR="$ROOT/lib"
export PLC_RUNTIME_O="$ROOT/build/runtime.a"
export PLC_TARGET_TRIPLE="$("$PLC_CC" -S "$ROOT/tests/cases/int_42$EXT" \
    | sed -n 's/^target triple = "\(.*\)"$/\1/p')"

if [ -t 1 ]; then
    C_OK=$'\033[32m'; C_NG=$'\033[31m'; C_DIM=$'\033[2m'; C_END=$'\033[0m'
else
    C_OK=''; C_NG=''; C_DIM=''; C_END=''
fi

rm -rf "$BOOT"
mkdir -p "$BOOT"

step() { printf "%s──%s %s\n" "$C_DIM" "$C_END" "$1"; }

# ── stage1：C 版がセルフホスト版コンパイラをビルドする ──
step "stage1 = C 版がビルド"
if ! "$PLC_CC" "$ROOT/selfhost/main$EXT" -o "$BOOT/stage1"; then
    echo "stage1 のビルドに失敗しました"
    exit 1
fi

# ── stage2：stage1 が自分自身をビルドする ──
step "stage2 = stage1 がビルド（セルフホスト版コンパイラが自分自身を）"
if ! "$BOOT/stage1" "$ROOT/selfhost/main$EXT" -o "$BOOT/stage2" --keep-ll; then
    echo "stage2 のビルドに失敗しました"
    exit 1
fi

# ── stage3：stage2 が自分自身をビルドする ──
step "stage3 = stage2 がビルド"
if ! "$BOOT/stage2" "$ROOT/selfhost/main$EXT" -o "$BOOT/stage3" --keep-ll; then
    echo "stage3 のビルドに失敗しました"
    exit 1
fi

echo

# ── ① 生成された IR が一致するか（これが本体）──
n=0; bad=0
for f in "$BOOT"/stage2.*.ll; do
    g="${f/stage2./stage3.}"
    n=$((n + 1))
    cmp -s "$f" "$g" || { bad=$((bad + 1)); echo "  差分: $(basename "$f")"; }
done

if [ "$bad" -ne 0 ]; then
    printf "%s✗ stage2 と stage3 の IR が違います（%d / %d 本）%s\n" \
           "$C_NG" "$bad" "$n" "$C_END"
    exit 1
fi
printf "%s★ stage2 と stage3 が出す IR が完全一致（%d 本）%s\n" "$C_OK" "$n" "$C_END"

# ── ② 実行ファイルも一致するか ──
#
# 注意: リンカは実行ファイルに「毎回変わる印」を埋めます。
#    そのままだと必ず差が出るので、比較のときだけ止めます。
#      macOS … UUID           → -Wl,-no_uuid
#      Linux … build-id       → -Wl,--build-id=none
#    ★ 「違いが出た」ではなく「どこが違うのか」を調べてから判断すること。
#
# 注意: この比較は「おまけ」です。**本体は ① の IR の一致**なので、
#    印を止められない環境（Windows など）では飛ばします。
CLANG="${PLC_CLANG:-clang}"
case "$(uname -s 2>/dev/null || echo Unknown)" in
    Darwin) NOSTAMP="-Wl,-no_uuid" ;;
    Linux)  NOSTAMP="-Wl,--build-id=none" ;;
    *)      NOSTAMP="" ;;
esac

if [ -z "$NOSTAMP" ]; then
    printf "%s（実行ファイルの比較は、この環境では飛ばします）%s\n" "$C_DIM" "$C_END"
    echo
    printf "%s★ 不動点に到達しました（stage2 == stage3）%s\n" "$C_OK" "$C_END"
    printf "%s   本言語コンパイラは、自分自身をコンパイルできます。%s\n" "$C_DIM" "$C_END"
    exit 0
fi

# 注意: **出力の「名前」まで同じにします。**
#    Apple Silicon の macOS はリンク時に ad-hoc 署名を付け、その中に
#    **実行ファイル名から作った識別子**が入ります。cmp2 / cmp3 という
#    違う名前で作ると、中身が同じでも署名が変わって必ず差が出ます
#    （Intel の macOS では署名されないので気づけませんでした。CI が見つけた差）。
mkdir -p "$BOOT/cmp/a" "$BOOT/cmp/b"
if ! "$CLANG" -O0 "$BOOT"/stage2.*.ll "$PLC_RUNTIME_O" $NOSTAMP \
        -o "$BOOT/cmp/a/$LANG_CC" 2>/dev/null; then
    echo "比較用のリンクに失敗しました"
    exit 1
fi
"$CLANG" -O0 "$BOOT"/stage3.*.ll "$PLC_RUNTIME_O" $NOSTAMP \
    -o "$BOOT/cmp/b/$LANG_CC" 2>/dev/null

if cmp -s "$BOOT/cmp/a/$LANG_CC" "$BOOT/cmp/b/$LANG_CC"; then
    printf "%s★ 実行ファイルもバイト単位で一致（UUID を除く）%s\n" "$C_OK" "$C_END"
else
    printf "%s✗ 実行ファイルが違います%s\n" "$C_NG" "$C_END"
    cmp "$BOOT/cmp/a/$LANG_CC" "$BOOT/cmp/b/$LANG_CC" | head -3
    exit 1
fi

echo
printf "%s★ 不動点に到達しました（stage2 == stage3）%s\n" "$C_OK" "$C_END"
printf "%s   本言語コンパイラは、自分自身をコンパイルできます。%s\n" "$C_DIM" "$C_END"
