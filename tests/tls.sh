#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Copyright 2026 Shota Iwamoto
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# TLS の受け入れテスト
#
# ★ **自己署名の証明書をその場で作って、自分に繋ぎます。** 外には出ません。
#   通信できない場所でも、相手の都合でも結果が変わらないようにするためです。
#
# ★ **秘密鍵はリポジトリに入れません。** 走らせるたびに作って捨てます。
#   試験用と書いてあっても、鍵が版管理に入っていると、いつか本物として
#   使われます。
#
# ★ 見ているのは主に「繋がってはいけないものが繋がらないこと」です
#   （中身は tests/tls_probe<ext> を読んでください）。
#
# 使い方: tests/tls.sh
#
# 注意: TLS を組み込んでいないビルド（既定）では**飛ばします**。
#   これは失敗ではありません——組み込まないことを選べるのが設計です。

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# ★ 実行ファイル名は Makefile の LANG_CC / LANG_EXT が決めます。
#   ここに名前を書き写すと改名のたびに直すことになるので、make に訊きます。
LANG_CC="$(make -s -C "$ROOT" print-LANG_CC)"
EXT="$(make -s -C "$ROOT" print-LANG_EXT)"
PLC="$ROOT/build/$LANG_CC"
[ -x "$PLC" ] || [ ! -x "$PLC.exe" ] || PLC="$PLC.exe"
WORK="$ROOT/tests/tmp/tls"
PROBE="$ROOT/tests/tls_probe$EXT"

if [ ! -x "$PLC" ]; then
    echo "$LANG_CC が見つかりません: ${PLC}（先に 'make'）"
    exit 1
fi
if ! command -v openssl > /dev/null 2>&1; then
    echo "  skip  TLS のテスト（openssl のコマンドがありません）"
    exit 0
fi

export PLC_LIB_DIR="$ROOT/lib"
export PLC_RUNTIME_O="$ROOT/build/runtime.a"

rm -rf "$WORK"
mkdir -p "$WORK"

if ! "$PLC" "$PROBE" -o "$WORK/probe" > "$WORK/build.log" 2>&1; then
    echo "  FAIL  TLS のテスト（試験のプログラムを建てられません）"
    sed 's/^/          /' "$WORK/build.log"
    exit 1
fi

# ★ **TLS を組み込んでいないビルドでは飛ばします。**
#   探り役に訊きます（3 が「組み込んでいません」。引数なしで走らせたときの
#   2 と分けてあるので、渡し忘れが「飛ばす」に化けません）。
"$WORK/probe" > /dev/null 2>&1
if [ $? -eq 3 ]; then
    echo "  skip  TLS のテスト（TLS を組み込んでいません。'make TLS=1' で入ります）"
    exit 0
fi

# ── 自己署名の証明書を作る ──────────────────────────────
#
# ★ **localhost のものとして作ります。** 名前の照合が効いていることを、
#   同じ相手に 127.0.0.1 として繋いで確かめるためです。
# 注意: 1 日で切れます。走らせるたびに作り直すので、それで足ります
#   （長い有効期限の証明書を置いておくと、いつか手元の道具が本物として
#   信じはじめます）。
if ! openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$WORK/server.key" -out "$WORK/server.pem" \
        -days 1 -subj "/CN=localhost" \
        -addext "subjectAltName=DNS:localhost" \
        > "$WORK/openssl.log" 2>&1; then
    echo "  FAIL  TLS のテスト（証明書を作れません）"
    sed 's/^/          /' "$WORK/openssl.log"
    exit 1
fi
chmod 600 "$WORK/server.key"

want="往復: echo:ping
暗号化されている: True
版: True
知らない証明書: 断った
名前が違う: 断った
平文の相手に TLS: 断った
https の場所に http: 断った
https の往復: 200 secure /hello"

got="$("$WORK/probe" "$WORK" 2>&1)"
rc=$?

if [ -t 1 ]; then
    C_OK=$'\033[32m'; C_NG=$'\033[31m'; C_END=$'\033[0m'
else
    C_OK=''; C_NG=''; C_END=''
fi

if [ "$rc" -ne 0 ]; then
    printf "  %sFAIL%s  TLS のテスト（終了コード %s）\n" "$C_NG" "$C_END" "$rc"
    printf '%s\n' "$got" | sed 's/^/          /'
    exit 1
fi

if [ "$got" != "$want" ]; then
    printf "  %sFAIL%s  TLS のテスト（出力が違います）\n" "$C_NG" "$C_END"
    printf '          --- 期待 ---\n'
    printf '%s\n' "$want" | sed 's/^/          /'
    printf '          --- 実際 ---\n'
    printf '%s\n' "$got" | sed 's/^/          /'
    exit 1
fi

printf "  %sok%s    TLS のテスト（8 件）\n" "$C_OK" "$C_END"
rm -rf "$WORK"
exit 0
