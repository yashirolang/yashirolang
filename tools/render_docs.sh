#!/usr/bin/env bash
# render_docs.sh — 文書のひな型に、いまの言語名を流し込む
#
# ★ なぜこれが要るのか（docs/design/naming.md）
#   この言語の名前は将来また変わります。前回の改名でいちばん時間を食ったのは
#   `docs/` の一括置換でした。そこで **docs/ には言語名を一切書かない**ことにして、
#   代わりに次の合い言葉（プレースホルダ）を書きます。
#
#       {{name}}     人が読む言語名
#       {{ext}}      ソースの拡張子（ドット込み）
#       {{cc}}       コンパイラのコマンド名
#       {{pm}}       パッケージマネージャのコマンド名
#       {{version}}  版番号
#       {{repo}}     リポジトリの URL
#       {{repodir}}  clone したときに出来るディレクトリ名（URL の末尾）
#
#   ★ 合い言葉そのものを文章に書きたいとき（「{{cc}} と書きます」の説明など）は
#     {{!cc}} のように ! を挟みます。流し込みのいちばん最後に {{cc}} へ戻すので、
#     値に置き換わりません。
#
#   値は Makefile の LANG_* が唯一の出どころです。ここでは **make に訊く**だけで、
#   スクリプト自身は名前を 1 文字も持ちません。
#
# 使い方:
#   tools/render_docs.sh              # docs/ → build/docs/ を作り直す
#   tools/render_docs.sh --check      # 流し込まずに、ひな型に生の言語名が
#                                     # 混ざっていないかだけ見る
#   tools/render_docs.sh FILE...      # 指定のファイルを標準出力に流し込む
#
# ⚠️ 生成物（build/docs/）は git に入れません。読みたいときに作ります。
#    唯一の例外は README.md です（GitHub の入口なので、生成したものを
#    コミットします。README.md.in がひな型、`make readme` で作り直します）。
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

OUT_DIR="${OUT_DIR:-build/docs}"

# ── 値は make にだけ訊く（ここに名前を書き写さない）────────────
NAME="$(make -s print-LANG_NAME)"
EXT="$(make -s print-LANG_EXT)"
CC="$(make -s print-LANG_CC)"
PM="$(make -s print-LANG_PM)"
VERSION="$(make -s print-LANG_VERSION)"
REPO="$(make -s print-LANG_REPO)"
# clone したときのディレクトリ名は URL の末尾（末尾の .git と / は落とす）
REPODIR="${REPO%.git}"; REPODIR="${REPODIR%/}"; REPODIR="${REPODIR##*/}"

# ★ 置換は perl に環境変数で渡します。値に / や . が入っても壊れないように、
#   sed の区切り文字ではなく環境変数を使うのが安全です。
render() { # ひな型を標準入力で受け、流し込んだものを標準出力に出す
    PL_NAME="$NAME" PL_EXT="$EXT" PL_CC="$CC" PL_PM="$PM" PL_VERSION="$VERSION" \
    PL_REPO="$REPO" PL_REPODIR="$REPODIR" \
    perl -pe '
        s/\{\{name\}\}/$ENV{PL_NAME}/g;
        s/\{\{ext\}\}/$ENV{PL_EXT}/g;
        s/\{\{cc\}\}/$ENV{PL_CC}/g;
        s/\{\{pm\}\}/$ENV{PL_PM}/g;
        s/\{\{version\}\}/$ENV{PL_VERSION}/g;
        s/\{\{repodir\}\}/$ENV{PL_REPODIR}/g;
        s/\{\{repo\}\}/$ENV{PL_REPO}/g;
        # ★ 最後に、逃がしてあった {{!xxx}} を {{xxx}} に戻します。
        #   （上の置換はどれも ! を含む形に当たらないので、ここまで生き残ります）
        s/\{\{!/{{/g;
    '
}

# ── --check … ひな型に生の言語名が混ざっていないか ──────────────
#
# ★ 改名しても文書を直さずに済む、という約束が守られているかの見張りです。
#   `make check-naming` から呼ばれ、CI で回ります。
if [ "${1:-}" = "--check" ]; then
    fail=0
    # 探すのは「いまの名前」です。ひな型に書いてあったら、改名のときに
    # 置き去りにされる場所なので落とします。
    # ⚠️ ただの grep では足りません。旧パッケージマネージャ名が `pen` だった
    #   とき、`grep -F pen` は **`append` に当たって**しまいました。
    #   語の切れ目で探すのは tools/name_grep.pl の仕事です。
    #   naming.md だけは改名の履歴を持つので除きます。
    for pat in "$NAME" "$CC" "$PM" "$EXT"; do
        hits="$(find docs -type f -name '*.md' ! -path 'docs/design/naming.md' -print0 \
                | PAT="$pat" xargs -0 perl tools/name_grep.pl 2>/dev/null || true)"
        if [ -n "$hits" ]; then
            echo "★ docs/ に生の言語名が書かれています（'$pat'）:"
            echo "$hits" | sed 's/^/    /'
            echo "  → プレースホルダ（{{name}} {{ext}} {{cc}} {{pm}} {{version}} {{repo}}）に直してください"
            fail=$((fail + 1))
        fi
    done
    if [ "$fail" -ne 0 ]; then
        exit 1
    fi
    echo "  ok    docs/ に言語名は書かれていません"
    exit 0
fi

# ── ファイル指定 … 標準出力に流し込む ──────────────────────────
if [ $# -gt 0 ]; then
    for f in "$@"; do
        render < "$f"
    done
    exit 0
fi

# ── 既定 … docs/ を build/docs/ に作り直す ─────────────────────
rm -rf "$OUT_DIR"
n=0
while IFS= read -r f; do
    dest="$OUT_DIR/${f#docs/}"
    mkdir -p "$(dirname "$dest")"
    render < "$f" > "$dest"
    n=$((n + 1))
done < <(find docs -type f -name '*.md' | sort)

echo "  $n 本の文書に名前を流し込みました → $OUT_DIR/"
