# markers.awk — テストケースの先頭コメントから期待値を 1 回で全部読み取る
#
# 🤔 なぜ awk 1 本にまとめたのか
#   以前はここで `sed` を 11 回、`tr` を 4 回、`grep` を 1 回起動していました。
#   ケースは 600 件近くあるので、**1 万プロセス近く**を毎回作っていたことに
#   なります。Linux では安いのですが、Windows（MSYS2）では `fork` を
#   エミュレートするため 1 桁高くつき、CI の Windows ジョブが目に見えて
#   遅くなっていました。⚠️ 直したのは呼び出しの回数だけで、**読み取る
#   中身は 1 バイトも変えていません**。
#
# 使い方:
#   eval "$(awk -v stdin_file=... -f tests/markers.awk ケース)"
#
# ★ シェルの変数代入をそのまま吐きます。値は単引用符で囲み、値の中の
#   単引用符は '\'' に置き換えます（シェルの流儀）。

function q(s,    r) {           # シェルの単引用符でくるむ
    gsub(/'/, "'\\''", s)
    return "'" s "'"
}

function emit(name, v) {        # 変数 1 つを出す
    printf "%s=%s\n", name, q(v)
}

# ⚠️ **末尾の改行を落とします。** 以前は `$(sed ...)` で受けており、シェルの
#   コマンド置換は末尾の改行を**全部**落とします。`# OUTPUT: ` だけの行を
#   最後に書いたケース（lib_strings_bytes）が、これが無いと落ちます。
#   ★ 先頭や途中の空行は落としません（そこはコマンド置換も残します）。
function chomp(s) {
    sub(/\n+$/, "", s)
    return s
}

# 「1 行にまとめる」もの（空白 1 個で連結し、末尾の空白を落とす）
function joined(s,    r) {
    gsub(/\n/, " ", s)
    while (s ~ /  /) gsub(/  /, " ", s)
    sub(/ +$/, "", s)
    return s
}

# 複数行の値を貯める。
# ⚠️ **空行が 1 行目に来る場合があります**（`# OUTPUT: ` だけの行）。
#   「中身が空かどうか」で判断すると、その行を落としてしまうので、
#   **何行貯めたか**を別に数えます。
function add(key, line) {
    if (cnt[key]++ == 0) acc[key] = line
    else acc[key] = acc[key] "\n" line
}

BEGIN { first_exit = ""; got_exit = 0; first_stage0 = ""; got_stage0 = 0 }

{
    line = $0
    gsub(/\r/, "", line)        # ⚠️ Windows のチェックアウト対策（strip_cr）
    if (line !~ /^#/) next

    if (match(line, /^# *EXIT: */))          { v = substr(line, RLENGTH + 1)
        if (!got_exit) { first_exit = v; got_exit = 1 }                      ; next }
    if (match(line, /^# *ERROR: */))         { add("err",   substr(line, RLENGTH + 1)); next }
    if (match(line, /^# *OUTPUT: */))        { add("out",   substr(line, RLENGTH + 1)); next }
    if (match(line, /^# *STDIN: */))         { add("sin",   substr(line, RLENGTH + 1)); next }
    if (match(line, /^# *TOKENS: */))        { add("tok",   substr(line, RLENGTH + 1)); next }
    # ⚠️ IR-NOT を先に見ます（`# IR:` の規則は `IR-NOT:` に当たりませんが、
    #   読む人が取り違えないように順序でも示しておきます）
    if (match(line, /^# *IR-NOT: */))        { add("irnot", substr(line, RLENGTH + 1)); next }
    if (match(line, /^# *IR: */))            { add("ir",    substr(line, RLENGTH + 1)); next }
    if (match(line, /^# *WARN: */))          { add("warn",  substr(line, RLENGTH + 1)); next }
    if (match(line, /^# *EXPLAIN-MUT: */))   { add("expl",  substr(line, RLENGTH + 1)); next }
    if (match(line, /^# *FLAGS: */))         { add("flags", substr(line, RLENGTH + 1)); next }
    if (match(line, /^# *EXACT-IR:/))        { exact  = 1                                     ; next }
    if (match(line, /^# *STAGE0-ONLY: */))   { v = substr(line, RLENGTH + 1)
        if (!got_stage0) { first_stage0 = v; got_stage0 = 1 }                ; next }
}

END {
    emit("want_exit",    first_exit)
    emit("want_error",   chomp(acc["err"]))
    emit("want_output",  chomp(acc["out"]))
    emit("want_tokens",  joined(acc["tok"]))
    emit("want_ir",      chomp(acc["ir"]))
    emit("want_ir_not",  chomp(acc["irnot"]))
    emit("want_warn",    chomp(acc["warn"]))
    emit("want_explain", chomp(acc["expl"]))
    emit("extra_flags",  joined(acc["flags"]))
    emit("has_exact_ir", exact ? "1" : "")
    emit("stage0_only",  first_stage0)
    # ★ STDIN はファイルに落とします（無ければ空のファイル）。
    #   ⚠️ 与えないケースでも必ず繋ぐので、端末や CI の標準入力を
    #     読んでしまうことがありません。
    printf "" > stdin_file
    if (cnt["sin"] > 0) printf "%s\n", acc["sin"] > stdin_file
    close(stdin_file)
}
