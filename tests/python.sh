#!/bin/bash
# Python から呼ぶ試験（設計 docs/ja/design/ffi.md）
#
# ★ tests/python/<名前> のソースを --python で作り、隣の <名前>_test.py を走らせます。
#   test.py は最後に "ok" と出せば合格です（途中の assert で落ちれば不合格）。
#
# 注意: **python3 が無い環境では飛ばします**（失敗ではありません）。
#   コンパイラの構築にも、ライブラリの構築にも Python は要りません。
#   要るのは「Python から呼べるか」を確かめるこの試験だけです。
#
# 使い方:
#   tests/python.sh                       tests/python/ のソースを全部
#   PLC_CC=build/stage2 tests/python.sh   別のコンパイラで
set -u
# ★ panic の文面は実行時の言語で決まります（既定は日本語）。使う人の設定を外して、
#   既定の日本語で確かめます（docs/ja/design/i18n-diagnostics.md §10）。
unset PLC_MSG_LANG

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LANG_CC="$(make -s -C "$ROOT" print-LANG_CC)"
EXT="$(make -s -C "$ROOT" print-LANG_EXT)"
PLC_CC="${PLC_CC:-$ROOT/build/$LANG_CC}"
[ -x "$PLC_CC" ] || [ ! -x "$PLC_CC.exe" ] || PLC_CC="$PLC_CC.exe"
export PLC_LIB_DIR="$ROOT/lib"

PY="$(command -v python3 || command -v python || true)"
if [ -z "$PY" ]; then
    echo "  skip  Python の試験（python3 がありません）"
    exit 0
fi

TMP="$ROOT/tests/tmp/python"
rm -rf "$TMP"
mkdir -p "$TMP"

pass=0; fail=0
for src in "$ROOT"/tests/python/*"$EXT"; do
    name="$(basename "$src" "$EXT")"
    out="$TMP/$name"
    if ! "$PLC_CC" --python "$src" -o "$out" > "$TMP/$name.build.log" 2>&1; then
        echo "  FAIL  $name（ライブラリを作れませんでした）"
        sed 's/^/          /' "$TMP/$name.build.log" | head -10
        fail=$((fail + 1))
        continue
    fi
    res="$(cd "$out" && PYTHONPATH="$out" "$PY" "$ROOT/tests/python/${name}_test.py" 2>&1)"
    if [ "$(printf '%s' "$res" | tail -1)" = "ok" ]; then
        echo "  ok    $name (python)"
        pass=$((pass + 1))
    else
        echo "  FAIL  $name"
        printf '%s\n' "$res" | sed 's/^/          /' | tail -15
        fail=$((fail + 1))
    fi
done

echo "────────────────────────────────"
if [ "$fail" -eq 0 ]; then
    echo "Python: $pass 件すべて合格"
    exit 0
fi
echo "Python: $pass 件合格 / $fail 件失敗"
exit 1
