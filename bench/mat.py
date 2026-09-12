import time
N = 512

def make(seed):
    m = []
    s = seed
    for i in range(N):
        row = []
        for j in range(N):
            s = (s * 1103515 + 12345) % 2147483648
            row.append((s % 1000) / 1000.0)
        m.append(row)
    return m

a = make(1)
b = make(7)
c = [[0.0] * N for _ in range(N)]

t0 = time.perf_counter()
for i in range(N):
    ai = a[i]
    ci = c[i]
    for k in range(N):
        aik = ai[k]
        bk = b[k]
        for j in range(N):
            ci[j] += aik * bk[j]
ms = (time.perf_counter() - t0) * 1000.0

t = sum(c[i][i] for i in range(N))
print("RESULT %f" % t)
print("TIME_MS %.3f" % ms)
