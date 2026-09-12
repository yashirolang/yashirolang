/* 行列積（平坦な配列。C がいちばん速く書ける形） */
#include <stdlib.h>
#include "bench_time.h"
#define N 512
int main(void) {
    double *a = malloc(N*N*sizeof(double)), *b = malloc(N*N*sizeof(double));
    double *c = calloc(N*N, sizeof(double));
    long long s = 1;
    for (int i = 0; i < N*N; i++) { s = (s*1103515+12345) % 2147483648LL; a[i] = (double)(s%1000)/1000.0; }
    s = 7;
    for (int i = 0; i < N*N; i++) { s = (s*1103515+12345) % 2147483648LL; b[i] = (double)(s%1000)/1000.0; }

    long long t0 = bench_ns();
    for (int i = 0; i < N; i++)
        for (int k = 0; k < N; k++) {
            double aik = a[i*N+k];
            for (int j = 0; j < N; j++) c[i*N+j] += aik * b[k*N+j];
        }
    long long t1 = bench_ns();

    double t = 0.0;
    for (int i = 0; i < N; i++) t += c[i*N+i];
    printf("RESULT %f\nTIME_MS %.3f\n", t, (double)(t1 - t0) / 1e6);
    return 0;
}
