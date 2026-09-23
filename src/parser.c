#include "parser.h"

#include <string.h>

#include "diag.h"

// ── パーサの状態 ────────────────────────────────────────────
// トークン配列と「今どこを見ているか」の位置だけを持ちます。
typedef struct {
    TokenVec toks;
    int pos;
    int hidden;  // 脱糖で作る隠し変数の連番
} Parser;

// ── トークン操作の基本部品 ──────────────────────────────────
// この 4 つの関数だけで、パーサ全体を書きます。

// 現在のトークンを覗く（消費しない）
static Token *peek(Parser *p) { return &p->toks.data[p->pos]; }

// n 個先のトークンを覗く（消費しない）。
//
// ★ トークンを「配列」にした理由が、ここで回収されます。
//   「x : int = 1（変数宣言）」と「x = 1（代入）」はどちらも IDENT で始まるため、
//   2 個目のトークンを見ないと区別できません。
//   配列なら O(1)、リンクリストなら辿る必要があります。
static Token *peek_at(Parser *p, int n) {
    int i = p->pos + n;
    if (i >= p->toks.len) i = p->toks.len - 1;  // EOF より先は EOF を返す
    return &p->toks.data[i];
}

// 現在のトークンを消費して返す（1 つ進む）
static Token *advance(Parser *p) {
    Token *t = peek(p);
    if (t->kind != TK_EOF) p->pos++;
    return t;
}

// 現在のトークンが指定した記号なら消費して返す。違えば NULL。
// 二項演算子のループでこれを使います。
static Token *consume(Parser *p, const char *op) {
    if (tok_is(peek(p), op)) return advance(p);
    return NULL;
}

// 現在のトークンが指定したキーワードなら消費して返す。違えば NULL。
// and / or / not は記号ではなくキーワードなので、consume() が使えません。
static Token *consume_kw(Parser *p, const char *kw) {
    if (tok_is_kw(peek(p), kw)) return advance(p);
    return NULL;
}

// 対応する閉じ記号を要求する。無ければ「開き記号はここ」を添えてエラーにする。
//
//   open  … 対応する開き記号のトークン（'(' や '[' ）
//   close … 要求する閉じ記号（")" や "]"）
//
// ★ 開き記号の位置を覚えて示すのが、この章の中心的な改善です。
//   閉じ忘れは「どこが閉じられていないか」が分からないと直せません。
//
// 引数リストの ')' でも添字の ']' でもそのまま使えます。
static Token *expect_close(Parser *p, const char *close, Token *open) {
    Token *t = peek(p);
    if (tok_is(t, close)) return advance(p);

    char *open_text = xstrndup(open->loc, (size_t)open->len);

    Diag d = {0};
    d.message = diag_fmt("閉じ括弧 '%s' がありません", close);
    d.primary.tok = t;
    d.primary.label = diag_fmt("ここに '%s' が必要です", close);
    d.related.tok = open;
    d.related.label = diag_fmt("対応する '%s' はここです", open_text);
    d.hint = "括弧の対応を確認してください";
    diag_fail(&d);
}

// トークン種別を指定して要求する。
//
// ★ 「必要になる」と予告しておいた関数です。
//   NEWLINE / INDENT / DEDENT を要求する block() のために実装しました。
//
// 注意: 仮想トークンの名前（INDENT）をそのままユーザーに見せないこと。
//    「INDENT が必要です」では利用者に意味が伝わりません。
//    what と hint には人間の言葉を渡します。
// トークン種別の日本語名。
// 注意: token_kind_name() は "INDENT" のような内部名を返すので、
//    利用者に見せる診断ではこちらを使います。
static const char *tok_kind_ja(TokenKind kind) {
    switch (kind) {
        case TK_EOF: return "ファイルの終わり";
        case TK_INT: return "整数";
        case TK_FLOAT: return "浮動小数点数";
        case TK_PUNCT: return "記号";
        case TK_IDENT: return "名前";
        case TK_KEYWORD: return "予約語";
        case TK_STR: return "文字列";
        case TK_NEWLINE: return "改行";
        case TK_INDENT: return "字下げ";
        case TK_DEDENT: return "字下げの終わり";
        default: UNREACHABLE();
    }
}

static Token *expect(Parser *p, TokenKind kind, const char *what,
                     const char *hint) {
    Token *t = peek(p);
    if (t->kind == kind) return advance(p);

    Diag d = {0};
    d.message = diag_fmt("%sが必要です", what);
    d.primary.tok = t;
    d.primary.label = diag_fmt("ここは%sです", tok_kind_ja(t->kind));
    d.hint = hint;
    diag_fail(&d);
}

// ── 文法規則（優先順位の階層）─────────────────────────────────
//
// ★ この章の中心概念。
//
// 優先順位の「弱い」演算子を上（先に呼ばれる関数）、
// 「強い」演算子を下（後から呼ばれる関数）に置くと、
// それだけで優先順位が実現されます。
//
// なぜそうなるのかは docs/spec/grammar.md 第5節に完全なトレースがあります。
//
//   expr        ::= or_expr                          弱い ↑
//   or_expr     ::= and_expr    { "or"  and_expr }
//   and_expr    ::= not_expr    { "and" not_expr }
//   not_expr    ::= "not" not_expr | comparison
//   comparison  ::= bitor_expr [ compop bitor_expr ]  （連鎖不可）
//   bitor_expr  ::= bitxor_expr { "|"  bitxor_expr }
//   bitxor_expr ::= bitand_expr { "^"  bitand_expr }
//   bitand_expr ::= shift_expr  { "&"  shift_expr }
//   shift_expr  ::= add_expr    { ("<<"|">>") add_expr }
//   add_expr    ::= mul_expr    { ("+"|"-")   mul_expr }
//   mul_expr    ::= unary       { ("*"|"/"|"//"|"%") unary }
//   unary       ::= ("-"|"+"|"~") unary | power
//   power       ::= primary [ "**" unary ]           （実装）
//   primary     ::= INT | True | False | IDENT | "(" expr ")"   強い ↓

static Node *expr(Parser *p);
static Node *call_arg(Parser *p);   // 実引数 1 つ（キーワード引数を含む。A-38）
static Node *list_comp(Parser *p, Token *open, Node *elem);
static char *hidden_name(Parser *p, const char *tag);
static Node *or_expr(Parser *p);
static Node *unary(Parser *p);
static Token *type_name_token(Parser *p, const char *what);
static Node *type_ref(Parser *p, const char *what);

// primary ::= INT | "(" expr ")"
// f"..." を連結式に脱糖する
//
//   f"a{x}b"  →  "a" + str(x) + "b"
//
// ★ **パーサだけで完結します。** 新しいノードも、意味解析の規則も、
//   コード生成の分岐も要りません（elif や複合代入と同じ「脱糖」の手）。
//
// ★ 埋め込んだ式は **その場で字句解析し直して**構文解析します。
//   こうすると f"{a + b * 2}" のような任意の式がそのまま書けます。
//
// 注意: 中身は必ず str(...) で包みます。パーサの時点では型が分からないためです
//   （str には str→str の恒等もあるので、文字列を入れても通ります）。
//
// 注意: 波括弧そのものを書きたいときは {{ }} と重ねます（Python と同じ）。
static Node *fstring(Parser *p, Token *t) {
    const char *src = t->text;
    Node *result = NULL;

    StrBuf lit;
    sb_init(&lit);
    int litlen = 0;

    // ここまでの文字列部分を 1 つの項として足す
    #define FLUSH_LIT()                                                        \
        do {                                                                   \
            if (litlen > 0) {                                                  \
                Node *sn = new_str_node(t, sb_str(&lit), litlen);              \
                result = result ? new_binop_node(t, OP_ADD, result, sn) : sn;  \
                sb_init(&lit);                                                 \
                litlen = 0;                                                    \
            }                                                                  \
        } while (0)

    for (const char *q = src; *q;) {
        if (q[0] == '{' && q[1] == '{') { sb_printf(&lit, "{"); litlen++; q += 2; continue; }
        if (q[0] == '}' && q[1] == '}') { sb_printf(&lit, "}"); litlen++; q += 2; continue; }

        if (*q == '}') {
            Diag d = {0};
            d.message = "f-string に対応しない '}' があります";
            d.primary.tok = t;
            d.primary.label = "ここです";
            d.hint = "'}' そのものを書くには '}}' と重ねてください";
            diag_fail(&d);
        }

        if (*q != '{') { sb_printf(&lit, "%c", *q); litlen++; q++; continue; }

        // ── 埋め込み式 ──
        FLUSH_LIT();
        q++;                       // '{'
        const char *ex = q;
        int depth = 1;
        while (*q && depth > 0) {
            if (*q == '{') depth++;
            else if (*q == '}') depth--;
            if (depth > 0) q++;
        }
        if (depth != 0) {
            Diag d = {0};
            d.message = "f-string の '{' が閉じられていません";
            d.primary.tok = t;
            d.primary.label = "ここです";
            diag_fail(&d);
        }
        int exlen = (int)(q - ex);
        q++;                       // '}'

        if (exlen == 0) {
            Diag d = {0};
            d.message = "f-string の '{}' が空です";
            d.primary.tok = t;
            d.primary.label = "ここに式が必要です";
            diag_fail(&d);
        }

        // 注意: 部分文字列を字句解析するので、**改行を足して**論理行を閉じます
        //   （tokenize は行の終わりに NEWLINE を要求します）。
        StrBuf sub;
        sb_init(&sub);
        sb_printf(&sub, "%.*s\n", exlen, ex);

        Parser sp = {0};
        sp.toks = tokenize(t->file, sb_str(&sub));
        sp.pos = 0;
        Node *inner = expr(&sp);

        // 注意: **式を最後まで読み切ったかを確かめます。**
        //    これが無いと f"{x:>8}" のような書式指定が「x」だけ読まれて
        //    **黙って無視され**ます。エラーにするより悪い挙動です。
        if (peek(&sp)->kind != TK_NEWLINE && peek(&sp)->kind != TK_EOF) {
            Diag d = {0};
            d.message = diag_fmt("f-string の中の式を解釈できません: '%.*s'",
                                 exlen, ex);
            d.primary.tok = t;
            d.primary.label = "ここです";
            d.hint = "書式指定（f\"{x:>8}\" のような桁揃え）はありません。"
                     "strings.lpad / strings.rpad を使ってください";
            diag_fail(&d);
        }

        // ★ 位置は f-string のトークンに揃えます。部分文字列の中の位置を
        //   そのまま出すと、元のソースに無い行番号になってしまいます。
        Node *call = new_node(ND_CALL, t);
        call->name = "str";
        call->args = inner;

        result = result ? new_binop_node(t, OP_ADD, result, call) : call;
    }
    FLUSH_LIT();
    #undef FLUSH_LIT

    // 中身が空（f""）なら空文字列
    if (!result) result = new_str_node(t, "", 0);
    return result;
}

static Node *primary(Parser *p) {
    // 括弧：優先順位を無視して中身を先に計算させる。
    // 再帰的に expr() を呼び戻すのがポイント（階層の一番上に戻る）。
    //
    // ★ 開き括弧のトークンを覚えておきます。
    //   閉じ括弧が無かったとき、エラーで「対応する '(' はここ」と示すために
    //   使います。診断の要点です。
    Token *open = consume(p, "(");
    if (open) {
        Node *n = expr(p);

        // ★ タプルのリテラル (a, b)
        //
        //   注意: **要素が 2 つ以上のときだけ**です。`(x)` は「括弧で囲んだ x」で、
        //     Python の `(1,)` にあたる 1 要素のタプルは入れていません
        //     （`(int)` と区別できないため。言語仕様 5.6）。
        //
        //   ★ 戻り値の位置と同じ節点（ND_TUPLE）を作るだけなので、
        //     意味解析も codegen も変えずに済みます。
        if (tok_is(peek(p), ",")) {
            Node *tup = new_node(ND_TUPLE, open);
            tup->body = n;
            Node *tail = n;
            while (consume(p, ",")) {
                if (tok_is(peek(p), ")")) break;   // 末尾のカンマを許す
                tail->next = expr(p);
                tail = tail->next;
            }
            expect_close(p, ")", open);
            return tup;
        }

        expect_close(p, ")", open);
        return n;
    }

    Token *t = peek(p);
    if (t->kind == TK_INT) {
        advance(p);
        return new_int_node(t, t->ival);
    }
    if (t->kind == TK_FLOAT) {
        advance(p);
        return new_float_node(t, t->text);
    }
    if (t->kind == TK_IDENT) {
        advance(p);
        return new_var_node(t, t->text);
    }
    if (t->kind == TK_STR) {
        advance(p);
        return new_str_node(t, t->text, t->slen);
    }
    if (t->kind == TK_FSTRING) {
        advance(p);
        return fstring(p, t);
    }

    // None リテラル。
    // ★ 「値を返さない」を表す型の None（-> None）とは別物です。
    //   ここで作るのは「ヌルポインタという値」で、型は TY_NULL になります。
    if (tok_is_kw(t, "None")) {
        advance(p);
        return new_node(ND_NONE, t);
    }

    // リストリテラル [1, 2, 3]。
    //
    // ★ '[' の意味は位置で決まります：式の先頭ならリテラル、
    //   式の直後（postfix）なら添字。階層化された文法の副産物で、
    //   特別な処理は要りません。
    if (tok_is(t, "[")) {
        advance(p);
        Node *n = new_node(ND_LIST, t);
        Node head = {0};
        Node *cur = &head;
        if (!tok_is(peek(p), "]")) {
            for (;;) {
                cur->next = expr(p);
                cur = cur->next;

                // ★ 内包表記 [E for x in xs if C]
                //
                //   注意: **構文解析での脱糖にはできません。** for やカンマ代入と
                //     違って、内包表記は**式の位置**に現れます。文に持ち上げると、
                //     三項演算子や and / or の右側に書かれたときに
                //     「評価されないはずのもの」を評価してしまいます。
                //     だから節点（ND_LISTCOMP）にして、意味解析と codegen が
                //     その場でループを組み立てます。
                if (cur == head.next && tok_is_kw(peek(p), "for"))
                    return list_comp(p, t, cur);

                if (!consume(p, ",")) break;
                if (tok_is(peek(p), "]")) break;  // 末尾のカンマを許す
            }
        }
        expect_close(p, "]", t);
        n->body = head.next;
        return n;
    }

    // True / False は予約語だが、式として使える（値を持つリテラル）。
    // ★ 「予約語はエラー」の判定より前に置くこと。
    //   後ろに置くと True が「'True' は予約語です」になってしまいます。
    if (tok_is_kw(t, "True")) {
        advance(p);
        return new_bool_node(t, true);
    }
    if (tok_is_kw(t, "False")) {
        advance(p);
        return new_bool_node(t, false);
    }

    // 予約語が式の位置に来た場合は、専用の説明を出す。
    // 「式が必要です」だけだと、なぜ変数名として使えないのか分かりません。
    if (t->kind == TK_KEYWORD)
        error_at_hint(t, "予約語は変数名として使えません（言語仕様 2.5）",
                      "'%s' は予約語です", t->text);

    // 「何が来るべきだったか」を具体的に伝える。
    Diag d = {0};
    d.message = "式が必要です";
    d.primary.tok = t;
    d.primary.label = t->kind == TK_EOF ? "ここでファイルが終わっています"
                                        : "ここには式が来るはずです";
    d.hint = "式とは整数リテラル、変数名、または '(' で囲んだ式のことです";
    diag_fail(&d);
}

// 内包表記
//
//   [ E for x in xs ]            → rhs = xs
//   [ E for i in range(a, b) ]   → args = range の引数
//   [ E for x in xs if C ]       → els = C
//
// ★ ここでは**形を作るだけ**です。ループ変数の型も結果の型も
//   意味解析（check_listcomp）が決めます。
//
// 注意: 隠し変数の宣言 3 つを body に並べておきます。alloca の収集
//   （codegen の collect_allocas）が body をたどるので、これで箱が用意されます。
static Node *list_comp(Parser *p, Token *open, Node *elem) {
    Token *ft = peek(p);
    advance(p);  // 'for'

    Token *var = peek(p);
    if (var->kind != TK_IDENT)
        error_at_hint(var, "内包表記は [式 for 変数 in 対象] の形で書きます",
                      "ループ変数の名前が必要です");
    advance(p);

    if (!tok_is_kw(peek(p), "in"))
        error_at_hint(peek(p), "内包表記は [式 for 変数 in 対象] の形で書きます",
                      "'in' が必要です");
    advance(p);

    Node *n = new_node(ND_LISTCOMP, open);
    n->lhs = elem;
    n->lhs->next = NULL;

    // 対象。range(...) だけは「並び」ではなく数え上げなので、別に持ちます。
    //
    // 注意: **for 文とまったく同じ規則**にします（言語仕様 5.5）。
    //   増分は整数リテラルだけです。符号が実行時に決まると、条件式が
    //   `(step>0 and i<end) or (step<0 and i>end)` になってしまうためです。
    //
    // 注意: **対象は or_expr で読みます**（expr ではありません）。expr は三項演算子
    //   まで読むので、`[x for x in xs if c]` の `if` を三項演算子の if と
    //   取り違えて「else がありません」と言い出します（Python も同じ理由で、
    //   ここは or_test までしか読みません）。
    Node *it = or_expr(p);
    if (it->kind == ND_CALL && it->name && strcmp(it->name, "range") == 0 &&
        !it->mod_name) {
        Node *a1 = it->args;
        Node *a2 = a1 ? a1->next : NULL;
        Node *a3 = a2 ? a2->next : NULL;
        if (!a1 || (a3 && a3->next))
            error_at_hint(it->tok, "range は 1〜3 個の引数を取ります",
                          "range の引数の個数が違います");
        Node *start, *stop;
        if (!a2) {
            start = new_int_node(ft, 0);
            stop = a1;
        } else {
            start = a1;
            stop = a2;
        }
        long long step = 1;
        if (a3) {
            long long sign = 1;
            Node *lit = a3;
            if (lit->kind == ND_UNARY && lit->op == OP_NEG) {
                sign = -1;
                lit = lit->lhs;
            }
            if (lit->kind != ND_INT)
                error_at_hint(a3->tok,
                              "増分は整数リテラルで書いてください"
                              "（例: range(0, 10, 2)）。変数を使いたい場合は "
                              "for 文で書けます",
                              "range の増分が定数ではありません");
            step = sign * lit->ival;
            if (step == 0)
                error_at_hint(a3->tok, "増分が 0 だと無限ループになります",
                              "range の増分に 0 は使えません");
        }
        start->next = stop;
        stop->next = NULL;
        n->args = start;
        n->ival = step;
    } else {
        n->rhs = it;
    }

    // if の条件（省略できます）
    if (tok_is_kw(peek(p), "if")) {
        advance(p);
        n->els = or_expr(p);   // 注意: ここも三項演算子まで読ませない
    }

    if (tok_is_kw(peek(p), "for"))
        error_at_hint(peek(p),
                      "for は 1 つだけです（入れ子にしたいときは、いったん"
                      "変数に入れてください）",
                      "内包表記の 'for' が 2 つあります");

    expect_close(p, "]", open);

    // 隠し宣言 3 つ：ループ変数 / 結果の list / 添字
    Node *lv = new_node(ND_VARDECL, var);
    lv->name = var->text;
    Node *res = new_node(ND_VARDECL, ft);
    res->name = hidden_name(p, "comp.res");
    Node *ix = new_node(ND_VARDECL, ft);
    ix->name = hidden_name(p, "comp.ix");
    lv->next = res;
    res->next = ix;
    n->body = lv;
    return n;
}

// postfix ::= primary { "(" [ arg_list ] ")" }
//
// ★ 呼び出しは「後置演算子」。優先順位は最も強い部類なので、
//   階層のいちばん下（primary のすぐ上）に 1 段挟みます。
//   xs[0] や p.f を足すときも、このループに追加するだけです。
static Node *postfix(Parser *p) {
    Node *n = primary(p);

    for (;;) {
        Token *open = peek(p);

        // 添字 xs[i]とスライス xs[a:b]
        //
        // ★ どちらも '[' で始まるので、区切りの ':' が出るまでは同じ形です。
        //   注意: 開始・終端はどちらも省略できます（xs[:3] / xs[2:] / xs[:]）。
        if (consume(p, "[")) {
            Node *lo = NULL;
            if (!tok_is(peek(p), ":")) lo = expr(p);

            if (consume(p, ":")) {
                Node *sl = new_node(ND_SLICE, open);
                sl->lhs = n;
                sl->rhs = lo;
                if (!tok_is(peek(p), "]")) sl->els = expr(p);
                expect_close(p, "]", open);
                n = sl;
                continue;
            }

            Node *idx = new_node(ND_INDEX, open);
            idx->lhs = n;
            idx->rhs = lo;

            // ★ 2 次元の添字 m[i, j]。
            //   注意: 添字は **next でつないだ並び**にします（引数リストと同じ形）。
            //     list[T] と str は 1 つだけです（意味解析で弾きます）。
            Node *tail = lo;
            while (consume(p, ",")) {
                tail->next = expr(p);
                tail = tail->next;
            }

            expect_close(p, "]", open);
            n = idx;
            continue;
        }

        // メソッド呼び出し xs.append(v)と
        // フィールドアクセス t.kind。
        //
        // ★ どちらも "." IDENT まで同じ形です。続きが '(' かどうかで分かれます。
        //   ループの中に分岐を 1 個足すだけなので、t.next.kind のような
        //   連鎖も自動的に通ります。
        if (consume(p, ".")) {
            Token *name_tok = peek(p);
            if (name_tok->kind != TK_IDENT)
                error_at_hint(name_tok,
                              "'.' の後にはフィールド名かメソッド名を書きます"
                              "（例: t.kind / xs.append(1)）",
                              "フィールド名かメソッド名が必要です");
            advance(p);

            Token *mopen = peek(p);
            if (!tok_is(mopen, "(")) {
                // フィールドアクセス（予告を回収）
                Node *f = new_node(ND_FIELD, name_tok);
                f->lhs = n;
                f->name = name_tok->text;
                n = f;
                continue;
            }
            advance(p);  // "("

            Node *m = new_node(ND_METHOD, name_tok);
            m->lhs = n;
            m->name = name_tok->text;

            Node mhead = {0};
            Node *mcur = &mhead;
            if (!tok_is(peek(p), ")")) {
                for (;;) {
                    mcur->next = call_arg(p);
                    mcur = mcur->next;
                    if (!consume(p, ",")) break;
                }
            }
            expect_close(p, ")", mopen);
            m->args = mhead.next;
            n = m;
            continue;
        }

        if (!consume(p, "(")) return n;

        // 呼べるのは名前だけ（第一級関数は v1 未対応）
        if (n->kind != ND_VAR) {
            Diag d = {0};
            d.message = "この式は呼び出せません";
            d.primary.tok = n->tok;
            d.primary.label = "呼び出せるのは関数の名前だけです";
            d.hint = "関数を値として扱うことは v1 では対応していません";
            diag_fail(&d);
        }

        Node *call = new_node(ND_CALL, n->tok);
        call->name = n->name;

        Node head = {0};
        Node *cur = &head;
        if (!tok_is(peek(p), ")")) {
            for (;;) {
                cur->next = call_arg(p);
                cur = cur->next;
                if (!consume(p, ",")) break;
            }
        }
        expect_close(p, ")", open);
        call->args = head.next;
        n = call;
    }
}

// 実引数を 1 つ読む（A-38）。
//
// ★ `name = 値` ならキーワード引数です。**印は `arg_name` に付けます**
//   （式そのものはふつうに読むので、木の形は今までどおり）。
//
// 注意: **2 トークン先まで見て決めます。** `x == 1` は比較で、`x = 1` は
//   キーワード引数です。`=` と `==` は別のトークンなので、ここは曖昧に
//   なりません。
//
// 注意: **並べ替えと既定値の穴埋めは sema の仕事です。** ここでは
//   「名前が付いている」ことだけを記録します（どの引数に当たるかは、
//   呼び先が決まらないと分かりません）。
static Node *call_arg(Parser *p) {
    Token *t = peek(p);
    if (t->kind == TK_IDENT && tok_is(peek_at(p, 1), "=")) {
        advance(p);   // 名前
        advance(p);   // "="
        Node *v = expr(p);
        v->arg_name = t->text;
        v->arg_name_tok = t;
        return v;
    }
    return expr(p);
}

// power ::= postfix [ "**" unary ]
//
// 注意: '**' は実行時エラーにします（負の指数を実行時エラーにするため
//    ランタイムが必要）。今は親切なメッセージを出すだけにします。
//
// ★ 検査をここに置く理由：
//    '**' は「基数を読み終えた後」に現れます。unary() の入口に置くと、
//    2 ** 10 の '**' は誰にも見られず、最終的に program() の
//    「式の後に余分なトークンがあります」になってしまいます
//    （実際にそのバグを踏みました）。
//
//    のちにこの関数はこうなります（右結合なので unary() を再帰で呼ぶ）:
//        Node *base = primary(p);
//        if (consume(p, "**"))
//            return new_binop_node(t, OP_POW, base, unary(p));
//        return base;
static Node *power(Parser *p) {
    Node *base = postfix(p);  // ★ primary から postfix に差し替え

    // ★ 上に「のちにこうなります」と書いておいたとおりの形。
    //   右結合なので、右辺は unary() を再帰で呼びます（2 ** 3 ** 2 = 2 ** 9）。
    Token *t = peek(p);
    if (consume(p, "**")) return new_binop_node(t, OP_POW, base, unary(p));

    return base;
}

// unary ::= ("-" | "+" | "~") unary | power
//
// ★ 右結合の書き方：自分自身を再帰で呼ぶ。
//    これで "- - 5" が (- (- 5)) と右から結合します。
static Node *unary(Parser *p) {
    Token *t = peek(p);

    if (consume(p, "-")) return new_unary_node(t, OP_NEG, unary(p));
    if (consume(p, "+")) return new_unary_node(t, OP_POS, unary(p));
    if (consume(p, "~")) return new_unary_node(t, OP_BITNOT, unary(p));

    return power(p);
}

// mul_expr ::= unary { ("*" | "/" | "//" | "%") unary }
//
// ★ 左結合の書き方：while ループで lhs を上書きし続ける。
//    これで "8 // 4 // 2" が ((8//4)//2) と左から結合します。
static Node *mul_expr(Parser *p) {
    Node *lhs = unary(p);
    for (;;) {
        Token *t = peek(p);
        // ★ "//" を "/" より先に判定すること。
        //    字句解析側で最長一致しているので実際は衝突しませんが、
        //    順序を意識する習慣をつけます。
        if (consume(p, "//"))     lhs = new_binop_node(t, OP_FLOORDIV, lhs, unary(p));
        else if (consume(p, "*")) lhs = new_binop_node(t, OP_MUL, lhs, unary(p));
        else if (consume(p, "/")) lhs = new_binop_node(t, OP_TRUEDIV, lhs, unary(p));
        else if (consume(p, "%")) lhs = new_binop_node(t, OP_MOD, lhs, unary(p));
        else return lhs;
    }
}

// add_expr ::= mul_expr { ("+" | "-") mul_expr }
static Node *add_expr(Parser *p) {
    Node *lhs = mul_expr(p);
    for (;;) {
        Token *t = peek(p);
        if (consume(p, "+"))      lhs = new_binop_node(t, OP_ADD, lhs, mul_expr(p));
        else if (consume(p, "-")) lhs = new_binop_node(t, OP_SUB, lhs, mul_expr(p));
        else return lhs;
    }
}

// shift_expr ::= add_expr { ("<<" | ">>") add_expr }
static Node *shift_expr(Parser *p) {
    Node *lhs = add_expr(p);
    for (;;) {
        Token *t = peek(p);
        if (consume(p, "<<"))      lhs = new_binop_node(t, OP_SHL, lhs, add_expr(p));
        else if (consume(p, ">>")) lhs = new_binop_node(t, OP_SHR, lhs, add_expr(p));
        else return lhs;
    }
}

// bitand_expr ::= shift_expr { "&" shift_expr }
static Node *bitand_expr(Parser *p) {
    Node *lhs = shift_expr(p);
    for (;;) {
        Token *t = peek(p);
        if (consume(p, "&")) lhs = new_binop_node(t, OP_BITAND, lhs, shift_expr(p));
        else return lhs;
    }
}

// bitxor_expr ::= bitand_expr { "^" bitand_expr }
static Node *bitxor_expr(Parser *p) {
    Node *lhs = bitand_expr(p);
    for (;;) {
        Token *t = peek(p);
        if (consume(p, "^")) lhs = new_binop_node(t, OP_BITXOR, lhs, bitand_expr(p));
        else return lhs;
    }
}

// bitor_expr ::= bitxor_expr { "|" bitxor_expr }
static Node *bitor_expr(Parser *p) {
    Node *lhs = bitxor_expr(p);
    for (;;) {
        Token *t = peek(p);
        if (consume(p, "|")) lhs = new_binop_node(t, OP_BITOR, lhs, bitxor_expr(p));
        else return lhs;
    }
}

// 比較演算子なら対応する OpKind を返す。違えば -1。
static int compare_op(Token *t) {
    if (tok_is(t, "==")) return OP_EQ;
    if (tok_is(t, "!=")) return OP_NE;
    if (tok_is(t, "<=")) return OP_LE;  // "<" より先に見る（最長一致の習慣）
    if (tok_is(t, ">=")) return OP_GE;
    if (tok_is(t, "<")) return OP_LT;
    if (tok_is(t, ">")) return OP_GT;
    return -1;
}

// comparison ::= bitor_expr [ compop bitor_expr ]
//
// 注意: 連鎖しません（言語仕様 4.1）。while ではなく if で書くのがポイントです。
static Node *comparison(Parser *p) {
    Node *lhs = bitor_expr(p);

    // ★ is / is not。右辺は None だけを許します（sema が検査）。
    //   一般の同一性比較にしないのは、クラスの == が既に参照比較だからです
    //   （区別を説明できない記号は増やさない）。
    Token *is_tok = peek(p);
    if (tok_is_kw(is_tok, "is")) {
        advance(p);
        OpKind op = OP_IS;
        if (consume_kw(p, "not")) op = OP_ISNOT;
        Node *rhs = bitor_expr(p);
        return new_binop_node(is_tok, op, lhs, rhs);
    }

    // ★ in / not in。比較と同じ段に置きます（Python と同じ優先度）。
    Token *in_tok = peek(p);
    if (tok_is_kw(in_tok, "in")) {
        advance(p);
        return new_binop_node(in_tok, OP_IN, lhs, bitor_expr(p));
    }
    if (tok_is_kw(in_tok, "not") && tok_is_kw(peek_at(p, 1), "in")) {
        advance(p);
        advance(p);
        return new_binop_node(in_tok, OP_NOTIN, lhs, bitor_expr(p));
    }

    Token *t = peek(p);
    int op = compare_op(t);
    if (op < 0) return lhs;  // 比較演算子がない
    advance(p);

    Node *rhs = bitor_expr(p);

    // ★ 比較の連鎖を禁止する。
    //   放っておいても (1 < 2) < 3 で「bool と int の比較」の型エラーには
    //   なりますが、なぜ bool が出てくるのか利用者には分かりません。
    //   構文の段階で捕まえて、直し方を示します。
    Token *t2 = peek(p);
    if (compare_op(t2) >= 0) {
        Diag d = {0};
        d.message = "比較演算子を連鎖させることはできません";
        d.primary.tok = t2;
        d.primary.label = "2 つ目の比較演算子です";
        d.related.tok = t;
        d.related.label = "1 つ目の比較演算子はここです";
        d.hint = "Python と違い連鎖比較は使えません。'and' で繋いでください"
                 "（例: a < b and b < c）";
        diag_fail(&d);
    }

    return new_binop_node(t, (OpKind)op, lhs, rhs);
}

// not_expr ::= "not" not_expr | comparison
//
// ★ 右結合：自分自身を再帰で呼ぶ（unary と同じ形）。
static Node *not_expr(Parser *p) {
    Token *t = peek(p);
    if (consume_kw(p, "not")) return new_unary_node(t, OP_NOT, not_expr(p));
    return comparison(p);
}

// and_expr ::= not_expr { "and" not_expr }
//
// ★ 左結合：while ループで lhs を上書きする（add_expr と同じ形）。
static Node *and_expr(Parser *p) {
    Node *lhs = not_expr(p);
    for (;;) {
        Token *t = peek(p);
        if (consume_kw(p, "and"))
            lhs = new_logical_node(t, OP_AND, lhs, not_expr(p));
        else
            return lhs;
    }
}

// or_expr ::= and_expr { "or" and_expr }
static Node *or_expr(Parser *p) {
    Node *lhs = and_expr(p);
    for (;;) {
        Token *t = peek(p);
        if (consume_kw(p, "or"))
            lhs = new_logical_node(t, OP_OR, lhs, and_expr(p));
        else
            return lhs;
    }
}

// expr ::= or_expr [ "if" or_expr "else" expr ]
//
// ★ 三項演算子。**いちばん優先度が低い**ので、or_expr の外側に
//   1 段だけ積みます。
//
// 注意: else 側は expr（自分自身）を呼ぶので **右結合**になります。
//     a if p else b if q else c  →  a if p else (b if q else c)
//   Python と同じ結合です。左結合にすると読めない式になります。
static Node *expr(Parser *p) {
    Node *lhs = or_expr(p);
    Token *t = peek(p);
    if (!tok_is_kw(t, "if")) return lhs;
    advance(p);

    Node *n = new_node(ND_COND, t);
    n->rhs = lhs;              // 条件が真のときの値（前に書く）
    n->lhs = or_expr(p);       // 条件
    if (!consume_kw(p, "else")) {
        Diag d = {0};
        d.message = "三項演算子に 'else' がありません";
        d.primary.tok = peek(p);
        d.primary.label = "ここに 'else' が必要です";
        d.hint = "書き方は 'a if 条件 else b' です（値を返す式なので、"
                 "条件が偽のときの値も必ず要ります）";
        diag_fail(&d);
    }
    n->els = expr(p);          // 偽のときの値
    return n;
}

// 論理行の終わり（NEWLINE）を要求する
static void expect_newline(Parser *p) {
    Token *t = peek(p);
    if (t->kind != TK_NEWLINE) {
        Diag d = {0};
        d.message = "文の後に余分なトークンがあります";
        d.primary.tok = t;
        d.primary.label = "ここから先が解釈できません";
        d.hint = "1 行に書けるのは 1 つの文です（改行で区切ってください）";
        diag_fail(&d);
    }
    advance(p);
}

// var_decl ::= IDENT ":" type "=" expr
//
// 型注釈は必須です（言語仕様 3.3）。
static Node *var_decl(Parser *p) {
    Token *name_tok = advance(p);  // IDENT（呼び出し元が確認済み）
    advance(p);                    // ":"

    // 型注釈。ここでは「名前を記録する」だけで、
    // それが有効な型かどうかの判断は sema に任せます。
    Node *tr = type_ref(p, "型注釈には型名を書きます（例: x: int = 0）");

    // 初期化式は必須（言語仕様 5.1：未初期化変数を作らせない）
    if (!tok_is(peek(p), "=")) {
        Diag d = {0};
        d.message = "変数宣言には初期化式が必要です";
        d.primary.tok = peek(p);
        d.primary.label = "ここに '= 初期値' が必要です";
        d.hint = "本言語では未初期化の変数を作れません（例: x: int = 0）";
        diag_fail(&d);
    }
    advance(p);  // "="

    Node *n = new_node(ND_VARDECL, name_tok);
    n->name = name_tok->text;
    n->type_ref = tr;
    n->rhs = expr(p);
    return n;
}

// 複合代入の記号なら、対応する演算子を返す。違えば -1。
static int aug_op(Token *t) {
    if (tok_is(t, "+=")) return OP_ADD;
    if (tok_is(t, "-=")) return OP_SUB;
    if (tok_is(t, "*=")) return OP_MUL;
    if (tok_is(t, "//=")) return OP_FLOORDIV;
    if (tok_is(t, "%=")) return OP_MOD;
    return -1;
}

// 脱糖で使う道具（実体は for 文のところにあります）
static char *hidden_name(Parser *p, const char *tag);
static Node *hidden_decl(Token *tok, char *name, Node *init);

// 代入先と同じ形のノードを、対象を隠し変数に差し替えて作り直す。
//
// ★ 「読み」と「書き」で 2 つ作ります。同じノードを使い回しても動きますが、
//   1 つのノードが木の 2 か所に現れるのは（sema が 2 回検査することになり）
//   後から読む人を必ず混乱させます。
// ★ 添字は 1 つとは限りません（m[i, j]）。隠し変数の名前も並びで渡します。
static Node *retarget(Node *target, char *obj, char **idx, int nidx) {
    Node *n = new_node(target->kind, target->tok);
    n->lhs = new_var_node(target->tok, obj);
    if (target->kind == ND_INDEX) {
        Node head = {0};
        Node *cur = &head;
        for (int i = 0; i < nidx; i++) {
            cur->next = new_var_node(target->tok, idx[i]);
            cur = cur->next;
        }
        n->rhs = head.next;
    } else {
        n->name = target->name;  // ND_FIELD
    }
    return n;
}

// 複合代入の脱糖（言語仕様 5.2）。
//
//   x += e      →  x = x + e
//
//   t.f += e    →  aug.obj.0 = t              ← 対象は 1 回だけ評価する
//                  aug.obj.0.f = aug.obj.0.f + e
//
//   xs[i] += e  →  aug.obj.0 = xs
//                  aug.idx.1 = i               ← 添字も 1 回だけ
//                  aug.obj.0[aug.idx.1] = aug.obj.0[aug.idx.1] + e
//
// 注意: 上に「のちに必要になる」と予告した書き換えです。実際にはクラスが入るまで
//    先送りされ、その間 xs[f()] += 1 は**コンパイラを落としていました**
//    （左辺を変数だと決め打ちして name を読んでいたため）。
static Node *aug_assign(Parser *p, Token *t, OpKind op, Node *target, Node *rhs) {
    // 変数はそのまま。2 回評価しても副作用がないので、隠し変数は要りません。
    if (target->kind == ND_VAR) {
        Node *n = new_node(ND_ASSIGN, t);
        n->lhs = target;
        n->rhs = new_binop_node(t, op, new_var_node(target->tok, target->name), rhs);
        return n;
    }

    Node head = {0};
    Node *cur = &head;

    char *obj = hidden_name(p, "aug.obj");
    cur->next = hidden_decl(target->tok, obj, target->lhs);
    cur = cur->next;

    // ★ 添字は 1 つとは限りません（m[i, j] += v）。全部を 1 回ずつ
    //   隠し変数に入れます。注意: 並びから外すときに next を切ること
    //   （切らないと、隠し変数の初期化式に隣の添字がぶら下がったままになります）。
    char *idx[8];
    int nidx = 0;
    if (target->kind == ND_INDEX) {
        for (Node *ix = target->rhs; ix;) {
            Node *nx = ix->next;
            ix->next = NULL;
            if (nidx == 8)
                error_at(target->tok, "添字が多すぎます（8 個までです）");
            idx[nidx] = hidden_name(p, "aug.idx");
            cur->next = hidden_decl(target->tok, idx[nidx], ix);
            cur = cur->next;
            nidx++;
            ix = nx;
        }
    }

    Node *asg = new_node(ND_ASSIGN, t);
    asg->lhs = retarget(target, obj, idx, nidx);  // 書き
    asg->rhs = new_binop_node(t, op, retarget(target, obj, idx, nidx), rhs);  // 読み
    cur->next = asg;

    // 隠し変数をこの文の中に閉じ込めるため、ブロックで包む（for と同じ）
    Node *blk = new_node(ND_BLOCK, t);
    blk->body = head.next;
    return blk;
}

// simple_stmt ::= var_decl | assign_stmt | expr_stmt
//
// ★ 代入文と式文の区別のしかた（docs/spec/grammar.md 第4節）
//
//   左辺を先に「式」として読み、その後に '=' が続いていたら
//   「今読んだ式は代入先だった」と解釈し直します。
//
//   こうすると xs[0] = 1 や p.f = 1にも
//   そのまま対応できます。左辺を読み切るまで代入かどうか判らないからです。
static Node *simple_stmt(Parser *p) {
    Token *t0 = peek(p);

    // break / continue / pass
    if (tok_is_kw(t0, "break")) { advance(p); return new_node(ND_BREAK, t0); }
    if (tok_is_kw(t0, "continue")) { advance(p); return new_node(ND_CONTINUE, t0); }
    if (tok_is_kw(t0, "pass")) { advance(p); return new_node(ND_PASS, t0); }

    // assert_stmt ::= "assert" expr [ "," expr ]
    //
    // ★ **パーサで脱糖します。**
    //     assert cond, msg   →   if not cond: panic(msg)
    //   新しいノードも意味解析の規則も要りません。elif や複合代入と同じ手です。
    //
    // 注意: Python の -O のような「assert を消す」切り替えは**入れません**。
    //   「本番では検査が消える」のは、事故のもとになるためです。
    if (tok_is_kw(t0, "assert")) {
        advance(p);
        Node *cond = expr(p);

        // メッセージ（省略時は位置を入れた既定の文言）
        Node *msg = NULL;
        if (tok_is(peek(p), ",")) {
            advance(p);
            msg = expr(p);
        } else {
            StrBuf sb;
            sb_init(&sb);
            sb_printf(&sb, "assertion failed: %s:%d", t0->file, t0->line);
            msg = new_str_node(t0, sb_str(&sb), (int)strlen(sb_str(&sb)));
        }

        // panic(msg) を作る
        Node *call = new_node(ND_CALL, t0);
        call->name = "panic";
        call->args = msg;

        // 注意: if の本体は **ND_BLOCK** でなければなりません。ふつうに書いた
        //    if は必ずブロックを持つので、後段（sema / codegen）はそれを
        //    前提にしています。脱糖でも同じ形にします。
        Node *body = new_node(ND_BLOCK, t0);
        body->body = call;

        Node *n = new_node(ND_IF, t0);
        n->lhs = new_unary_node(t0, OP_NOT, cond);
        n->body = body;
        return n;
    }

    // raise_stmt ::= "raise" expr
    //
    // ★ 仕様 §8 には try / except / raises しか書かれていませんでしたが、
    //   **エラーを作る側の構文**が必要です（さもないと誰もエラーを起こせません）。
    //   仕様のほうに raise を足しました。
    if (tok_is_kw(t0, "raise")) {
        advance(p);
        Node *n = new_node(ND_RAISE, t0);
        n->lhs = expr(p);
        return n;
    }

    // return_stmt ::= "return" [ expr { "," expr } ]
    //
    // ★ カンマで並べるとタプルになります（複数戻り値）。
    if (tok_is_kw(t0, "return")) {
        advance(p);
        Node *n = new_node(ND_RETURN, t0);
        // 値の有無は「次が改行か」で判断する
        if (peek(p)->kind != TK_NEWLINE) {
            Node *first = expr(p);
            if (tok_is(peek(p), ",")) {
                Node *tup = new_node(ND_TUPLE, t0);
                tup->body = first;
                Node *tail = first;
                while (consume(p, ",")) {
                    tail->next = expr(p);
                    tail = tail->next;
                }
                n->lhs = tup;
            } else {
                n->lhs = first;
            }
        }
        return n;
    }

    // IDENT の次が ':' なら変数宣言。2 トークン先読みで判別する。
    if (peek(p)->kind == TK_IDENT && tok_is(peek_at(p, 1), ":")) return var_decl(p);

    // ★ 分解代入 q, r = divmod(17, 5)
    //
    // 注意: **宣言も兼ねます**（型は右辺のタプルから決まります）。宣言と代入を
    //   分けている言語ですが、ここで型注釈を書かせると
    //     q: int, r: int = ...
    //   となって Python から離れすぎます。**右辺から決まるものは書かせない**、
    //   という判断です（roadmap.md §0 の ③）。
    if (peek(p)->kind == TK_IDENT && tok_is(peek_at(p, 1), ",")) {
        Token *ut = peek(p);
        Node *n = new_node(ND_UNPACK, ut);
        Node *tail = NULL;
        for (;;) {
            Token *nm = peek(p);
            if (nm->kind != TK_IDENT)
                error_at_hint(nm, "受け取る名前を書いてください（例: q, r = f()）",
                              "名前が必要です");
            advance(p);
            Node *v = new_node(ND_VARDECL, nm);
            v->name = nm->text;
            if (tail) tail->next = v; else n->params = v;
            tail = v;
            if (!consume(p, ",")) break;
        }
        if (!consume(p, "="))
            error_at_hint(peek(p),
                          "分解代入は 'q, r = f()' の形で書きます",
                          "'=' が必要です");
        n->rhs = expr(p);

        // ★ 右辺もカンマで並べられます（`a, b = b, a`）。
        //
        //   注意: **こちらは「代入」です**（宣言は兼ねません）。
        //     宣言も兼ねるのは `q, r = f()`（右辺がタプル 1 つ）のほうです。
        //     入れ替えに使うのが目的なので、**既にある変数**へ書きます。
        //
        //   a, b = b, a   →   swap.0 = b       ← 右辺を先に全部読む
        //                     swap.1 = a
        //                     a = swap.0
        //                     b = swap.1
        //
        //   ★ 右辺を全部読んでから書くので、入れ替えが正しく動きます。
        if (tok_is(peek(p), ",")) {
            Node vhead = {0};
            Node *vtail = &vhead;
            vtail->next = n->rhs;
            vtail = vtail->next;
            int nvals = 1;
            while (consume(p, ",")) {
                vtail->next = expr(p);
                vtail = vtail->next;
                nvals++;
            }

            int nnames = 0;
            for (Node *v = n->params; v; v = v->next) nnames++;
            if (nnames != nvals)
                error_at_hint(ut,
                              diag_fmt("左辺は %d 個、右辺は %d 個です", nnames,
                                       nvals),
                              "受け取る名前と値の数が違います");

            Node head = {0};
            Node *cur = &head;

            // ① 右辺を全部、隠し変数に入れる（読みを書きより先に済ませる）
            char *tmp[8];
            int k = 0;
            for (Node *v = vhead.next; v;) {
                Node *nx = v->next;
                v->next = NULL;
                // 注意: 上限は selfhost/parser にも同じ数を書いてあります
                //   （2 実装で同じものを受け付けるため）。
                if (k == 8)
                    error_at_hint(ut, "並べられるのは 8 個までです",
                                  "値が多すぎます");
                tmp[k] = hidden_name(p, "swap");
                cur->next = hidden_decl(ut, tmp[k], v);
                cur = cur->next;
                k++;
                v = nx;
            }

            // ② 隠し変数から、それぞれの名前へ代入する
            k = 0;
            for (Node *v = n->params; v; v = v->next, k++) {
                Node *asg = new_node(ND_ASSIGN, ut);
                asg->lhs = new_var_node(v->tok, v->name);
                asg->rhs = new_var_node(ut, tmp[k]);
                cur->next = asg;
                cur = cur->next;
            }

            Node *blk = new_node(ND_BLOCK, ut);
            blk->body = head.next;
            return blk;
        }
        return n;
    }

    Node *lhs = expr(p);
    Token *t = peek(p);

    int aug = aug_op(t);
    if (!tok_is(t, "=") && aug < 0) {
        // 式文になれるのは呼び出しだけ（docs/spec/type-system.md 6 節）。
        //
        // ★ 当初は「プログラムの値＝最後の式」だったので、裸の式を
        //   文として書けました。足場を外したので本来の厳しさに戻します。
        if (lhs->kind != ND_CALL && lhs->kind != ND_METHOD) {
            Diag d = {0};
            d.message = "この式は文として書けません";
            d.primary.tok = lhs->tok;
            d.primary.label = "計算した値がどこにも使われていません";
            d.hint = "結果を変数に代入するか、関数の呼び出しを書いてください";
            diag_fail(&d);
        }
        return lhs;  // 式文（呼び出し）
    }

    // ここから代入。左辺が代入先になれるか確認する。
    // ★ 設計どおり、条件を 1 つ足すだけで xs[0] = v に対応できました。
    //   t.kind = v も、また 1 つ足すだけです。
    if (lhs->kind != ND_VAR && lhs->kind != ND_INDEX && lhs->kind != ND_FIELD) {
        Diag d = {0};
        d.message = "この式には代入できません";
        d.primary.tok = lhs->tok;
        d.primary.label = "代入先にできるのは変数・添字 xs[i]・フィールド t.f だけです";
        d.hint = "計算結果を代入したい場合は、左辺に変数を書いてください";
        diag_fail(&d);
    }
    advance(p);  // "=" または複合代入記号

    Node *rhs = expr(p);

    // ★ 複合代入は脱糖する（言語仕様 5.2）
    //     x += e  →  x = x + e
    //
    // 注意: 左辺が「読み」と「書き」の 2 回現れます。変数なら 2 回評価しても
    //    同じですが、xs[f()] += 1 や t.g().f += 1 では f() が 2 回呼ばれます。
    //    上に「のちに必要になる」と書いた書き換えを、ここで実装します。
    if (aug >= 0) return aug_assign(p, t, (OpKind)aug, lhs, rhs);

    Node *n = new_node(ND_ASSIGN, t);
    n->lhs = lhs;
    n->rhs = rhs;
    return n;
}

// ブロックを開く ':' を要求する
static void expect_colon(Parser *p, const char *what) {
    if (consume(p, ":")) return;

    Diag d = {0};
    d.message = diag_fmt("%sの後に ':' が必要です", what);
    d.primary.tok = peek(p);
    d.primary.label = "ここに ':' が必要です";
    d.hint = "ブロックを開く行は ':' で終わり、次の行を字下げします";
    diag_fail(&d);
}

static Node *stmt(Parser *p);

// block ::= NEWLINE INDENT stmt { stmt } DEDENT
//
// ★ 字句解析器に合成させた仮想トークンを、ここで初めて消費します。
//   波括弧言語の "{" stmt { stmt } "}" とまったく同じ形になっているのが要点です。
//   オフサイドルールの複雑さは、すべて字句解析器の中に閉じ込められています。
static Node *block(Parser *p) {
    Token *head_tok = peek(p);

    expect(p, TK_NEWLINE, "改行", "':' の後は改行してブロックを字下げしてください");
    expect(p, TK_INDENT, "字下げされたブロック",
           "':' の次の行は字下げしてください（スペース 4 個を推奨）");

    Node head = {0};
    Node *cur = &head;
    // 注意: TK_EOF も終了条件に入れる。字句解析器は末尾で DEDENT を必ず出すので
    //    理屈の上では到達しませんが、入れておかないと万一のとき無限ループになります。
    while (peek(p)->kind != TK_DEDENT && peek(p)->kind != TK_EOF) {
        cur->next = stmt(p);
        cur = cur->next;
    }
    expect(p, TK_DEDENT, "ブロックの終わり", NULL);

    Node *blk = new_node(ND_BLOCK, head_tok);
    blk->body = head.next;
    return blk;
}

// if_stmt ::= "if" expr ":" block { "elif" expr ":" block } [ "else" ":" block ]
//
// ★ elif は「else の中に if が 1 個ある」形に脱糖します。
//
//     if a:   A          if a:  A
//     elif b: B    →     else:
//     else:   C              if b: B
//                            else: C
//
//   ND_ELIF のようなノードが不要になり、意味解析もコード生成も
//   「if は 2 分岐」だけを扱えば済みます。
//   複合代入（x += e → x = x + e）と同じ発想です。
static Node *if_stmt(Parser *p);

// ── 場合分け（A-37）──────────────────────────────────────
//
// match_stmt ::= "match" expr ":" NEWLINE INDENT case_clause+ DEDENT
// case_clause ::= "case" (expr | "_") ":" NEWLINE block
//
// ★ **枝は if の連なりに落とします**（codegen が上から順に比べます）。
//   新しい制御構造を IR に増やさないためです。
//
// ★ `case` は**予約語にしません**。「試験の 1 件」のような普通の名前で、
//   予約語にすると既存のコードが壊れます。`match` の本体の中で、行の
//   先頭に来たときだけ `case` として読みます（`scope` と同じ扱い）。
//
// 注意: **網羅の検査は sema の仕事です。** ここでは形だけを読みます
//   （どの枝があるかは、型が決まらないと分かりません）。
static Node *match_stmt(Parser *p) {
    Token *kw = advance(p);  // "match"

    Node *n = new_node(ND_MATCH, kw);
    n->lhs = expr(p);

    expect_colon(p, "match");
    expect(p, TK_NEWLINE, "改行", "':' の後は改行して case を字下げしてください");
    expect(p, TK_INDENT, "字下げされた case",
           "case は字下げして書きます（スペース 4 個を推奨）");

    Node head = {0};
    Node *cur = &head;
    while (peek(p)->kind != TK_DEDENT && peek(p)->kind != TK_EOF) {
        Token *c = peek(p);
        if (!(c->kind == TK_IDENT && strcmp(c->text, "case") == 0)) {
            Diag d = {0};
            d.message = "match の中に書けるのは case だけです";
            d.primary.tok = c;
            d.primary.label = "ここには case を書きます";
            d.hint = "書き方は case <値>: です（どれにも当たらないときは case _:）";
            diag_fail(&d);
        }
        advance(p);  // "case"

        Node *cn = new_node(ND_CASE, c);
        // ★ `case _` は「それ以外」。lhs を持ちません。
        //   注意: `_` は名前ではなく**印**として読みます。束縛はしません
        //     （束縛できるようにすると、中身つきの枝が入ったときに
        //     「どこまでが名前でどこからが値か」が曖昧になります）。
        Token *pat = peek(p);
        if (pat->kind == TK_IDENT && strcmp(pat->text, "_") == 0 &&
            tok_is(peek_at(p, 1), ":")) {
            advance(p);
        } else {
            cn->lhs = expr(p);
        }

        expect_colon(p, "case");
        cn->body = block(p);
        cur->next = cn;
        cur = cur->next;
    }
    expect(p, TK_DEDENT, "字下げの終わり", "match の本体が閉じていません");

    if (!head.next)
        error_at_hint(kw, "case を 1 つ以上書いてください",
                      "空の match は書けません");
    n->body = head.next;
    return n;
}

static Node *if_stmt(Parser *p) {
    // 先頭は "if"（stmt から呼ばれたとき）か "elif"（自分自身から呼ばれたとき）。
    // どちらも「条件 ':' ブロック」という同じ構造なので、同じ関数で読めます。
    Token *t = advance(p);

    Node *n = new_node(ND_IF, t);
    n->lhs = expr(p);
    expect_colon(p, "if の条件");
    n->body = block(p);

    if (tok_is_kw(peek(p), "elif")) {
        n->els = if_stmt(p);  // ★ 再帰 1 行で elif が何個でも繋がる
    } else if (consume_kw(p, "else")) {
        expect_colon(p, "'else'");
        n->els = block(p);
    }
    return n;
}

// 脱糖で作る隠し変数の名前。
//
// 注意: '.' を含むので、利用者が書ける識別子とは絶対に衝突しません。
//    ネストしても衝突しないよう連番を振ります（シャドーイング禁止のため、
//    同名だと内側の for が「外側を隠しています」というエラーになる）。
static char *hidden_name(Parser *p, const char *tag) {
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "%s.%d", tag, p->hidden++);
    return sb_str(&sb);
}

// 隠し変数の宣言（型注釈なし = 初期化式の型を使う）
static Node *hidden_decl(Token *tok, char *name, Node *init) {
    Node *n = new_node(ND_VARDECL, tok);
    n->name = name;
    n->rhs = init;
    return n;  // type_ref は NULL のまま
}

// for_stmt ::= "for" IDENT "in" expr ":" block
//
// ★ この章の主題：for は新しい機能ではなく while の書き換えです。
//   構文解析器で書き換えてしまえば、sema も codegen も for を知らずに済みます。
//   複合代入や elif と同じ「脱糖」の手
//
//     for x in xs:            for.it.0 = xs          ← 対象は 1 回だけ評価
//         BODY          →     for.ix.0: int = 0
//                             while for.ix.0 < len(for.it.0):
//                                 x = for.it.0[for.ix.0]
//                                 BODY
//                               incr: for.ix.0 += 1  ← continue の飛び先
//
// 注意: 脱糖で作るノードにはソース上の位置がないので、
//    for のトークンを流用します（全ノードが tok を持つ約束）。
// list をまわる for の脱糖（ enumerate と共用にしました）
//
//   for x in xs:            → idx_tok == NULL
//   for i, x in enumerate(xs):  → idx_tok が添字の変数
//
// ★ 違いは「隠しの添字変数を、利用者の名前でも束縛するか」だけです。
//   enumerate のために新しい仕組みを足していません。
static Node *for_over_list(Parser *p, Token *t, Node *iter, Token *var_tok,
                           Token *idx_tok, Node *body) {
    Node head = {0};
    Node *cur = &head;

    char *ix = hidden_name(p, "for.ix");

    // for.it.N = <対象>（★ 1 回だけ評価する）
    char *it = hidden_name(p, "for.it");
    cur->next = hidden_decl(t, it, iter);
    cur = cur->next;

    // for.ix.N: int = 0
    cur->next = hidden_decl(t, ix, new_int_node(t, 0));
    cur = cur->next;

    // for.ix.N < len(for.it.N)
    Node *lencall = new_node(ND_CALL, t);
    lencall->name = "len";
    lencall->args = new_var_node(t, it);
    Node *cond = new_binop_node(t, OP_LT, new_var_node(t, ix), lencall);

    // x = for.it.N[for.ix.N]
    Node *idx = new_node(ND_INDEX, t);
    idx->lhs = new_var_node(t, it);
    idx->rhs = new_var_node(t, ix);
    Node *bind = hidden_decl(t, var_tok->text, idx);

    // ★ enumerate なら、添字も利用者の名前で束縛します（i = for.ix.N）
    if (idx_tok) {
        Node *ibind = hidden_decl(t, idx_tok->text, new_var_node(t, ix));
        ibind->next = bind;
        bind = ibind;
    }

    // 本体の先頭にループ変数の束縛を差し込む
    Node *last = bind;
    while (last->next) last = last->next;
    last->next = body->body;
    body->body = bind;

    Node *inc = new_node(ND_ASSIGN, t);
    inc->lhs = new_var_node(t, ix);
    inc->rhs = new_binop_node(t, OP_ADD, new_var_node(t, ix), new_int_node(t, 1));

    Node *wh = new_node(ND_WHILE, t);
    wh->lhs = cond;
    wh->body = body;
    wh->incr = inc;

    cur->next = wh;

    Node *blk = new_node(ND_BLOCK, t);
    blk->body = head.next;
    return blk;
}

static Node *for_stmt(Parser *p) {
    Token *t = advance(p);  // "for"

    Token *var_tok = peek(p);
    if (var_tok->kind != TK_IDENT)
        error_at_hint(var_tok, "for のループ変数は名前で書きます（例: for x in xs:）",
                      "ループ変数の名前が必要です");
    advance(p);

    // ★ for i, x in enumerate(xs)
    //
    // 注意: タプルはありません。**この形だけ**を特別に認めます
    //   （2 つ目の変数は「添字」に固定。一般の分解代入ではありません）。
    Token *idx_tok = NULL;
    if (tok_is(peek(p), ",")) {
        advance(p);
        idx_tok = var_tok;          // 1 つ目が添字
        var_tok = peek(p);          // 2 つ目が要素
        if (var_tok->kind != TK_IDENT)
            error_at_hint(var_tok,
                          "書き方は 'for i, x in enumerate(xs):' です",
                          "2 つ目のループ変数の名前が必要です");
        advance(p);
    }

    if (!consume_kw(p, "in"))
        error_at_hint(peek(p), "for は「for 変数 in 対象:」の形で書きます",
                      "'in' が必要です");

    Node *iter = NULL;

    // range(...) は特別扱い（言語仕様 5.5：イテレータプロトコルは作らない）
    bool is_range = peek(p)->kind == TK_IDENT &&
                    strcmp(peek(p)->text, "range") == 0 &&
                    tok_is(peek_at(p, 1), "(");

    // ★ enumerate(xs) も同じく特別扱いです。
    //   注意: **中身を剥がすだけ**：enumerate(xs) → xs として読み、
    //     隠しの添字変数を 2 つ目のループ変数に束縛します。
    if (peek(p)->kind == TK_IDENT && strcmp(peek(p)->text, "enumerate") == 0 &&
        tok_is(peek_at(p, 1), "(")) {
        if (!idx_tok) {
            Diag d = {0};
            d.message = "enumerate には 2 つのループ変数が必要です";
            d.primary.tok = peek(p);
            d.primary.label = "添字と要素の 2 つを受け取ります";
            d.hint = "書き方は 'for i, x in enumerate(xs):' です";
            diag_fail(&d);
        }
        advance(p);                     // enumerate
        Token *eopen = advance(p);      // (
        iter = expr(p);
        expect_close(p, ")", eopen);

        expect_colon(p, "for の対象");
        Node *ebody = block(p);
        return for_over_list(p, t, iter, var_tok, idx_tok, ebody);
    }

    if (idx_tok) {
        Diag d = {0};
        d.message = "ループ変数を 2 つ書けるのは enumerate だけです";
        d.primary.tok = peek(p);
        d.primary.label = "ここには enumerate(...) が必要です";
        d.hint = "タプルはありません。'for i, x in enumerate(xs):' の形だけです";
        diag_fail(&d);
    }

    Node *start = NULL, *stop = NULL;
    long long step = 1;

    if (is_range) {
        advance(p);                 // "range"
        Token *open = advance(p);   // "("

        Node *a1 = expr(p);
        Node *a2 = NULL, *a3 = NULL;
        if (consume(p, ",")) a2 = expr(p);
        if (consume(p, ",")) a3 = expr(p);
        expect_close(p, ")", open);

        if (!a2) {
            start = new_int_node(t, 0);
            stop = a1;
        } else {
            start = a1;
            stop = a2;
        }

        if (a3) {
            // 注意: 増分は整数リテラルだけ（v1 の制限）。
            //    符号が実行時に決まると条件式が複雑になります
            //    （(step>0 and i<end) or (step<0 and i>end) を組み立てることになる）。
            //    符号がコンパイル時に分かれば '<' か '>' を選ぶだけで済みます。
            long long sign = 1;
            Node *lit = a3;
            if (lit->kind == ND_UNARY && lit->op == OP_NEG) {
                sign = -1;
                lit = lit->lhs;
            }
            if (lit->kind != ND_INT)
                error_at_hint(a3->tok,
                              "増分は整数リテラルで書いてください（例: range(0, 10, 2)）。"
                              "変数を使いたい場合は while で書けます",
                              "range の増分が定数ではありません");
            step = sign * lit->ival;
            if (step == 0)
                error_at_hint(a3->tok, "増分が 0 だと無限ループになります",
                              "range の増分に 0 は使えません");
        }
    } else {
        iter = expr(p);
    }

    expect_colon(p, "for の対象");
    Node *body = block(p);

    // ── ここから脱糖 ──
    Node head = {0};
    Node *cur = &head;

    char *ix = hidden_name(p, "for.ix");
    Node *cond = NULL;
    Node *bind = NULL;

    if (is_range) {
        // for.ix.N: int = start
        cur->next = hidden_decl(t, ix, start);
        cur = cur->next;

        // 増分の符号で条件の向きが変わる
        cond = new_binop_node(t, step > 0 ? OP_LT : OP_GT, new_var_node(t, ix), stop);

        // x = for.ix.N
        bind = hidden_decl(t, var_tok->text, new_var_node(t, ix));
    } else {
        // for.it.N = <対象>（★ 1 回だけ評価する）
        char *it = hidden_name(p, "for.it");
        cur->next = hidden_decl(t, it, iter);
        cur = cur->next;

        // for.ix.N: int = 0
        cur->next = hidden_decl(t, ix, new_int_node(t, 0));
        cur = cur->next;

        // for.ix.N < len(for.it.N)
        Node *lencall = new_node(ND_CALL, t);
        lencall->name = "len";
        lencall->args = new_var_node(t, it);
        cond = new_binop_node(t, OP_LT, new_var_node(t, ix), lencall);

        // x = for.it.N[for.ix.N]
        Node *idx = new_node(ND_INDEX, t);
        idx->lhs = new_var_node(t, it);
        idx->rhs = new_var_node(t, ix);
        bind = hidden_decl(t, var_tok->text, idx);
    }

    // 本体の先頭にループ変数の束縛を差し込む
    bind->next = body->body;
    body->body = bind;

    // for.ix.N += 1（または step）— ★ continue の飛び先になる
    Node *inc = new_node(ND_ASSIGN, t);
    inc->lhs = new_var_node(t, ix);
    inc->rhs = new_binop_node(t, OP_ADD, new_var_node(t, ix), new_int_node(t, step));

    Node *wh = new_node(ND_WHILE, t);
    wh->lhs = cond;
    wh->body = body;
    wh->incr = inc;

    cur->next = wh;

    // 隠し変数を for 文のスコープに閉じ込めるため、ブロックで包む
    Node *blk = new_node(ND_BLOCK, t);
    blk->body = head.next;
    return blk;
}

// while_stmt ::= "while" expr ":" block
static Node *while_stmt(Parser *p) {
    Token *t = advance(p);  // "while"

    Node *n = new_node(ND_WHILE, t);
    n->lhs = expr(p);
    expect_colon(p, "while の条件");
    n->body = block(p);
    return n;
}

// try_stmt ::= "try" ":" block { except_clause }
// except_clause ::= "except" type [ "as" IDENT ] ":" block
//
// ★ 見た目は Python の例外ですが、実体は戻り値の検査です
//   （docs/design/error-handling.md）。アンワインドはしません。
static Node *try_stmt(Parser *p) {
    Token *kw = advance(p);  // "try"
    expect_colon(p, "try");

    Node *n = new_node(ND_TRY, kw);
    n->body = block(p);

    Node head = {0};
    Node *cur = &head;
    while (tok_is_kw(peek(p), "except")) {
        Token *ek = advance(p);
        Node *ex = new_node(ND_EXCEPT, ek);
        ex->type_ref = type_ref(p, "except には捕まえるエラーの型名を書きます"
                                   "（例: except IOError:）");
        // as で受け取る名前（省略できる）
        if (tok_is_kw(peek(p), "as")) {
            advance(p);
            Token *nm = peek(p);
            if (nm->kind != TK_IDENT)
                error_at_hint(nm, "as の後には変数名を書きます（例: except IOError as e:）",
                              "変数名が必要です");
            advance(p);
            ex->name = nm->text;
        }
        expect_colon(p, "except");
        ex->body = block(p);
        cur->next = ex;
        cur = ex;
    }

    if (!head.next)
        error_at_hint(peek(p),
                      "try には except を 1 つ以上書きます（例: except IOError as e:）",
                      "この try には except がありません");
    n->els = head.next;
    return n;
}

// stmt ::= simple_stmt NEWLINE | if_stmt | while_stmt
//
// のちに return と def が加わります。
static Node *stmt(Parser *p) {
    Token *t = peek(p);

    if (tok_is_kw(t, "if")) return if_stmt(p);
    if (tok_is_kw(t, "while")) return while_stmt(p);
    // ★ match は v1 の時点で予約済みなので、そのまま見ます（A-37）。
    if (tok_is_kw(t, "match")) return match_stmt(p);
    if (tok_is_kw(t, "for")) return for_stmt(p);
    if (tok_is_kw(t, "try")) return try_stmt(p);

    // ── pragma 文 ──
    //
    //   pragma target "riscv64-unknown-elf"
    //
    // ★ 「このファイルはどの機械向けか」をソースに書けるようにします。
    //   コマンドラインの --target より弱く、書いてあれば既定値を上書きします。
    if (tok_is_kw(t, "pragma")) {
        Token *kw = advance(p);
        Token *name = peek(p);
        if (name->kind != TK_IDENT)
            error_at_hint(name, "pragma の後には設定名を書きます（例: pragma target \"...\"）",
                          "設定名が必要です");
        advance(p);
        Node *n = new_node(ND_PRAGMA, kw);
        n->name = name->text;
        Token *v = peek(p);
        if (v->kind == TK_STR) {
            advance(p);
            n->sval = v->text;
            n->slen = v->slen;
        }
        return n;
    }

    // ── scope: ブロック（A-18 の scoped spawn）──
    //
    // ★ **予約語にはしません。** `scope` は「範囲」を表す普通の名前で、
    //   このコンパイラ自身が `scope: rc[Scope] | None` というフィールドに
    //   使っています。予約語にすると既存のコードが壊れます。
    //
    // ★ 代わりに**文脈で決めます**：`scope :` の次が改行ならブロック、
    //   そうでなければ今までどおりの変数宣言です。3 トークン先読みで
    //   完全に分かれるので、あいまいさは残りません
    //   （Python の match / case と同じ「柔らかい予約語」の扱い）。
    //
    //     scope:                    ← ブロック
    //         t: Thread[int] = ...
    //
    //     scope: rc[Scope] | None   ← 変数宣言（今までどおり）
    // 注意: tok_is は記号（TK_PUNCT）専用なので、識別子の中身は text で比べます。
    if (peek(p)->kind == TK_IDENT && strcmp(peek(p)->text, "scope") == 0 &&
        tok_is(peek_at(p, 1), ":") && peek_at(p, 2)->kind == TK_NEWLINE) {
        Token *kw = advance(p);
        expect_colon(p, "scope");
        Node *n = new_node(ND_SCOPE, kw);
        n->body = block(p);
        return n;
    }

    // ── 契約（事前条件・事後条件。A-29）──
    //
    //   requires b != 0          ← 関数の入口で確かめる
    //   ensures result >= 0      ← return のたびに確かめる
    //
    // ★ **予約語にはしません**（`scope` と同じ「柔らかい予約語」の扱い）。
    //   `requires` も `ensures` も普通の名前として使えるままにします。
    //   区別は**次のトークン**で付けます：代入・修飾・呼び出し・添字の記号が
    //   続くなら、今までどおりの文（`requires = 3` など）です。
    if (t->kind == TK_IDENT &&
        (strcmp(t->text, "requires") == 0 || strcmp(t->text, "ensures") == 0) &&
        !tok_is(peek_at(p, 1), "=") && !tok_is(peek_at(p, 1), "+=") &&
        !tok_is(peek_at(p, 1), "-=") && !tok_is(peek_at(p, 1), "*=") &&
        !tok_is(peek_at(p, 1), "//=") && !tok_is(peek_at(p, 1), "%=") &&
        !tok_is(peek_at(p, 1), ":") && !tok_is(peek_at(p, 1), ".") &&
        !tok_is(peek_at(p, 1), "(") && !tok_is(peek_at(p, 1), "[") &&
        !tok_is(peek_at(p, 1), ",") && peek_at(p, 1)->kind != TK_NEWLINE) {
        Token *kw = advance(p);
        bool is_req = strcmp(kw->text, "requires") == 0;
        Node *n = new_node(is_req ? ND_REQUIRES : ND_ENSURES, kw);
        n->lhs = expr(p);
        expect_newline(p);
        return n;
    }

    // ── unsafe: ブロック ──
    //
    // ★ 中でだけ生ポインタを触れます（仕様 §10.1）。
    //   注意: unsafe は「借用検査を止める」ものではありません。止めるのは
    //     「ポインタ操作の禁止」だけです。
    if (tok_is_kw(t, "unsafe")) {
        Token *kw = advance(p);
        expect_colon(p, "unsafe");
        Node *n = new_node(ND_UNSAFE, kw);
        n->body = block(p);
        return n;
    }

    // 対応する if が無い elif / else。
    // 放っておいても primary() の「予約語は変数名として使えません」に
    // 捕まりますが、それでは何が悪いのか分かりません。
    if (tok_is_kw(t, "elif") || tok_is_kw(t, "else")) {
        Diag d = {0};
        d.message = diag_fmt("対応する if がない '%s' です", t->text);
        d.primary.tok = t;
        d.primary.label = "この行に対応する 'if' が見つかりません";
        d.hint = "'elif' / 'else' は 'if' と同じ字下げの位置に書いてください";
        diag_fail(&d);
    }

    Node *s = simple_stmt(p);
    expect_newline(p);
    return s;
}

// 型注釈に書ける名前を 1 つ読む。
//
// 注意: 'None' は予約語（TK_KEYWORD）なので、IDENT だけを受け付けると弾かれます。
//    型名として書ける予約語は今のところ None だけです。
static Token *type_name_token(Parser *p, const char *what) {
    Token *t = peek(p);
    if (t->kind == TK_IDENT || tok_is_kw(t, "None")) {
        advance(p);
        return t;
    }
    error_at_hint(t, what, "型名が必要です");
}

// type_ref ::= [ IDENT "." ] IDENT [ "[" type_ref "]" ]
//
// ★ 型注釈が木になりました。list[list[int]] のように入れ子になるので、
//   文字列 1 個では表せません。再帰下降なら再帰 1 行で読めます。
// ★ モジュール修飾（lexer.Token）が書けるようになりました。
static Node *type_ref(Parser *p, const char *what) {
    // ★ タプル型 (A, B)
    //   注意: **1 要素のタプルは書けません。** (int) は「括弧で囲んだ int」と
    //     区別できないためです。Python の (1,) のような記法は入れません。
    Token *tuo = peek(p);
    if (tok_is(tuo, "(")) {
        advance(p);
        Node *n = new_node(ND_TYPEREF, tuo);
        n->name = "(tuple)";
        Node *tail = NULL;
        for (;;) {
            Node *a = type_ref(p, "タプルの要素の型を書いてください");
            Node *slot = new_node(ND_TYPEREF, a->tok);
            slot->lhs = a;
            if (tail) tail->next = slot; else n->targs = slot;
            tail = slot;
            if (!consume(p, ",")) break;
        }
        expect_close(p, ")", tuo);
        int cnt = 0;
        for (Node *a = n->targs; a; a = a->next) cnt++;
        if (cnt < 2)
            error_at_hint(tuo,
                          "2 つ以上の型を書いてください（1 要素のタプルはありません）",
                          "タプルの要素が %d 個です", cnt);
        return n;
    }

    // ★ 関数型 fn(A, B) -> C
    //
    // 注意: 'fn' は **予約語にしていません。** 型の位置でだけ、識別子 "fn" の
    //    直後に '(' が来たときに関数型として読みます。予約語にすると
    //    既存のコードで fn という名前が使えなくなるためです
    //    （型を読む関数と式を読む関数が別なので、ここで迷いません）。
    Token *ft = peek(p);
    if (ft->kind == TK_IDENT && strcmp(ft->text, "fn") == 0 &&
        tok_is(peek_at(p, 1), "(")) {
        advance(p);                     // fn
        Token *open = advance(p);       // (

        Node *n = new_node(ND_TYPEREF, ft);
        n->name = "fn";

        Node *tail = NULL;
        if (!tok_is(peek(p), ")")) {
            for (;;) {
                Node *a = type_ref(p, "引数の型を書いてください");
                if (tail) tail->next = a; else n->body = a;
                tail = a;
                if (!consume(p, ",")) break;
            }
        }
        expect_close(p, ")", open);

        if (!consume(p, "->")) {
            Diag d = {0};
            d.message = "関数型には戻り型が必要です";
            d.primary.tok = peek(p);
            d.primary.label = "ここに '->' と戻り型を書いてください";
            d.hint = "書き方は fn(int, str) -> bool です";
            diag_fail(&d);
        }
        n->rhs = type_ref(p, "戻り型を書いてください");
        return n;
    }

    Token *t = type_name_token(p, what);

    Node *n = new_node(ND_TYPEREF, t);
    n->name = t->text;

    // モジュール修飾 lexer.Token（★ 1 段だけ。a.b.Token は書けない）
    if (tok_is(peek(p), ".")) {
        advance(p);
        Token *m = type_name_token(p, "モジュール修飾の後には型名を書きます"
                                      "（例: lexer.Token）");
        n->mod_name = n->name;
        n->name = m->text;
        n->tok = m;
        // ★ パッケージ（A-32）：`pkg.mod.Token` のように何段でも書けます。
        //   最後の 1 つが型名で、その手前までがモジュール名です。
        while (tok_is(peek(p), ".")) {
            advance(p);
            Token *seg = type_name_token(p, "モジュール修飾の後には型名を書きます"
                                            "（例: pkg.mod.Token）");
            StrBuf mn;
            sb_init(&mn);
            sb_printf(&mn, "%s.%s", n->mod_name, n->name);
            n->mod_name = sb_str(&mn);
            n->name = seg->text;
            n->tok = seg;
        }
    }

    Token *open = peek(p);
    if (consume(p, "[")) {
        // ★ 型引数は **複数**取れるようになりました（Dict[str, int]）。
        //   注意: 1 個目は今までどおり lhs にも入れます。list[T] / rc[T] / ptr[T]
        //     を読む側のコードを変えずに済ませるためです。
        Node *ta_tail = NULL;
        for (;;) {
            Node *a = type_ref(p, "型引数を書いてください（例: list[int]）");
            if (!n->lhs) n->lhs = a;
            Node *slot = new_node(ND_TYPEREF, a->tok);
            slot->lhs = a;
            if (ta_tail) ta_tail->next = slot; else n->targs = slot;
            ta_tail = slot;
            if (!consume(p, ",")) break;
        }
        expect_close(p, "]", open);
    }

    // ★ T | None。'|' の後ろは None だけです。
    //   型の '|' と式の '|'（ビット OR）は同じ記号ですが、
    //   型を読む関数と式を読む関数が別なので、ここでは迷いません
    //   （docs/spec/grammar.md 6 節）。
    if (tok_is(peek(p), "|")) {
        Token *bar = advance(p);
        if (!tok_is_kw(peek(p), "None"))
            error_at_hint(peek(p),
                          "型の '|' の後ろに書けるのは None だけです"
                          "（共用体型はありません）",
                          "'| None' の形で書いてください");
        advance(p);
        n->nullable = true;
        n->tok = bar;
    }
    return n;
}

// raises に書けるのは「エラー型の名前」だけ。
//
// 注意: type_ref は使えません。`raises A | B` の '|' を
//    「T | None」の '|' と読んでしまうからです。
//    **同じ記号でも、読む文脈が違えば別の文法**です（同じ判断です）。
static Node *raises_type(Parser *p) {
    Token *t = type_name_token(p, "raises にはエラーの型名を書きます"
                                 "（例: raises IOError）");
    Node *n = new_node(ND_TYPEREF, t);
    n->name = t->text;

    if (tok_is(peek(p), ".")) {  // モジュール修飾（例: errors.IOError）
        advance(p);
        Token *m = type_name_token(p, "モジュール修飾の後には型名を書きます");
        n->mod_name = n->name;
        n->name = m->text;
        n->tok = m;
    }
    return n;
}

// 既定値を 1 つ読む（A-38）。**リテラルだけ**を許します。
//
// 注意: ここで断らずに通すと、あとで「呼び出し側に置き換える」ときに
//   名前や呼び出しが紛れ込みます。入口で形を縛るのがいちばん確実です。
static Node *default_value(Parser *p, Token *pname) {
    Token *t = peek(p);

    // ★ 符号つきの数（-1 / +1）。単項マイナスは式ですが、**数のリテラルに
    //   限って**認めます（`-1` が書けないと使いものになりません）。
    if ((tok_is(t, "-") || tok_is(t, "+")) &&
        (peek_at(p, 1)->kind == TK_INT || peek_at(p, 1)->kind == TK_FLOAT)) {
        Token *sign = advance(p);
        Node *lit = primary(p);
        if (tok_is(sign, "+")) return lit;
        Node *neg = new_node(ND_UNARY, sign);
        neg->op = OP_NEG;
        neg->lhs = lit;
        return neg;
    }

    // ★ 列挙の枝（Color.Red / mod.Color.Red）。sema が定数に畳みます。
    //
    // 注意: **ここでは点で繋いだ名前だけを自分で読みます。** 式の読み取り
    //   （postfix）に任せると、`f()` や `xs[0]` まで通ってしまい、
    //   「リテラルだけ」という約束が崩れます。
    if (t->kind == TK_IDENT) {
        Node *e = new_node(ND_VAR, t);
        e->name = t->text;
        advance(p);
        while (tok_is(peek(p), ".")) {
            advance(p);
            Token *seg = peek(p);
            if (seg->kind != TK_IDENT)
                error_at_hint(seg, "'.' の後には名前を書きます",
                              "ここには名前が必要です");
            advance(p);
            Node *fld = new_node(ND_FIELD, seg);
            fld->lhs = e;
            fld->name = seg->text;
            e = fld;
        }
        // ★ `g()` や `xs[0]` は**ここで**断ります（点で繋いだ名前の続きに
        //   見えるので、黙って読み飛ばすと「名前だけ」を受け取ったことに
        //   なります）。
        if (tok_is(peek(p), "(") || tok_is(peek(p), "[")) {
            Diag d = {0};
            d.message = tok_is(peek(p), "(")
                            ? "既定値に呼び出しは書けません"
                            : "既定値に添字は書けません";
            d.primary.tok = peek(p);
            d.primary.label = "ここは実行しないと決まりません";
            d.hint = "数 / 文字列 / True / False / None / 列挙の枝 だけが書けます"
                     "（既定値は呼び出し側に置き換わるので、"
                     "実行して決まる値は書けません）";
            diag_fail(&d);
        }
        if (e->kind != ND_FIELD) {
            Diag d = {0};
            d.message = "既定値に変数は書けません";
            d.primary.tok = t;
            d.primary.label = "ここには値を書きます";
            d.hint = "数 / 文字列 / True / False / None / 列挙の枝 だけが書けます"
                     "（変えられる値を既定値にすると、呼ぶたびに答えが変わります）";
            diag_fail(&d);
        }
        return e;
    }

    if (t->kind == TK_INT || t->kind == TK_FLOAT || t->kind == TK_STR ||
        tok_is_kw(t, "True") || tok_is_kw(t, "False") || tok_is_kw(t, "None"))
        return primary(p);

    Diag d = {0};
    d.message = diag_fmt("引数 '%s' の既定値に書けるのはリテラルだけです",
                         pname->text);
    d.primary.tok = t;
    d.primary.label = "ここは実行しないと決まりません";
    d.hint = "数 / 文字列 / True / False / None / 列挙の枝 だけが書けます。"
             "list が要るときは 'xs: list[int] | None = None' と書きます"
             "（既定値の list は呼び出しをまたいで共有されるため、書けません）";
    diag_fail(&d);
    return NULL;
}

// param ::= IDENT ":" [ "own" | "mut" ] type
//          | [ "mut" ] "self"
//
// ★ 受け取り方（own / mut）を読みます。**意味づけはまだしません。**
//   own / mut は「型の前」に置きます（言語仕様 v2 §11）。
//
//   なぜ名前の前ではなく型の前なのか
//     名前の前だと `mut xs: list[int]` となり、Python の
//     「名前: 型」という並びの途中に修飾が割り込みます。型の前に置けば
//     「xs は『可変で借りた list[int]』である」と、型注釈の一部として読めます。
//     例外は self で、self には型注釈が無いので `mut self` と書きます。
//
// ★ メソッドの第 1 引数 self だけは型注釈を書きません
//   （そのクラスに決まっているので、書かせても意味がない）。
//   型は sema が入れます。「型注釈のない ND_VARDECL」と同じ抜け道で、
//   利用者が書くふつうの引数は今までどおり型注釈が必須です。
static Node *param(Parser *p, bool allow_self) {
    // 名前の前に書けるのは 'mut self' の mut だけ。
    // それ以外（mut x: T / own x: T）は「型の前に書く」と案内します。
    Token *lead = NULL;
    if (tok_is_kw(peek(p), "mut") || tok_is_kw(peek(p), "own")) lead = advance(p);

    Token *name_tok = peek(p);
    if (name_tok->kind != TK_IDENT)
        error_at_hint(name_tok, "引数は「名前: 型」の形で書きます（例: n: int）",
                      "引数名が必要です");
    advance(p);

    if (allow_self && strcmp(name_tok->text, "self") == 0 &&
        !tok_is(peek(p), ":")) {
        if (lead && tok_is_kw(lead, "own"))
            error_at_hint(lead, "self の所有権は奪えません（'mut self' なら書けます）",
                          "self に 'own' は書けません");
        Node *n = new_node(ND_PARAM, name_tok);
        n->name = name_tok->text;
        n->mode = lead ? PM_MUT : PM_BORROW;
        return n;  // type_ref は NULL のまま（sema がクラスの型を入れる）
    }

    if (lead)
        error_at_hint(lead,
                      diag_fmt("'%s' は型の前に書きます（例: %s: %s list[int]）",
                               lead->text, name_tok->text, lead->text),
                      "ここには書けません");

    if (!consume(p, ":"))
        error_at_hint(peek(p), "引数には型注釈が必須です（例: n: int）",
                      "引数名の後に ':' が必要です");

    // ★ 型の前の own / mut
    ParamMode mode = PM_BORROW;
    if (tok_is_kw(peek(p), "own")) {
        advance(p);
        mode = PM_OWN;
    } else if (tok_is_kw(peek(p), "mut")) {
        advance(p);
        mode = PM_MUT;
    }

    Node *tr = type_ref(p, "引数には型注釈が必須です（例: n: int）");

    Node *n = new_node(ND_PARAM, name_tok);
    n->name = name_tok->text;
    n->type_ref = tr;
    n->mode = mode;

    // ── 既定値（A-38）──
    //
    // ★ **書けるのはリテラルだけです**（数・文字列・True / False / None・
    //   列挙の枝・符号つきの数）。式を書けるようにしない理由は 2 つです。
    //
    //   ① **可変な既定値の罠を書けなくする。** Python の `def f(xs=[])` は、
    //      その list が**呼び出しをまたいで共有**されます。リテラルだけなら
    //      この形が書けません（list が欲しい場面は
    //      `xs: list[int] | None = None` と書きます）。
    //
    //   ② **名前をどちらで解決するかを決めずに済む。** `n: int = len(XS)` の
    //      XS は定義側の名前か、呼び出し側の名前か。既定値は**呼び出し側に
    //      置き換える**作りなので、名前が入ると答えが 2 つになります。
    if (tok_is(peek(p), "=")) {
        advance(p);
        n->rhs = default_value(p, name_tok);
    }
    return n;
}

// func_def ::= "def" IDENT "(" [ param_list ] ")" "->" type ":" block
//
// in_class … クラス本体の中か（true なら第 1 引数に self を書ける）
static Node *func_def_x(Parser *p, bool in_class, bool in_iface) {
    Token *kw = advance(p);  // "def"

    Token *name_tok = peek(p);
    if (name_tok->kind != TK_IDENT)
        error_at_hint(name_tok, "def の後には関数名を書きます（例: def f() -> int:）",
                      "関数名が必要です");
    advance(p);

    Node *n = new_node(ND_FUNC, kw);
    n->name = name_tok->text;

    // ★ 型引数 def first[T](xs: list[T]) -> T | None:
    //   注意: クラスと同じ形です（名前だけを並べる。制約は書けません）。
    Token *topen = peek(p);
    if (tok_is(topen, "[")) {
        advance(p);
        Node *tail = NULL;
        for (;;) {
            Token *tp = peek(p);
            if (tp->kind != TK_IDENT)
                error_at_hint(tp, "型引数は名前で書きます（例: def f[T](x: T) -> T:）",
                              "型引数の名前が必要です");
            advance(p);
            Node *slot = new_node(ND_TYPEREF, tp);
            slot->name = tp->text;
            if (tail) tail->next = slot; else n->targs = slot;
            tail = slot;
            if (!consume(p, ",")) break;
        }
        expect_close(p, "]", topen);
    }

    Token *open = peek(p);
    if (!consume(p, "("))
        error_at_hint(open, "関数名の後には引数リストが必要です（例: f() や f(n: int)）",
                      "'(' が必要です");

    Node head = {0};
    Node *cur = &head;
    if (!tok_is(peek(p), ")")) {
        for (;;) {
            // self を書けるのは「クラス本体の中の、第 1 引数」だけ
            cur->next = param(p, in_class && cur == &head);
            cur = cur->next;
            if (!consume(p, ",")) break;
        }
    }
    expect_close(p, ")", open);
    n->params = head.next;

    // メソッドの第 1 引数は self でなければならない（言語仕様 5.10）。
    // 注意: ここで弾いておけば、sema は「メソッドの第 1 引数は self」と仮定できます。
    if (in_class && (!n->params || strcmp(n->params->name, "self") != 0)) {
        Diag d = {0};
        d.message = diag_fmt("メソッド '%s' の第 1 引数は self でなければなりません",
                             n->name);
        d.primary.tok = n->params ? n->params->tok : name_tok;
        d.primary.label = "ここに self が必要です";
        d.hint = "本言語は self を明示的に書きます"
                 "（例: def show(self) -> None:）";
        diag_fail(&d);
    }

    // 戻り型は必須（言語仕様 3.3）。
    // 省略を許すと再帰関数で「戻り型を知るには本体が要り、
    //    本体を見るには戻り型が要る」という循環に陥ります。
    if (!consume(p, "->"))
        error_at_hint(peek(p),
                      "戻り型は省略できません。値を返さないなら -> None と書きます",
                      "'->' と戻り型が必要です");

    n->type_ref = type_ref(p, "戻り型には型名を書きます（例: -> int / -> None）");

    // ── raises 節 ──
    //
    //   def read(path: str) -> Config raises IOError:
    //   def load(path: str) -> Config raises IOError | ParseError:
    //
    // ★ 戻り型の「後ろ」に置きます（言語仕様 v2 §11）。
    //   前に置くと `-> raises IOError Config` のようになり、
    //   「何を返すのか」が読みにくくなります。
    if (tok_is_kw(peek(p), "raises")) {
        advance(p);
        Node rhead = {0};
        Node *rcur = &rhead;
        for (;;) {
            rcur->next = raises_type(p);
            rcur = rcur->next;
            if (!consume(p, "|")) break;
        }
        n->raises = rhead.next;
    }

    // ★ インタフェースの中では本体を書きません。
    //   `def show(self) -> str` で改行します（':' も本体も書かない）。
    if (in_iface) {
        if (tok_is(peek(p), ":")) {
            Diag d = {0};
            d.message = "インタフェースのメソッドに本体は書けません";
            d.primary.tok = peek(p);
            d.primary.label = "ここに ':' は書けません";
            d.hint = "インタフェースは「何ができるか」だけを並べます"
                     "（実装はクラス側に書きます）";
            diag_fail(&d);
        }
        expect_newline(p);
        return n;
    }

    expect_colon(p, "def の宣言");
    n->body = block(p);
    return n;
}

static Node *func_def(Parser *p, bool in_class) {
    return func_def_x(p, in_class, false);
}

// interface_def ::= "interface" IDENT ":" NEWLINE INDENT { def_sig } DEDENT
//
// ★ 「振る舞いだけ」を集めたものです。フィールドは持ちません
//   （持てるようにすると、それは継承になります）。
static Node *iface_def(Parser *p) {
    Token *kw = advance(p);  // "interface"

    Token *name_tok = peek(p);
    if (name_tok->kind != TK_IDENT)
        error_at_hint(name_tok,
                      "interface の後には名前を書きます（例: interface Show:）",
                      "インタフェース名が必要です");
    advance(p);

    Node *n = new_node(ND_IFACE, kw);
    n->name = name_tok->text;

    expect_colon(p, "interface の宣言");
    expect(p, TK_NEWLINE, "改行", "':' の後は改行して本体を字下げしてください");
    expect(p, TK_INDENT, "字下げされた本体",
           "インタフェースの中身は字下げして書きます（スペース 4 個を推奨）");

    Node head = {0};
    Node *cur = &head;
    while (peek(p)->kind != TK_DEDENT && peek(p)->kind != TK_EOF) {
        if (!tok_is_kw(peek(p), "def")) {
            Diag d = {0};
            d.message = "インタフェースに書けるのはメソッドの宣言だけです";
            d.primary.tok = peek(p);
            d.primary.label = "ここには def を書きます";
            d.hint = "フィールドは持てません（持てるようにすると継承になります）";
            diag_fail(&d);
        }
        cur->next = func_def_x(p, true, true);
        cur = cur->next;
    }
    expect(p, TK_DEDENT, "字下げの終わり", "インタフェースの本体が閉じていません");

    if (!head.next)
        error_at_hint(kw, "メソッドを 1 つ以上書いてください",
                      "空のインタフェースは書けません");
    n->body = head.next;
    return n;
}

// ── 列挙（A-37）──────────────────────────────────────────
//
// enum_def ::= "enum" IDENT ":" NEWLINE INDENT (IDENT NEWLINE)+ DEDENT
//
// ★ **枝は名前だけです**（中身は持ちません）。番号も書かせません——
//   宣言した順に 0 から振ります。番号を書けるようにすると
//   「番号を合わせるために枝を並べ替える」コードが生まれ、枝を足すのが
//   怖い変更になります。外との数のやり取りが要るなら、それは変換関数の
//   仕事で、型の仕事ではありません。
//
// ★ `enum` は**予約語にしません**（`scope` と同じ扱い）。ふつうの名前
//   としても使えるので、`enum: int = 0` のようなグローバル変数を
//   壊さないためです。`enum` の次が名前なら宣言、と文脈で決めます。
static Node *enum_def(Parser *p) {
    Token *kw = advance(p);  // "enum"

    Token *name_tok = peek(p);
    if (name_tok->kind != TK_IDENT)
        error_at_hint(name_tok,
                      "enum の後には名前を書きます（例: enum Color:）",
                      "列挙の名前が必要です");
    advance(p);

    Node *n = new_node(ND_ENUM, kw);
    n->name = name_tok->text;

    expect_colon(p, "enum の宣言");
    expect(p, TK_NEWLINE, "改行", "':' の後は改行して本体を字下げしてください");
    expect(p, TK_INDENT, "字下げされた本体",
           "列挙の枝は字下げして書きます（スペース 4 個を推奨）");

    Node head = {0};
    Node *cur = &head;
    long long next_val = 0;
    while (peek(p)->kind != TK_DEDENT && peek(p)->kind != TK_EOF) {
        Token *v = peek(p);
        if (v->kind != TK_IDENT) {
            Diag d = {0};
            d.message = "列挙に書けるのは枝の名前だけです";
            d.primary.tok = v;
            d.primary.label = "ここには名前を書きます";
            d.hint = "1 行に 1 つ、名前だけを書きます（中身を持つ枝はまだありません）";
            diag_fail(&d);
        }
        advance(p);

        // 注意: **同じ名前の枝を断ります。** 通すと、あとに書いたほうが
        //   決して選ばれない match ができ、それが黙って通ります。
        for (Node *q = head.next; q; q = q->next)
            if (strcmp(q->name, v->text) == 0) {
                Diag d = {0};
                d.message = diag_fmt("枝 '%s' が 2 回あります", v->text);
                d.primary.tok = v;
                d.primary.label = "2 つめの定義です";
                d.related.tok = q->tok;
                d.related.label = "最初の定義はここです";
                diag_fail(&d);
            }

        Node *ev = new_node(ND_ENUMVAL, v);
        ev->name = v->text;
        ev->ival = next_val++;
        cur->next = ev;
        cur = cur->next;
        expect(p, TK_NEWLINE, "改行", "枝は 1 行に 1 つ書きます");
    }
    expect(p, TK_DEDENT, "字下げの終わり", "列挙の本体が閉じていません");

    if (!head.next)
        error_at_hint(kw, "枝を 1 つ以上書いてください",
                      "空の列挙は書けません");
    n->body = head.next;
    return n;
}

// field_decl ::= IDENT ":" type NEWLINE
//
// ★ 変数宣言（var_decl）とよく似ていますが、初期化式を取りません。
//   フィールドの初期値は init メソッドで決めます（12.6 節）。
static Node *field_decl(Parser *p) {
    Token *name_tok = advance(p);  // IDENT（呼び出し元が確認済み）
    advance(p);                    // ":"

    Node *n = new_node(ND_FIELDDECL, name_tok);
    n->name = name_tok->text;
    n->type_ref = type_ref(p, "フィールドには型注釈が必須です（例: kind: int）");

    if (tok_is(peek(p), "=")) {
        Diag d = {0};
        d.message = "フィールドに初期値は書けません";
        d.primary.tok = peek(p);
        d.primary.label = "ここに '=' は書けません";
        d.hint = "初期値は init メソッドで代入してください"
                 "（例: def init(self) -> None: / self.kind = 0）";
        diag_fail(&d);
    }
    expect_newline(p);
    return n;
}

// class_def ::= "class" IDENT ":" NEWLINE INDENT { field_decl } { func_def } DEDENT
//
// ★ block() を使わないのは、クラス本体が「文の列」ではなく「宣言の列」だからです
//   （トップレベルと同じ性質）。だから字下げの処理も自前で書きます。
static Node *class_def(Parser *p) {
    Token *kw = advance(p);  // "class"

    Token *name_tok = peek(p);
    if (name_tok->kind != TK_IDENT)
        error_at_hint(name_tok, "class の後にはクラス名を書きます（例: class Token:）",
                      "クラス名が必要です");
    advance(p);

    Node *n = new_node(ND_CLASS, kw);
    n->name = name_tok->text;

    // ★ 型引数 class Dict[K, V]:
    //   注意: 名前だけを並べます（制約は書けません。design/generics-and-interfaces.md）。
    Token *topen = peek(p);
    if (consume(p, "[")) {
        Node *tail = NULL;
        for (;;) {
            Token *tp = peek(p);
            if (tp->kind != TK_IDENT)
                error_at_hint(tp, "型引数は名前で書きます（例: class Dict[K, V]:）",
                              "型引数の名前が必要です");
            advance(p);
            Node *slot = new_node(ND_TYPEREF, tp);
            slot->name = tp->text;
            if (tail) tail->next = slot; else n->targs = slot;
            tail = slot;
            if (!consume(p, ",")) break;
        }
        expect_close(p, "]", topen);
    }

    // ★ 実装するインタフェース class Point(Show):
    //   注意: **継承ではありません。** 書けるのはインタフェース名だけで、
    //     フィールドも実装も受け継ぎません（design/generics-and-interfaces.md §2）。
    Token *iopen = peek(p);
    if (consume(p, "(")) {
        Node *tail = NULL;
        for (;;) {
            Token *it = peek(p);
            if (it->kind != TK_IDENT)
                error_at_hint(it,
                              "class の '(' に書けるのはインタフェース名です"
                              "（継承はありません）",
                              "インタフェース名が必要です");
            advance(p);
            Node *slot = new_node(ND_TYPEREF, it);
            slot->name = it->text;
            // モジュール修飾（shapes.Drawable）
            if (tok_is(peek(p), ".")) {
                advance(p);
                Token *m = peek(p);
                if (m->kind != TK_IDENT)
                    error_at_hint(m, "モジュール修飾の後には名前を書きます",
                                  "インタフェース名が必要です");
                advance(p);
                slot->mod_name = slot->name;
                slot->name = m->text;
                slot->tok = m;
            }
            if (tail) tail->next = slot; else n->ifaces = slot;
            tail = slot;
            if (!consume(p, ",")) break;
        }
        expect_close(p, ")", iopen);
    }

    expect_colon(p, "class の宣言");
    expect(p, TK_NEWLINE, "改行", "':' の後は改行してクラス本体を字下げしてください");
    expect(p, TK_INDENT, "字下げされたクラス本体",
           "クラスの中身は字下げして書きます（スペース 4 個を推奨）");

    // ★ フィールドとメソッドを 1 本のリストにまとめます。
    //   program() がトップレベルで def とグローバル変数を混ぜているのと同じ形です。
    Node head = {0};
    Node *cur = &head;
    Node *first_method = NULL;

    while (peek(p)->kind != TK_DEDENT && peek(p)->kind != TK_EOF) {
        Token *t = peek(p);

        if (tok_is_kw(t, "def")) {
            cur->next = func_def(p, true);  // ★ true = self を書ける
            cur = cur->next;
            if (!first_method) first_method = cur;
            continue;
        }

        if (t->kind == TK_IDENT && tok_is(peek_at(p, 1), ":")) {
            // 注意: フィールドはメソッドより先（文法がそう決めている）。
            //    レイアウトを確定してからメソッドを型検査したいためです。
            if (first_method) {
                Diag d = {0};
                d.message = "フィールドはメソッドより前に書いてください";
                d.primary.tok = t;
                d.primary.label = "このフィールド宣言がメソッドより後ろにあります";
                d.related.tok = first_method->tok;
                d.related.label = "最初のメソッドはここです";
                d.hint = "クラス本体は「フィールドを全部 → メソッドを全部」の順です"
                         "（文法定義 3 節）";
                diag_fail(&d);
            }
            cur->next = field_decl(p);
            cur = cur->next;
            continue;
        }

        Diag d = {0};
        d.message = "クラスの中に書けるのはフィールドとメソッドだけです";
        d.primary.tok = t;
        d.primary.label = "ここには書けません";
        d.hint = "フィールドは「名前: 型」、メソッドは 'def' で始めます:\n"
                 "             class Token:\n"
                 "                 kind: int\n"
                 "\n"
                 "                 def show(self) -> None:\n"
                 "                     print(self.kind)";
        diag_fail(&d);
    }
    expect(p, TK_DEDENT, "クラス本体の終わり", NULL);

    n->body = head.next;
    return n;
}

// program ::= { class_def | func_def | global_var | NEWLINE } EOF
//
// ★ トップレベルは「宣言だけ」です（言語仕様 6.3）。
//   実行文は書けません。プログラムの入口は def main() -> int: です。
// extern_def ::= "extern" "def" IDENT "(" [ param_list ] ")" "->" type NEWLINE
//
// ★ 新しいノード種別は作りません。ND_FUNC の body が NULL——
//   「宣言はあるが定義がない」という意味そのものです。
static Node *extern_def(Parser *p) {
    advance(p);  // "extern"

    if (!tok_is_kw(peek(p), "def"))
        error_at_hint(peek(p), "extern の後には def を書きます"
                               "（例: extern def pl_system(cmd: str) -> int）",
                      "extern の後に def が必要です");
    advance(p);  // "def"

    Token *name_tok = peek(p);
    if (name_tok->kind != TK_IDENT)
        error_at_hint(name_tok, "extern def の後には C の関数名を書きます",
                      "関数名が必要です");
    advance(p);

    Token *open = peek(p);
    if (!consume(p, "("))
        error_at_hint(peek(p), "引数リストを書いてください（例: (path: str)）",
                      "'(' が必要です");

    Node head = {0};
    Node *cur = &head;
    if (!tok_is(peek(p), ")")) {
        for (;;) {
            cur->next = param(p, false);
            cur = cur->next;
            if (!consume(p, ",")) break;
        }
    }
    expect_close(p, ")", open);

    if (!consume(p, "->"))
        error_at_hint(peek(p), "戻り型を書いてください（例: -> int）",
                      "'->' が必要です");

    Node *n = new_node(ND_FUNC, name_tok);
    n->name = name_tok->text;
    n->params = head.next;
    n->type_ref = type_ref(p, "戻り型を書いてください（例: -> int）");

    // 注意: 本体は読みません。':' を書いていたらここで気づけるようにします。
    if (tok_is(peek(p), ":"))
        error_at_hint(peek(p), "extern 宣言は本体を持ちません（改行で終わります）",
                      "extern def に ':' は書けません");
    expect_newline(p);

    n->body = NULL;  // ★ extern の印
    return n;
}

// import_stmt ::= "import" IDENT NEWLINE
//
// ★ 新しいノード種別はこの 1 つだけです。
//   式の側（lexer.make(1) / lexer.MAX）はpostfix() がそのまま読みます。
//   「'.' の左がモジュールかどうか」は名前解決の話なので sema が決めます（13.3 節）。
static Node *import_stmt(Parser *p) {
    Token *kw = advance(p);  // "import"

    Token *name_tok = peek(p);
    if (name_tok->kind != TK_IDENT)
        error_at_hint(name_tok,
                      "import の後にはモジュール名を書きます（例: import lexer）",
                      "モジュール名が必要です");
    advance(p);

    // ★ パッケージ（A-32）：`import pkg.mod` と書けます。
    //
    //   名前は**ドットのまま**持ちます（"pkg.mod"）。使うときも
    //   `pkg.mod.f()` と**全部書きます**。短い名前で束ねると、
    //   標準ライブラリと同じ名前のパッケージがまた使えなくなるためです。
    //   ファイルは `pkg/mod` を探します（module.c が変換します）。
    StrBuf full;
    sb_init(&full);
    sb_printf(&full, "%s", name_tok->text);
    while (tok_is(peek(p), ".")) {
        advance(p);
        Token *seg = peek(p);
        if (seg->kind != TK_IDENT)
            error_at_hint(seg, "ドットの後にはモジュール名を書きます"
                               "（例: import pkg.mod）",
                          "モジュール名が必要です");
        advance(p);
        sb_printf(&full, ".%s", seg->text);
    }

    Node *n = new_node(ND_IMPORT, kw);
    n->name = sb_str(&full);
    expect_newline(p);
    return n;
}

// 範囲の端（整数のリテラル。先頭の '-' を許す）。
//
// 注意: **式ではありません。** 端は「コンパイル時に決まっている 2 つの数」で
//   なければならないので、ここでは足し算も名前も受け取りません。
static Node *int_literal(Parser *p, const char *what) {
    Token *t = peek(p);
    bool neg = false;
    if (tok_is(t, "-")) {
        advance(p);
        neg = true;
        t = peek(p);
    }
    if (t->kind != TK_INT) {
        Diag d = {0};
        d.message = "ここには整数のリテラルが必要です";
        d.primary.tok = t;
        d.primary.label = what;
        d.hint = "範囲の端はコンパイル時に決まっている必要があります"
                 "（例: type Percent = int range(0, 100)）";
        diag_fail(&d);
    }
    advance(p);
    return new_int_node(t, neg ? -t->ival : t->ival);
}

// 範囲型（部分型）の宣言。A-28。
//
//   type Percent = int range(0, 100)
//
// ★ 'type' も 'range' も **予約語にしていません**（'fn' と同じ扱い）。
//   トップレベルで「IDENT 'type' の次が IDENT で、その次が '='」のときだけ
//   この規則に入ります。`type: int = 0` のようなグローバル変数や、
//   `type` という名前のフィールドは今までどおり書けます
//   （selfhost/ast{{ext}} の Node.type がまさにそれです）。
static Node *range_decl(Parser *p) {
    advance(p);                      // "type"（呼び出し元が確認済み）
    Token *name_tok = advance(p);    // 部分型の名前
    advance(p);                      // "="

    // 基底の型。いまは int だけです。
    Token *base = type_name_token(p, "範囲型の基底は int です（例: int range(0, 100)）");
    if (strcmp(base->text, "int") != 0)
        error_at_hint(base, "いま範囲を付けられるのは int だけです",
                      "'%s' には範囲を付けられません", base->text);

    Token *rw = peek(p);
    if (rw->kind != TK_IDENT || strcmp(rw->text, "range") != 0) {
        Diag d = {0};
        d.message = "範囲型には range(下端, 上端) が必要です";
        d.primary.tok = rw;
        d.primary.label = "ここに range(...) を書いてください";
        d.hint = "書き方は type Percent = int range(0, 100) です（両端を含みます）";
        diag_fail(&d);
    }
    advance(p);                      // "range"
    Token *open = peek(p);
    if (!consume(p, "("))
        error_at_hint(open, "range の後には '(' が必要です",
                      "ここに '(' を書いてください");

    Node *lo = int_literal(p, "下端には整数のリテラルを書いてください");
    if (!consume(p, ","))
        error_at_hint(peek(p), "下端と上端はカンマで区切ります（例: range(0, 100)）",
                      "ここに ',' が必要です");
    Node *hi = int_literal(p, "上端には整数のリテラルを書いてください");
    expect_close(p, ")", open);

    if (lo->ival > hi->ival)
        error_at_hint(name_tok, "下端は上端以下でなければなりません",
                      "range(%lld, %lld) は空の範囲です", lo->ival, hi->ival);

    Node *n = new_node(ND_RANGEDECL, name_tok);
    n->name = name_tok->text;
    n->lhs = lo;
    n->rhs = hi;
    return n;
}

static Node *program(Parser *p) {
    // ★ 「ダミーの先頭ノード」を使うと、リスト構築が分岐なしで書けます。
    //   head.next が最初の要素になり、「空かどうか」の場合分けが消えます。
    Node head = {0};
    Node *cur = &head;

    Token *first = peek(p);

    while (peek(p)->kind != TK_EOF) {
        Token *t = peek(p);

        if (t->kind == TK_INDENT) {
            Diag d = {0};
            d.message = "予期しないインデントです";
            d.primary.tok = t;
            d.primary.label = "この行が余分に字下げされています";
            d.hint = "トップレベルに書けるのは def / class / import / extern とグローバル変数だけです"
                     "（言語仕様 6.3）";
            diag_fail(&d);
        }
        if (t->kind == TK_DEDENT) {
            // 字句解析器の不整合。ユーザーのミスでは起こり得ない。
            UNREACHABLE();
        }

        if (tok_is_kw(t, "def")) {
            cur->next = func_def(p, false);
            cur = cur->next;
            continue;
        }

        if (tok_is_kw(t, "class")) {
            cur->next = class_def(p);
            cur = cur->next;
            continue;
        }

        if (tok_is_kw(t, "interface")) {
            cur->next = iface_def(p);
            cur = cur->next;
            continue;
        }

        if (tok_is_kw(t, "import")) {
            cur->next = import_stmt(p);
            cur = cur->next;
            continue;
        }

        if (tok_is_kw(t, "extern")) {
            cur->next = extern_def(p);
            cur = cur->next;
            continue;
        }

        // ★ pragma はトップレベルに書く「設定」です（実行文ではない）
        if (tok_is_kw(t, "pragma")) {
            cur->next = stmt(p);
            cur = cur->next;
            expect_newline(p);
            continue;
        }

        // 列挙 ::= "enum" IDENT ":" NEWLINE ...
        //
        // 注意: **2 つ先まで見てから決めます。** 'enum' は予約語ではないので、
        //    `enum: int = 0`（グローバル変数）と区別が要ります。
        //    宣言なら次が名前、変数なら次が ':' です。
        if (t->kind == TK_IDENT && strcmp(t->text, "enum") == 0 &&
            peek_at(p, 1)->kind == TK_IDENT) {
            cur->next = enum_def(p);
            cur = cur->next;
            continue;
        }

        // 範囲型 ::= "type" IDENT "=" int range "(" INT "," INT ")" NEWLINE
        //
        // 注意: **3 つ先まで見てから決めます。** 'type' は予約語ではないので、
        //    `type: int = 0`（グローバル変数）と区別が要ります。
        if (t->kind == TK_IDENT && strcmp(t->text, "type") == 0 &&
            peek_at(p, 1)->kind == TK_IDENT && tok_is(peek_at(p, 2), "=")) {
            cur->next = range_decl(p);
            cur = cur->next;
            expect_newline(p);
            continue;
        }

        // グローバル変数 ::= IDENT ":" type "=" expr NEWLINE
        if (t->kind == TK_IDENT && tok_is(peek_at(p, 1), ":")) {
            cur->next = var_decl(p);
            cur = cur->next;
            expect_newline(p);
            continue;
        }

        // それ以外はトップレベルに書けない
        Diag d = {0};
        d.message = "トップレベルに実行文は書けません";
        d.primary.tok = t;
        d.primary.label = "ここに書けるのは def / class / import / extern とグローバル変数だけです";
        d.hint = "処理は main の中に書いてください:\n"
                 "             def main() -> int:\n"
                 "                 ...\n"
                 "                 return 0";
        diag_fail(&d);
    }

    Node *blk = new_node(ND_BLOCK, first);
    blk->body = head.next;
    return blk;
}

// ── 入口 ───────────────────────────────────────────────────

Node *parse(TokenVec toks) {
    Parser p = {.toks = toks, .pos = 0, .hidden = 0};
    return program(&p);
}
