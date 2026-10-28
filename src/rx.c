#include "rx.h"
#include "textutil.h"
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#define NOCAP ((size_t)-1)
#define STEP_LIMIT 400000
#define MAX_PROG 20000

typedef struct { wchar_t lo, hi; } Range;
typedef struct { Range *r; int n, cap; int neg; } Class;

typedef enum { N_EMPTY, N_CHAR, N_ANY, N_CLASS, N_CAT, N_ALT, N_REP, N_GROUP, N_BOL, N_EOL, N_WB, N_NWB } NType;
typedef struct Node { NType t; wchar_t c; int cls; struct Node *a, *b; int min, max, greedy, group; } Node;

typedef enum { I_CHAR, I_ANY, I_CLASS, I_SPLIT, I_JMP, I_SAVE, I_BOL, I_EOL, I_WB, I_NWB, I_MATCH } IOp;
typedef struct { IOp op; int x, y; wchar_t c; } Inst;

struct Rx {
    Inst *prog; int n, cap;
    Class *cls; int ncls, clscap;
    int ngroups, icase;
};

typedef struct {
    const wchar_t *p; size_t pos, len;
    Rx *rx;
    wchar_t *err; size_t errcap;
    int failed;
    Node **pool; int npool, poolcap;
} Parser;

static void fail(Parser *ps, const wchar_t *msg)
{
    if (!ps->failed && ps->err && ps->errcap) {
        wcsncpy(ps->err, msg, ps->errcap - 1);
        ps->err[ps->errcap - 1] = 0;
    }
    ps->failed = 1;
}

static Node *mk(Parser *ps, NType t)
{
    Node *n = (Node *)calloc(1, sizeof *n);
    if (!n) { fail(ps, L"Out of memory."); return NULL; }
    n->t = t; n->cls = -1; n->greedy = 1; n->max = -1;
    if (ps->npool == ps->poolcap) {
        int nc = ps->poolcap ? ps->poolcap * 2 : 64;
        Node **p = (Node **)realloc(ps->pool, (size_t)nc * sizeof *p);
        if (!p) { free(n); fail(ps, L"Out of memory."); return NULL; }
        ps->pool = p; ps->poolcap = nc;
    }
    ps->pool[ps->npool++] = n;
    return n;
}

static int new_class(Parser *ps)
{
    Rx *r = ps->rx;
    if (r->ncls == r->clscap) {
        int nc = r->clscap ? r->clscap * 2 : 8;
        Class *p = (Class *)realloc(r->cls, (size_t)nc * sizeof *p);
        if (!p) { fail(ps, L"Out of memory."); return -1; }
        r->cls = p; r->clscap = nc;
    }
    memset(&r->cls[r->ncls], 0, sizeof(Class));
    return r->ncls++;
}

static void cls_add(Parser *ps, Class *c, wchar_t lo, wchar_t hi)
{
    if (c->n == c->cap) {
        int nc = c->cap ? c->cap * 2 : 8;
        Range *p = (Range *)realloc(c->r, (size_t)nc * sizeof *p);
        if (!p) { fail(ps, L"Out of memory."); return; }
        c->r = p; c->cap = nc;
    }
    c->r[c->n].lo = lo; c->r[c->n].hi = hi; c->n++;
}

/* \d \w \s as ranges (ASCII plus letters via the Unicode-aware helper at match time is overkill: keep it explicit) */
static void add_escape_class(Parser *ps, Class *c, wchar_t e)
{
    switch (e) {
    case 'd': cls_add(ps, c, '0', '9'); break;
    case 'w': cls_add(ps, c, '0', '9'); cls_add(ps, c, 'a', 'z'); cls_add(ps, c, 'A', 'Z'); cls_add(ps, c, '_', '_');
              cls_add(ps, c, 0xC0, 0x24F); cls_add(ps, c, 0x370, 0x1FFF); break;
    case 's': cls_add(ps, c, ' ', ' '); cls_add(ps, c, '\t', '\r'); cls_add(ps, c, 0xA0, 0xA0); break;
    }
}

static wchar_t simple_escape(wchar_t e)
{
    switch (e) { case 't': return '\t'; case 'n': return '\n'; case 'r': return '\r'; case 'f': return '\f'; case 'v': return '\v'; }
    return e;
}

static Node *parse_alt(Parser *ps);

static Node *parse_class(Parser *ps)
{
    int ci = new_class(ps);
    if (ci < 0) return NULL;
    Class *c = &ps->rx->cls[ci];
    if (ps->pos < ps->len && ps->p[ps->pos] == '^') { c->neg = 1; ps->pos++; }
    int first = 1;
    while (!ps->failed) {
        if (ps->pos >= ps->len) { fail(ps, L"Missing ] in the pattern."); return NULL; }
        wchar_t ch = ps->p[ps->pos++];
        if (ch == ']' && !first) break;
        first = 0;
        if (ch == '\\') {
            if (ps->pos >= ps->len) { fail(ps, L"Pattern ends with a backslash."); return NULL; }
            wchar_t e = ps->p[ps->pos++];
            if (e == 'd' || e == 'w' || e == 's') { add_escape_class(ps, c, e); continue; }
            if (e == 'D' || e == 'W' || e == 'S') { fail(ps, L"\\D, \\W and \\S are not supported inside [ ]."); return NULL; }
            ch = simple_escape(e);
        }
        wchar_t hi = ch;
        if (ps->pos + 1 < ps->len && ps->p[ps->pos] == '-' && ps->p[ps->pos + 1] != ']') {
            ps->pos++;
            hi = ps->p[ps->pos++];
            if (hi == '\\' && ps->pos < ps->len) hi = simple_escape(ps->p[ps->pos++]);
            if (hi < ch) { fail(ps, L"A range in [ ] is backwards."); return NULL; }
        }
        cls_add(ps, c, ch, hi);
    }
    Node *n = mk(ps, N_CLASS);
    if (n) n->cls = ci;
    return n;
}

static Node *parse_atom(Parser *ps)
{
    if (ps->pos >= ps->len) return mk(ps, N_EMPTY);
    wchar_t ch = ps->p[ps->pos++];
    Node *n;
    switch (ch) {
    case '(': {
        int grp = -1;
        if (ps->pos + 1 < ps->len && ps->p[ps->pos] == '?' && ps->p[ps->pos + 1] == ':') ps->pos += 2;
        else grp = ++ps->rx->ngroups;
        Node *inner = parse_alt(ps);
        if (ps->failed) return NULL;
        if (ps->pos >= ps->len || ps->p[ps->pos] != ')') { fail(ps, L"Missing ) in the pattern."); return NULL; }
        ps->pos++;
        if (grp < 0) return inner;
        n = mk(ps, N_GROUP);
        if (n) { n->a = inner; n->group = grp; }
        return n;
    }
    case '[': return parse_class(ps);
    case '.': return mk(ps, N_ANY);
    case '^': return mk(ps, N_BOL);
    case '$': return mk(ps, N_EOL);
    case '*': case '+': case '?': fail(ps, L"A quantifier has nothing before it."); return NULL;
    case '\\': {
        if (ps->pos >= ps->len) { fail(ps, L"Pattern ends with a backslash."); return NULL; }
        wchar_t e = ps->p[ps->pos++];
        if (e == 'b') return mk(ps, N_WB);
        if (e == 'B') return mk(ps, N_NWB);
        if (e == 'd' || e == 'w' || e == 's' || e == 'D' || e == 'W' || e == 'S') {
            int ci = new_class(ps);
            if (ci < 0) return NULL;
            Class *c = &ps->rx->cls[ci];
            wchar_t lower = (wchar_t)towlower(e);
            add_escape_class(ps, c, lower);
            c->neg = (e != lower);
            n = mk(ps, N_CLASS);
            if (n) n->cls = ci;
            return n;
        }
        n = mk(ps, N_CHAR);
        if (n) n->c = simple_escape(e);
        return n;
    }
    default:
        n = mk(ps, N_CHAR);
        if (n) n->c = ch;
        return n;
    }
}

static int parse_int(Parser *ps, int *out)
{
    int any = 0, v = 0;
    while (ps->pos < ps->len && ps->p[ps->pos] >= '0' && ps->p[ps->pos] <= '9') {
        v = v * 10 + (ps->p[ps->pos++] - '0');
        if (v > 1000) { fail(ps, L"A repeat count is too large (limit 1000)."); return 0; }
        any = 1;
    }
    *out = v;
    return any;
}

static Node *parse_rep(Parser *ps)
{
    Node *a = parse_atom(ps);
    while (a && !ps->failed && ps->pos < ps->len) {
        wchar_t q = ps->p[ps->pos];
        int mn, mx;
        if (q == '*') { mn = 0; mx = -1; ps->pos++; }
        else if (q == '+') { mn = 1; mx = -1; ps->pos++; }
        else if (q == '?') { mn = 0; mx = 1; ps->pos++; }
        else if (q == '{') {
            size_t save = ps->pos;
            ps->pos++;
            int lo, hi;
            if (!parse_int(ps, &lo)) { ps->pos = save; break; }       /* a literal { */
            hi = lo;
            if (ps->pos < ps->len && ps->p[ps->pos] == ',') {
                ps->pos++;
                if (!parse_int(ps, &hi)) hi = -1;
            }
            if (ps->pos >= ps->len || ps->p[ps->pos] != '}') { ps->pos = save; break; }
            ps->pos++;
            mn = lo; mx = hi;
            if (mx >= 0 && mx < mn) { fail(ps, L"A repeat count is backwards."); return NULL; }
        } else break;
        Node *r = mk(ps, N_REP);
        if (!r) return NULL;
        r->a = a; r->min = mn; r->max = mx;
        if (ps->pos < ps->len && ps->p[ps->pos] == '?') { r->greedy = 0; ps->pos++; }
        a = r;
    }
    if (a && a->t == N_CHAR && 0) return a;
    return a;
}

static Node *parse_cat(Parser *ps)
{
    Node *left = NULL;
    while (!ps->failed && ps->pos < ps->len && ps->p[ps->pos] != '|' && ps->p[ps->pos] != ')') {
        Node *r = parse_rep(ps);
        if (!r) return NULL;
        if (!left) left = r;
        else { Node *c = mk(ps, N_CAT); if (!c) return NULL; c->a = left; c->b = r; left = c; }
    }
    return left ? left : mk(ps, N_EMPTY);
}

static Node *parse_alt(Parser *ps)
{
    Node *left = parse_cat(ps);
    while (left && !ps->failed && ps->pos < ps->len && ps->p[ps->pos] == '|') {
        ps->pos++;
        Node *right = parse_cat(ps);
        if (!right) return NULL;
        Node *a = mk(ps, N_ALT);
        if (!a) return NULL;
        a->a = left; a->b = right; left = a;
    }
    return left;
}

/* ------------------------------------------------------------ code generation */

static int emit(Parser *ps, IOp op, int x, int y, wchar_t c)
{
    Rx *r = ps->rx;
    if (r->n >= MAX_PROG) { fail(ps, L"The pattern is too large."); return -1; }
    if (r->n == r->cap) {
        int nc = r->cap ? r->cap * 2 : 64;
        Inst *p = (Inst *)realloc(r->prog, (size_t)nc * sizeof *p);
        if (!p) { fail(ps, L"Out of memory."); return -1; }
        r->prog = p; r->cap = nc;
    }
    r->prog[r->n].op = op; r->prog[r->n].x = x; r->prog[r->n].y = y; r->prog[r->n].c = c;
    return r->n++;
}

static void gen(Parser *ps, const Node *n)
{
    Rx *r = ps->rx;
    if (!n || ps->failed) return;
    switch (n->t) {
    case N_EMPTY: break;
    case N_CHAR: emit(ps, I_CHAR, 0, 0, n->c); break;
    case N_ANY: emit(ps, I_ANY, 0, 0, 0); break;
    case N_CLASS: emit(ps, I_CLASS, n->cls, 0, 0); break;
    case N_BOL: emit(ps, I_BOL, 0, 0, 0); break;
    case N_EOL: emit(ps, I_EOL, 0, 0, 0); break;
    case N_WB: emit(ps, I_WB, 0, 0, 0); break;
    case N_NWB: emit(ps, I_NWB, 0, 0, 0); break;
    case N_CAT: gen(ps, n->a); gen(ps, n->b); break;
    case N_GROUP:
        emit(ps, I_SAVE, 2 * n->group, 0, 0);
        gen(ps, n->a);
        emit(ps, I_SAVE, 2 * n->group + 1, 0, 0);
        break;
    case N_ALT: {
        int s = emit(ps, I_SPLIT, 0, 0, 0);
        if (s < 0) return;
        r->prog[s].x = r->n;
        gen(ps, n->a);
        int j = emit(ps, I_JMP, 0, 0, 0);
        if (j < 0) return;
        r->prog[s].y = r->n;
        gen(ps, n->b);
        r->prog[j].x = r->n;
        break;
    }
    case N_REP: {
        for (int i = 0; i < n->min && !ps->failed; i++) gen(ps, n->a);
        if (n->max < 0) {                                   /* x* */
            int s = emit(ps, I_SPLIT, 0, 0, 0);
            if (s < 0) return;
            int body = r->n;
            gen(ps, n->a);
            emit(ps, I_JMP, s, 0, 0);
            if (ps->failed) return;
            int out = r->n;
            r->prog[s].x = n->greedy ? body : out;
            r->prog[s].y = n->greedy ? out : body;
        } else {
            int opt = n->max - n->min;
            int splits[1001];
            int ns = 0;
            for (int i = 0; i < opt && !ps->failed; i++) {  /* x?x?... nested so later copies need earlier ones */
                int s = emit(ps, I_SPLIT, 0, 0, 0);
                if (s < 0) return;
                splits[ns++] = s;
                r->prog[s].x = r->n;                       /* patched for lazy below */
                gen(ps, n->a);
            }
            int out = r->n;
            for (int i = 0; i < ns; i++) {
                int s = splits[i];
                int body = s + 1;
                r->prog[s].x = n->greedy ? body : out;
                r->prog[s].y = n->greedy ? out : body;
            }
        }
        break;
    }
    }
}

void rx_free(Rx *r)
{
    if (!r) return;
    free(r->prog);
    for (int i = 0; i < r->ncls; i++) free(r->cls[i].r);
    free(r->cls);
    free(r);
}

Rx *rx_compile(const wchar_t *pattern, int ignore_case, wchar_t *err, size_t errcap)
{
    Parser ps;
    memset(&ps, 0, sizeof ps);
    Rx *r = (Rx *)calloc(1, sizeof *r);
    if (!r) return NULL;
    r->icase = ignore_case != 0;
    ps.p = pattern; ps.len = wcslen(pattern); ps.rx = r; ps.err = err; ps.errcap = errcap;
    if (err && errcap) err[0] = 0;
    Node *root = parse_alt(&ps);
    if (!ps.failed && ps.pos < ps.len) fail(&ps, L"Unmatched ) in the pattern.");
    if (!ps.failed) {
        emit(&ps, I_SAVE, 0, 0, 0);
        gen(&ps, root);
        emit(&ps, I_SAVE, 1, 0, 0);
        emit(&ps, I_MATCH, 0, 0, 0);
    }
    for (int i = 0; i < ps.npool; i++) free(ps.pool[i]);
    free(ps.pool);
    if (ps.failed) { rx_free(r); return NULL; }
    return r;
}

int rx_groups(const Rx *r) { return r->ngroups; }

/* ------------------------------------------------------------------- matcher */

static int is_word(wchar_t c) { return is_alnum_c(c) || c == '_'; }

static int cls_has(const Class *c, wchar_t ch)
{
    for (int i = 0; i < c->n; i++) if (ch >= c->r[i].lo && ch <= c->r[i].hi) return 1;
    return 0;
}

static int cls_match(const Rx *r, const Class *c, wchar_t ch)
{
    int in = cls_has(c, ch);
    if (!in && r->icase) {
        wchar_t lo = (wchar_t)towlower(ch), up = (wchar_t)towupper(ch);
        in = cls_has(c, lo) || cls_has(c, up);
    }
    return in != c->neg;
}

typedef struct { int pc; size_t pos; int slot; size_t old; } Frame;      /* slot >= 0: restore caps[slot] = old */

int rx_match_at(const Rx *r, const wchar_t *t, size_t n, size_t i, size_t *caps)
{
    int ncap = 2 * (r->ngroups + 1);
    size_t *cur = caps;
    for (int k = 0; k < ncap; k++) cur[k] = NOCAP;
    size_t cap = 256, sp = 0;
    Frame *st = (Frame *)malloc(cap * sizeof *st);
    if (!st) return 0;
    long steps = 0;
    int pc = 0, ok = 0;
    size_t pos = i;
    for (;;) {
        if (++steps > STEP_LIMIT) break;
        int fail_ = 0;
        const Inst *in = &r->prog[pc];
        switch (in->op) {
        case I_CHAR:
            if (pos < n && (t[pos] == in->c || (r->icase && fold_char(t[pos]) == fold_char(in->c)))) { pos++; pc++; }
            else fail_ = 1;
            break;
        case I_ANY:
            if (pos < n && t[pos] != '\n') { pos++; pc++; } else fail_ = 1;
            break;
        case I_CLASS:
            if (pos < n && cls_match(r, &r->cls[in->x], t[pos])) { pos++; pc++; } else fail_ = 1;
            break;
        case I_BOL: if (pos == 0 || t[pos - 1] == '\n') pc++; else fail_ = 1; break;
        case I_EOL: if (pos == n || t[pos] == '\n') pc++; else fail_ = 1; break;
        case I_WB: case I_NWB: {
            int a = pos > 0 && is_word(t[pos - 1]), b = pos < n && is_word(t[pos]);
            if ((a != b) == (in->op == I_WB)) pc++; else fail_ = 1;
            break;
        }
        case I_JMP: pc = in->x; break;
        case I_SPLIT:
            if (sp == cap) {
                cap *= 2;
                Frame *ns = (Frame *)realloc(st, cap * sizeof *st);
                if (!ns) { free(st); return 0; }
                st = ns;
            }
            st[sp].pc = in->y; st[sp].pos = pos; st[sp].slot = -1; st[sp].old = 0; sp++;
            pc = in->x;
            break;
        case I_SAVE:
            if (sp == cap) {
                cap *= 2;
                Frame *ns = (Frame *)realloc(st, cap * sizeof *st);
                if (!ns) { free(st); return 0; }
                st = ns;
            }
            st[sp].pc = -1; st[sp].pos = 0; st[sp].slot = in->x; st[sp].old = cur[in->x]; sp++;
            cur[in->x] = pos;
            pc++;
            break;
        case I_MATCH: ok = 1; break;
        }
        if (ok) break;
        if (fail_) {
            for (;;) {
                if (sp == 0) { free(st); return 0; }
                Frame f = st[--sp];
                if (f.slot >= 0) { cur[f.slot] = f.old; continue; }
                pc = f.pc; pos = f.pos;
                break;
            }
        }
    }
    free(st);
    return ok;
}

int rx_search(const Rx *r, const wchar_t *t, size_t n, size_t from, int dir, size_t *caps)
{
    if (dir >= 0) {
        for (size_t i = from; i <= n; i++) if (rx_match_at(r, t, n, i, caps)) return 1;
    } else {
        size_t i = from > n ? n : from;
        for (;;) {
            if (rx_match_at(r, t, n, i, caps)) return 1;
            if (i == 0) break;
            i--;
        }
    }
    return 0;
}

size_t rx_expand(const wchar_t *repl, const wchar_t *text, const size_t *caps, int ngroups, wchar_t *out)
{
    size_t len = 0;
    for (const wchar_t *p = repl; *p; p++) {
        if (*p == '$' && p[1]) {
            int g = -1;
            if (p[1] == '$') { if (out) out[len] = '$'; len++; p++; continue; }
            if (p[1] == '&') g = 0;
            else if (p[1] >= '0' && p[1] <= '9') g = p[1] - '0';
            if (g >= 0) {
                p++;
                if (g <= ngroups && caps[2 * g] != NOCAP) {
                    size_t l = caps[2 * g + 1] - caps[2 * g];
                    if (out) memcpy(out + len, text + caps[2 * g], l * sizeof(wchar_t));
                    len += l;
                }
                continue;
            }
        }
        if (out) out[len] = *p;
        len++;
    }
    if (out) out[len] = 0;
    return len;
}
