// pygen.h — 外へ出す関数を Python から呼ぶための .py を作る（設計 ffi.md §6）
#ifndef PLC_PYGEN_H
#define PLC_PYGEN_H

#include "module.h"

// 全モジュールの「外へ出す関数」を呼ぶ .py の中身を返す。
// libname は隣に置く共有ライブラリのファイル名（"_stats.dylib" など）。
char *pygen(Module *mods, const char *libname);

#endif  // PLC_PYGEN_H
