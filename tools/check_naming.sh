#!/usr/bin/env bash
# check_naming.sh — 「言語名がどこにも書き写されていないか」の見張り
#
# ★ なぜこれが要るのか（docs/design/naming.md）
#   この言語の名前は将来また変わります。改名を `tools/rename.sh` の 1 回で
#   終わらせるには、**名前の文字列が決められた場所にしか無い**ことが前提です。
#   その前提が崩れていないかを、機械に見張らせます。CI で毎回回ります。
#
#   ⚠️ これは「名前を探して置換する」道具ではありません。**書き漏れを見つけて
#     落とす**道具です。落ちたら、その場所を名前に依存しない書き方に直します。
#
#       C 版のコード      → PLC_LANG_* マクロ（src/langinfo.h）
#       この言語のコード  → import langinfo → langinfo.cc() など
#       文書（docs/）     → {{cc}} {{ext}} {{pm}} …（make docs で流し込む）
#       シェル・CI        → make -s print-LANG_CC などで make に訊く
#       Makefile          → $(LANG_CC) $(LANG_EXT) …
#
# 使い方:
#   tools/check_naming.sh        # make check-naming から呼ばれます
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

NAME="$(make -s print-LANG_NAME)"
EXT="$(make -s print-LANG_EXT)"
CC="$(make -s print-LANG_CC)"
PM="$(make -s print-LANG_PM)"

# ── 名前を書いてよい場所 ────────────────────────────────────
#
# ★ ここに挙げた 5 つだけが例外です。増やさないでください。
#   増やすということは「改名のときに手で直す場所が増える」ということです。
#
#   Makefile           … LANG_* の定義そのもの（唯一の出どころ）
#   src/langinfo.h     … C 版の既定値（Makefile の -D が無いとき用）
#   lib/langinfo<ext>  … この言語で書かれた側の定義
#   docs/design/naming.md … 改名の履歴（旧名を消すと履歴にならない）
#   README.md          … README.md.in から生成したもの（GitHub の入口）
allowed() {
    case "$1" in
        ./Makefile|./src/langinfo.h|"./lib/langinfo$EXT") return 0 ;;
        ./docs/design/naming.md|./README.md)              return 0 ;;
        *) return 1 ;;
    esac
}

# ── 見に行くファイルを数え上げる ────────────────────────────
#
# ★ **git に訊きます。** 追跡しているファイルと、追跡していないが
#   .gitignore で無視されてもいないファイルの 2 つです。
#
#   ⚠️ これで `build/` `tests/tmp/` `bench/out/` のような**生成物が自動で
#     外れます**。生成物には当然いまの名前が入っていますが、それは
#     「書き漏れ」ではないので、見ると誤検出になります。
#     除外リストを手で持つと .gitignore と二重管理になるので持ちません。
list_files() {
    if git rev-parse --git-dir >/dev/null 2>&1; then
        git ls-files --cached --others --exclude-standard -z
    else
        # git リポジトリでないときの保険（配布物を展開しただけ、など）
        find . -type f -not -path './.git/*' -not -path './build/*' -print0
    fi
}

fail=0
report() { # 見出し 値 ヒット
    echo "★ 言語名が直接書かれています（$1: '$2'）"
    echo "$3" | sed 's/^/    /'
    fail=$((fail + 1))
}

# ★ 同じ値が 2 つの変数に入っていることがあります（例: LANG_NAME と LANG_CC が
#   どちらも同じ綴り）。同じ文字列を 2 度報告しても意味が無いので 1 度にします。
seen=""
for pair in "LANG_NAME|$NAME" "LANG_CC|$CC" "LANG_PM|$PM" "LANG_EXT|$EXT"; do
    label="${pair%%|*}"
    value="${pair#*|}"
    case " $seen " in *" $value "*) continue ;; esac
    seen="$seen $value"

    hits=""
    while IFS= read -r -d "" f; do
        f="./$f"
        allowed "$f" && continue
        # ⚠️ テキストファイルだけを見ます（実行ファイルに偶然並ぶバイト列は無視）
        LC_ALL=C grep -qI . "$f" 2>/dev/null || continue
        # ⚠️ ただの grep では足りません。旧パッケージマネージャ名が `pen` だった
        #   とき `grep -F pen` は `append` に当たりました。語の切れ目で探します。
        #
        # ⚠️ 拡張子を、この言語で書かれたソースの中で探すときは
        #   **コメント行だけ**にします。`s.xx` のような属性アクセスと
        #   字面が同じで見分けが付かないためです（tools/name_grep.pl の説明）。
        co=""
        if [ "$label" = "LANG_EXT" ]; then
            case "$f" in *"$EXT") co=1 ;; esac
        fi
        one="$(PAT="$value" COMMENTS_ONLY="$co" perl tools/name_grep.pl "$f" 2>/dev/null || true)"
        [ -n "$one" ] && hits="$hits$one
"
    done < <(list_files)

    [ -n "$hits" ] && report "$label" "$value" "$(printf '%s' "$hits")"
done

echo
if [ "$fail" -ne 0 ]; then
    cat <<'MSG'
★ 名前の書き漏れが見つかりました。名前に依存しない書き方に直してください。

    C 版のコード      → PLC_LANG_* マクロ（src/langinfo.h）
    この言語のコード  → import langinfo → langinfo.cc() / .ext() / .pm() …
    文書（docs/）     → {{cc}} {{ext}} {{pm}} …（make docs で読める形が出ます）
    シェル・CI        → LANG_CC="$(make -s print-LANG_CC)" のように make に訊く
    Makefile          → $(LANG_CC) $(LANG_EXT) …
MSG
    exit 1
fi
echo "  ok    言語名は定義の場所にしか書かれていません"
