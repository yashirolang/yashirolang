#!/usr/bin/env bash
# render_template.sh — ひな型に、いまの言語名を流し込む
#
# ★ 文書（README.md と docs/）には**実際の名前が書いてあります**。
#   ここで流し込むのは、**文書ではないもの**だけです。いまの利用者は
#   `.github/release-body.md.in`（Release の本文）1 つです。
#   CI の YAML に名前を書き写さないために使います（docs/design/naming.md）。
#
#       {{name}}     人が読む言語名
#       {{ext}}      ソースの拡張子（ドット込み）
#       {{cc}}       コンパイラのコマンド名
#       {{pm}}       パッケージマネージャのコマンド名
#       {{version}}  版番号
#       {{repo}}     リポジトリの URL
#
#   値は Makefile の LANG_* が唯一の出どころです。ここでは **make に訊く**だけで、
#   スクリプト自身は名前を 1 文字も持ちません。
#
# 使い方:
#   tools/render_template.sh FILE...   # 流し込んだものを標準出力に出す
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

NAME="$(make -s print-LANG_NAME)"
EXT="$(make -s print-LANG_EXT)"
CC="$(make -s print-LANG_CC)"
PM="$(make -s print-LANG_PM)"
VERSION="$(make -s print-LANG_VERSION)"
REPO="$(make -s print-LANG_REPO)"

# ★ 置換は perl に環境変数で渡します。値に / や . が入っても壊れないように、
#   sed の区切り文字ではなく環境変数を使うのが安全です。
for f in "$@"; do
    PL_NAME="$NAME" PL_EXT="$EXT" PL_CC="$CC" PL_PM="$PM" \
    PL_VERSION="$VERSION" PL_REPO="$REPO" \
    perl -pe '
        s/\{\{name\}\}/$ENV{PL_NAME}/g;
        s/\{\{ext\}\}/$ENV{PL_EXT}/g;
        s/\{\{cc\}\}/$ENV{PL_CC}/g;
        s/\{\{pm\}\}/$ENV{PL_PM}/g;
        s/\{\{version\}\}/$ENV{PL_VERSION}/g;
        s/\{\{repo\}\}/$ENV{PL_REPO}/g;
    ' < "$f"
done
