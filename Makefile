# 本言語コンパイラ (stage0) のビルド
#
# 使い方:
#   make            コンパイラをビルド
#   make test       テストを全部実行（C 版のテスト + 解放の検査 + セルフホストの検証）
#   make drop-asan  --drop で生成したプログラムを AddressSanitizer で検査）
#   make drop-leak  そのうえでリークも数える（Linux のみ）
#   make kernel     ベアメタル（RISC-V）のカーネルをビルド
#   make qemu       そのカーネルを QEMU で動かす（Ctrl-A X で終了）
#   make qemu-test  カーネルの出力を自動で検証する
#   make pm            パッケージマネージャをビルド
#   make pm-test       パッケージマネージャの受け入れテスト（git リポジトリを作って動かす）
#   make coverage      C 版コンパイラのカバレッジを測る
#   make stdlib-usage  標準ライブラリの API がテストからどれだけ呼ばれているか
#   make selfhost-test  セルフホスト版と C 版の出力を比較（5 本）
#   make bootstrap      3 段ビルドと不動点の検証
#   make bootstrap-test セルフホスト版コンパイラでテストを全部通す
#   make asan       AddressSanitizer 付きでビルド（メモリバグ調査用）
#   make install    <prefix>/bin と <prefix>/lib/plc に入れる（PREFIX=… で変更）
#   make dist       配布用のディレクトリを build/dist に作る
#   make clean      生成物を削除

# ── 動かす環境（Linux / macOS / Windows）──────────────────────
#
# ★ このコンパイラは「LLVM IR のテキストを出して、clang に渡す」作りなので、
#   clang さえあればどの OS でも同じように動きます。
#   OS ごとに違うのは、ここに集めた 4 つだけです。
UNAME_S := $(shell uname -s 2>/dev/null || echo Unknown)
IS_MAC  := $(filter Darwin,$(UNAME_S))
IS_WIN  := $(filter MINGW% MSYS% CYGWIN%,$(UNAME_S))

# Windows（MSYS2 / Git Bash）では実行ファイルに .exe が付きます
ifneq ($(IS_WIN),)
  EXEEXT := .exe
else
  EXEEXT :=
endif

# ★ コンパイラ本体は C11 が通れば何でビルドしても構いません（gcc でも可）。
#   ただし **IR を扱うのは clang** です（LLVM IR のテキストを読めるのは clang だけ）。
#   make CC=gcc のように明示したときは、その指定を尊重します。
ifeq ($(origin CC),default)
  CC := clang
endif
CLANG ?= clang
CFLAGS  := -std=c11 -g -O0 -Wall -Wextra -Wno-unused-parameter
RUNTIME_CFLAGS := -std=c11 -O2 -Wall -Wextra

# ★ POSIX ではスレッドのリンクに -pthread が要ります。
ifeq ($(IS_WIN),)
  PTHREAD_LDFLAGS := -pthread
else
  PTHREAD_LDFLAGS :=
endif

# ── 言語の名前まわり（★ 改名するときはここだけ）─────────────
# 言語名は将来変わります。名前に依存する値は**この 5 つが全部**です。
#   LANG_NAME … 人が読む言語名（--help や IR のメタデータに出る）
#   LANG_EXT  … ソースの拡張子（ドット込み）
#   LANG_CC   … コンパイラのコマンド名＝生成される実行ファイル名
#   LANG_PM   … パッケージマネージャのコマンド名
#   LANG_VERSION … 版番号（--version が出す値）
#   LANG_REPO … リポジトリの URL（README の clone 先・Releases のリンク）
#
# 対になる定義: src/langinfo.h（C 版）/ lib/langinfo$(LANG_EXT)（この言語で書かれた側）
# ★ 手で直さずに `tools/rename.sh` を使ってください（3 か所を一度に揃えます）。
#
# ⚠️ ここ以外のどこにも名前を書かないこと。文書は docs/ のひな型が
#   {{name}} {{ext}} {{cc}} {{pm}} という合い言葉で書かれていて、
#   `make docs` が流し込みます。書き漏れは `make check-naming` が見つけます。
LANG_NAME := yashirolang
LANG_EXT  := .ys
LANG_CC   := yashirolang
LANG_PM   := ysm
LANG_VERSION := 0.25.0
LANG_REPO := https://github.com/yashirolang/yashirolang
CFLAGS  += -DPLC_LANG_NAME='"$(LANG_NAME)"' \
           -DPLC_LANG_EXT='"$(LANG_EXT)"' \
           -DPLC_LANG_CC='"$(LANG_CC)"' \
           -DPLC_LANG_PM='"$(LANG_PM)"' \
           -DPLC_LANG_VERSION='"$(LANG_VERSION)"'

# ── ターゲット triple の自動取得 ──────────────────────────────
# 生成する LLVM IR に書き込む triple。
#
# ⚠️ `clang -print-target-triple` を使ってはいけません。
#    macOS ではそれが返す値（x86_64-apple-darwin25.5.0）と、clang が実際に
#    IR に書く値（x86_64-apple-macosx26.0.0）が異なり、警告の原因になります。
#    「clang 自身に空の C ファイルの IR を吐かせて、そこから抜き出す」のが確実です。
#    ⚠️ Windows には /dev/null が無いことがあるので、空ファイルを作って渡します。
HOST_TRIPLE := $(shell printf '' > .plc-empty.c 2>/dev/null; \
                 $(CLANG) -S -emit-llvm -x c .plc-empty.c -o - 2>/dev/null \
                 | sed -n 's/^target triple = "\(.*\)"$$/\1/p'; \
                 rm -f .plc-empty.c)
CFLAGS  += -DPLC_TARGET_TRIPLE='"$(HOST_TRIPLE)"'

# ── macOS のユニバーサルバイナリ（make UNIVERSAL=1）─────────────
#
# ★ 配布物を Intel Mac と Apple Silicon の**両方で動かす**ための指定です。
#   1 つの実行ファイルに 2 つの機械語を入れます（Mach-O の fat 形式）。
#
# ⚠️ **triple も 2 つ要ります。** 生成する IR に書く triple は
#    「いま動いている側」でなければならないのに、既定では
#    ビルド時に 1 つだけ埋め込まれます。x86_64 の Mac で arm64 の
#    triple を書いた IR を出すと、動かない実行ファイルができます。
#    そこで両方を渡し、C 側（src/codegen.c）で選ばせます。
#
#   普段のビルドには一切影響しません（UNIVERSAL を指定したときだけ）。
ifeq ($(UNIVERSAL),1)
  ARCHFLAGS := -arch x86_64 -arch arm64
  CFLAGS  += $(ARCHFLAGS)
  RUNTIME_CFLAGS += $(ARCHFLAGS)
  triple_for = $(shell printf '' > .plc-empty.c 2>/dev/null; \
                 $(CLANG) -arch $(1) -S -emit-llvm -x c .plc-empty.c -o - 2>/dev/null \
                 | sed -n 's/^target triple = "\(.*\)"$$/\1/p'; \
                 rm -f .plc-empty.c)
  CFLAGS  += -DPLC_TARGET_TRIPLE_X86_64='"$(call triple_for,x86_64)"' \
             -DPLC_TARGET_TRIPLE_ARM64='"$(call triple_for,arm64)"'
endif

# ── LLVM のツール（opt / lli / llvm-as / ld.lld）──────────────
#
# ★ 探す順番：① Homebrew（macOS）→ ② llvm-config → ③ PATH。
#   Linux では distro の LLVM がそのまま PATH にあります。
LLVM_BIN := $(shell brew --prefix llvm 2>/dev/null)/bin
ifeq ($(wildcard $(LLVM_BIN)/opt),)
  LLVM_BIN := $(shell llvm-config --bindir 2>/dev/null)
endif
ifeq ($(wildcard $(LLVM_BIN)/opt),)
  LLVM_BIN := $(patsubst %/,%,$(dir $(shell which opt 2>/dev/null)))
endif
OPT      := $(LLVM_BIN)/opt
LLI      := $(LLVM_BIN)/lli
LLVM_AS  := $(LLVM_BIN)/llvm-as

# ── ランタイム（core / hosted の 2 つに分割）───────────────
# ユーザーのプログラムにリンクされる C のコード。
#
#   core.c   … libc に依存しない核（ベアメタルでもリンクできる）
#   hosted.c … PC 上で動かすときのフック実装 + ファイル入出力など
#
# ⚠️ コンパイラ本体（-O0 -g）とは目的が違うので -O2 でビルドします。
#    ランタイムは「ユーザーのプログラムの一部」として動くからです。
RUNTIME_CORE := runtime/core.c
RUNTIME_HOSTED := runtime/hosted.c

# ★ 静的ライブラリ（.a）にまとめます。
#   ⚠️ 以前は `ld -r`（部分リンク）でしたが、Windows では使えません。
#     `ar` はどの環境にもあり、clang のリンク行にそのまま渡せます。
RUNTIME_OBJ := build/runtime.a
AR ?= ar

# ★ 生成したプログラムをリンクするのに使う clang（実行時に呼ぶ相手）。
#   ⚠️ clang-18 のように名前が違う環境があるので、埋め込みつつ
#     環境変数 PLC_CLANG で上書きできるようにします。
CFLAGS  += -DPLC_CLANG='"$(CLANG)"'

# コンパイラにランタイムの場所を教える。
# ⚠️ stage0 だけの割り切り（ビルドツリー内で完結すればよい）。
CFLAGS  += -DPLC_RUNTIME_O='"$(abspath $(RUNTIME_OBJ))"'

# ── 標準ライブラリ ───────────────────────────────────────────
# import が探す 2 つ目の場所。本言語で書かれた lib/*$(LANG_EXT) があります。
CFLAGS  += -DPLC_LIB_DIR='"$(abspath lib)"'

SRCS    := $(wildcard src/*.c)
OBJS    := $(SRCS:src/%.c=build/%.o)
DEPS    := $(OBJS:.o=.d)
TARGET  := build/$(LANG_CC)$(EXEEXT)


# ── 小さなメソッドが呼び出し元に畳まれるか ──────────────────
#
# ★ **時間ではなく構造で見ます。** clang に -Rpass-missed=inline で
#   直接聞くので、機械が変わっても答えは変わりません。
#
#   ⚠️ 外れの経路（範囲外・桁あふれ・None）の**呼び出し 1 つにつき約 25 点**
#     かかります（規約 R13）。cost が threshold 以上になると畳まれません。
#
#   make inline-report … 表示するだけ（人が見る用）
#   make inline-check  … 畳まれていなければ **落ちます**（CI 用）
#
# ⚠️ clang の指摘の文言が変わったら、この検査は「指摘なし＝合格」に倒れます。
#   見落とす側に倒れるので、番人としては安全側です。
inline-report: $(TARGET) $(RUNTIME_OBJ)
	@echo "── 使う clang"
	@$(CLANG) --version | head -2
	@echo ""
	@echo "── linalg.Matrix.get / set は linalg.matmul に畳まれるか"
	@rm -rf build/inline && mkdir -p build/inline
	@./$(TARGET) -O2 --keep-ll tests/cases/linalg_matmul_perf$(LANG_EXT) -o build/inline/x >/dev/null 2>&1 || true
	@out=$$($(CLANG) -O2 -Rpass-missed=inline build/inline/x.*.ll $(RUNTIME_OBJ) -o /dev/null 2>&1 | grep -E "linalg.Matrix.(get|set). not inlined into .linalg.matmul." | sort -u); \
	if [ -n "$$out" ]; then \
	  echo "  ⚠️ 畳まれていません（内側ループに呼び出しが残ります）"; \
	  echo "$$out" | sed "s/^/  /"; \
	else \
	  echo "  ★ 畳まれています（not inlined の指摘なし）"; \
	fi

inline-check: $(TARGET) $(RUNTIME_OBJ)
	@rm -rf build/inline && mkdir -p build/inline
	@./$(TARGET) -O2 --keep-ll tests/cases/linalg_matmul_perf$(LANG_EXT) -o build/inline/x >/dev/null 2>&1 || true
	@out=$$($(CLANG) -O2 -Rpass-missed=inline build/inline/x.*.ll $(RUNTIME_OBJ) -o /dev/null 2>&1 | grep -E "linalg.Matrix.(get|set). not inlined into .linalg.matmul." | sort -u); \
	if [ -n "$$out" ]; then \
	  echo "⚠️ linalg.Matrix.get / set が畳まれていません（内側ループに呼び出しが残ります）"; \
	  echo "$$out" | sed "s/^/  /"; \
	  echo "  → 外れの経路の呼び出しを増やしていないか確かめてください（規約 R13）"; \
	  exit 1; \
	fi; \
	echo "★ linalg.Matrix.get / set は畳まれています（$$($(CLANG) --version | head -1)）"

# ── コンパイラ自身の所有権検査の指摘を全部見る ──────────────
#
# ★ ロードマップの「いちばん大きな宿題」（コンパイラ自身が検査を通らない）を
#   数えるための道具です。ふつうのビルドは先頭 20 件で打ち切るので、
#   **上限を外した専用のコンパイラ**を建てて数えます。
#
#   make own-report        … 種別ごと・ファイルごとの件数
#   make own-report LIST=1 … 指摘そのものを全部出す
#
# ⚠️ **--warn-own が要ります**（A-24）。所有権の指摘は既定でエラーになり、
#   1 件目で止まるようになったので、数えるには警告へ落とし直します。
own-report: $(RUNTIME_OBJ)
	@mkdir -p build
	@$(CC) $(CFLAGS) -w -DOWNCK_MAX_REPORT=100000 $(SRCS) -o build/$(LANG_CC)-ownall
	@if [ -n "$(LIST)" ]; then \
	  ./build/$(LANG_CC)-ownall --warn-own selfhost/main$(LANG_EXT) -o /dev/null 2>&1 | grep -v "^$$"; \
	else \
	  echo "── 種別ごと"; \
	  ./build/$(LANG_CC)-ownall --warn-own selfhost/main$(LANG_EXT) -o /dev/null 2>&1 \
	    | grep -oE "^warning\[E-[A-Z]+-[0-9]\]" | sort | uniq -c | sort -rn; \
	  echo ""; \
	  echo "── ファイルごと（指摘 1 件が数行に出るので目安です）"; \
	  ./build/$(LANG_CC)-ownall --warn-own selfhost/main$(LANG_EXT) -o /dev/null 2>&1 \
	    | grep -oE "selfhost/[a-z_]+\$(LANG_EXT)" | sort | uniq -c | sort -rn; \
	fi

.PHONY: all clean test test-one selfhost-test bootstrap bootstrap-test asan drop-asan drop-leak info inline-report inline-check own-report own-strict pm pm-test coverage coverage-detail stdlib-usage

all: $(TARGET) $(RUNTIME_OBJ)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@

# ★ 2 つを 1 つの静的ライブラリにまとめます。
#   こうしておくと、コンパイラ側は「ランタイムは 1 本のファイル」という
#   これまでの前提のままで済みます（.o でも .a でも clang に渡せます）。
$(RUNTIME_OBJ): $(RUNTIME_CORE) $(RUNTIME_HOSTED) runtime/core.h
	@mkdir -p build
	$(CC) $(RUNTIME_CFLAGS) -c $(RUNTIME_CORE) -o build/core.o
	$(CC) $(RUNTIME_CFLAGS) -c $(RUNTIME_HOSTED) -o build/hosted.o
	$(AR) rcs $@ build/core.o build/hosted.o

# -MMD -MP でヘッダの依存関係を自動生成する。
# これがないと、ヘッダを直したのに再ビルドされず不思議なバグに悩まされます。
build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

# ── パッケージマネージャ ────────────────────────────────────
#
# ★ パッケージマネージャ自身も **この言語で書かれています**（tools/pm/）。だから作るには
#   コンパイラが要ります。所有権の検査 3 つを全部エラーにして建てます
#   （配って使うものなので、警告のまま出さないため）。
# ⚠️ ディレクトリ名（tools/pm）とターゲット名（make pm）は**わざと名前非依存**です。
#   実行ファイル名だけが $(LANG_PM) で決まります。
PM      := build/$(LANG_PM)$(EXEEXT)
PM_SRCS := $(wildcard tools/pm/*$(LANG_EXT))

pm: $(PM)

$(PM): $(TARGET) $(RUNTIME_OBJ) $(PM_SRCS)
	@mkdir -p build
	@PLC_LIB_DIR=$(abspath lib) PLC_RUNTIME_O=$(abspath $(RUNTIME_OBJ)) \
	 ./$(TARGET) -O2 --deny-move --deny-borrow --deny-mut \
	   tools/pm/main$(LANG_EXT) -o $@
	@echo "できました: $(PM)"

# 受け入れテスト（本物の git リポジトリを作って一通り動かす）
pm-test: $(PM)
	@tests/pm.sh

# ── カバレッジ ──────────────────────────────────────────────
#
# ★ 測るのは **C 版コンパイラ（src/*.c）**です。本体と**同じ CFLAGS** に
#   計装の 2 つを足して建て、テストを全部通してから llvm-cov に集計させます。
#   （同じ定義で建てないと「別のコード」を測ることになります）
# ⚠️ 本言語で書かれた側（selfhost/ lib/）はこの方法では測れません。
#   言語に計装の仕組みが無いためです。tests/stdlib_usage.sh が別に数えます。
COV_DIR := build/cov
COV_CC  := $(COV_DIR)/$(LANG_CC)$(EXEEXT)
COV_FLAGS := -fprofile-instr-generate -fcoverage-mapping

$(COV_CC): $(SRCS) $(RUNTIME_OBJ)
	@mkdir -p $(COV_DIR)
	$(CC) $(CFLAGS) $(COV_FLAGS) $(SRCS) -o $@

coverage: $(COV_CC) $(RUNTIME_OBJ)
	@tests/coverage.sh

coverage-detail: $(COV_CC) $(RUNTIME_OBJ)
	@tests/coverage.sh --detail

# 標準ライブラリの API がテストからどれだけ呼ばれているか
stdlib-usage:
	@tests/stdlib_usage.sh

# ── テスト ──────────────────────────────────────────────────
test: $(TARGET) $(RUNTIME_OBJ) $(PM)
	@tests/run_tests.sh
	@tests/drop_asan.sh
	@tests/selfhost.sh
	@tests/pm.sh

# 1 ケースだけ実行: make test-one CASE=tests/cases/int_42$(LANG_EXT)
test-one: $(TARGET) $(RUNTIME_OBJ)
	@tests/run_tests.sh $(CASE)

# ── セルフホストの検証 ──────────────────────────────────────
# セルフホスト版のコンパイラ（stage1）が C 版と同じものを出すか。
#   トークン列 → AST → 診断 → IR → 実行結果 の 5 本を比べます。
#   ★ テストケースをそのまま検証データに使います。
selfhost-test: $(TARGET) $(RUNTIME_OBJ)
	@tests/selfhost.sh

# ── ブートストラップ ────────────────────────────────────────
# stage1（C 版がビルド）→ stage2（stage1 がビルド）→ stage3（stage2 がビルド）
# stage2 == stage3 なら不動点に到達＝セルフホスト完成。
bootstrap: $(TARGET) $(RUNTIME_OBJ)
	@tests/bootstrap.sh

# セルフホスト版コンパイラ（stage2）でテストを全部通す。
#   ★ 「C 版と同じ出力を出す」より強い確認です。
bootstrap-test: bootstrap
	@PLC_CC=$(abspath build/boot/stage2) \
	 PLC_LIB_DIR=$(abspath lib) \
	 PLC_RUNTIME_O=$(abspath $(RUNTIME_OBJ)) \
	 PLC_TARGET_TRIPLE=$(HOST_TRIPLE) \
	 tests/run_tests.sh

# ── 解放（drop）の検査 ──────────────────────────────────────
# tests/cases/drop_*$(LANG_EXT) を --drop 付きで生成し、AddressSanitizer 付きで
# リンクして走らせます。**二重解放と解放後の使用**を実行時に捕まえる網です。
drop-asan: $(TARGET)
	@tests/drop_asan.sh

# ★ リークの残りを数える。Linux でだけ動きます
#   （macOS の AddressSanitizer に LeakSanitizer は入っていません）。
drop-leak: $(TARGET)
	@tests/drop_asan.sh --leaks

# ── AddressSanitizer ビルド ─────────────────────────────────
# セグフォの原因が分からないときに使います。
#   make asan && ./build/$(LANG_CC)-asan tests/cases/int_42$(LANG_EXT)
asan: $(RUNTIME_OBJ)
	@mkdir -p build
	$(CC) $(CFLAGS) -fsanitize=address,undefined $(SRCS) -o build/$(LANG_CC)-asan

# ── ベアメタル ──────────────────────────────────────────────
#
# ★ ターゲットは RISC-V（riscv64-unknown-elf）です。
#   決め手は「手元にある道具」でした：
#     - Apple の clang には RISC-V のバックエンドが無い → Homebrew の LLVM を使う
#     - x86 の ELF リンカは無いが、riscv64-elf-ld はある
#     - qemu-system-riscv64 の virt マシンは -bios none で ELF を直接起動できる
#
# 使うもの: brew install llvm riscv64-elf-binutils qemu
LLVM_CLANG := $(LLVM_BIN)/clang
ifeq ($(wildcard $(LLVM_CLANG)),)
  LLVM_CLANG := $(CLANG)
endif

# ★ リンカは環境にあるものを使います。
#   ld.lld（LLVM に付属。Linux で入れやすい）→ riscv64-elf-ld（Homebrew の cross binutils）
RV_LD := $(shell command -v ld.lld 2>/dev/null || command -v riscv64-elf-ld 2>/dev/null \
           || echo riscv64-elf-ld)
RV_TRIPLE  := riscv64-unknown-elf
RV_ARCH    := -march=rv64g -mabi=lp64 -mcmodel=medany -mno-relax
RV_CFLAGS  := --target=$(RV_TRIPLE) $(RV_ARCH) -ffreestanding -O2
KDIR       := build/kernel

# ── 証明の確かめ（A-34）──────────────────────────────────
#
# ★ 「消せる」と判断した検査を**残したまま**全ケースを走らせます。
#   外れたら `prover was wrong (this is a compiler bug)` で止まるので、
#   **解析の誤りが利用者ではなく私たちに返ってきます**。
#
# ⚠️ IR の形そのものを見るケース（# EXACT-IR:）は飛ばします。
#   「検査が消えたこと」を見る試験は、この設定と必ずぶつかるためです。
.PHONY: prove-verify prove-report
prove-verify: $(TARGET) $(RUNTIME_OBJ)
	@PLC_EXTRA_FLAGS=--verify-prove tests/run_tests.sh

# 消えた検査の数（コンパイラ自身を材料にします）
prove-report: $(TARGET) $(RUNTIME_OBJ)
	@./$(TARGET) --prove-report -S selfhost/main$(LANG_EXT) > /dev/null

# ── BLAS 連携の確認（A-33）────────────────────────────────
#
# ★ `make test` には入れません。BLAS はどの環境にもあるとは限らないためです
#   （無い環境で「落ちた」と言われるより、走らせないほうが正直です）。
#
#   macOS      : Accelerate（OS に入っています）
#   Linux ほか : -lopenblas か -lblas
.PHONY: blas-test
blas-test: $(TARGET) $(RUNTIME_OBJ)
	@mkdir -p build
	@if [ "$$(uname -s)" = "Darwin" ]; then \
	    LINK="-framework Accelerate"; \
	else \
	    LINK="-lopenblas"; \
	fi; \
	if ! ./$(TARGET) -O2 examples/blas_matmul$(LANG_EXT) $$LINK \
	        -o build/blas_matmul 2> build/blas.err; then \
	    echo "（BLAS が見つからないので飛ばします）"; \
	    head -3 build/blas.err | sed 's/^/    /'; \
	    exit 0; \
	fi; \
	./build/blas_matmul

.PHONY: kernel qemu qemu-test

kernel: $(TARGET) $(KDIR)/kernel.elf

$(KDIR)/kernel.elf: kernel/kernel$(LANG_EXT) kernel/boot.s kernel/hooks.c kernel/link.ld                     runtime/core.c runtime/core.h $(TARGET)
	@mkdir -p $(KDIR)
	@echo "── 本言語本体（$(LANG_EXT) → RISC-V の .o）"
	PLC_CLANG=$(LLVM_CLANG) PLC_CFLAGS="$(RV_ARCH)" 	  ./$(TARGET) -c kernel/kernel$(LANG_EXT) -o $(KDIR)/kernel_main.o
	@echo "── ランタイムの核（libc なし）"
	$(LLVM_CLANG) $(RV_CFLAGS) -DPL_FREESTANDING -c runtime/core.c -o $(KDIR)/core.o
	@echo "── カーネル側のフック（UART と bump allocator）"
	$(LLVM_CLANG) $(RV_CFLAGS) -c kernel/hooks.c -o $(KDIR)/hooks.o
	@echo "── 起動アセンブリ"
	$(LLVM_CLANG) --target=$(RV_TRIPLE) $(RV_ARCH) -c kernel/boot.s -o $(KDIR)/boot.o
	@echo "── リンク"
	$(RV_LD) -T kernel/link.ld $(KDIR)/boot.o $(KDIR)/kernel_main.o 	         $(KDIR)/core.o $(KDIR)/hooks.o -o $@ 2>&1 | grep -v "RWX permissions" || true

# QEMU で動かす（Ctrl-A X で抜ける）
qemu: kernel
	qemu-system-riscv64 -machine virt -bios none -kernel $(KDIR)/kernel.elf -nographic

# ★ ベアメタルの自動検証：シリアルに出た文字列で判定します。
#   道具（qemu / riscv64-elf-ld）が無い環境ではスキップして緑にします。
qemu-test: kernel
	@tests/qemu.sh

# ── インストールと配布 ──────────────────────────────────────
#
# ★ 配る形（どの OS でも同じ）:
#     <prefix>/bin/$(LANG_CC)
#     <prefix>/lib/plc/runtime.a
#     <prefix>/lib/plc/lib/*$(LANG_EXT)
#
#   コンパイラは「実行ファイルからの相対」でこの 2 つを探すので、
#   展開した場所がどこでも動きます（src/main.c の runtime_o を参照）。
PREFIX ?= /usr/local
DESTDIR ?=

.PHONY: install uninstall dist

# ★ LICENSE も一緒に置きます（配る実体に付いて回るべきものなので）。
install: all
	@mkdir -p "$(DESTDIR)$(PREFIX)/bin" "$(DESTDIR)$(PREFIX)/lib/plc/lib"
	cp $(TARGET) "$(DESTDIR)$(PREFIX)/bin/"
	cp $(RUNTIME_OBJ) "$(DESTDIR)$(PREFIX)/lib/plc/"
	cp lib/*$(LANG_EXT) "$(DESTDIR)$(PREFIX)/lib/plc/lib/"
	cp LICENSE "$(DESTDIR)$(PREFIX)/lib/plc/"
	@$(MAKE) --no-print-directory $(PM)
	cp $(PM) "$(DESTDIR)$(PREFIX)/bin/"
	@echo "インストールしました: $(DESTDIR)$(PREFIX)/bin/$(LANG_CC)$(EXEEXT)"
	@echo "                      $(DESTDIR)$(PREFIX)/bin/$(LANG_PM)$(EXEEXT)"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/$(LANG_CC)$(EXEEXT)" "$(DESTDIR)$(PREFIX)/bin/$(LANG_PM)$(EXEEXT)"
	rm -rf "$(DESTDIR)$(PREFIX)/lib/plc"

# 配布用のディレクトリ（そのまま zip / tar.gz にできる形）
DIST_NAME ?= $(LANG_NAME)-$(UNAME_S)-$(shell uname -m 2>/dev/null || echo unknown)
dist: all $(PM)
	rm -rf build/dist/$(DIST_NAME)
	@mkdir -p build/dist/$(DIST_NAME)/bin build/dist/$(DIST_NAME)/lib/plc/lib
	cp $(TARGET) $(PM) build/dist/$(DIST_NAME)/bin/
	cp $(RUNTIME_OBJ) build/dist/$(DIST_NAME)/lib/plc/
	cp lib/*$(LANG_EXT) build/dist/$(DIST_NAME)/lib/plc/lib/
	cp README.md LICENSE build/dist/$(DIST_NAME)/ 2>/dev/null || 	  cp README.md build/dist/$(DIST_NAME)/
	@echo "配布物: build/dist/$(DIST_NAME)"

# ── 情報表示 ────────────────────────────────────────────────
#
# ★ `make print-LANG_NAME` のように、変数を 1 つだけ値で取り出せます。
#   CI やスクリプトが**言語名を書き写さずに**済むようにするためです。
#   例: d="$(make -s print-LANG_NAME)-linux-x86_64"
print-%:
	@echo "$($*)"

info:
	@echo "CC           = $(CC)"
	@echo "HOST_TRIPLE  = $(HOST_TRIPLE)"
	@echo "LLVM_BIN     = $(LLVM_BIN)"
	@echo "SRCS         = $(SRCS)"
	@echo "LANG_NAME    = $(LANG_NAME)"
	@echo "LANG_EXT     = $(LANG_EXT)"
	@echo "LANG_CC      = $(LANG_CC)"
	@echo "LANG_PM      = $(LANG_PM)"
	@echo "LANG_VERSION = $(LANG_VERSION)"
	@echo "LANG_REPO    = $(LANG_REPO)"
	@echo "UNAME_S      = $(UNAME_S)"
	@echo "CLANG        = $(CLANG)"
	@echo "RUNTIME      = $(RUNTIME_OBJ)"
	@echo "UNIVERSAL    = $(UNIVERSAL) $(ARCHFLAGS)"

clean:
	rm -rf build a.out a.out.ll tests/tmp

# ── 文書に名前を流し込む ────────────────────────────────────
#
# ★ docs/ のファイルは**ひな型**です。言語名を持たせないために
#   {{name}} {{ext}} {{cc}} {{pm}} {{version}} {{repo}} と書いてあります。
#   読める形（実際の名前が入ったもの）はここで作ります。
#
#   make docs        … docs/ → build/docs/ を作り直す（git に入れない生成物）
#   make readme      … README.md.in → README.md（★ これだけはコミットする。
#                      GitHub の入口なので、ひな型のままでは読めないため）
#   make check-naming … ひな型に生の言語名が混ざっていないかを見張る（CI で回る）
.PHONY: docs readme check-naming

docs:
	@tools/render_docs.sh

readme:
	@tools/render_docs.sh README.md.in > README.md
	@echo "  README.md を README.md.in から作り直しました"

# ⚠️ README.md は生成物です。README.md.in を直してから `make readme` を
#   走らせてください。ここでは「生成し直しても変わらないか」も確かめます。
check-naming:
	@mkdir -p build
	@tools/render_docs.sh --check
	@tools/render_docs.sh README.md.in > build/README.md.expected
	@if ! diff -u README.md build/README.md.expected > /dev/null 2>&1; then \
	    echo "★ README.md が README.md.in と食い違っています。"; \
	    diff -u README.md build/README.md.expected | head -40; \
	    echo "  → make readme を走らせてコミットしてください"; \
	    exit 1; \
	fi
	@echo "  ok    README.md は README.md.in と一致しています"
	@tools/check_naming.sh

# ── コンパイラ自身を「所有権エラー」で建てる ────────────────
#
# ★ ロードマップの「いちばん大きな宿題」がここで終わりました。
#   指摘を**警告ではなくエラー**にして、自分自身を建て直します。
#   通れば「コンパイラ自身が安全性検査を通る」ことの証明です。
own-strict: $(TARGET) $(RUNTIME_OBJ)
	@mkdir -p build
	@./$(TARGET) --deny-move --deny-borrow --deny-mut selfhost/main$(LANG_EXT) \
	  -o build/$(LANG_CC)-strict
	@echo "★ 所有権検査（エラー扱い）を通りました: build/$(LANG_CC)-strict"
