import time
N = 20000000
t0 = time.perf_counter()
s = 0
for i in range(N):
    s = (s + i * 3 + 7) % 1000000007
ms = (time.perf_counter() - t0) * 1000.0
print("RESULT %d" % s)
print("TIME_MS %.3f" % ms)
