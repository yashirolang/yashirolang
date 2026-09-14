#!/bin/bash
# 解放（drop）の検査
#
# ★ tests/cases/drop_*$EXT と rc_*$EXT を **--drop 付き**で生成し、AddressSanitizer を
#   リンクして実行します。通常のテスト（run_tests.sh）では観測できない
#   **二重解放・解放後の使用**を、実行時に捕まえるための網です。
#
# ⚠️ **リーク検査（LeakSanitizer）は既定で切ります。**
#    この検査で見たいのは「二重解放」と「解放後の使用」——**壊れる間違い**です。
#    リークは宿題として残してあるもの（式の途中の一時値など）なので、
#    ここで落とすと「壊れていないのに赤い」状態が続いてしまいます。
#
#    ★ リークを数えたいときは `tests/drop_asan.sh --leaks` か `make drop-leak`。
#      Linux でだけ動きます（macOS の ASan に LeakSanitizer は入っていません）。
#
# 使い方:
#   tests/drop_asan.sh                     drop_*$EXT を全部
#   tests/drop_asan.sh tests/cases/x$EXT    1 本だけ

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# ★ 実行ファイル名は Makefile の LANG_CC / LANG_PM が決めます。
#   ここに名前を書き写すと改名のたびに直すことになるので、make に訊きます。
LANG_CC="$(make -s -C "$ROOT" print-LANG_CC)"
EXT="$(make -s -C "$ROOT" print-LANG_EXT)"
LANG_PM="$(make -s -C "$ROOT" print-LANG_PM)"
PLC_CC="${PLC_CC:-$ROOT/build/$LANG_CC}"
# ★ Windows（MSYS2）では .exe が付きます
[ -x "$PLC_CC" ] || [ ! -x "$PLC_CC.exe" ] || PLC_CC="$PLC_CC.exe"
TMP="$ROOT/tests/tmp"
mkdir -p "$TMP"

# ⚠️ Windows では ws2_32 を明示的にリンクします（tests/selfhost.sh と同じ理由）。
LINK_LIBS=""
case "$(uname -s 2>/dev/null || echo unknown)" in
    MINGW*|MSYS*|CYGWIN*) LINK_LIBS="-lws2_32" ;;
esac

DETECT_LEAKS=0
if [ "${1:-}" = "--leaks" ]; then
    DETECT_LEAKS=1
    shift
fi
export ASAN_OPTIONS="detect_leaks=$DETECT_LEAKS"

if [ $# -gt 0 ]; then
    CASES=("$@")
else
    CASES=()
    while IFS= read -r line; do CASES+=("$line"); done \
        < <(ls "$ROOT"/tests/cases/drop_*$EXT "$ROOT"/tests/cases/rc_*$EXT 2>/dev/null | sort)
fi

if [ -t 1 ]; then
    C_OK=$'\033[32m'; C_NG=$'\033[31m'; C_DIM=$'\033[2m'; C_END=$'\033[0m'
else
    C_OK=''; C_NG=''; C_DIM=''; C_END=''
fi

# ★ AddressSanitizer が **使えるかどうかを、実際に試して**確かめます。
#   ASan は「clang に付いてくる別のライブラリ」です。clang があっても実体
#   （libclang_rt.asan）が入っていないことがあり、MSYS2 の clang がそれです。
#   version を見て判断すると外すので、**小さいプログラムを 1 本リンクしてみる**
#   のがいちばん確実です（CI の Windows が教えてくれた差）。
CLANG="${PLC_CLANG:-clang}"
if ! echo 'int main(void){return 0;}' \
        | "$CLANG" -fsanitize=address -x c - -o "$TMP/asan_probe" 2>/dev/null; then
    printf "%s（この環境には AddressSanitizer が無いので、検査を飛ばします）%s\n" \
           "$C_DIM" "$C_END"
    exit 0
fi
rm -f "$TMP/asan_probe" "$TMP/asan_probe.exe"

pass=0
fail=0

for f in "${CASES[@]}"; do
    name="$(basename "$f")"
    base="${name%$EXT}"

    # ★ --keep-ll で .ll を残させます（-S は複数モジュールを 1 本に並べるだけで
    #   リンクできないため。import を使うケースもここで検査したい）。
    rm -f "$TMP/$base.drop".*.ll
    if ! "$PLC_CC" --drop --keep-ll "$f" -o "$TMP/$base.drop" 2>"$TMP/$base.warn"; then
        printf "  %sFAIL%s  %s（IR の生成に失敗）\n" "$C_NG" "$C_END" "$name"
        head -5 "$TMP/$base.warn" | sed 's/^/          /'
        fail=$((fail + 1))
        continue
    fi

    # ★ ランタイムも一緒に ASan でビルドする（解放するのはランタイム側なので）
    if ! "$CLANG" -fsanitize=address -O0 "$TMP/$base.drop".*.ll "$ROOT/runtime/core.c" "$ROOT/runtime/hosted.c" \
            $LINK_LIBS -o "$TMP/$base.asan" 2>"$TMP/$base.link"; then
        printf "  %sFAIL%s  %s（リンクに失敗）\n" "$C_NG" "$C_END" "$name"
        head -5 "$TMP/$base.link" | sed 's/^/          /'
        fail=$((fail + 1))
        continue
    fi

    out="$("$TMP/$base.asan" < /dev/null 2>"$TMP/$base.asan.err")"
    rc=$?

    # ASan は問題を見つけると stderr に "ERROR: AddressSanitizer" を出して
    # 終了コードを 1 にします。期待する終了コードはケースごとに違うので、
    # 「ASan の報告が出ていないこと」を合格条件にします。
    if grep -q "AddressSanitizer" "$TMP/$base.asan.err"; then
        printf "  %sFAIL%s  %s\n" "$C_NG" "$C_END" "$name"
        head -8 "$TMP/$base.asan.err" | sed 's/^/          /'
        fail=$((fail + 1))
        continue
    fi

    printf "  %sok%s    %s %s(asan, exit=%d)%s\n" "$C_OK" "$C_END" "$name" \
           "$C_DIM" "$rc" "$C_END"
    pass=$((pass + 1))
done

echo
echo "────────────────────────────────"
if [ "$fail" -eq 0 ]; then
    if [ "$DETECT_LEAKS" = "1" ]; then
        printf "%s全 %d 件がリーク無しで通りました%s\n" "$C_OK" "$pass" "$C_END"
    else
        printf "%s全 %d 件が AddressSanitizer で問題なし（二重解放・解放後の使用）%s\n" \
               "$C_OK" "$pass" "$C_END"
    fi
    exit 0
else
    printf "%s%d 件パス / %d 件失敗%s\n" "$C_NG" "$pass" "$fail" "$C_END"
    exit 1
fi
