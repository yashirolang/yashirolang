#!/bin/sh
# tools/check_msgs.sh — 診断の英語の表（msgs/en.tsv）とソースを突き合わせる
#
# ★ docs/ja/design/i18n-diagnostics.md §8。sh と awk だけで書きます（「clang だけで建つ」）。
#
#   ① ソースで使っている鍵が、全部 en.tsv にある（訳し忘れ）
#   ② en.tsv の鍵が、どれかのソースで使われている（消した診断の訳が残っていない）
#   ③ 日本語と英語で、差し込みの番号（{0} {1} …）の集合が同じ
#   ④ C 版とセルフホスト版で、同じ鍵の日本語が同じ／同じ鍵が 2 つの日本語で使われていない
#   ⑤ en.tsv の 3 列目（日本語の控え）が、ソースの日本語と同じ（控えが古びていない）
#   ⑥ 鍵の置き場所: s で始まる区分（ssema など）は stage1 だけ、それ以外は C 版に必ずある
#      （C 版だけの鍵は構いません。--help の全文など、stage1 は行ごとに鍵を持つため）
#
# 注意: 日本語は**1 つの文字列リテラル**で書く約束です（ここで読めるように）。
#   MSG0("鍵", "日本語") / MSG2("鍵", "日本語 {0} {1}", a, b)   … C 版
#   MSGK("鍵", "日本語")                                        … C 版の静的な表
#   msg0("鍵", "日本語") / msg("鍵", "日本語 {0}", [a])         … セルフホスト版
set -u
cd "$(dirname "$0")/.."
# ★ 拡張子は make に訊きます（名前を書き写すと改名のたびに直すことになるため）
EXT="$(make -s print-LANG_EXT)"
TMP="${TMPDIR:-/tmp}/check_msgs.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# 「鍵<TAB>日本語<TAB>どこで」を並べる
{
    grep -noE 'MSG([0-9]|K)\("[^"]+", "([^"\\]|\\.)*"' src/*.c src/*.h 2>/dev/null | sed 's/^/c:/'
    grep -noE '(msg0?|diag\.msg0?)\("[^"]+", "([^"\\]|\\.)*"' selfhost/*"$EXT" 2>/dev/null | sed 's/^/s:/'
} | awk '
    {
        side = substr($0, 1, 1)
        rest = substr($0, 3)
        # file:line:MSGn("key", "ja"
        i = index(rest, ":"); file = substr(rest, 1, i - 1); rest = substr(rest, i + 1)
        i = index(rest, ":"); line = substr(rest, 1, i - 1); rest = substr(rest, i + 1)
        i = index(rest, "(\""); rest = substr(rest, i + 2)
        i = index(rest, "\""); key = substr(rest, 1, i - 1); rest = substr(rest, i + 1)
        i = index(rest, "\""); ja = substr(rest, i + 1); ja = substr(ja, 1, length(ja) - 1)
        if (file ~ /src\/diag\.h$/) next      # 説明の中の例
        printf "%s\t%s\t%s\t%s:%s\n", side, key, ja, file, line
    }' > "$TMP/uses"

grep -v '^#' msgs/en.tsv | grep -v '^$' > "$TMP/en"

fail=0
awk -F'\t' -v uses="$TMP/uses" '
    function nums(s,    out, i, c, n) {
        out = ""
        while (match(s, /\{[0-9]+\}/)) {
            n = substr(s, RSTART + 1, RLENGTH - 2)
            if (index(" " out " ", " " n " ") == 0) out = out " " n
            s = substr(s, RSTART + RLENGTH)
        }
        return sortnums(out)
    }
    function sortnums(s,    a, k, i, j, t, r) {
        k = split(s, a, " ")
        for (i = 1; i <= k; i++) for (j = i + 1; j <= k; j++) if (a[j] + 0 < a[i] + 0) { t = a[i]; a[i] = a[j]; a[j] = t }
        r = ""; for (i = 1; i <= k; i++) r = r " " a[i]
        return r
    }
    FNR == NR {
        if ($1 in en) { printf "  en.tsv に同じ鍵が 2 行あります: %s\n", $1; bad = 1 }
        en[$1] = $2; ref[$1] = $3; next
    }
    {
        side = $1; key = $2; ja = $3; where = $4
        used[key] = 1
        if (!(key in en)) { printf "  訳がありません: %s（%s）\n", key, where; bad = 1 }
        else if (nums(ja) != nums(en[key])) {
            printf "  差し込みが違います: %s（日本語{%s } / 英語{%s }）%s\n", key, nums(ja), nums(en[key]), where; bad = 1
        }
        if ((key in jaof) && jaof[key] != ja) {
            printf "  同じ鍵に違う日本語があります: %s\n    %s（%s）\n    %s（%s）\n", key, jaof[key], jawhere[key], ja, where; bad = 1
        }
        # 控えはソースの文字列リテラルから \" だけを戻したもの
        plain = ja; gsub(/\\"/, "\"", plain)
        if ((key in en) && ref[key] != plain) {
            printf "  日本語の控えがソースと違います: %s（%s）\n    控え: %s\n    正本: %s\n", key, where, ref[key], plain; bad = 1
        }
        jaof[key] = ja; jawhere[key] = where
        sides[key] = sides[key] side
    }
    END {
        for (k in en) if (!(k in used)) { printf "  ソースで使われていない訳があります: %s\n", k; bad = 1 }
        for (k in sides) {
            s1only = (k ~ /^s(lexer|parser|module|sema|ownck|main)\./)
            if (s1only && index(sides[k], "c") > 0) { printf "  stage1 だけの鍵を C 版で使っています: %s\n", k; bad = 1 }
            if (!s1only && index(sides[k], "c") == 0) { printf "  C 版に無い鍵です（stage1 だけなら s で始まる区分にします）: %s\n", k; bad = 1 }
        }
        exit bad
    }' "$TMP/en" "$TMP/uses" || fail=1

n=$(cut -f2 "$TMP/uses" | sort -u | wc -l | tr -d ' ')
if [ "$fail" -ne 0 ]; then
    echo "check-msgs: 食い違いがあります（docs/ja/design/i18n-diagnostics.md §8）"
    exit 1
fi
echo "  ok    診断の英語の表はソースと一致しています（鍵 $n 個）"
