/*
 * rx.h - a small backtracking regular-expression engine. PURE C, wide characters, no dependencies.
 * Used by pronunciation rules and Find/Replace.
 *
 * Syntax: literals, . (any char except newline), [abc] [a-z] [^...] with \d \w \s inside, \d \D \w \W \s \S,
 * \b \B, ^ $ (line anchors), groups ( ) and (?: ), alternation |, quantifiers * + ? {n} {n,} {n,m} and their
 * lazy forms (*? +? ?? {n,m}?), escapes \. \\ \t \n \r etc. No backreferences or lookaround.
 * Replacement text: $1..$9 groups, $0 or $& whole match, $$ a dollar sign.
 * Matching is bounded by a step limit, so a nasty pattern fails to match instead of hanging.
 */
#ifndef RX_H
#define RX_H
#include <stddef.h>
#include <wchar.h>

typedef struct Rx Rx;

/* NULL on a syntax error (message in err). */
Rx  *rx_compile(const wchar_t *pattern, int ignore_case, wchar_t *err, size_t errcap);
void rx_free(Rx *r);
int  rx_groups(const Rx *r);                      /* number of capture groups */
/* Try to match starting exactly at text[i]. caps[2k], caps[2k+1] = start/end of group k (k=0 whole match;
 * (size_t)-1 if the group did not take part). `caps` must hold 2*(rx_groups+1) entries. */
int  rx_match_at(const Rx *r, const wchar_t *text, size_t n, size_t i, size_t *caps);
/* Search from `from` forward (dir>0) or backward (dir<0). Returns 1 and fills caps. */
int  rx_search(const Rx *r, const wchar_t *text, size_t n, size_t from, int dir, size_t *caps);
/* Expand `repl` using caps; returns the length. If out != NULL it must have room for that length + 1. */
size_t rx_expand(const wchar_t *repl, const wchar_t *text, const size_t *caps, int ngroups, wchar_t *out);
#endif
