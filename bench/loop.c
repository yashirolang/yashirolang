#include "bench_time.h"
#define N 20000000
int main(void) {
    long long t0 = bench_ns(), s = 0;
    for (long long i = 0; i < N; i++) s = (s + i * 3 + 7) % 1000000007LL;
    printf("RESULT %lld\nTIME_MS %.3f\n", s, (double)(bench_ns() - t0) / 1e6);
    return 0;
}
