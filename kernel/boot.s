# boot.s — RISC-V の起動アセンブリ
#
# ★ QEMU の virt マシンは 0x80000000 に置いた ELF の先頭へ飛びます。
#   C や本言語が動く前に必要なのは 2 つだけです。
#     ① スタックポインタ（sp）を用意する
#     ② カーネル本体を呼ぶ
#
# ⚠️ ここだけはアセンブリで書きます。「スタックが無い状態で動くコード」は
#    高級言語では書けないからです（レジスタの使い方を言語が決めてしまうため）。

.section .text.start
.globl _start
_start:
    la sp, stack_top          # ① スタックを用意する
    la t0, trap_entry         # ② 割り込みの入口を登録する
    csrw mtvec, t0
    call kernel.main          # ③ 本言語で書いた main を呼ぶ
hang:
    wfi                       # 戻ってきたら、割り込みを待ちながら停止
    j hang

# ── 割り込みの入口 ────────────────────────────────────
#
# ★ ここもアセンブリでなければ書けません。割り込みは「関数の途中」に
#   割り込んでくるので、**壊してよいレジスタが 1 つもありません**。
#   呼び出し規約で「呼ばれた側が壊してよい」ことになっているレジスタ
#   （caller-saved）を、全部退避してから本言語の関数を呼びます。
.section .text
.align 4
.globl trap_entry
trap_entry:
    addi sp, sp, -128
    sd ra,   0(sp)
    sd t0,   8(sp)
    sd t1,  16(sp)
    sd t2,  24(sp)
    sd t3,  32(sp)
    sd t4,  40(sp)
    sd t5,  48(sp)
    sd t6,  56(sp)
    sd a0,  64(sp)
    sd a1,  72(sp)
    sd a2,  80(sp)
    sd a3,  88(sp)
    sd a4,  96(sp)
    sd a5, 104(sp)
    sd a6, 112(sp)
    sd a7, 120(sp)

    call kernel.trap          # ★ 中身は本言語で書く

    ld ra,   0(sp)
    ld t0,   8(sp)
    ld t1,  16(sp)
    ld t2,  24(sp)
    ld t3,  32(sp)
    ld t4,  40(sp)
    ld t5,  48(sp)
    ld t6,  56(sp)
    ld a0,  64(sp)
    ld a1,  72(sp)
    ld a2,  80(sp)
    ld a3,  88(sp)
    ld a4,  96(sp)
    ld a5, 104(sp)
    ld a6, 112(sp)
    ld a7, 120(sp)
    addi sp, sp, 128
    mret                      # 割り込まれた場所へ戻る

.section .bss
.align 16
stack_bottom:
    .space 16384              # 16KB のスタック
stack_top:
