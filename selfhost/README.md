# selfhost/ — セルフホスト製の yashirolang コンパイラ (stage1)

**C 版からの移植で完成しました。**
**yashirolang コンパイラは自分自身をコンパイルできます**（`make bootstrap`）。

`src/` の C 版と **1:1 で対応**させます。この対応を崩さないでください。
崩すと「C 版のどこを見れば正解がわかるか」が失われます。

| C 版 | セルフホスト版 | 状態 |
|---|---|---|
| `src/lexer.h` | `selfhost/token` | ✅ |
| `src/lexer.c` | `selfhost/lexer` | ✅ |
| `src/parser.c` | `selfhost/parser` | ✅ |
| `src/ast.c` | `selfhost/ast` | ✅ |
| `src/sema.c` | `selfhost/sema` | ✅ |
| `src/diag.c` | `selfhost/diag` | ✅ |
| `src/types.c` | `selfhost/ast` に同居 | ✅ |
| `src/module.c` | `selfhost/module` | ✅ |
| `src/codegen.c` | `selfhost/codegen` | ✅ |
| `src/main.c` | `selfhost/main` | ✅ |

## 検証方法

各段階で C 版が「正解」を持っていることを利用します。

```bash
# トークン列が一致するか（tests/selfhost.sh が全ファイルで自動比較）
make selfhost-test
#   → トークン列一致 348 件 / AST 一致 311 件 / 型検査 一致 174 件 /
#     IR 一致 174 件 / stage1 の IR で実行して一致 156 件
#
# 1 ファイルだけ見るなら
./build/yashirolang --dump-tokens tests/cases/x.ys > /tmp/c.txt
./build/stage1-lexer          tests/cases/x.ys > /tmp/m.txt
diff /tmp/c.txt /tmp/m.txt

# AST（S 式）が一致するか
./build/yashirolang --dump-ast tests/cases/x.ys > /tmp/c.txt
./build/stage1-ast         tests/cases/x.ys > /tmp/m.txt
diff /tmp/c.txt /tmp/m.txt

# 型検査の診断が一致するか（メッセージ全文）
./build/yashirolang --check tests/cases/x.ys 2> /tmp/c.txt
./build/stage1-check    tests/cases/x.ys 2> /tmp/m.txt
diff /tmp/c.txt /tmp/m.txt

# IR が一致するか／その IR が動くか
./build/yashirolang -S       tests/cases/x.ys > /tmp/c.ll
./build/stage1-codegen   tests/cases/x.ys > /tmp/m.ll
diff /tmp/c.ll /tmp/m.ll
clang /tmp/m.ll build/runtime.o -o /tmp/x && /tmp/x

# 不動点の検証
make bootstrap        # stage2 == stage3 なら成功
make bootstrap-test   # セルフホスト版コンパイラでテストを全部通す
```

**移植は機械的に行ってください。ここで独創性を発揮しないこと。**
アルゴリズムと評価順序を C 版と揃えることで、出力の完全一致を目指せます。

詳細は [../docs/design/self-hosting.md](../docs/design/self-hosting.md)。
