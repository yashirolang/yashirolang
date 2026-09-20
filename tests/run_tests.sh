#!/bin/bash
# 本言語テストランナー
#
# テストケースは tests/cases/*$EXT です。期待値はファイル先頭のコメントに書きます。
#
#   # EXIT: 42        → コンパイル・実行して終了コードが 42 であること
#   # OUTPUT: hello   → 標準出力が "hello" であること（複数行は行ごとに書く）
#   # STDIN: abc      → 標準入力に "abc\n" を与える（複数行は行ごとに書く）
#   # ERROR: メッセージ → コンパイルが失敗し、stderr にその文字列を含むこと
#                       （複数行書くと、そのすべてを含むことを要求する）
#   # IR: <文字列>     → -S が出す LLVM IR にその文字列を含むこと
#   # IR-NOT: <文字列> → -S が出す LLVM IR にその文字列を**含まない**こと
#                       ★ 速さを時間で測ると CI の負荷でぶれます。
#                         「呼び出しが出ていないこと」のような**構造**は、
#                         こちらで固定してください。
#   # TOKENS: INT PUNCT INT NEWLINE EOF
#                     → --dump-tokens のトークン種別の並びが一致すること
#                       （複数行書くと空白で連結して比較する）
#   # WARN: メッセージ → コンパイルは成功し、stderr にその文字列を含むこと
#                       既定が警告である検査を確かめるため
#   # FLAGS: --deny-move
#                     → コンパイラに渡す追加のオプション
#                       （環境変数 PLC_EXTRA_FLAGS で全ケースに足せる）
#                       @ROOT@ はリポジトリの場所に置き換わる（-I のテスト用）
#   # EXPLAIN-MUT: 12:5: 'c' が変更されます
#                     → --explain-mut の出力にその文字列を含むこと
#   # EXACT-IR: 理由
#                     → **IR の形そのもの**を見るケース。PLC_EXTRA_FLAGS が
#                       付いているときは飛ばす（検査が消えたことを見る試験は
#                       --verify-prove と必ずぶつかるため）。
#   # STAGE0-ONLY: 理由
#                     → C 版（build/<LANG_CC>）でだけ実行する。
#                       ★ 所有権検査は C 版にしかありません。
#                         セルフホスト版へ移植するまでは、
#                         make bootstrap-test でこのケースを飛ばします。
#
# 複数ファイル（import）のテストは 1 ケース 1 ディレクトリです:
#
#   tests/mods/<ケース名>/main$EXT   ← 入口。期待値のコメントもここに書く
#   tests/mods/<ケース名>/lexer$EXT  ← import されるモジュール
#
# ★ 1 ディレクトリにまとめるのは、import の探索場所が
#   「入口ファイルのあるディレクトリ」だからです。tests/cases に置くと、
#   モジュール側のファイルまで単体のテストケースとして拾われてしまいます。
#
# 使い方:
#   tests/run_tests.sh                     全ケース
#   tests/run_tests.sh tests/cases/x$EXT    1 ケースだけ
#   tests/run_tests.sh tests/mods/y/main$EXT

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# ★ 実行ファイル名は Makefile の LANG_CC / LANG_PM が決めます。
#   ここに名前を書き写すと改名のたびに直すことになるので、make に訊きます。
LANG_CC="$(make -s -C "$ROOT" print-LANG_CC)"
EXT="$(make -s -C "$ROOT" print-LANG_EXT)"
LANG_PM="$(make -s -C "$ROOT" print-LANG_PM)"

# ★ 使うコンパイラを差し替えられるようにする。
#   PLC_CC=build/stage2 tests/run_tests.sh とすれば、
#   **セルフホスト版コンパイラでテスト全部を通す**ことができます。
PLC_CC="${PLC_CC:-$ROOT/build/$LANG_CC}"
# ★ Windows（MSYS2）では .exe が付きます
[ -x "$PLC_CC" ] || [ ! -x "$PLC_CC.exe" ] || PLC_CC="$PLC_CC.exe"
TMP="$ROOT/tests/tmp"

# 使っているのが C 版（stage0）かどうか。STAGE0-ONLY のケースを回すかを決めます。
if [ "${PLC_STAGE0:-}" = "1" ] || [ "$PLC_CC" = "$ROOT/build/$LANG_CC" ] \
   || [ "$PLC_CC" = "$ROOT/build/$LANG_CC.exe" ]; then
    is_stage0=1
else
    is_stage0=0
fi

if [ ! -x "$PLC_CC" ]; then
    echo "コンパイラが見つかりません: $PLC_CC"
    echo "先に 'make' を実行してください。"
    exit 1
fi

mkdir -p "$TMP"

if [ $# -gt 0 ]; then
    CASES=("$@")
else
    # ソートして順序を安定させる（テスト結果が実行ごとに変わらないように）
    CASES=()
    while IFS= read -r line; do CASES+=("$line"); done < <(ls "$ROOT"/tests/cases/*$EXT | sort)
    # 複数モジュールのケース（tests/mods/<名前>/main$EXT）
    if [ -d "$ROOT/tests/mods" ]; then
        while IFS= read -r line; do CASES+=("$line"); done \
            < <(ls "$ROOT"/tests/mods/*/main$EXT 2>/dev/null | sort)
    fi
fi

pass=0
fail=0
skip=0
failed_names=()

# 色（端末でないときは付けない）
if [ -t 1 ]; then
    C_OK=$'\033[32m'; C_NG=$'\033[31m'; C_DIM=$'\033[2m'; C_END=$'\033[0m'
else
    C_OK=''; C_NG=''; C_DIM=''; C_END=''
fi

report_fail() {
    local name="$1" reason="$2"
    printf "  %sFAIL%s  %s\n" "$C_NG" "$C_END" "$name"
    # 理由をインデントして表示する
    printf "%s" "$reason" | sed 's/^/          /'
    echo
    fail=$((fail + 1))
    failed_names+=("$name")
}

for case_file in "${CASES[@]}"; do
    # 複数モジュールのケースは「ディレクトリ名」で呼ぶ（main$EXT ばかりになるため）
    case "$case_file" in
        *tests/mods/*) name="$(basename "$(dirname "$case_file")")/main$EXT" ;;
        *)              name="$(basename "$case_file")" ;;
    esac
    base="$(echo "${name%$EXT}" | tr '/' '_')"
    exe="$TMP/$base"

    # ── 期待値をヘッダコメントから読み取る ──
    #
    # ★ **awk 1 回で全部読みます**（tests/markers.awk）。
    #   以前はここで sed を 11 回・tr を 4 回・grep を 1 回起動していました。
    #   ケースは 600 件近くあるので、**1 万プロセス近く**を毎回作っていた
    #   ことになります。注意: Windows（MSYS2）は fork をエミュレートするため
    #   1 プロセスが Linux より 1 桁高く、CI の Windows ジョブが目に見えて
    #   遅くなっていました。注意: 読み取る中身は 1 バイトも変えていません。
    #
    #   ここで入る変数: want_exit / want_error / want_output / want_tokens
    #                   want_ir / want_ir_not / want_warn / want_explain
    #                   extra_flags / has_exact_ir / stage0_only
    #
    # 注意: \r は awk の中で落とします（Windows のチェックアウトで混ざることが
    #    あります。.gitattributes で変換は止めていますが、既存の作業コピー
    #    でも動くように）。
    strip_cr() { tr -d '\r'; }

    # ★ 標準入力を与えるケース。
    #   注意: 与えないケースでも **必ず空のファイルに繋ぎます**。繋がないと
    #     端末や CI の標準入力をそのまま読んでしまい、結果が環境で変わります。
    stdin_file="$TMP/$(basename "$case_file" "$EXT").stdin"
    eval "$(awk -v stdin_file="$stdin_file" -f "$ROOT/tests/markers.awk" "$case_file")"
    # ★ -I のテストのために @ROOT@ をリポジトリの場所に置き換えます。
    #   FLAGS のパスは「実行したディレクトリ」からの相対になってしまうので、
    #   どこから走らせても同じ結果になるようにするためです。
    extra_flags="${extra_flags//@ROOT@/$ROOT}"
    # ★ 全ケースに同じオプションを足して回すための口。
    #   「--drop を既定にしたら何件壊れるか」のような棚卸しに使います。
    #     PLC_EXTRA_FLAGS=--drop tests/run_tests.sh
    #
    #   注意: **ケースの FLAGS より前**に置きます。打ち消し合うオプション
    #     （--drop と --no-drop）は後に書いたほうが勝つので、この順なら
    #     ケース側の指定が勝ちます。診断を見せるためのテストが
    #     「--no-drop」と書いて自衛できるのは、この順のおかげです。
    # ★ `# EXACT-IR: 理由` … **IR の形そのもの**を見るケース。
    #   PLC_EXTRA_FLAGS が付いているときは飛ばします。
    #   注意: 「検査が消えたこと」を見る試験は、--verify-prove（検査を残す）と
    #     必ずぶつかります。ぶつけたまま赤にすると、本当の失敗が埋もれます。
    if [ -n "${PLC_EXTRA_FLAGS:-}" ] && [ -n "$has_exact_ir" ]; then
        skip=$((skip + 1))
        continue
    fi

    extra_flags="${PLC_EXTRA_FLAGS:-} $extra_flags"

    # ★ C 版でしか動かないケースは、セルフホスト版で回すときに飛ばす
    #   注意: 計装ビルド（build/cov/<LANG_CC>）のように場所が違う C 版もあるので、
    #     PLC_STAGE0=1 で「これは C 版だ」と明示できます。
    if [ -n "$stage0_only" ] && [ "$is_stage0" -eq 0 ]; then
        printf "  %sskip%s  %s %s(%s)%s\n" "$C_DIM" "$C_END" "$name" \
               "$C_DIM" "$stage0_only" "$C_END"
        skip=$((skip + 1))
        continue
    fi

    if [ -z "$want_exit" ] && [ -z "$want_error" ] && [ -z "$want_output" ] \
       && [ -z "$want_tokens" ] && [ -z "$want_warn" ] && [ -z "$want_explain" ] \
       && [ -z "$want_ir" ] && [ -z "$want_ir_not" ]; then
        report_fail "$name" \
            "期待値のコメント（# EXIT: / # OUTPUT: / # ERROR: / # TOKENS: / # IR: / # IR-NOT: / # WARN: / # EXPLAIN-MUT:）がありません"
        continue
    fi

    # ── IR: / IR-NOT: 生成された LLVM IR を構造で見る──
    #
    # ★ 「呼び出しが出ていないこと」のような性質を**時間ではなく構造**で
    #   固定します。時間で測ると、CI の負荷でぶれて偽の失敗になります。
    if [ -n "$want_ir" ] || [ -n "$want_ir_not" ]; then
        # shellcheck disable=SC2086
        actual_ir="$("$PLC_CC" $extra_flags -S "$case_file" 2>/dev/null)"
        ir_bad=""
        while IFS= read -r want; do
            [ -z "$want" ] && continue
            printf '%s' "$actual_ir" | grep -qF -- "$want" || ir_bad="$ir_bad
  - 含まれていません: $want"
        done <<EOF_IR
$want_ir
EOF_IR
        while IFS= read -r want; do
            [ -z "$want" ] && continue
            printf '%s' "$actual_ir" | grep -qF -- "$want" && ir_bad="$ir_bad
  - 含まれています（含まないはず）: $want"
        done <<EOF_IRNOT
$want_ir_not
EOF_IRNOT
        if [ -n "$ir_bad" ]; then
            report_fail "$name" "IR が期待と違います:$ir_bad"
            continue
        fi
        # IR だけのケースはここで合格
        if [ -z "$want_exit" ] && [ -z "$want_error" ] && [ -z "$want_output" ] \
           && [ -z "$want_warn" ] && [ -z "$want_tokens" ]; then
            printf "  %sok%s    %s %s(ir)%s\n" "$C_OK" "$C_END" "$name" \
                   "$C_DIM" "$C_END"
            pass=$((pass + 1))
            continue
        fi
    fi

    # ── TOKENS: 字句解析器の出力だけを検証する ──
    #
    # ★ 構文解析より前の段階を独立してテストできます。
    #   NEWLINE / INDENT / DEDENT のような仮想トークンは、それを消費する
    #   構文（if / def）が無くても、ここで正しさを確認できます。
    #   セルフホスト版字句解析器の検証にも、この仕組みを使います。
    if [ -n "$want_tokens" ]; then
        actual_tokens="$("$PLC_CC" --dump-tokens "$case_file" 2>/dev/null \
                         | awk '{print $2}' | tr '\n' ' ' | tr -s ' ' | sed 's/ *$//')"
        if [ "$actual_tokens" != "$want_tokens" ]; then
            report_fail "$name" "トークン列が期待と違います
期待: $want_tokens
実際: $actual_tokens"
            continue
        fi
        # TOKENS だけのケースはここで合格
        if [ -z "$want_exit" ] && [ -z "$want_error" ] && [ -z "$want_output" ] \
           && [ -z "$want_warn" ]; then
            printf "  %sok%s    %s %s(tokens)%s\n" "$C_OK" "$C_END" "$name" \
                   "$C_DIM" "$C_END"
            pass=$((pass + 1))
            continue
        fi
    fi

    # ── EXPLAIN-MUT: 変更される実引数の一覧を検証する──
    #
    # ★ --dump-tokens を TOKENS: で検証するのと同じ形です。
    #   「表示するだけ」の option は、その表示そのものをテストします。
    if [ -n "$want_explain" ]; then
        actual_explain="$("$PLC_CC" --explain-mut "$case_file" 2>/dev/null)"
        missing=""
        while IFS= read -r want; do
            [ -z "$want" ] && continue
            printf '%s' "$actual_explain" | grep -qF -- "$want" || missing="$missing
  - $want"
        done <<EOF_EXPLAIN
$want_explain
EOF_EXPLAIN
        if [ -n "$missing" ]; then
            report_fail "$name" "--explain-mut の出力に含まれていない期待文字列があります:$missing
実際の出力:
$actual_explain"
            continue
        fi
        if [ -z "$want_exit" ] && [ -z "$want_error" ] && [ -z "$want_output" ] \
           && [ -z "$want_warn" ]; then
            printf "  %sok%s    %s %s(explain-mut)%s\n" "$C_OK" "$C_END" "$name" \
                   "$C_DIM" "$C_END"
            pass=$((pass + 1))
            continue
        fi
    fi

    # ── コンパイル ──
    # shellcheck disable=SC2086  # extra_flags は複数のオプションに分かれてほしい
    compile_err="$("$PLC_CC" $extra_flags "$case_file" -o "$exe" 2>&1 >/dev/null)"
    compile_rc=$?

    # ── ERROR: コンパイルが失敗し、指定文字列を含むことを期待 ──
    if [ -n "$want_error" ]; then
        if [ "$compile_rc" -eq 0 ]; then
            report_fail "$name" "コンパイルが成功してしまいました（失敗を期待）
期待するエラー: $want_error"
            continue
        fi

        # 期待する文字列を 1 行ずつ確認する
        missing=""
        nchecks=0
        while IFS= read -r want; do
            [ -z "$want" ] && continue
            nchecks=$((nchecks + 1))
            printf '%s' "$compile_err" | grep -qF -- "$want" || missing="$missing
  - $want"
        done <<EOF_WANT
$want_error
EOF_WANT

        if [ -n "$missing" ]; then
            report_fail "$name" "エラー出力に含まれていない期待文字列があります:$missing
実際の出力:
$compile_err"
        else
            printf "  %sok%s    %s %s(error x%d)%s\n" "$C_OK" "$C_END" "$name" \
                   "$C_DIM" "$nchecks" "$C_END"
            pass=$((pass + 1))
        fi
        continue
    fi

    # ── ここから先はコンパイル成功を期待 ──
    if [ "$compile_rc" -ne 0 ]; then
        report_fail "$name" "コンパイルに失敗しました
$compile_err"
        continue
    fi

    # ── WARN: 成功したうえで、警告が出ていることを期待──
    if [ -n "$want_warn" ]; then
        missing=""
        while IFS= read -r want; do
            [ -z "$want" ] && continue
            printf '%s' "$compile_err" | grep -qF -- "$want" || missing="$missing
  - $want"
        done <<EOF_WARN
$want_warn
EOF_WARN
        if [ -n "$missing" ]; then
            report_fail "$name" "警告に含まれていない期待文字列があります:$missing
実際の出力:
$compile_err"
            continue
        fi
    fi

    # ── 実行 ──
    # ★ Windows（MSYS2）では実行ファイルに .exe が付きます
    [ -x "$exe" ] || [ ! -x "$exe.exe" ] || exe="$exe.exe"
    # 注意: パイプで受けると $? が最後のコマンド（tr）のものになります。
    #    終了コードは**プログラム自身**のものを見なければ意味がないので、
    #    先に受け取ってから \r を落とします。
    actual_output="$("$exe" < "$stdin_file" 2>/dev/null)"
    actual_exit=$?
    actual_output="$(printf '%s' "$actual_output" | strip_cr)"

    ok=1
    reason=""

    if [ -n "$want_exit" ] && [ "$actual_exit" -ne "$want_exit" ]; then
        ok=0
        reason="終了コードが違います: 期待 $want_exit, 実際 $actual_exit"
    fi

    if [ -n "$want_output" ] && [ "$actual_output" != "$want_output" ]; then
        ok=0
        reason="$reason
標準出力が違います:
--- 期待 ---
$want_output
--- 実際 ---
$actual_output"
    fi

    if [ "$ok" -eq 1 ]; then
        detail="exit=$actual_exit"
        printf "  %sok%s    %s %s(%s)%s\n" "$C_OK" "$C_END" "$name" "$C_DIM" "$detail" "$C_END"
        pass=$((pass + 1))
    else
        report_fail "$name" "$reason"
    fi
done

echo
echo "────────────────────────────────"
if [ "$fail" -eq 0 ]; then
    printf "%s全 %d 件パス%s\n" "$C_OK" "$pass" "$C_END"
    [ "$skip" -gt 0 ] && printf "%s（%d 件スキップ）%s\n" "$C_DIM" "$skip" "$C_END"
    exit 0
else
    printf "%s%d 件パス / %d 件失敗 / %d 件スキップ%s\n" \
           "$C_NG" "$pass" "$fail" "$skip" "$C_END"
    for n in "${failed_names[@]}"; do echo "  - $n"; done
    exit 1
fi
