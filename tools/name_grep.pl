#!/usr/bin/env perl
# name_grep.pl — 「言語名がそこに書かれているか」だけを探す小道具
#
# ★ tools/check_naming.sh と tools/rename.sh が使います。
#   ただの grep では足りません。旧パッケージマネージャ名が `pen` だったとき、
#   `grep -F pen` は **`append` に当たって**しまいました。語の切れ目で探します。
#
# 使い方:  PAT="<探す値>" perl tools/name_grep.pl FILE...
#          COMMENTS_ONLY=1 を足すと、コメント行だけを見ます（下記）。
#   見つかった行を "ファイル:行番号:本文" で出します（無ければ何も出しません）。
use strict;
use warnings;

my $pat = $ENV{PAT} // die "PAT が要ります\n";

# ★ 前後に「語の切れ目」を要求します。ただし拡張子のように**語でない文字で
#   始まる**値の頭には \b を付けません。空白の直後に拡張子が来たとき、
#   空白もドットもどちらも語でない文字なので \b が成り立たないためです。
my $head = ($pat =~ /^\w/) ? '\b' : '';
my $tail = ($pat =~ /\w$/) ? '\b' : '';
my $re   = qr/$head\Q$pat\E$tail/;

# ★ COMMENTS_ONLY … コメント行だけを見ます。
#
#   注意: これが要る理由。拡張子を探すとき、**属性アクセスと見分けが付きません**。
#     この言語のソースで `s.xx` と書けば、それは「変数 s の xx という欄」ですが、
#     字面は「ys という名前の、拡張子つきファイル」とまったく同じです
#     （lib/plot は折れ線の y 座標を `ys` という欄に持っていて、実際に当たりました）。
#
#   そこでこの言語で書かれたソースについては、**コメント行だけ**を見ます。
#   拡張子を書き写してしまって困るのは「lib/math.xx を参照」のような
#   コメントであって、コードの側は langinfo 経由で書くからです。
my $comments_only = $ENV{COMMENTS_ONLY};

for my $f (@ARGV) {
    open(my $fh, '<', $f) or next;
    while (my $line = <$fh>) {
        next if $comments_only && $line !~ /^\s*#/;
        next unless $line =~ $re;
        chomp $line;
        print "$f:$.:$line\n";
    }
    close $fh;
}
