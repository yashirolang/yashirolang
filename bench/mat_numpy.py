# 注意: 同じ土俵ではありません（BLAS はブロック化・SIMD・マルチスレッド）。
#   「本言語がどこを目指しうるか」の上限として置いてあります。
import time
try:
    import numpy as np
except ImportError:
    print("RESULT 0")
    print("TIME_MS -1")
    raise SystemExit(0)

N = 512

def make(seed):
    s = seed
    d = np.empty(N * N)
    for i in range(N * N):
        s = (s * 1103515 + 12345) % 2147483648
        d[i] = (s % 1000) / 1000.0
    return d.reshape(N, N)

a = make(1)
b = make(7)
t0 = time.perf_counter()
c = a @ b
ms = (time.perf_counter() - t0) * 1000.0
print("RESULT %f" % float(np.trace(c)))
print("TIME_MS %.3f" % ms)
