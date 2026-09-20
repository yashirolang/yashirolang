#!/bin/bash
# bench/run.sh — 速さを測る。
#
# ★ 測り方の約束（docs/roadmap.md §3-B1〜B3 で直したこと）
#   ① **プロセスの起動時間を引き算しません。** どのプログラムも自分の中で
#      単調時計を読み、測りたいループだけの時間を TIME_MS として出します
#      （本言語は lib/time$EXT、C は bench_time.h、Python は perf_counter）。
#      参考として「起動だけ」の時間も別の行で出します。
#   ② **最適化の水準を必ず書きます。** コンパイラの既定は -O0 です。
#      -O0 と -O2 を別の行にして、どちらの数字か分からなくならないようにします。
#   ③ **答え合わせをします。** 言語ごとに RESULT を突き合わせ、
#      違っていたら赤字で出します（速いが間違っている、を防ぐため）。
#
# 使い方:
#   bench/run.sh            全部
#   bench/run.sh loop mat   種目を選ぶ
#   BENCH_REPS=5 bench/run.sh   繰り返し回数（既定 3。最小値を採る）
set -u
cd "$(dirname "$0")"
ROOT="$(cd .. && pwd)"
# ★ 実行ファイル名は Makefile の LANG_CC / LANG_PM が決めます。
#   ここに名前を書き写すと改名のたびに直すことになるので、make に訊きます。
LANG_CC="$(make -s -C "$ROOT" print-LANG_CC)"
EXT="$(make -s -C "$ROOT" print-LANG_EXT)"
LANG_PM="$(make -s -C "$ROOT" print-LANG_PM)"
POC="$ROOT/build/$LANG_CC"
[ -x "$POC" ] || POC="$POC.exe"
export PLC_RUNTIME_O="$ROOT/build/runtime.a"
# 注意: stage1（セルフホスト版）は標準ライブラリの場所を**実行時に**環境変数で読みます
#   （C 版はビルド時に埋め込み）。これが無いと import io で即座に失敗します。
export PLC_LIB_DIR="$ROOT/lib"
REPS=${BENCH_REPS:-3}
CC=${CC:-clang}
# ★ 作ったものは全部 out/ に入れます（bench/ 自体は git で追跡するため）。
OUT=out
mkdir -p "$OUT"

if [ ! -x "$POC" ]; then
    echo "コンパイラがありません。先に make を実行してください。" >&2
    exit 1
fi


# 表示幅で左詰めする（日本語は 2 桁ぶんの幅を取る）。
# 注意: printf の %-30s は**バイト数**で数えるので、日本語が入ると崩れます。
#   LC_ALL=C の awk（バイト単位）で「ASCII は 1、それ以外は 3 バイトで 2 桁」と数えます。
pad() {
    LC_ALL=C awk -v s="$1" -v w="$2" 'BEGIN{
        n = 0; b = 0
        for (i = 1; i <= length(s); i++) {
            c = substr(s, i, 1)
            if (c ~ /^[\001-\177]$/) n++; else b++
        }
        d = n + b / 3 * 2
        pad = ""
        while (d + length(pad) < w) pad = pad " "
        printf "%s%s", s, pad
    }'
}

BASE=""      # 比の分母（C -O2 の時間）
FAIL=0

# measure <表示名> <コマンド...>
#   プログラムが出す TIME_MS の最小値と RESULT を拾って 1 行にする。
measure() {
    local name="$1"; shift
    if ! command -v "$1" >/dev/null 2>&1 && [ ! -x "$1" ]; then
        printf '  %s %10s\n' "$(pad "$name" 34)" "（なし）"
        return
    fi
    local best="" res="" out t r
    for _ in $(seq "$REPS"); do
        out=$("$@" 2>/dev/null) || { printf '  %s %10s\n' "$(pad "$name" 34)" "（失敗）"; return; }
        t=$(printf '%s\n' "$out" | awk '/^TIME_MS/{print $2}')
        r=$(printf '%s\n' "$out" | awk '/^RESULT/{print $2}')
        [ -n "$t" ] || { printf '  %s %10s\n' "$(pad "$name" 34)" "（TIME_MS 無し）"; return; }
        # 注意: 負の TIME_MS は「その処理系が入っていない」の合図です（numpy など）
        case "$t" in -*) printf '  %s %10s\n' "$(pad "$name" 34)" "（なし）"; return;; esac
        best=$(awk -v a="$t" -v b="$best" 'BEGIN{print (b=="" || a+0<b+0) ? a : b}')
        res="$r"
    done
    local ratio="—"
    if [ -n "$BASE" ]; then
        ratio=$(awk -v a="$best" -v b="$BASE" 'BEGIN{printf "%.1f 倍", a/b}')
    fi
    printf '  %s %10s ms  %8s   %s\n' "$(pad "$name" 34)" "${best}" "${ratio}" "${res}"
    LAST_BEST="$best"
    LAST_RES="$res"
}

# set_base <コマンド...> — この種目の分母（C -O2）を測る
set_base() {
    BASE=""
    measure "$@"
    BASE="${LAST_BEST:-}"
    REF_RES="${LAST_RES:-}"
}

# 答え合わせ（RESULT が分母のものと一致しているか）
#
# 注意: 表記は言語ごとに違います（C の printf %f は "65440.873640"、
#   本言語の str(float) は "65440.87364"）。**数として**比べます。
#   相対誤差 1e-9 まで許します（浮動小数の足す順序は同じなので、本来は一致します）。
check() {
    [ -n "${REF_RES:-}" ] && [ -n "${LAST_RES:-}" ] || return 0
    local same
    same=$(awk -v a="${REF_RES}" -v b="${LAST_RES}" 'BEGIN{
        d = a - b; if (d < 0) d = -d
        m = (a < 0 ? -a : a); if (m < 1) m = 1
        print (d / m < 1e-9) ? "1" : "0"
    }')
    if [ "$same" != "1" ]; then
        echo "    答えが違います: ${LAST_RES} （C は ${REF_RES}）"
        FAIL=1
    fi
}

build_c()  { $CC -O2 -o "$2" "$1" || exit 1; }
build_po() { "$POC" "$3" "$1" -o "$2" || exit 1; }

want() {   # 種目を選ぶ（引数が無ければ全部）
    [ -z "$SEL" ] && return 0
    case " $SEL " in *" $1 "*) return 0;; esac
    return 1
}
SEL="$*"

echo "═══ ビルド ═══"
$CC -O0 -o $OUT/empty_c_O0 empty.c && $CC -O2 -o $OUT/empty_c_O2 empty.c
"$POC" -O0 empty$EXT -o $OUT/empty_po_O0 && "$POC" -O2 empty$EXT -o $OUT/empty_po_O2
for f in loop fp mat; do
    $CC -O0 -o $OUT/${f}_c_O0 $f.c && $CC -O2 -o $OUT/${f}_c_O2 $f.c
    "$POC" -O0 $f$EXT -o $OUT/${f}_po_O0
    "$POC" -O2 $f$EXT -o $OUT/${f}_po_O2
    "$POC" -O2 --no-overflow-check $f$EXT -o $OUT/${f}_po_O2_noovf
    # 注意: Rust は既定（-O）だと**桁あふれを検査しません**。本言語は検査するので、
    #   条件を揃えた -C overflow-checks=on の版も作ります。
    if command -v rustc >/dev/null 2>&1; then
        (cd "$OUT" && rustc -O -o ${f}_rs ../$f.rs 2>/dev/null
         rustc -O -C overflow-checks=on -o ${f}_rs_ovf ../$f.rs 2>/dev/null)
    fi
done
$CC -O2 -o $OUT/mat_flat_c_O2 mat_flat.c
"$POC" -O2 mat_flat$EXT -o $OUT/mat_flat_po_O2
"$POC" -O2 mat_linalg$EXT -o $OUT/mat_linalg_po_O2
"$POC" -O2 --no-overflow-check mat_linalg$EXT -o $OUT/mat_linalg_po_O2_noovf
"$POC" -O0 mat_linalg$EXT -o $OUT/mat_linalg_po_O0
echo "完了"

echo ""
echo "═══ プロセスの起動（この時間は下の表に入っていません）═══"
tstart() {   # 外から測るしかないもの（＝プロセスの起動を含む時間）
    local name="$1"; shift
    local best="" t sec
    local TIMEFORMAT='%3R'
    if ! "$@" >/dev/null 2>&1; then
        printf '  %s %10s\n' "$(pad "$name" 34)" "（失敗）"
        return
    fi
    for _ in $(seq "$REPS"); do
        sec=$( { time "$@" >/dev/null 2>&1; } 2>&1 | tail -1 )
        t=$(awk -v s="$sec" 'BEGIN{printf "%.3f", s * 1000}')
        best=$(awk -v a="$t" -v b="$best" 'BEGIN{print (b=="" || a+0<b+0) ? a : b}')
    done
    printf '  %s %10s ms\n' "$(pad "$name" 34)" "$best"
}
tstart "C   （何もしない）" $OUT/empty_c_O2
tstart "本言語（何もしない）" $OUT/empty_po_O2

if want loop; then
echo ""
echo "═══ 整数ループ 2000 万回 ═══"
set_base "C            -O2" $OUT/loop_c_O2
measure "Rust         -O（検査なし）" $OUT/loop_rs
measure "Rust         -O（検査あり）" $OUT/loop_rs_ovf
measure "本言語     -O2" $OUT/loop_po_O2;            check
measure "本言語     -O2（検査なし）" $OUT/loop_po_O2_noovf; check
measure "C            -O0" $OUT/loop_c_O0
measure "本言語     -O0（既定）" $OUT/loop_po_O0;    check
measure "Python 3" python3 loop.py
fi

if want fp; then
echo ""
echo "═══ float の級数 2000 万項 ═══"
set_base "C            -O2" $OUT/fp_c_O2
measure "Rust         -O" $OUT/fp_rs
measure "本言語     -O2" $OUT/fp_po_O2
measure "C            -O0" $OUT/fp_c_O0
measure "本言語     -O0（既定）" $OUT/fp_po_O0
measure "Python 3" python3 fp.py
fi

if want mat; then
echo ""
echo "═══ 行列積 512³（二重リスト／二重ポインタ）═══"
set_base "C   二重ポインタ -O2" $OUT/mat_c_O2
measure "C   平坦配列     -O2" $OUT/mat_flat_c_O2
measure "Rust Vec<Vec<f64>> -O" $OUT/mat_rs
measure "本言語二重リスト -O2" $OUT/mat_po_O2;            check
measure "本言語二重リスト -O2（検査なし）" $OUT/mat_po_O2_noovf; check
measure "本言語二重リスト -O0（既定）" $OUT/mat_po_O0;    check
measure "Python 3" python3 mat.py
measure "numpy（BLAS。土俵が違う）" python3 mat_numpy.py

echo ""
echo "═══ 行列積 512³（平坦な list[float]。版分けが効く形）═══"
echo "  ★ C の平坦配列と同じ書き方です。ここが「素の三重ループ」の実力です。"
measure "本言語平坦     -O2" $OUT/mat_flat_po_O2;            check

echo ""
echo "═══ 行列積 512³（linalg.Matrix ＝ 平坦 ＋ get/set）═══"
echo "  ★ 上の「二重リスト」との差が、小さなメソッドのインライン化の指標です。"
measure "本言語 linalg  -O2" $OUT/mat_linalg_po_O2;            check
measure "本言語 linalg  -O2（検査なし）" $OUT/mat_linalg_po_O2_noovf; check
measure "本言語 linalg  -O0（既定）" $OUT/mat_linalg_po_O0;    check
fi

if want compile; then
echo ""
echo "═══ コンパイルの速さ（注意: どれもプロセスの起動を含みます）═══"
echo "  ★ このコンパイラは LLVM IR のテキストを出すところまでが自分の仕事で、"
echo "    最適化とリンクは clang に任せます。-S は「自分の仕事だけ」の時間です。"
tstart "$LANG_CC -S mat${EXT}（46 行）" "$POC" -S mat$EXT
tstart "$LANG_CC -S selfhost（10641 行）" "$POC" -S "$ROOT/selfhost/main$EXT"
tstart "$LANG_CC --check selfhost" "$POC" --check "$ROOT/selfhost/main$EXT"
tstart "$LANG_CC -O2 mat${EXT}（clang 込み）" "$POC" -O2 mat$EXT -o $OUT/mat_tmp_out
tstart "clang    -O2 mat.c（比較）" $CC -O2 mat.c -o $OUT/mat_tmp_out_c
fi

if want selfhost; then
echo ""
echo "═══ 自己コンパイル（型検査まで。注意: 起動時間を含みます）═══"
tstart "C 版 stage0" "$POC" --check "$ROOT/selfhost/main$EXT"
if [ -x "$ROOT/build/stage1" ]; then
    tstart "セルフホスト版 stage1" "$ROOT/build/stage1" --check "$ROOT/selfhost/main$EXT"
else
    printf '  %s %10s\n' "$(pad "セルフホスト版 stage1" 34)" "（make bootstrap が要ります）"
fi
fi

echo ""
[ $FAIL -eq 0 ] && echo "答えはすべて一致しました。" || echo "注意: 答えの食い違いがあります。"
