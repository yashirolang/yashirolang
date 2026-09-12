#!/usr/bin/env bash
# rename.sh — 言語の名前を変える（唯一の手順）
#
# ★ 名前に依存する値は 6 つだけです（docs/design/naming.md）。
#     LANG_NAME … 人が読む言語名
#     LANG_EXT  … ソースの拡張子（ドット込み）
#     LANG_CC   … コンパイラのコマンド名
#     LANG_PM   … パッケージマネージャのコマンド名
#     LANG_VERSION … 版番号
#     LANG_REPO … リポジトリの URL
#
#   この 6 つが書いてあるのは Makefile と src/langinfo.h と
#   lib/langinfo.<ext> の 3 か所だけです。手で直すとどれかを必ず忘れるので、
#   **このスクリプトから直します**（拡張子を変えたときのファイル名変更も込み）。
#
# 使い方:
#   tools/rename.sh --name <言語名> --ext <.拡張子> --cc <コマンド名> \
#                   --pm <PM のコマンド名> --repo <リポジトリの URL>
#   tools/rename.sh --ext <.拡張子>        # 変えたいものだけでよい
#   tools/rename.sh --show                 # いまの値を見るだけ
#
# ⚠️ ここに**実在しそうな名前を例として書かないこと**。
#   いつか誰かがちょうどその名前に改名したとき、この行が
#   `make check-naming` に「書き漏れ」として拾われてしまいます
#   （実際、例に書いた名前へ改名する試験で引っかかりました）。
#
# ★ このスクリプトは**文書やコードの中身を置換しません**。
#   置換が要らないように、文書はひな型（{{cc}} などの合い言葉）で、
#   コードは langinfo 経由で書いてあるからです。やることは
#   「① ファイル名の拡張子」「② 定義 3 か所」「③ README の作り直し」だけです。
#
#   ⚠️ 昔の版はリポジトリ全体を一括置換していましたが、**テストの中の
#     ただの文字列**（`Item("pen")` のような試験データ）まで書き換えてしまう
#     ため、やめました。置換しないことが安全です。
#
# ⚠️ 走らせたあとは必ず `make clean && make test && make bootstrap` を
#   通してください。2 つの langinfo がずれていれば selfhost-test が落ちます。
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

cur() { make -s "print-$1"; }

OLD_NAME="$(cur LANG_NAME)"
OLD_EXT="$(cur LANG_EXT)"
OLD_CC="$(cur LANG_CC)"
OLD_PM="$(cur LANG_PM)"
OLD_VERSION="$(cur LANG_VERSION)"
OLD_REPO="$(cur LANG_REPO)"

NEW_NAME="$OLD_NAME"; NEW_EXT="$OLD_EXT"; NEW_CC="$OLD_CC"
NEW_PM="$OLD_PM";     NEW_VERSION="$OLD_VERSION"; NEW_REPO="$OLD_REPO"

show() {
    printf 'LANG_NAME    = %s\n' "$OLD_NAME"
    printf 'LANG_EXT     = %s\n' "$OLD_EXT"
    printf 'LANG_CC      = %s\n' "$OLD_CC"
    printf 'LANG_PM      = %s\n' "$OLD_PM"
    printf 'LANG_VERSION = %s\n' "$OLD_VERSION"
    printf 'LANG_REPO    = %s\n' "$OLD_REPO"
}

usage() { sed -n '2,36p' "$0"; exit "${1:-0}"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --name)    NEW_NAME="$2";    shift 2 ;;
        --ext)     NEW_EXT="$2";     shift 2 ;;
        --cc)      NEW_CC="$2";      shift 2 ;;
        --pm)      NEW_PM="$2";      shift 2 ;;
        --version) NEW_VERSION="$2"; shift 2 ;;
        --repo)    NEW_REPO="$2";    shift 2 ;;
        --show)    show; exit 0 ;;
        -h|--help) usage 0 ;;
        *) echo "知らない引数です: $1" >&2; usage 1 ;;
    esac
done

# 拡張子は必ずドットで始まること（".xx" であって "xx" ではない）
case "$NEW_EXT" in
    .?*) ;;
    *) echo "拡張子はドットから書いてください（先頭がドット）: $NEW_EXT" >&2; exit 1 ;;
esac

echo "── 変更内容 ──────────────────────────"
printf '  LANG_NAME    %s → %s\n' "$OLD_NAME"    "$NEW_NAME"
printf '  LANG_EXT     %s → %s\n' "$OLD_EXT"     "$NEW_EXT"
printf '  LANG_CC      %s → %s\n' "$OLD_CC"      "$NEW_CC"
printf '  LANG_PM      %s → %s\n' "$OLD_PM"      "$NEW_PM"
printf '  LANG_VERSION %s → %s\n' "$OLD_VERSION" "$NEW_VERSION"
printf '  LANG_REPO    %s → %s\n' "$OLD_REPO"    "$NEW_REPO"
echo

# ── ① ソースファイルの拡張子を変える ──────────────────────
#   ⚠️ .git と build は触りません。git 管理下のものは git mv を使うので
#     履歴が「改名」として残ります。
if [ "$NEW_EXT" != "$OLD_EXT" ]; then
    n=0
    while IFS= read -r f; do
        new="${f%$OLD_EXT}$NEW_EXT"
        if git ls-files --error-unmatch "$f" >/dev/null 2>&1; then
            git mv -- "$f" "$new"
        else
            mv -- "$f" "$new"
        fi
        n=$((n + 1))
    done < <(find . -path ./.git -prune -o -path ./build -prune -o -name "*$OLD_EXT" -print)
    echo "  ソース $n 本の拡張子を変えました（$OLD_EXT → ${NEW_EXT}）"
fi

# ── ② 名前が決まる 3 か所を書き換える ──────────────────────
# ⚠️ 値は**環境変数で**perl に渡します。s/…/…/ の中に直接埋めると、
#   URL の「/」が区切り文字とぶつかって perl が構文エラーになります
#   （LANG_REPO を足したときに実際そうなりました）。
mf_set() { # Makefile の LANG_X := 値
    K="$1" V="$2" perl -i -pe 's/^(\Q$ENV{K}\E\s*:=\s*).*$/$1$ENV{V}/' Makefile
}
mf_set LANG_NAME    "$NEW_NAME"
mf_set LANG_EXT     "$NEW_EXT"
mf_set LANG_CC      "$NEW_CC"
mf_set LANG_PM      "$NEW_PM"
mf_set LANG_VERSION "$NEW_VERSION"
mf_set LANG_REPO    "$NEW_REPO"

h_set() { # src/langinfo.h の #define PLC_LANG_X "値"（後ろのコメントは残す）
    K="$1" V="$2" perl -i -pe 's/^(#define \Q$ENV{K}\E )"[^"]*"/$1"$ENV{V}"/' src/langinfo.h
}
h_set PLC_LANG_NAME    "$NEW_NAME"
h_set PLC_LANG_EXT     "$NEW_EXT"
h_set PLC_LANG_CC      "$NEW_CC"
h_set PLC_LANG_PM      "$NEW_PM"
h_set PLC_LANG_VERSION "$NEW_VERSION"

LANGINFO="lib/langinfo$NEW_EXT"
p_set() { # lib/langinfo.<ext> の X: str = "値"
    K="$1" V="$2" perl -i -pe 's/^(\Q$ENV{K}\E: str = )"[^"]*"/$1"$ENV{V}"/' "$LANGINFO"
}
p_set NAME    "$NEW_NAME"
p_set EXT     "$NEW_EXT"
p_set CC      "$NEW_CC"
p_set PM      "$NEW_PM"
p_set VERSION "$NEW_VERSION"

echo "  Makefile / src/langinfo.h / $LANGINFO を揃えました"

# ── ③ README を作り直す ────────────────────────────────────
#   ★ README.md は README.md.in から生成したものです（GitHub の入口なので、
#     ひな型のままでは読めないため、生成物をコミットします）。
make -s readme
echo

# ── ④ ほんとうに揃ったか確かめる ────────────────────────────
fail=0
check() { # 名前 期待値 実際
    if [ "$2" = "$3" ]; then
        printf '  ok    %-12s %s\n' "$1" "$2"
    else
        printf '  FAIL  %-12s 期待 %s / 実際 %s\n' "$1" "$2" "$3"
        fail=$((fail + 1))
    fi
}
check LANG_NAME    "$NEW_NAME"    "$(cur LANG_NAME)"
check LANG_EXT     "$NEW_EXT"     "$(cur LANG_EXT)"
check LANG_CC      "$NEW_CC"      "$(cur LANG_CC)"
check LANG_PM      "$NEW_PM"      "$(cur LANG_PM)"
check LANG_VERSION "$NEW_VERSION" "$(cur LANG_VERSION)"
check LANG_REPO    "$NEW_REPO"    "$(cur LANG_REPO)"
grep -q "\"$NEW_NAME\"" src/langinfo.h || { echo "  FAIL  src/langinfo.h に $NEW_NAME がありません"; fail=$((fail + 1)); }
grep -q "\"$NEW_NAME\"" "$LANGINFO"    || { echo "  FAIL  $LANGINFO に $NEW_NAME がありません";    fail=$((fail + 1)); }

echo
# ⑤ 名前の書き漏れが無いか（新しい名前で探し直します）
if ! make -s check-naming; then
    fail=$((fail + 1))
fi

echo
if [ "$fail" -ne 0 ]; then
    echo "★ 揃っていません（$fail 件）。上の FAIL を直してください。"
    exit 1
fi

cat <<MSG
★ 名前を変えました。仕上げに次を通してください:

    make clean && make test && make bootstrap

  ⚠️ 2 つの langinfo がずれていると selfhost-test が落ちます。
     つまり「揃っているか」はテストが自動で見張ります。

  ★ 手で直すのは 1 つだけです:
    - docs/design/naming.md の「改名の履歴」に 1 行足す
      （履歴に旧名が要るので、この文書だけは検査の対象外です）
MSG
