/*
 * pronscope.h - scopes for pronunciation entries (chapter or a range of sentences). PURE C.
 *
 *   ""                       global
 *   "chapter:<title>"        every sentence of the chapter whose heading text is <title> (case-insensitive)
 *   "range:<A>\x1F<n>\x1F<B>\x1F<m>"   from the sentence that opens with A (n-th such sentence, 0-based) to the
 *                            sentence that opens with B (m-th). The anchors are the sentences' opening words,
 *                            so a range survives edits elsewhere in the text; if an anchor text is edited the
 *                            entry stops applying (and the pronunciation list says so).
 */
#ifndef PRONSCOPE_H
#define PRONSCOPE_H
#include "pron.h"
#include "doc.h"
#include "chapters.h"

wchar_t *pronscope_chapter(const wchar_t *title);                           /* malloc'd */
wchar_t *pronscope_range(const Document *doc, int first, int last);         /* malloc'd; NULL on failure */
/* Fill s_first/s_last of every scoped entry for the current document (call after each document rebuild
 * and after the dictionary changes). Entries whose anchor/chapter cannot be found are disabled. */
void     pron_resolve_scopes(PronDict *d, const Document *doc, const ChapterList *chs);
/* Human readable description ("Chapter 3", "Selection: \"It was a dark...\""), `ok` set to 0 if it does not resolve. */
void     pronscope_describe(const PronEntry *e, wchar_t *out, size_t cap);
#endif
