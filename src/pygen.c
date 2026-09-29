// pygen.c — 外へ出す関数を Python から呼ぶための .py を作る（設計 ffi.md §6）
//
// ★ 作るのは ctypes だけを使う素の Python です。Python の C API は使いません。
//   Python の版ごとに作り直さずに済み、構築に Python のヘッダも要りません
//   （「clang だけで建つ」を保つため。ffi.md §6.1）。
//
// ★ 型の検査は **Python 側で先に**行います（C に渡る前に TypeError）。
//   int と float は境界でも混ぜません（ロードマップ ②。ffi.md §3）。
//
// 注意: 生成物に言語名を書くのは先頭の 1 行だけで、それも langinfo から取ります
//   （docs/design/naming.md）。
//
// ★ 対になる定義: selfhost/pygen（2 つの実装が同じ .py を出します）

#include "pygen.h"

#include <stdio.h>
#include <string.h>

#include "ast.h"
#include "langinfo.h"
#include "sema.h"
#include "util.h"

// Python の文字列リテラル。**必ず ASCII だけで書きます**（どのバイトも \xNN で逃がす）。
// ★ bytes にしてから decode するので、UTF-8 の並びがそのまま戻ります。
static void py_str_lit(StrBuf *b, const char *s, int len) {
    sb_printf(b, "b\"");
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') sb_printf(b, "%c", c);
        else sb_printf(b, "\\x%02x", c);
    }
    sb_printf(b, "\".decode(\"utf-8\")");
}

// 既定値を Python の式にする（リテラルだけ。A-38 がそう縛っています）
static void py_default(StrBuf *b, Node *d) {
    if (d->kind == ND_UNARY) {
        sb_printf(b, "-");
        py_default(b, d->lhs);
        return;
    }
    switch (d->kind) {
        case ND_INT: sb_printf(b, "%lld", d->ival); return;
        case ND_FLOAT: sb_printf(b, "%s", d->sval); return;
        case ND_BOOL: sb_printf(b, "%s", d->ival ? "True" : "False"); return;
        case ND_STR: py_str_lit(b, d->sval, d->slen); return;
        default: sb_printf(b, "None"); return;  // sema が通さない形
    }
}

// 引数 1 つを C に渡す形にする Python の式（名前は _a<k>）
static const char *py_conv(Type *t) {
    if (t->kind == TY_INT) return "_int";
    if (t->kind == TY_FLOAT) return "_float";
    if (t->kind == TY_BOOL) return "_bool";
    if (t->kind == TY_STR) return "_str";
    switch (t->elem->kind) {
        case TY_INT: return "_list_int";
        case TY_FLOAT: return "_list_float";
        case TY_BOOL: return "_list_bool";
        default: return "_list_str";
    }
}

// C の引数の型（ctypes）の並び
static void py_argtypes(StrBuf *b, Type *t) {
    if (t->kind == TY_INT || t->kind == TY_BOOL) sb_printf(b, "_c.c_int64, ");
    else if (t->kind == TY_FLOAT) sb_printf(b, "_c.c_double, ");
    else if (t->kind == TY_STR) sb_printf(b, "_c.c_char_p, _c.c_int64, ");
    else if (t->elem->kind == TY_FLOAT) sb_printf(b, "_c.POINTER(_c.c_double), _c.c_int64, ");
    else if (t->elem->kind == TY_STR)
        sb_printf(b, "_c.POINTER(_c.c_char_p), _c.POINTER(_c.c_int64), _c.c_int64, ");
    else sb_printf(b, "_c.POINTER(_c.c_int64), _c.c_int64, ");
}

// 戻り値を受ける箱（ctypes）
static const char *py_out_type(Type *t) {
    if (t->kind == TY_FLOAT) return "_c.c_double";
    if (t->kind == TY_STR || t->kind == TY_LIST) return "_c.c_void_p";
    return "_c.c_int64";
}

// 箱から Python の値を取り出す式
static void py_take(StrBuf *b, Type *t) {
    if (t->kind == TY_INT || t->kind == TY_FLOAT) sb_printf(b, "_out.value");
    else if (t->kind == TY_BOOL) sb_printf(b, "_out.value != 0");
    else if (t->kind == TY_STR) sb_printf(b, "_take_str(_out.value)");
    else if (t->elem->kind == TY_INT) sb_printf(b, "_take_list(_out.value, _c.c_int64, int)");
    else if (t->elem->kind == TY_FLOAT)
        sb_printf(b, "_take_list(_out.value, _c.c_double, float)");
    else if (t->elem->kind == TY_BOOL)
        sb_printf(b, "_take_list(_out.value, _c.c_int64, bool)");
    else sb_printf(b, "_take_list_str(_out.value)");
}

// ── 共通の部分（どのライブラリでも同じ）──
static const char *PY_PRELUDE =
    "import ctypes as _c\n"
    "import os as _os\n"
    "import sys as _sys\n"
    "\n"
    "_here = _os.path.dirname(_os.path.abspath(__file__))\n"
    "if _sys.platform == \"win32\":\n"
    "    _os.add_dll_directory(_here)\n"
    "_lib = _c.CDLL(_os.path.join(_here, _LIBNAME))\n"
    "\n"
    "\n"
    "class Error(Exception):\n"
    "    \"\"\"raises に書かれたエラーの共通の親\"\"\"\n"
    "\n"
    "\n"
    "class Panic(Exception):\n"
    "    \"\"\"panic（範囲外・桁あふれ・0 除算・契約違反など。プログラムの誤りです）\"\"\"\n"
    "\n"
    "\n"
    "def _sig(f, res, args):\n"
    "    f.restype = res\n"
    "    f.argtypes = args\n"
    "    return f\n"
    "\n"
    "\n"
    "_sig(_lib.pl_ffi_error_kind, _c.c_char_p, [])\n"
    "_sig(_lib.pl_ffi_error_message, _c.c_char_p, [])\n"
    "_sig(_lib.pl_ffi_str_len, _c.c_int64, [_c.c_void_p])\n"
    "_sig(_lib.pl_ffi_str_data, _c.c_void_p, [_c.c_void_p])\n"
    "_sig(_lib.pl_ffi_list_len, _c.c_int64, [_c.c_void_p])\n"
    "_sig(_lib.pl_ffi_list_data, _c.c_void_p, [_c.c_void_p])\n"
    "_sig(_lib.pl_ffi_list_str_at, _c.c_void_p, [_c.c_void_p, _c.c_int64])\n"
    "_sig(_lib.pl_ffi_free_str, None, [_c.c_void_p])\n"
    "_sig(_lib.pl_ffi_free_list, None, [_c.c_void_p])\n"
    "_sig(_lib.pl_ffi_free_list_str, None, [_c.c_void_p])\n"
    "\n"
    "_I64_MIN = -(1 << 63)\n"
    "_I64_MAX = (1 << 63) - 1\n"
    "\n"
    "\n"
    "def _fail(st, fn):\n"
    "    kind = _lib.pl_ffi_error_kind().decode(\"utf-8\")\n"
    "    msg = _lib.pl_ffi_error_message().decode(\"utf-8\", \"replace\")\n"
    "    if st == 2:\n"
    "        raise Panic(fn + \": \" + msg)\n"
    "    raise _ERRORS.get(kind, Error)(msg)\n"
    "\n"
    "\n"
    "def _what(v):\n"
    "    return type(v).__name__\n"
    "\n"
    "\n"
    "# ★ bool は int の子ですが、int としては受けません（型の取り違えを防ぐため）\n"
    "def _int(v, fn, name):\n"
    "    if type(v) is not int:\n"
    "        raise TypeError(fn + \": '\" + name + \"' は int です（\" + _what(v) + \" が渡されました）\")\n"
    "    if v < _I64_MIN or v > _I64_MAX:\n"
    "        raise OverflowError(fn + \": '\" + name + \"' が 64 ビットの int に収まりません\")\n"
    "    return (v,)\n"
    "\n"
    "\n"
    "# ★ int は float として受けません（暗黙の型変換はしない）\n"
    "def _float(v, fn, name):\n"
    "    if type(v) is not float:\n"
    "        raise TypeError(fn + \": '\" + name + \"' は float です（\" + _what(v) +\n"
    "                        \" が渡されました。float(x) にしてください）\")\n"
    "    return (v,)\n"
    "\n"
    "\n"
    "def _bool(v, fn, name):\n"
    "    if type(v) is not bool:\n"
    "        raise TypeError(fn + \": '\" + name + \"' は bool です（\" + _what(v) + \" が渡されました）\")\n"
    "    return (1 if v else 0,)\n"
    "\n"
    "\n"
    "def _str(v, fn, name):\n"
    "    if not isinstance(v, str):\n"
    "        raise TypeError(fn + \": '\" + name + \"' は str です（\" + _what(v) + \" が渡されました）\")\n"
    "    b = v.encode(\"utf-8\")\n"
    "    return (b, len(b))\n"
    "\n"
    "\n"
    "def _list(v, fn, name):\n"
    "    if not isinstance(v, list):\n"
    "        raise TypeError(fn + \": '\" + name + \"' は list です（\" + _what(v) + \" が渡されました）\")\n"
    "    return v\n"
    "\n"
    "\n"
    "def _elem(fn, name, i):\n"
    "    return name + \"[\" + str(i) + \"]\"\n"
    "\n"
    "\n"
    "def _list_int(v, fn, name):\n"
    "    xs = [_int(x, fn, _elem(fn, name, i))[0] for i, x in enumerate(_list(v, fn, name))]\n"
    "    return ((_c.c_int64 * len(xs))(*xs), len(xs))\n"
    "\n"
    "\n"
    "def _list_float(v, fn, name):\n"
    "    xs = [_float(x, fn, _elem(fn, name, i))[0] for i, x in enumerate(_list(v, fn, name))]\n"
    "    return ((_c.c_double * len(xs))(*xs), len(xs))\n"
    "\n"
    "\n"
    "def _list_bool(v, fn, name):\n"
    "    xs = [_bool(x, fn, _elem(fn, name, i))[0] for i, x in enumerate(_list(v, fn, name))]\n"
    "    return ((_c.c_int64 * len(xs))(*xs), len(xs))\n"
    "\n"
    "\n"
    "def _list_str(v, fn, name):\n"
    "    bs = [_str(x, fn, _elem(fn, name, i))[0] for i, x in enumerate(_list(v, fn, name))]\n"
    "    return ((_c.c_char_p * len(bs))(*bs), (_c.c_int64 * len(bs))(*[len(b) for b in bs]),\n"
    "            len(bs))\n"
    "\n"
    "\n"
    "# ★ 取っ手は読んだらすぐ写して解放します（Python 側に中の値を残しません）\n"
    "def _take_str(h):\n"
    "    try:\n"
    "        n = _lib.pl_ffi_str_len(h)\n"
    "        return _c.string_at(_lib.pl_ffi_str_data(h), n).decode(\"utf-8\")\n"
    "    finally:\n"
    "        _lib.pl_ffi_free_str(h)\n"
    "\n"
    "\n"
    "def _take_list(h, ctype, conv):\n"
    "    try:\n"
    "        n = _lib.pl_ffi_list_len(h)\n"
    "        if n == 0:\n"
    "            return []\n"
    "        arr = (ctype * n).from_address(_lib.pl_ffi_list_data(h))\n"
    "        return [conv(x) for x in arr]\n"
    "    finally:\n"
    "        _lib.pl_ffi_free_list(h)\n"
    "\n"
    "\n"
    "def _take_list_str(h):\n"
    "    try:\n"
    "        out = []\n"
    "        for i in range(_lib.pl_ffi_list_len(h)):\n"
    "            s = _lib.pl_ffi_list_str_at(h, i)\n"
    "            n = _lib.pl_ffi_str_len(s)\n"
    "            out.append(_c.string_at(_lib.pl_ffi_str_data(s), n).decode(\"utf-8\"))\n"
    "        return out\n"
    "    finally:\n"
    "        _lib.pl_ffi_free_list_str(h)\n";

// 宣言をそのまま見せる（docstring 用）
static void py_decl(StrBuf *b, Node *fn) {
    sb_printf(b, "extern def %s(", fn->name);
    for (Node *pm = fn->params; pm; pm = pm->next)
        sb_printf(b, "%s%s: %s%s", pm == fn->params ? "" : ", ", pm->name,
                  pm->mode == PM_OWN ? "own " : "", type_name(pm->type));
    sb_printf(b, ") -> %s", type_name(fn->type));
    for (Node *r = fn->raises; r; r = r->next)
        sb_printf(b, "%s%s", r == fn->raises ? " raises " : " | ", r->type->name);
}

static void py_func(StrBuf *b, Module *m, Node *fn) {
    char *sym = export_symbol(m->name, fn->name);
    bool has_ret = fn->type->kind != TY_NONE;

    // C の関数の形
    sb_printf(b, "\n\n_f_%s = _sig(_lib.%s, _c.c_int64, [", fn->name, sym);
    for (Node *pm = fn->params; pm; pm = pm->next) py_argtypes(b, pm->type);
    if (has_ret) sb_printf(b, "_c.POINTER(%s)", py_out_type(fn->type));
    sb_printf(b, "])\n\n\n");

    // Python の関数（引数名と既定値は宣言のまま）
    sb_printf(b, "def %s(", fn->name);
    for (Node *pm = fn->params; pm; pm = pm->next) {
        sb_printf(b, "%s%s", pm == fn->params ? "" : ", ", pm->name);
        if (pm->rhs) {
            sb_printf(b, "=");
            py_default(b, pm->rhs);
        }
    }
    sb_printf(b, "):\n    \"\"\"");
    py_decl(b, fn);
    sb_printf(b, "\"\"\"\n");
    int k = 0;
    for (Node *pm = fn->params; pm; pm = pm->next, k++)
        sb_printf(b, "    _a%d = %s(%s, \"%s\", \"%s\")\n", k, py_conv(pm->type), pm->name,
                  fn->name, pm->name);
    if (has_ret) sb_printf(b, "    _out = %s()\n", py_out_type(fn->type));
    sb_printf(b, "    _st = _f_%s(", fn->name);
    k = 0;
    for (Node *pm = fn->params; pm; pm = pm->next, k++)
        sb_printf(b, "%s*_a%d", k ? ", " : "", k);
    if (has_ret) sb_printf(b, "%s_c.byref(_out)", k ? ", " : "");
    sb_printf(b, ")\n");
    sb_printf(b, "    if _st != 0:\n        _fail(_st, \"%s\")\n", fn->name);
    if (has_ret) {
        sb_printf(b, "    return ");
        py_take(b, fn->type);
        sb_printf(b, "\n");
    }
}

char *pygen(Module *mods, const char *libname) {
    StrBuf b;
    sb_init(&b);
    sb_printf(&b, "# " PLC_LANG_CC " " PLC_LANG_VERSION
                  " が生成しました（--python）。手で書き換えないでください。\n");
    sb_printf(&b, "_LIBNAME = \"%s\"\n\n", libname);
    sb_printf(&b, "%s", PY_PRELUDE);

    // エラーのクラス（raises に出てくるものを 1 回ずつ。出てきた順）
    StrBuf errs;
    sb_init(&errs);
    const char *seen[256];
    int nseen = 0;
    for (Module *m = mods; m; m = m->next)
        for (Node *d = m->ast->body; d; d = d->next) {
            if (d->kind != ND_FUNC || !d->is_export) continue;
            for (Node *r = d->raises; r; r = r->next) {
                const char *nm = r->type->name;
                bool dup = false;
                for (int i = 0; i < nseen; i++)
                    if (strcmp(seen[i], nm) == 0) dup = true;
                if (dup || nseen == 256) continue;
                seen[nseen++] = nm;
                sb_printf(&b, "\n\nclass %s(Error):\n    pass\n", nm);
                sb_printf(&errs, "    \"%s\": %s,\n", nm, nm);
            }
        }
    sb_printf(&b, "\n\n_ERRORS = {\n%s}\n", sb_str(&errs));

    for (Module *m = mods; m; m = m->next)
        for (Node *d = m->ast->body; d; d = d->next)
            if (d->kind == ND_FUNC && d->is_export) py_func(&b, m, d);
    return sb_str(&b);
}
