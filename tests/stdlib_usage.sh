#!/bin/bash
# 標準ライブラリの API がどれだけ使われているかを数える
#
# ★ これは**カバレッジではありません。** 本言語には計装の仕組みが
#   まだ無いので（ロードマップ B-2）、代わりに「lib/*$EXT が公開している
#   関数のうち、どれがテスト・例・コンパイラ本体から**呼ばれているか**」を
#   ソースの静的な検索で数えます。
#
#   数えるのは 2 つです:
#     ① `モジュール名.関数名(` … 外のファイルからの呼び出し
#     ② そのモジュール自身の中の `関数名(` … 内部の助けとして使われている
#   ②を数えないと、plot のように「公開しているのはメソッドで、
#   トップレベルの def は内部の道具」というモジュールが 0% に見えます。
#
#   メソッド（`x.foo()`）は呼ばれる型が分からないので数えません。
#
# 使い方: tests/stdlib_usage.sh [--missing]   （--missing で未使用の一覧）

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXT="$(make -s -C "$ROOT" print-LANG_EXT)"
show_missing=0
[ "${1:-}" != "--missing" ] || show_missing=1

# 呼び出し側として見る場所（lib 自身も互いに呼び合うので入れる）
SEARCH=()
for d in tests examples selfhost bench kernel lib tools; do
    [ -d "$ROOT/$d" ] && SEARCH+=("$ROOT/$d")
done

if [ -t 1 ]; then C_DIM=$'\033[2m'; C_END=$'\033[0m'; else C_DIM=''; C_END=''; fi

total=0
used=0
printf "  %-12s %6s %6s  %s\n" "モジュール" "公開" "使用" "使用率"
echo "  ────────────────────────────────────────"

for f in "$ROOT"/lib/*$EXT; do
    mod="$(basename "$f" "$EXT")"
    # 公開している関数（トップレベルの def。extern と _ 始まりは除く）
    # ⚠️ ジェネリック（def f[T](…)）も拾うので [.*] を挟めるようにします
    funcs="$(sed -n 's/^def \([a-zA-Z][a-zA-Z0-9_]*\)\(\[[^]]*\]\)\{0,1\}(.*/\1/p' \
             "$f" | sort -u)"
    [ -n "$funcs" ] || continue

    n=0; u=0; missing=""
    while IFS= read -r fn; do
        [ -n "$fn" ] || continue
        n=$((n + 1))
        # ① 外から `mod.fn(` で呼ばれているか
        if grep -rqF --include="*$EXT" "$mod.$fn(" "${SEARCH[@]}" 2>/dev/null; then
            u=$((u + 1))
        # ② 自分のモジュールの中で呼ばれているか
        #    （定義の行 `def fn(` を除いて 1 回でも出てくれば「使われている」）
        elif [ "$(grep -cE "(^|[^A-Za-z0-9_.])$fn\(" "$f")" -gt \
                "$(grep -cE "^def $fn(\[[^]]*\])?\(" "$f")" ]; then
            u=$((u + 1))
        else
            missing="$missing $fn"
        fi
    done <<EOF
$funcs
EOF

    pct=0
    [ "$n" -eq 0 ] || pct=$(( u * 100 / n ))
    printf "  %-12s %6d %6d  %3d%%\n" "$mod" "$n" "$u" "$pct"
    if [ "$show_missing" -eq 1 ] && [ -n "$missing" ]; then
        printf "%s      未使用:%s%s\n" "$C_DIM" "$missing" "$C_END"
    fi
    total=$((total + n))
    used=$((used + u))
done

echo "  ────────────────────────────────────────"
pct=0
[ "$total" -eq 0 ] || pct=$(( used * 100 / total ))
printf "  %-12s %6d %6d  %3d%%\n" "合計" "$total" "$used" "$pct"
echo ""
echo "⚠️ メソッド（x.foo()）は数えていません。呼ばれる型が静的に追えないためです。"
[ "$show_missing" -eq 1 ] || echo "   未使用の一覧: tests/stdlib_usage.sh --missing"
