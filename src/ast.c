#include "ast.h"

#include <stdio.h>

// インタフェース・メソッドの総数（sema が数え、codegen が読む）
int pl_iface_slots = 0;

Node *new_node(NodeKind kind, Token *tok) {
    Node *n = xmalloc(sizeof(Node));  // calloc なので他のフィールドは 0 / NULL
    n->kind = kind;
    n->tok = tok;
    return n;
}

Node *new_int_node(Token *tok, long long value) {
    Node *n = new_node(ND_INT, tok);
    n->ival = value;
    return n;
}

// ★ float は **文字列**で持ちます（値ではありません）。理由は src/lexer.c の
//   read_number を参照。sval には正規化済みの LLVM リテラルが入ります。
Node *new_float_node(Token *tok, char *text) {
    Node *n = new_node(ND_FLOAT, tok);
    n->sval = text;
    return n;
}

Node *new_bool_node(Token *tok, bool value) {
    Node *n = new_node(ND_BOOL, tok);
    n->ival = value ? 1 : 0;
    return n;
}

Node *new_str_node(Token *tok, char *bytes, int len) {
    Node *n = new_node(ND_STR, tok);
    n->sval = bytes;
    n->slen = len;
    return n;
}

Node *new_binop_node(Token *tok, OpKind op, Node *lhs, Node *rhs) {
    Node *n = new_node(ND_BINOP, tok);
    n->op = op;
    n->lhs = lhs;
    n->rhs = rhs;
    return n;
}

// and / or 専用。形は二項演算と同じだが、コード生成が違うので種別を分ける。
Node *new_logical_node(Token *tok, OpKind op, Node *lhs, Node *rhs) {
    Node *n = new_node(ND_LOGICAL, tok);
    n->op = op;
    n->lhs = lhs;
    n->rhs = rhs;
    return n;
}

Node *new_var_node(Token *tok, char *name) {
    Node *n = new_node(ND_VAR, tok);
    n->name = name;
    return n;
}

Node *new_unary_node(Token *tok, OpKind op, Node *operand) {
    Node *n = new_node(ND_UNARY, tok);
    n->op = op;
    n->lhs = operand;  // 単項演算は lhs だけを使う
    return n;
}

const char *op_symbol(OpKind op) {
    switch (op) {
        case OP_ADD: return "+";
        case OP_SUB: return "-";
        case OP_MUL: return "*";
        case OP_TRUEDIV: return "/";
        case OP_FLOORDIV: return "//";
        case OP_MOD: return "%";
        case OP_BITAND: return "&";
        case OP_BITOR: return "|";
        case OP_BITXOR: return "^";
        case OP_SHL: return "<<";
        case OP_SHR: return ">>";
        case OP_POW: return "**";
        case OP_EQ: return "==";
        case OP_NE: return "!=";
        case OP_LT: return "<";
        case OP_LE: return "<=";
        case OP_GT: return ">";
        case OP_GE: return ">=";
        case OP_AND: return "and";
        case OP_OR: return "or";
        case OP_IS: return "is";
        case OP_ISNOT: return "is not";
        case OP_IN: return "in";
        case OP_NOTIN: return "not in";
        case OP_NEG: return "-";
        case OP_POS: return "+";
        case OP_BITNOT: return "~";
        case OP_NOT: return "not";
        default: UNREACHABLE();
    }
}

// ── S 式ダンプ ─────────────────────────────────────────────
// ノード種別が増えるたびに、ここに case を足します。
// インデント付きで出力するので、深い木でも構造が読めます。

// 仮引数の受け取り方の接頭辞。
// ★ 既定（借用）は何も出しません。既存の出力を変えないためです。
const char *param_mode_prefix(ParamMode mode) {
    if (mode == PM_OWN) return "own ";
    if (mode == PM_MUT) return "mut ";
    return "";
}

static void dump(Node *n, int depth);

// 実引数を 1 つ出す（A-38）。
//
// ★ 名前で渡した引数は **(kwarg 名前 …)** で包みます。包まないと
//   `f(1)` と `f(a = 1)` の木が同じに見えて、--dump-ast を突き合わせる
//   セルフホストの検証が「名前を落とす実装」を見逃します。
static void dump_arg(Node *a, int depth) {
    if (!a->arg_name) {
        dump(a, depth);
        return;
    }
    for (int i = 0; i < depth; i++) printf("  ");
    printf("(kwarg %s\n", a->arg_name);
    dump(a, depth + 1);
    for (int i = 0; i < depth; i++) printf("  ");
    printf(")\n");
}

static void dump(Node *n, int depth) {
    for (int i = 0; i < depth; i++) printf("  ");

    if (!n) {
        printf("(nil)\n");
        return;
    }

    switch (n->kind) {
        case ND_INT:
            printf("(int %lld)\n", n->ival);
            break;
        case ND_BOOL:
            printf("(bool %s)\n", n->ival ? "True" : "False");
            break;
        case ND_STR:
            printf("(str %d bytes)\n", n->slen);
            break;
        case ND_LOGICAL:
            printf("(logical %s\n", op_symbol(n->op));
            dump(n->lhs, depth + 1);
            dump(n->rhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_BINOP:
            printf("(binop %s\n", op_symbol(n->op));
            dump(n->lhs, depth + 1);
            dump(n->rhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_UNARY:
            printf("(unary %s\n", op_symbol(n->op));
            dump(n->lhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_NONE:
            printf("(none)\n");
            break;

        case ND_VAR:
            printf("(var %s)\n", n->name);
            break;
        case ND_TYPEREF:
            if (!n->lhs) { printf("(type %s)\n", n->name); break; }
            printf("(type %s\n", n->name);
            dump(n->lhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_LIST:
            printf("(list\n");
            for (Node *el = n->body; el; el = el->next) dump(el, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_INDEX:
            printf("(index\n");
            dump(n->lhs, depth + 1);
            dump(n->rhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_METHOD:
            printf("(method %s\n", n->name);
            dump(n->lhs, depth + 1);
            for (Node *a = n->args; a; a = a->next) dump_arg(a, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_FIELD:
            printf("(field %s\n", n->name);
            dump(n->lhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_CLASS:
            printf("(class %s", n->name);
            for (Node *i = n->ifaces; i; i = i->next)
                printf(" :%s", i->name);
            printf("\n");
            for (Node *m = n->body; m; m = m->next) dump(m, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_FIELDDECL:
            printf("(fielddecl %s\n", n->name);
            dump(n->type_ref, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_VARDECL:
            printf("(vardecl %s\n", n->name);
            dump(n->type_ref, depth + 1);
            dump(n->rhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_ASSIGN:
            printf("(assign\n");
            dump(n->lhs, depth + 1);
            dump(n->rhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_IF:
            printf("(if\n");
            dump(n->lhs, depth + 1);
            dump(n->body, depth + 1);
            if (n->els) dump(n->els, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_WHILE:
            printf("(while\n");
            dump(n->lhs, depth + 1);
            dump(n->body, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_BREAK:
            printf("(break)\n");
            break;
        case ND_CONTINUE:
            printf("(continue)\n");
            break;
        case ND_PASS:
            printf("(pass)\n");
            break;
        case ND_PRINT:
            printf("(print\n");
            dump(n->lhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        // ── エラー処理 ──
        case ND_TRY:
            printf("(try\n");
            dump(n->body, depth + 1);
            for (Node *ex = n->els; ex; ex = ex->next) dump(ex, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_PRAGMA:
            printf("(pragma %s)\n", n->name);
            break;
        // ★ 範囲型（A-28）。dump は 2 実装で 1 文字も違ってはいけません。
        case ND_RANGEDECL:
            printf("(rangedecl %s %lld %lld)\n", n->name, n->lhs->ival,
                   n->rhs->ival);
            break;
        // ★ 契約（A-29）。dump は 2 実装で 1 文字も違ってはいけません。
        case ND_REQUIRES:
            printf("(requires\n");
            dump(n->lhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_ENSURES:
            printf("(ensures\n");
            dump(n->lhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_SCOPE:
            printf("(scope\n");
            dump(n->body, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;

        // ★ 列挙と場合分け（A-37）。dump は 2 実装で 1 文字も違ってはいけません。
        case ND_ENUM:
            printf("(enum %s\n", n->name);
            for (Node *v = n->body; v; v = v->next) dump(v, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_ENUMVAL:
            printf("(enumval %s %lld)\n", n->name, n->ival);
            break;
        case ND_MATCH:
            printf("(match\n");
            dump(n->lhs, depth + 1);
            for (Node *c = n->body; c; c = c->next) dump(c, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_CASE:
            // 注意: `case _` は調べる式を持ちません（lhs が NULL）。
            printf("(case\n");
            if (n->lhs) dump(n->lhs, depth + 1);
            dump(n->body, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;

        case ND_UNSAFE:
            printf("(unsafe\n");
            dump(n->body, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_RAISE:
            printf("(raise\n");
            dump(n->lhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_EXCEPT:
            printf("(except %s\n", n->name ? n->name : "-");
            dump(n->type_ref, depth + 1);
            dump(n->body, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_FUNC:
            printf("(func %s\n", n->name);
            dump(n->type_ref, depth + 1);
            // ★ raises 節（無ければ何も出さない。既存の AST 比較を壊さないため）
            for (Node *r = n->raises; r; r = r->next) {
                for (int i = 0; i < depth + 1; i++) printf("  ");
                printf("(raises\n");
                dump(r, depth + 2);
                for (int i = 0; i < depth + 1; i++) printf("  ");
                printf(")\n");
            }
            for (Node *pm = n->params; pm; pm = pm->next) dump(pm, depth + 1);
            dump(n->body, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_PARAM:
            printf("(param %s%s\n", param_mode_prefix(n->mode), n->name);
            dump(n->type_ref, depth + 1);
            // ★ 既定値（A-38）。無ければ何も出しません（既存の比較を壊さないため）
            if (n->rhs) {
                for (int i = 0; i < depth + 1; i++) printf("  ");
                printf("(default\n");
                dump(n->rhs, depth + 2);
                for (int i = 0; i < depth + 1; i++) printf("  ");
                printf(")\n");
            }
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_CALL:
            printf("(call %s\n", n->name);
            for (Node *a = n->args; a; a = a->next) dump_arg(a, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_RETURN:
            if (!n->lhs) { printf("(return)\n"); break; }
            printf("(return\n");
            dump(n->lhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        // ★ 正規化済みの文字列をそのまま出します
        //   （selfhost/ast の dump と同じ形にすること）
        case ND_FLOAT:
            printf("(float %s)\n", n->sval);
            break;
        // ★ 三項演算子とスライス。省略された端は (nil) になります。
        case ND_COND:
            printf("(cond\n");
            dump(n->lhs, depth + 1);
            dump(n->rhs, depth + 1);
            dump(n->els, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_SLICE:
            printf("(slice\n");
            dump(n->lhs, depth + 1);
            dump(n->rhs, depth + 1);
            dump(n->els, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        // ★ タプルと分解代入
        case ND_TUPLE:
            printf("(tuple\n");
            for (Node *x = n->body; x; x = x->next) dump(x, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_UNPACK:
            printf("(unpack");
            for (Node *v = n->params; v; v = v->next) printf(" %s", v->name);
            printf("\n");
            dump(n->rhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        // ★ インタフェース（本体の無い def の並び）
        case ND_IFACE:
            printf("(interface %s\n", n->name);
            for (Node *m = n->body; m; m = m->next) dump(m, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_IMPORT:
            printf("(import %s)\n", n->name);
            break;

        // ★ 内包表記 [E for x in xs if C]
        //   body の 3 つは隠し宣言なので出しません（読む人の役に立たないため）。
        case ND_LISTCOMP:
            printf("(listcomp %s\n", n->body->name);
            dump(n->lhs, depth + 1);
            if (n->args) {
                for (int i = 0; i < depth + 1; i++) printf("  ");
                printf("(range step=%lld\n", n->ival);
                for (Node *a = n->args; a; a = a->next) dump(a, depth + 2);
                for (int i = 0; i < depth + 1; i++) printf("  ");
                printf(")\n");
            } else {
                dump(n->rhs, depth + 1);
            }
            if (n->els) dump(n->els, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;

        // ★ for 文（A-39）。sema が while に書き換える前の形です
        //   （--dump-ast は構文解析の直後に出すので、ここを通ります）。
        case ND_FOREACH:
            printf("(foreach %s\n", n->name);
            dump(n->lhs, depth + 1);
            dump(n->body, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        // ★ 中身を持つ枝のタグ（A-41）
        case ND_ENUMTAG:
            printf("(enumtag\n");
            dump(n->lhs, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        case ND_BLOCK:
            printf("(block\n");
            for (Node *s = n->body; s; s = s->next) dump(s, depth + 1);
            for (int i = 0; i < depth; i++) printf("  ");
            printf(")\n");
            break;
        default:
            UNREACHABLE();
    }
}

void dump_ast(Node *node) { dump(node, 0); }


// ── AST の深い複製（ジェネリクスの単相化で使う） ───────────
//
// ★ 単相化は「テンプレートの木を丸ごと複製して、型引数を置き換える」だけです。
//   置き換え自体は sema が「型引数の束縛」で行うので、ここでは**複製だけ**を
//   担当します。木を作り直すので、実体ごとに別の ir_name / type を持てます。
//
// 注意: **sema が埋める欄（type / ir_name / cls / builtin）は写しません。**
//   写すと「テンプレートを検査したときの結果」が実体に混入します。
//   複製した木は、まっさらな状態から検査し直します。
Node *ast_clone(Node *n) {
    if (!n) return NULL;

    Node *c = new_node(n->kind, n->tok);
    c->ival = n->ival;
    c->sval = n->sval;
    c->slen = n->slen;
    c->op = n->op;
    c->name = n->name;
    c->mod_name = n->mod_name;
    c->nullable = n->nullable;
    c->mode = n->mode;
    c->is_global = n->is_global;
    // ★ 畳んだ列挙の枝（A-37）。**これを写さないと、複製した既定値が
    //   ただの int に戻ります**（A-38 でそれを踏みました）。
    c->en = n->en;
    c->arg_name = n->arg_name;
    c->arg_name_tok = n->arg_name_tok;
    // ★ for の隠し変数（A-39）。
    //   注意: **同じ名前のまま複製します。** 実体ごとに別の関数になるので
    //     名前がぶつかることはありません（ジェネリクスの単相化）。
    c->hid_cur = n->hid_cur;
    c->hid_obj = n->hid_obj;
    c->is_lambda = n->is_lambda;   // A-42

    c->lhs = ast_clone(n->lhs);
    c->rhs = ast_clone(n->rhs);
    c->els = ast_clone(n->els);
    c->body = ast_clone(n->body);
    c->args = ast_clone(n->args);
    c->params = ast_clone(n->params);
    c->type_ref = ast_clone(n->type_ref);
    c->targs = ast_clone(n->targs);
    c->incr = ast_clone(n->incr);
    c->next = ast_clone(n->next);
    return c;
}
