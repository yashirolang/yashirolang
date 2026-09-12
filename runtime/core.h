// core.h — ランタイムの核が外に求めるもの
//
// ★ core.c は libc を呼びません。環境ごとに違う 4 つの操作だけを
//   「フック」として外から与えてもらいます。
//
//   PC の上で動かすとき      … runtime/hosted.c が実装する（calloc / stdout）
//   ベアメタル  … kernel/ 側が実装する（自前ヒープ / UART）
#ifndef PL_CORE_H
#define PL_CORE_H

// メモリを確保する（**ゼロ初期化されていること**）。足りなければ NULL。
void *pl_hook_alloc(long long size);

// 確保したメモリを返す。NULL を渡してもよい。
void pl_hook_free(void *p);

// 標準出力に相当する場所へ len バイト書く。
void pl_hook_write(const char *s, long long len);

// 回復不能なエラー。**戻ってきてはいけない**。
void pl_hook_panic(const char *msg);

#endif  // PL_CORE_H
