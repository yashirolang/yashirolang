#include "bench_time.h"
#define N 20000000
int main(void) {
    long long t0 = bench_ns();
    double s = 0.0, sign = 1.0;
    for (long long i = 0; i < N; i++) {
        s += sign / (double)(2 * i + 1);
        sign = -sign;
    }
    bench_report(s * 4.0, t0);
    return 0;
}
