// module.h — モジュール（= 1 ファイル）の読み込み
//
// ★ v1 の決めごと：1 ファイル = 1 モジュール = 1 名前空間 = 1 つの .ll
//
// 入口のファイルを 1 つ渡すと、import をたどって必要なファイルを全部読み、
// 「依存が先、依存する側が後」の順（トポロジカル順）に並べて返します。
// 循環 import はここで検出してエラーにします。
#ifndef PLC_MODULE_H
#define PLC_MODULE_H

#include "ast.h"

typedef struct Module Module;
struct Module {
    char *name;   // "lexer"（IR の名前修飾に使う）
    char *path;   // "…/lexer"
    char *dir;    // import を探すディレクトリ（入口ファイルのある場所）
    char *src;    // ソース（★ トークンが参照し続けるので解放しない）
    Node *ast;    // 構文解析した結果

    Module **deps;  // import しているモジュール
    int ndeps;

    char *ll_path;  // 出力する .ll のパス（main.c が決める）
    void *syms;     // 意味解析が使うシンボル表（sema.c の ModuleSyms *）

    // 深さ優先探索の状態。★ この 3 値が循環検出そのものです。
    //   0 = 未訪問 / 1 = 訪問中 / 2 = 完了
    int state;

    Module *next;  // 依存順のリスト（依存が先に来る）
};

// 追加の探索場所（-I <dir>）を登録する。★ load_modules より前に呼ぶこと。
//
// ★ パッケージマネージャが依存を置いた場所を教えるための口です。
//   優先順位は付けません。入口のディレクトリ・ここで足した場所・lib/ の
//   どれか 2 つ以上で同じ名前が見つかったらエラーにします（14.4 節と同じ）。
void module_set_search_paths(const char **dirs, int ndirs);

// 標準ライブラリ（lib/）の場所。★ 探す順番は環境変数 → 埋め込み → 実行ファイル相対。
//
// ⚠️ パッケージマネージャが「そのモジュール名が標準ライブラリと
//   ぶつかっていないか」を**入れる前に**確かめるために要ります
//   （--print-lib-dir で外から引けます）。
const char *module_lib_dir(void);

// 入口ファイルから import をたどって全モジュールを読む。
// 戻り値は依存順に並んだ先頭。*entry_out に入口モジュールを入れる。
Module *load_modules(const char *entry_path, Module **entry_out);

// dir か -I の探索場所に <name> があるか
//（「import を書き忘れていませんか」の診断用。★ lib/ は見ません）
bool module_file_exists(const char *dir, const char *name);

#endif  // PLC_MODULE_H
