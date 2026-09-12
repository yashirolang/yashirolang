// langinfo.h — 言語の「名前」と「拡張子」の唯一の定義場所
//
// ★ 言語名は将来変わるかもしれません。変わったときに直す場所を
//   ここ 1 か所（と Makefile の LANG_* / lib/langinfo）に閉じ込めます。
//   コンパイラ本体のコードは、名前の文字列を直接書かずに必ずこのマクロを使うこと。
//
// ⚠️ 内部の識別子（PLC_ 接頭辞、ランタイムの pl_ 接頭辞）は
//   **わざと言語名から独立**させてあります。改名しても変える必要はありません。
//   詳しくは docs/design/naming.md を参照。
#ifndef PLC_LANGINFO_H
#define PLC_LANGINFO_H

// Makefile が -DPLC_LANG_NAME=... などで上書きできます（既定はここ）。
#ifndef PLC_LANG_NAME
#define PLC_LANG_NAME "yashirolang"  // 人が読む言語名
#endif

#ifndef PLC_LANG_EXT
#define PLC_LANG_EXT ".ys"            // ソースファイルの拡張子（ドット込み）
#endif

#ifndef PLC_LANG_CC
#define PLC_LANG_CC "yashirolang"        // コンパイラのコマンド名
#endif

#ifndef PLC_LANG_PM
#define PLC_LANG_PM "ysm"             // パッケージマネージャのコマンド名
#endif

#ifndef PLC_LANG_VERSION
#define PLC_LANG_VERSION "0.15.2"      // 版番号（--version で出す）
#endif

// 拡張子の長さ（"." を含む）。sizeof は終端の '\0' を数えるので 1 引く。
#define PLC_LANG_EXT_LEN (sizeof(PLC_LANG_EXT) - 1)

// ── 生成する IR に書く target triple ────────────────────────
// ビルド時に Makefile が -DPLC_TARGET_TRIPLE=... で渡してきます。
// 万一渡されなかった場合でも動くようにフォールバックを置いておく。
#ifndef PLC_TARGET_TRIPLE
#define PLC_TARGET_TRIPLE ""
#endif

// ★ macOS のユニバーサルバイナリ（make UNIVERSAL=1）のための選び分け。
//
//   1 つの実行ファイルに x86_64 と arm64 の 2 つが入っているとき、
//   **書くべき triple は「いま動いている側」**です。ビルド時に 1 つだけ
//   埋め込むと、片方の機械で動かない実行ファイルを吐いてしまいます。
//   __x86_64__ / __aarch64__ は**各スライスをコンパイルするときに**
//   それぞれ定義されるので、これで正しい方が選ばれます。
#if defined(PLC_TARGET_TRIPLE_X86_64) && defined(__x86_64__)
#  undef  PLC_TARGET_TRIPLE
#  define PLC_TARGET_TRIPLE PLC_TARGET_TRIPLE_X86_64
#elif defined(PLC_TARGET_TRIPLE_ARM64) && defined(__aarch64__)
#  undef  PLC_TARGET_TRIPLE
#  define PLC_TARGET_TRIPLE PLC_TARGET_TRIPLE_ARM64
#endif

#endif  // PLC_LANGINFO_H
