# yashirolang Documentation

**yashirolang** is a statically typed, compiled language with Python syntax.
It has no GC: ownership and borrow checking keep memory safe.

```python
def main() -> int:
    for i in range(1, 4):
        print("hello " + str(i))
    return 0
```

```bash
yashirolang hello.ys -o hello && ./hello
```

> **Error messages in English:** the compiler prints Japanese by default.
> Set `export PLC_MSG_LANG=en` to get English diagnostics and runtime errors
> ([getting-started.md §2](getting-started.md#2-switch-error-messages-to-english)).

> **About translations:** Japanese is the source of truth. Documents marked *(Japanese)*
> have no English version yet and link to the Japanese original.

---

## Where to start

| | Document | Contents |
|---|---|---|
| 1 | **[getting-started.md](getting-started.md)** | Install, run, make a project |
| 2 | [tutorial.md](../ja/tutorial.md) *(Japanese)* | The language tour (differences from Python, ownership, concurrency, error handling) |
| 3 | [reference/](#reference) | Look things up (below) |

---

## Reference

| Document | Contents |
|---|---|
| [reference/cli.md](reference/cli.md) | Options of the `yashirolang` compiler |
| [reference/pkg.md](../ja/reference/pkg.md) *(Japanese)* | The `ysm` package manager and `package.pkg` |
| [reference/stdlib.md](../ja/reference/stdlib.md) *(Japanese)* | Standard library (strings, I/O, `dict` / `set` / `json`, numerics, plotting, tables …) |
| [reference/numerics.md](../ja/reference/numerics.md) *(Japanese)* | Numerics guide (mapping from numpy / scipy / matplotlib / pandas) |
| [reference/net.md](../ja/reference/net.md) *(Japanese)* | Sockets, TLS, HTTP (server and client) |

## Specification *(Japanese)*

The **single source of truth** for how the language behaves. If the implementation disagrees, one of them is wrong.

| Document | Contents |
|---|---|
| [spec/language-spec.md](../ja/spec/language-spec.md) | Lexical structure, types, expressions, statements, program structure, built-ins |
| [spec/safety-spec.md](../ja/spec/safety-spec.md) | Ownership, borrowing, mutability, drops, error handling, `unsafe` |
| [spec/type-system.md](../ja/spec/type-system.md) | The list of types and typing rules |
| [spec/grammar.md](../ja/spec/grammar.md) | Grammar (EBNF) |

## Compiler design *(Japanese)*

**You don't need these to use the language.** They are for people working on the compiler.
See the [Japanese index](../ja/README.md#処理系の設計) for the full list
(architecture, IR conventions, memory model, ownership checker, generics, concurrency, package manager, TLS, self-hosting, i18n of diagnostics …).

## History and plans *(Japanese)*

| Document | Contents |
|---|---|
| [roadmap.md](../ja/roadmap.md) | Where things stand, what's next, what was decided against |
| [changelog.md](../ja/changelog.md) | Changes per version |
