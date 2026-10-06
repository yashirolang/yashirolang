# `yashirolang` Reference

```
yashirolang [options] <input.ys>
```

You compile **only the entry file**. `import`s are followed automatically.

```bash
yashirolang hello.ys -o hello
```

---

## Output

| Option | Description |
|---|---|
| `-o <file>` | Name of the executable to write (default `a.out`). On Windows, `.exe` is appended to names without a `.` |
| `-c` | Emit an object file (`.o`) without linking |
| `--shared` | Build a shared library. `extern def`s with a body can be called from C as `<module>_<function>`. Without `-o`: `lib<module>.<ext>` ([design/ffi.md](../../ja/design/ffi.md) *(Japanese)*) |
| `--python` | Build a Python library. `-o` is a **directory** (default `.`); writes `_<module>.<ext>` and `<module>.py` (generated ctypes code) |
| `-I <dir>` | Add a place to search for `import`s (repeatable; `-Ideps` also works). The package manager [`ysm`](../../ja/reference/pkg.md) uses it to pass `deps/` |
| `-j <N>` | How many `clang` jobs to run at once (default: number of cores; `-j1` for sequential). Note: **the resulting executable does not depend on this** |
| `-S` | Write LLVM IR to stdout and exit |
| `--keep-ll` | Keep the `.ll` files after building the executable |
| `-O0` / `-O1` / `-O2` / `-O3` | Optimization level passed to clang (default `-O0`) |
| `--target=<triple>` | Target triple of the generated IR (e.g. `--target=riscv64-unknown-elf`) |

### `-I` and search order

`import x` is looked up in three places:

1. The directory of the entry file
2. Directories added with `-I` (in the order given)
3. The standard library (`lib/`)

**There is no precedence.** If a module is found in more than one place, it is an error —
whichever came first, something would be silently shadowed.

## Check only

| Option | Description |
|---|---|
| `--check` | Stop after type checking. Prints **nothing** if there are no problems (use the exit code) |

## Safety checks

**Everything is an error by default** (since 0.18.0, A-24). Double frees and use-after-free
stop the build without any options.

| Option | Description |
|---|---|
| `--warn-own` | **Downgrade** ownership findings to warnings (`E-MOVE-*` / `E-BORROW-*` / `E-MUT-*`). Note: the default before 0.17. **Code built with this is outside the language's safety guarantee** |
| `--deny-move` | Make use of moved values an error (`E-MOVE-1`, **default**) |
| `--deny-borrow` | Make storing/returning borrowed values an error (`E-BORROW-*`, **default**) |
| `--deny-mut` | Make writes through read-only borrows an error (`E-MUT-1`, **default**) |
| `--deny-store-borrow` | Make putting borrowed values into owning slots an error (`E-BORROW-7` / `E-BORROW-8`, **default**) |
| `--drop` | Insert frees at scope exits (**default**) |
| `--no-drop` | Do not insert frees (cancels `--drop`; **the last one wins**) |
| `--explain-mut` | List the arguments each call modifies, then exit |

**`--deny-*` exists so that, after `--warn-own`, you can turn checks back on one at a time.**
**The last one wins** (same rule as `--drop` / `--no-drop`).

```bash
yashirolang app.ys -o app                        # all four are errors (default)
yashirolang --warn-own app.ys -o app             # all four are warnings
yashirolang --warn-own --deny-move app.ys -o app # only moves are errors again
```

When bringing in old code, get everything through with `--warn-own` first,
then raise the checks one by one with `--deny-*`.

**Frees are inserted by default.** When a scope ends, the values it owned are freed.
The ownership checker decides what may be freed (borrowed and moved-from values are not).

Note: you only need `--no-drop` when you **deliberately write unsafe code to show a diagnostic**
(as the language's own tests do). Normally you never use it.

Note: **`--warn-own` has no effect in files that use `spawn`.** Downgrading ownership findings
would also downgrade the "no data races" guarantee to a warning (`E-SEND-*` is built directly on
the ownership checker's judgment of borrowed / owned / `rc`).

The checks for whether a value may be sent to a thread (`E-SEND-1` to `4`) are **always errors**;
there is no option to turn them off. **Both implementations behave the same** (A-25).

## Number checks

| Option | Description |
|---|---|
| (default) | At run time, check overflow of integer `+` `-` `*` `**`, unary `-`, `int(float)` / `int(str)`, and division by zero in `//` `%` `/` |
| `--no-overflow-check` | Remove the **overflow and float division-by-zero** checks above (an escape hatch for speed) |

Note: **index bounds checks cannot be removed.** They stay even with `--no-overflow-check`.
Where you want wrapping arithmetic, use `wrap_add` / `wrap_sub` / `wrap_mul`
(no option needed).

## Linking C libraries

| Option | Description |
|---|---|
| `-l<name>` / `-L<dir>` | Passed to clang as-is when linking (`-lopenblas`, etc.) |
| `-framework <name>` | Same (macOS frameworks, e.g. `-framework Accelerate`) |

```bash
yashirolang -O2 app.ys -framework Accelerate -o app   # macOS
yashirolang -O2 app.ys -lopenblas -o app              # Linux
```

You need these **when a function declared with `extern def` is implemented outside the standard library**
([`blas`](../../ja/reference/stdlib.md#blas) is an example).

Since they go through the shell, they are validated **once at the entry point**, like `-o`.

## Proofs (removing runtime checks, A-34)

| Option | Description |
|---|---|
| (default) | **Remove** runtime checks that interval analysis proves always hold (nothing extra to write) |
| `--no-prove` | Remove none (an escape hatch for comparison) |
| `--verify-prove` | **Keep** the checks the prover would remove. If one fails, it stops with `prover was wrong (this is a compiler bug)` |
| `--prove-report` | Print how many checks were removed, per kind |

```
$ yashirolang --lang=en --prove-report -S examples/fizzbuzz.ys > /dev/null
runtime checks removed by the prover:
  overflow          1 removed /      0 kept
  index             0 removed /      0 kept
  range type        0 removed /      0 kept
  contract          0 removed /      0 kept
  div by zero       3 removed /      0 kept
```

Five kinds can be removed: overflow (`+` `-` `*`), indexing (`xs[i]`), range types (A-28),
contracts (A-29), and **division by zero in `//` and `%`**. The last one is replaced by a single
instruction only when the divisor is a **positive compile-time constant**
(run-time divisors did not get faster; see [changelog 0.25.0–0.27.0](../../ja/changelog.md) *(Japanese)*).

Note: **`--verify-prove` is meant for CI.** If the analysis wrongly removes a check,
the bug comes back **to us, not to users**. It has already found one soundness bug
(0.25.0–0.26.0). **Run `make prove-verify` in CI.**

Note: with `--no-overflow-check`, the prover can show less. Integers may wrap, so
"if we got here, the value is in range" no longer holds (it errs on the safe side).

## Debugging

| Option | Description |
|---|---|
| `-g` | Emit debug info (the metadata DWARF is built from). Note: without it, the output does not change by a single byte |

```bash
yashirolang -g app.ys -o app
lldb ./app            # breakpoints, backtraces and variable contents
perf record ./app     # shows which lines are hot
```

**Three things are emitted: function frames, the line table, and variable names and types**
(A-30 and A-35). Besides stopping, stepping and profiling, you can **look inside values**.

```
(lldb) frame variable
(int) total = 9
(bool) ok = true
(str) msg = 0x0000000100003f28 "point"
(list[int]) xs = 0x0000600000d04000
(Point) p = 0x0000600000904030

(lldb) p *xs                 ← a list can be opened up
(list[int]) {
  data = 0x0000600000d04020
  len = 3
  cap = 4
}
(lldb) p xs->data[1]
(int) 2
(lldb) p *p                  ← a class shows its fields
(Point) (x = 4, y = 2.5)

(lldb) bt                    ← argument values appear in backtraces
  * frame #0: util.bump(c=0x0000600000d04040, by=5) at util.ys:8:1
    frame #1: main.main at main.ys:5:1
```

| Type you wrote | How the debugger shows it |
|---|---|
| `int` / `float` / `bool` | As plain values (`42` / `1.5` / `true`) |
| Range types (`type Percent = int range(0, 100)`) | `(Percent) rate = 42` (**under its own name**) |
| `str` | `0x... "hello"` (contents shown) |
| `list[T]` | `data` / `len` / `cap`; elements are `xs->data[i]` |
| Classes | Fields by name (`p *obj`) |
| `rc[T]` | `strong` / `borrow` / `value` (**the counts are visible too**) |
| `T \| None` | Same as `T`; None is `0x0` (null) |
| Tuples, functions, `Thread[R]` / `mutex[T]` | Address only (contents not described yet) |

Note: **reference types (`str` / `list` / classes / `rc`) stay pointers.**
`p xs` prints the address, `p *xs` the contents. Showing contents by default would leave
nothing but "unreadable" for a `T | None` that is None.

Note: **elements of `list[bool]` show up as 0 / 1 ints.** Every list element is 8 bytes,
and describing an 8-byte bool in DWARF makes the debugger refuse to index it (`len` in `p xs` is correct).

Note: **with optimizations, some variables are not visible** (`<variable not available>`).
Keep `-O0` (the default) while debugging.

Note: **if a class name collides with a type the debugger knows, only the display breaks.**
macOS lldb ships a formatter for an old type named `Rect`; a class with that name shows
`summary string parsing error` (read it with `frame variable --raw`). Renaming the class fixes it.

Global variables are not shown yet (only locals and arguments).

Note: **if the debugger can't open the source, check how you passed it.**
The file name in the debug info is **exactly what you passed at compile time**
(`yashirolang -g app.ys` records `app.ys`; an absolute path records the absolute path).
If you passed a relative path, start the debugger **from the same directory**.
The IDE and the VS Code extension pass absolute paths, so they work from anywhere.

Note: **on macOS, `<output>.dSYM` is created as well** (`dsymutil` runs automatically).
DWARF stays inside the `.o` files on macOS, so without it the debugger shows nothing.
Deleting it deletes the debug info.

## Information

| Option | Description |
|---|---|
| `--version` | Version, stage and target triple |
| `--print-lib-dir` | Show where the standard library is (the package manager [`ysm`](../../ja/reference/pkg.md) uses it to detect name collisions) |
| `-h`, `--help` | Usage |

## Language of diagnostics

| Option | Description |
|---|---|
| `--lang=ja` | Print diagnostics in Japanese (**default**) |
| `--lang=en` | Print diagnostics in English. Anything without a translation is printed in Japanese |
| `--lang=auto` | Look at `LC_ALL` → `LC_MESSAGES` → `LANG`: Japanese if it starts with `ja`, English otherwise |

The environment variable `PLC_MSG_LANG` (`ja` / `en` / `auto`) does the same; `--lang` wins.
**The default is Japanese regardless of locale** (so the language never changes silently on CI or someone else's machine).
To always get English, put `export PLC_MSG_LANG=en` in your shell profile
([getting-started.md §2](../getting-started.md#2-switch-error-messages-to-english)).

Every compiler diagnostic (lexing, parsing, types, ownership, modules, command line) has an English translation.
Diagnostic codes (`E-MOVE-1`, etc.) are the same in both languages, so you can search by code
([design](../../ja/design/i18n-diagnostics.md) §3.3 *(Japanese)*).
The `error[E-MOVE-1]:` format is the same in both languages.

Runtime errors of compiled programs are chosen by `PLC_MSG_LANG` **when the program runs**
(`--lang` only affects the compiler).

## Tracing the compiler

| Option | Description |
|---|---|
| `--dump-tokens` | Print the token stream and exit |
| `--dump-ast` | Print the AST as S-expressions and exit |

`--dump-tokens` and `--dump-ast` look at **the entry file only** (they don't follow `import`s).

---

## Environment variables

| Variable | Purpose |
|---|---|
| `PLC_CLANG` | The clang to use (when it has a different name, such as `clang-18`) |
| `PLC_RUNTIME_O` | Location of the runtime (`runtime.a`) |
| `PLC_LIB_DIR` | Location of the standard library (`*.ys`) |
| `PLC_TARGET_TRIPLE` | Default target triple |
| `PLC_CFLAGS` | Extra arguments passed to clang with `-c` |
| `PLC_MSG_LANG` | Language of diagnostics and runtime errors (`ja` / `en` / `auto`; default `ja`; see above) |
| `PLC_MSG_DIR` | Location of the English message table (`en.tsv`) (default: `msgs/` next to the standard library) |

**All of them override the built-in defaults.** By default the compiler finds the runtime and the
standard library relative to its own executable (`<exe>/../lib/plc/`), so it works wherever it is installed.

---

## Exit codes

| Value | Meaning |
|---|---|
| 0 | Success |
| 1 | Compile error, or clang failed |

The exit code of a compiled program is the **low 8 bits** of `main`'s return value
(`return -1` becomes 255; the compiler truncates it so every OS gives the same result).
