import time
N = 20000000
t0 = time.perf_counter()
s = 0.0
sign = 1.0
i = 0
while i < N:
    s += sign / float(2 * i + 1)
    sign = -sign
    i += 1
ms = (time.perf_counter() - t0) * 1000.0
print("RESULT %f" % (s * 4.0))
print("TIME_MS %.3f" % ms)
