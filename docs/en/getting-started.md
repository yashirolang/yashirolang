# The First 5 Minutes

From installing yashirolang to running your first program.
The language itself is covered in [tutorial.md](../ja/tutorial.md) *(Japanese)*.

---

## 1. Install

**All you need is clang.** yashirolang emits LLVM IR as text and leaves assembling and linking
to clang, so it works the same anywhere clang does.

| OS | What to install |
|---|---|
| Linux | `sudo apt install clang llvm make` (Debian / Ubuntu)<br>`sudo dnf install clang llvm make` (Fedora) |
| macOS | `xcode-select --install` (Apple clang is enough) |
| Windows | In the MINGW64 shell of [MSYS2](https://www.msys2.org/):<br>`pacman -S mingw-w64-x86_64-clang mingw-w64-x86_64-lld make diffutils grep coreutils` |

> Note: on Windows, use MSYS2 (or WSL). The build and the tests rely on bash and make.
> On WSL, the Linux steps work as is.

Then pick one of the following.

**A. Unpack a release (no build needed)**

Download the `.tar.gz` for your OS from [Releases](https://github.com/yashirolang/yashirolang/releases).

```bash
tar xzf yashirolang-linux-x86_64.tar.gz
cd yashirolang-*
./bin/yashirolang --version
```

**It works wherever you unpack it** (the compiler finds the standard library relative to its own executable).

**B. Build from source**

```bash
git clone https://github.com/yashirolang/yashirolang.git
cd yashirolang
make                      # → build/yashirolang
make test                 # check that everything passes (optional)
```

**To put it on your PATH:**

```bash
sudo make install                    # /usr/local by default
make install PREFIX=$HOME/.local     # for your user only
```

This installs two commands: the compiler `yashirolang` and the package manager `ysm`.

---

## 2. Switch error messages to English

Error messages are **Japanese by default**. The English messages (`msgs/en.tsv`) are included in
both the release archives and `make install`, so there is nothing else to install —
**just set one environment variable**:

```bash
echo 'export PLC_MSG_LANG=en' >> ~/.bashrc     # use ~/.zshrc for zsh
source ~/.bashrc
```

```
$ yashirolang --check bad.ys
error: mismatched types
  --> bad.ys:2:14
   |
 2 |     x: int = "a"
   |              ^^^ expression of type 'str'
   |
note: variable 'x' is declared as type 'int'
  --> bad.ys:2:5
   |
 2 |     x: int = "a"
   |     ^
   |
   = help: there are no implicit type conversions (language spec 3.5)
```

| Setting | Effect |
|---|---|
| `PLC_MSG_LANG=en` | Always English |
| `PLC_MSG_LANG=auto` | Japanese if your locale (`LC_ALL` → `LC_MESSAGES` → `LANG`) starts with `ja`, English otherwise |
| `yashirolang --lang=en …` | English for this run only (overrides the variable) |

- **Runtime errors of the programs you build** (`runtime error: index out of range: 5 (3)`) follow
  `PLC_MSG_LANG` **when the program runs**, not when it was compiled.
- `ysm`, the VS Code extension and the IDE inherit the same variable, so one setting covers everything.
- Diagnostic codes such as `E-MOVE-1` are identical in both languages — search by code to find
  material written in either language.

Why isn't the locale used by default? So that the same command prints the same text on every
machine — a CI box with `LANG=C.UTF-8` would otherwise silently switch languages.

---

## 3. Run something

```python
# hello.ys
def main() -> int:
    print("hello, world")
    return 0
```

```bash
$ yashirolang hello.ys -o hello
$ ./hello
hello, world
```

- **`main` is required.** Unlike Python, you can't put statements at the top level.
- The **low 8 bits** of `main`'s return value become the process exit code.
- If you omit the output name, you get `a.out`.

These are the options you'll use most (the rest are in [reference/cli.md](reference/cli.md)).

| | |
|---|---|
| `-o <file>` | Name of the executable to write |
| `--check` | Only run the type and safety checks (no executable) |
| `-O2` | Build with optimizations |
| `-g` | Make it debuggable with lldb / gdb |

---

## 4. Run something bigger

The programs in `examples/` in the repository run as is.

```bash
$ yashirolang examples/fizzbuzz.ys -o fizzbuzz && ./fizzbuzz
$ yashirolang examples/wordcount.ys -o wc && ./wc examples/sample.txt
$ yashirolang -O2 examples/parallel_matmul.ys -o matmul && ./matmul
```

| File | What it shows |
|---|---|
| `examples/fizzbuzz.ys` | The smallest example |
| `examples/wordcount.ys` | File I/O, `dict`, string handling |
| `examples/sales_report.ys` | Read a CSV, aggregate it, draw an SVG chart |
| `examples/parallel_matmul.ys` | Parallelism with `spawn` / `join` |
| `examples/webserver.ys` / `webclient.ys` | HTTP server and client |

---

## 5. Make it a project (`ysm`)

Once you have more than a few files, use the package manager. **There is no registry** —
dependencies point straight at git repositories.

```bash
$ ysm init myapp                # creates package.pkg (entry point: main.ys)
$ printf 'def main() -> int:\n    print("hi")\n    return 0\n' > main.ys
$ ysm build                     # → ./myapp
$ ./myapp
hi
```

Adding someone else's library:

```bash
$ ysm add toml https://github.com/user/toml-pkg 1.2.0   # omit the version to get the latest tag
$ ysm build
```

```python
import toml.parser        # comes from deps/toml/parser.ys
import json               # standard library; the names don't collide
```

Commit `package.lock` (written by `ysm`) to git. It pins contents by commit and tree SHA,
so a re-tagged release can't change what you get.

→ More in [reference/pkg.md](../ja/reference/pkg.md) *(Japanese)*

---

## 6. If you get stuck

| Symptom | Where to look |
|---|---|
| clang is not found | Install clang, or name the one to use, e.g. `PLC_CLANG=clang-18` |
| The standard library is not found | `yashirolang --print-lib-dir` shows where it looks (override with `PLC_LIB_DIR`) |
| Messages are in Japanese | Set `PLC_MSG_LANG=en` ([§2](#2-switch-error-messages-to-english)) |
| `note: English messages not found; showing Japanese` | `en.tsv` is missing next to the standard library. Reinstall, or point `PLC_MSG_DIR` at the directory containing `en.tsv` |
| `error[E-BORROW-…]` / `error[E-MOVE-…]` | These come from the ownership checker. See [tutorial.md §7](../ja/tutorial.md#7-所有権と借用--この言語の中心) *(Japanese)* |
| You don't understand a type error | The rules are in [spec/language-spec.md](../ja/spec/language-spec.md) *(Japanese)* |
