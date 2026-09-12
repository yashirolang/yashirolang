#!/bin/bash
# カバレッジを測る
#
# ★ 測るのは **C 版コンパイラ（src/*.c）の行・関数・分岐**です。
#   clang のソースベース計測（-fprofile-instr-generate -fcoverage-mapping）で
#   計装した処理系を建て、テストを全部通してから llvm-cov に集計させます。
#
# ⚠️ 本言語で書かれた側（selfhost/ と lib/）は**この方法では測れません**。
#   本言語自身に計装の仕組みが無いからです（ロードマップ B-2）。
#   代わりに tests/stdlib_usage.sh が「標準ライブラリのどの関数が
#   テストから呼ばれているか」を静的に数えます。
#
# 使い方:
#   tests/coverage.sh            全体の要約
#   tests/coverage.sh --detail   ファイルごとの内訳も出す

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# ★ 実行ファイル名は Makefile の LANG_CC が決めます（名前を書き写さない）。
LANG_CC="$(make -s -C "$ROOT" print-LANG_CC)"
EXT="$(make -s -C "$ROOT" print-LANG_EXT)"
COV="$ROOT/build/cov"
RAW="$COV/raw"
CC_BIN="$COV/$LANG_CC"

detail=0
[ "${1:-}" != "--detail" ] || detail=1

# ── llvm-profdata / llvm-cov を探す（Xcode → PATH → Homebrew）──
find_tool() {
    local name="$1" p
    p="$(command -v "$name" 2>/dev/null)" && { echo "$p"; return 0; }
    p="$(xcrun --find "$name" 2>/dev/null)" && { echo "$p"; return 0; }
    for d in "$(brew --prefix llvm 2>/dev/null)/bin" /usr/lib/llvm-*/bin; do
        [ -x "$d/$name" ] && { echo "$d/$name"; return 0; }
    done
    return 1
}

PROFDATA="$(find_tool llvm-profdata)" || {
    echo "llvm-profdata が見つかりません（LLVM を入れてください）"; exit 1; }
LLVM_COV="$(find_tool llvm-cov)" || {
    echo "llvm-cov が見つかりません（LLVM を入れてください）"; exit 1; }

if [ ! -x "$CC_BIN" ]; then
    echo "計装したコンパイラがありません: $CC_BIN"
    echo "'make coverage' から呼んでください（Makefile が同じ CFLAGS で建てます）"
    exit 1
fi

rm -rf "$RAW"
mkdir -p "$RAW"

echo "── テストを全部通す（計装した処理系で）──────"
export LLVM_PROFILE_FILE="$RAW/%p-%m.profraw"
export PLC_LIB_DIR="$ROOT/lib"
export PLC_RUNTIME_O="$ROOT/build/runtime.a"
# ★ 計装ビルドも C 版なので、STAGE0-ONLY のケース（所有権検査）も回します
export PLC_STAGE0=1
PLC_CC="$CC_BIN" "$ROOT/tests/run_tests.sh" > "$COV/tests.log" 2>&1
rc=$?
tail -2 "$COV/tests.log" | sed 's/^/  /'
if [ $rc -ne 0 ]; then
    echo "  ⚠️ テストが落ちています（カバレッジの数字は参考値です）"
fi

# ── オプションの巡回 ────────────────────────────────────────
#
# ★ run_tests.sh は「コンパイルして走らせる」経路しか通りません。
#   表示専用のオプション（--dump-ast / -S など）はここで一巡させます。
#   これを入れないと ast.c（S 式の表示）がほぼ未実行のままになります。
echo "── 表示系のオプションを一巡させる ──────────"
opts_run=0
for c in "$ROOT"/tests/cases/*$EXT; do
    "$CC_BIN" --dump-tokens "$c" > /dev/null 2>&1
    "$CC_BIN" --dump-ast    "$c" > /dev/null 2>&1
    opts_run=$((opts_run + 2))
done
for c in "$ROOT"/tests/mods/*/main$EXT; do
    "$CC_BIN" --check "$c" > /dev/null 2>&1
    opts_run=$((opts_run + 1))
done
# 残りの経路（IR 出力・所有権の説明・オブジェクト出力・別 target・最適化）
SAMPLE="$ROOT/tests/cases/fib$EXT"
[ -f "$SAMPLE" ] || SAMPLE="$(ls "$ROOT"/tests/cases/*$EXT | head -1)"
for extra in "-S" "--explain-mut" "--no-overflow-check -S" "-O2 -S" \
             "--target=riscv64-unknown-elf -S" "--drop -S" \
             "--deny-move --deny-borrow --deny-mut -S"; do
    # shellcheck disable=SC2086
    "$CC_BIN" $extra "$SAMPLE" > /dev/null 2>&1
    opts_run=$((opts_run + 1))
done
"$CC_BIN" -c "$SAMPLE" -o "$COV/sample.o" > /dev/null 2>&1
"$CC_BIN" --version > /dev/null 2>&1
"$CC_BIN" --help    > /dev/null 2>&1
"$CC_BIN" --nosuchoption "$SAMPLE" > /dev/null 2>&1
"$CC_BIN" > /dev/null 2>&1
opts_run=$((opts_run + 5))
echo "  $opts_run 回"

nraw=$(find "$RAW" -name '*.profraw' 2>/dev/null | wc -l | tr -d ' ')
echo "  収集したプロファイル: $nraw 本"
if [ "$nraw" -eq 0 ]; then
    echo "プロファイルが 1 つも出ていません"
    exit 1
fi

echo "── 集計 ────────────────────────────────────"
find "$RAW" -name '*.profraw' > "$COV/raw.list"
"$PROFDATA" merge -sparse -f "$COV/raw.list" -o "$COV/all.profdata" || exit 1

"$LLVM_COV" report "$CC_BIN" -instr-profile="$COV/all.profdata" \
    -ignore-filename-regex='(/usr/|/Applications/)' > "$COV/report.txt" 2>/dev/null

if [ "$detail" -eq 1 ]; then
    cat "$COV/report.txt"
else
    head -3 "$COV/report.txt"
    tail -3 "$COV/report.txt"
fi

"$LLVM_COV" show "$CC_BIN" -instr-profile="$COV/all.profdata" \
    -ignore-filename-regex='(/usr/|/Applications/)' \
    -show-line-counts-or-regions > "$COV/annotated.txt" 2>/dev/null

echo ""
echo "詳しい内訳: $COV/report.txt"
echo "行ごとの表示: $COV/annotated.txt"
