/* 行列積（二重ポインタ。本言語の list[list[float]] に対応する形） */
#include <stdlib.h>
#include "bench_time.h"
#define N 512
static double **make(long long seed) {
    double **m = malloc(N * sizeof(double *));
    long long s = seed;
    for (int i = 0; i < N; i++) {
        m[i] = malloc(N * sizeof(double));
        for (int j = 0; j < N; j++) {
            s = (s * 1103515 + 12345) % 2147483648LL;
            m[i][j] = (double)(s % 1000) / 1000.0;
        }
    }
    return m;
}
int main(void) {
    double **a = make(1), **b = make(7);
    double **c = malloc(N * sizeof(double *));
    for (int i = 0; i < N; i++) c[i] = calloc(N, sizeof(double));

    long long t0 = bench_ns();
    for (int i = 0; i < N; i++)
        for (int k = 0; k < N; k++) {
            double aik = a[i][k];
            for (int j = 0; j < N; j++) c[i][j] += aik * b[k][j];
        }
    long long t1 = bench_ns();

    double t = 0.0;
    for (int i = 0; i < N; i++) t += c[i][i];
    printf("RESULT %f\nTIME_MS %.3f\n", t, (double)(t1 - t0) / 1e6);
    return 0;
}
