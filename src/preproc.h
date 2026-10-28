/*
 * preproc.h - SOURCE -> NORMALIZED cleanup stage (Milestone 3). PURE C.
 *
 * The source text is never modified. preproc_run() produces a new normalized
 * text plus an offset map, so anything found in the normalized text (a
 * sentence span, say) can be shown back in the untouched source, and a caret
 * in the source can be located in the normalized text.
 *
 * Everything is one linear pass over lines/characters (plus a sort of the few
 * repeated-line candidates): no regex engine, no dictionaries, no learning.
 *
 * Passes (each can be switched off):
 *   repeated_lines   remove running headers/footers (a short line whose
 *                    digit-insensitive form repeats >= 3 times at roughly
 *                    regular intervals of >= 8 lines)
 *   page_numbers     remove "42", "Page 42", "42 of 300", "- 42 -", "Page iv" lines;
 *                    a bare number or roman numeral (i, ii, iv, XII ...) needs
 *                    evidence: a neighbour of the same kind 1-3 higher or lower,
 *                    >= 6 lines away (so lists/tables/lone words are left alone)
 *   join_lines       re-join hard-wrapped lines, only if the text actually looks
 *                    hard-wrapped; keeps paragraph/heading/list-item breaks
 *   fix_hyphens      "indus-\ntrial" -> "industrial"; "well-\nknown" and
 *                    "Anglo-\nSaxon" keep their hyphen
 *   normalize_ws     collapse runs of blanks, trim lines, blank-line runs -> one
 *   normalize_unicode ligatures, NBSP & friends, zero-width/soft hyphen, controls
 *   remove_urls      http(s)://, ftp://, www. tokens                  (off by default)
 *   remove_citations numeric citations like [12] [1, 2] [3-5]         (off by default)
 * Not implemented (and therefore not offered): footnotes, bibliography.
 */
#ifndef PREPROC_H
#define PREPROC_H
#include <stddef.h>
#include <wchar.h>

typedef struct PreprocOptions {
    int enabled;                  /* 0 = all passes off (text only gets \n line ends) */
    int repeated_lines, page_numbers, join_lines, fix_hyphens;
    int normalize_ws, normalize_unicode, remove_urls, remove_citations;
    int remove_footnotes;         /* footnote markers (superscript digits, [^n]) and note lines (leading superscript digits, [^n]:, dagger) */
    int remove_tables;            /* drop table-like blocks (tab / | / wide-space separated rows) */
    int table_rows;               /* ...or read them row by row: cells separated by commas, each row a sentence */
} PreprocOptions;

typedef enum { CH_HEADER = 1, CH_PAGENUM, CH_HYPHEN, CH_URL, CH_CITATION, CH_JOIN, CH_UNICODE, CH_FOOTNOTE, CH_TABLE } ChangeKind;

/* Individual edits, in SOURCE offsets, sorted by src_start (only when log_changes != 0).
 * CH_JOIN = a line terminator replaced by a space/nothing; CH_UNICODE = one fixed character. */
typedef struct Change { int kind; size_t src_start, src_len; } Change;

/* One run of the offset map. If norm_len == src_len the run is a 1:1 copy
 * (character i of the run came from source char src_start+i); otherwise every
 * normalized char in the run maps to src_start (expansions, joined newlines). */
typedef struct MapEntry { size_t norm_start, norm_len, src_start, src_len; } MapEntry;

typedef struct PreprocResult {
    wchar_t  *text;  size_t len;           /* normalized text, NUL-terminated; owned (may be taken) */
    MapEntry *map;   int nmap;
    Change   *changes; int nchanges;       /* only filled when log_changes != 0 */
    size_t src_len;
    size_t n_headers, n_pagenums, n_hyphens, n_joined, n_urls, n_citations, n_unicode, n_footnotes, n_tables;
    int hard_wrapped;                      /* the text was judged to be hard-wrapped */
    int cap_text, cap_map, cap_changes;    /* private */
} PreprocResult;

void preproc_default_options(PreprocOptions *o);
/* 1 = ok, 0 = out of memory (r is left empty). */
int  preproc_run(const wchar_t *src, size_t len, const PreprocOptions *o, int log_changes, PreprocResult *r);
void preproc_free(PreprocResult *r);

/* Normalized -> source. */
size_t preproc_norm_to_src(const PreprocResult *r, size_t norm_off);
size_t preproc_norm_end_to_src(const PreprocResult *r, size_t norm_end);   /* exclusive end */
/* Source -> normalized (offsets inside removed text resolve to the next kept char). */
size_t preproc_src_to_norm(const PreprocResult *r, size_t src_off);

#endif
