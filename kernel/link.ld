/* link.ld — カーネルの配置
 *
 * ★ QEMU の virt マシンは、-bios none のとき 0x80000000 から実行を始めます。
 *   .text.start（boot.s）を必ず先頭に置くのが要点です。
 */
ENTRY(_start)

SECTIONS {
    . = 0x80000000;

    .text : {
        *(.text.start)      /* ★ 起動コードを先頭に */
        *(.text*)
    }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }

    _end = .;
}
