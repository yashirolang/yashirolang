# tests/python.sh が回す。隣の kit を --python で作ったものを呼ぶ（設計 ffi.md）
import threading

import kit


def raises(exc, f, *a, **k):
    try:
        f(*a, **k)
    except exc as e:
        return str(e)
    raise AssertionError(f"{exc.__name__} が起きませんでした: {f.__name__}{a}")


# ── 値の往復 ──
assert kit.add(2, 3) == 5
assert kit.at([1, 2, 3], 1) == 2
assert kit.mean([1.0, 2.0, 4.0]) == 7.0 / 3.0
assert kit.greet("yo") == "yo! "
assert kit.greet("yo", times=2, sep="+") == "yo+yo+"
assert kit.greet("日本") == "日本! "
assert kit.flip([True, False]) == [False, True]
assert kit.twice(["a", "ß", ""]) == ["aa", "ßß", ""]
assert kit.squares(4) == [0.0, 1.0, 4.0, 9.0]
assert kit.squares(0) == []
assert kit.is_even(4) is True
assert kit.scale() == -3.0
assert kit.total([]) == 0

# ── raises → 例外（クラスごと。共通の親は kit.Error）──
assert raises(kit.EmptyError, kit.mean, []) == "空の list の平均は決まりません"
assert issubclass(kit.EmptyError, kit.Error)
assert raises(kit.Plain, kit.check, -1) == "Plain"      # message が無ければクラス名
assert raises(kit.EmptyError, kit.check, 0) == "0 です"
assert kit.check(5) == 5

# ── panic → kit.Panic（Error の子ではない）。その後も続けて呼べる ──
#   注意: panic の文面は実行時の言語で決まり、既定は日本語です（i18n-diagnostics.md §10）。
#     言語はライブラリを読み込んだときの PLC_MSG_LANG で決まります（tests/python.sh は外して走ります）。
assert "添字が範囲の外です" in raises(kit.Panic, kit.at, [1, 2, 3], 5)
assert "あふれました" in raises(kit.Panic, kit.add, 2**62, 2**62)
assert "契約違反" in raises(kit.Panic, kit.half, 3)
assert "exit(3)" in raises(kit.Panic, kit.bye, 3)
assert not issubclass(kit.Panic, kit.Error)
assert kit.half(8) == 4

# ── 型は混ぜない（Python 側で先に断る）──
raises(TypeError, kit.mean, [1, 2])
raises(TypeError, kit.add, True, 1)
raises(TypeError, kit.add, 1.0, 1)
raises(TypeError, kit.at, (1, 2), 0)
raises(TypeError, kit.greet, b"x")
raises(OverflowError, kit.add, 2**63, 1)

# ── Python の複数のスレッドから同時に呼ぶ（GIL を手放すので本当に並ぶ）──
data = list(range(100000))
got = []
lock = threading.Lock()


def work():
    for _ in range(10):
        r = kit.total(data)
        with lock:
            got.append(r)


ts = [threading.Thread(target=work) for _ in range(8)]
for t in ts:
    t.start()
for t in ts:
    t.join()
assert got == [sum(data)] * 80, set(got)

print("ok")
