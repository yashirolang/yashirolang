#!/bin/bash
# ベアメタル（RISC-V）の検証
#
# ★ QEMU の virt マシンでカーネルを起動し、**シリアルに出た文字列**を確かめます。
#   「動いた気がする」ではなく、出力で判定できるようにするのが目的です。
#
# 注意: 必要なもの: qemu-system-riscv64 / riscv64-elf-ld / Homebrew の LLVM。
#    無い環境では **スキップ**して緑にします（この本は macOS 以外でも読まれるため）。
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXT="$(make -s -C "$ROOT" print-LANG_EXT)"
ELF="$ROOT/build/kernel/kernel.elf"

if [ -t 1 ]; then
    C_OK=$'\033[32m'; C_NG=$'\033[31m'; C_DIM=$'\033[2m'; C_END=$'\033[0m'
else
    C_OK=''; C_NG=''; C_DIM=''; C_END=''
fi

for tool in qemu-system-riscv64 riscv64-elf-ld; do
    if ! command -v "$tool" > /dev/null 2>&1; then
        printf "  %sskip%s  ベアメタルの検証（%s が見つかりません）\n" \
               "$C_DIM" "$C_END" "$tool"
        exit 0
    fi
done

if [ ! -f "$ELF" ]; then
    echo "カーネルがありません: ${ELF}（先に 'make kernel'）"
    exit 1
fi

# 期待する出力（kernel/kernel$EXT が出すもの）
WANT=(
    "kernel on RISC-V (virt)"             # ベアメタルで print が動く
    "1 から 10 までの合計: 55"                #   for / range / str も動く
    "v2 の言葉: raises"                      #   list[str] も動く
    "tick 1"                                 # タイマ割り込みが来る
    "tick 3"                                 #   何度も来る（mtimecmp の再設定）
    "3 回割り込みが来ました"
    "halt"
)

OUT="$(timeout 10 qemu-system-riscv64 -machine virt -bios none -kernel "$ELF" \
        -nographic 2>/dev/null)"

fail=0
for w in "${WANT[@]}"; do
    if printf '%s' "$OUT" | grep -qF -- "$w"; then
        printf "  %sok%s    %s\n" "$C_OK" "$C_END" "$w"
    else
        printf "  %sFAIL%s  出力に含まれていません: %s\n" "$C_NG" "$C_END" "$w"
        fail=1
    fi
done

echo
echo "────────────────────────────────"
if [ "$fail" -eq 0 ]; then
    printf "%s★ ベアメタルの RISC-V でカーネルが動きました%s\n" "$C_OK" "$C_END"
    exit 0
fi
printf "%s✗ ベアメタルの検証に失敗しました%s\n" "$C_NG" "$C_END"
printf "%s実際の出力:%s\n%s\n" "$C_DIM" "$C_END" "$OUT"
exit 1
