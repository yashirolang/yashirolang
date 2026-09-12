/* bench_time.h — C 側の計測。本言語の lib/time と同じ単調時計を使う。
 *
 * ★ 測るのは「中の処理だけ」です。プロセスの起動時間は入りません
 *   （docs/roadmap.md §3-B1 で、起動時間の引き算が比を歪めていたため）。
 */
#ifndef BENCH_TIME_H
#define BENCH_TIME_H
#include <stdio.h>
#include <time.h>

static long long bench_ns(void) {
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
#endif
    if (timespec_get(&ts, TIME_UTC) == TIME_UTC)
        return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
    return 0;
}

static void bench_report(double result, long long t0) {
    printf("RESULT %f\nTIME_MS %.3f\n", result, (double)(bench_ns() - t0) / 1e6);
}
#endif
