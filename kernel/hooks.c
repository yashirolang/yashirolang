// hooks.c — カーネル側のランタイムのフック
//
// ★ core.c を libc から切り離したので、
//   ベアメタルで足りないのは **この 4 つだけ**です。
//   同じ core.c が、PC の上でもカーネルの中でも動きます。
#include "../runtime/core.h"

// ── UART（QEMU virt の 16550 互換。0x10000000）────────────────
//
// 注意: volatile が要ります。装置のレジスタは「書いた値が読めるとは限らない」
//    ので、最適化で消したりまとめたりされると壊れます。
static volatile unsigned char *const UART = (volatile unsigned char *)0x10000000UL;

static void uart_putc(char c) {
    if (c == '\n') *UART = '\r';  // 端末は CR+LF を期待する
    *UART = (unsigned char)c;
}

void pl_hook_write(const char *s, long long len) {
    for (long long i = 0; i < len; i++) uart_putc(s[i]);
}

// ── ヒープ（いちばん単純な bump allocator）──────────────────
//
// ★ 解放できません（pl_hook_free は何もしない）。
//   のちに本物のヒープに差し替えます。それまでは「確保しっぱなし」で足ります
//   （v1 のメモリモデルと同じ割り切り）。
extern char _end;  // リンカスクリプトが置く「カーネルの終わり」

static char *heap_next = 0;
static char *heap_limit = 0;

void *pl_hook_alloc(long long size) {
    if (!heap_next) {
        heap_next = &_end;
        heap_limit = heap_next + (8 << 20);  // 8MB
    }
    size = (size + 15) & ~15LL;  // 16 バイト境界にそろえる
    if (heap_next + size > heap_limit) return 0;
    char *p = heap_next;
    heap_next += size;
    for (long long i = 0; i < size; i++) p[i] = 0;  // ★ ゼロ初期化が約束
    return p;
}

void pl_hook_free(void *p) { (void)p; }

void pl_hook_panic(const char *msg) {
    pl_hook_write("panic: ", 7);
    long long n = 0;
    while (msg[n]) n++;
    pl_hook_write(msg, n);
    pl_hook_write("\n", 1);
    for (;;) __asm__ volatile("wfi");
}
