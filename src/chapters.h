/*
 * chapters.h - structure detection, user corrections, bookmarks (Milestone 6). PURE C.
 *
 * Detection looks at whole paragraphs only: a heading paragraph is short, has no sentence
 * punctuation, and matches one of
 *     CHAPTER 7   Chapter Seven   Chapter 7: Title   PART I   BOOK TWO     (number, roman, or number word)
 *     Prologue / Epilogue / Preface / Foreword / Introduction / Afterword / Appendix A ...
 *     I.   II   XIV                                                          (a bare roman numeral)
 * A heading with no title of its own takes the next short, unpunctuated paragraph as its title.
 * "PART I" directly followed (within 2 paragraphs) by a chapter heading is folded into that
 * chapter instead of becoming a tiny chapter of its own.
 *
 * Detection is never assumed to be right: the user can add or remove a heading or rename a
 * chapter. Those corrections are stored as "overrides" keyed by the heading paragraph's TEXT
 * (+ which occurrence of identical text), not by position, so they survive edits elsewhere.
 *
 * Bookmarks and the resume position use the same idea: they remember a semantic location
 * (chapter / paragraph in chapter / sentence in paragraph) plus the sentence's opening words,
 * and re-find the sentence by text if the structure moved.
 */
#ifndef CHAPTERS_H
#define CHAPTERS_H
#include <stddef.h>
#include <wchar.h>
#include "doc.h"
#include "ini.h"

typedef struct Chapter {
    int para;                  /* paragraph where the chapter starts */
    int first_sent;            /* first sentence of that paragraph */
    wchar_t *title;
    int level;                 /* 0 chapter/special, 1 part/book (only if standing alone) */
    int manual;                /* added by the user */
} Chapter;

typedef struct ChapterList { Chapter *c; int n, cap; } ChapterList;

typedef enum { CO_ADD = 1, CO_REMOVE, CO_RENAME } OverrideKind;
typedef struct ChapterOverride { int kind; wchar_t *key; int occurrence; wchar_t *title; } ChapterOverride;
typedef struct Overrides { ChapterOverride *o; int n, cap; } Overrides;

void chapters_free(ChapterList *l);
void overrides_free(Overrides *o);
/* Detect chapters in doc and apply the user's overrides. 1 ok, 0 OOM. `ov` may be NULL. */
int  chapters_detect(ChapterList *out, const Document *doc, const Overrides *ov);

int  chapter_of_sentence(const ChapterList *l, int sent);                       /* -1 before the first chapter */
int  chapter_last_sentence(const ChapterList *l, const Document *doc, int ch);  /* last sentence of chapter ch */
int  chapter_first_sentence(const ChapterList *l, const Document *doc, int ch); /* ch == -1: the front matter */

/* User corrections. `para` is a paragraph index of doc. Return 1 on success. */
int  overrides_add_heading(Overrides *o, const Document *doc, int para, const wchar_t *title);
int  overrides_remove_heading(Overrides *o, const Document *doc, int para);   /* hides a detected one / forgets a manual one */
int  overrides_rename(Overrides *o, const Document *doc, int para, const wchar_t *title);
void overrides_to_ini(const Overrides *o, Ini *d);
void overrides_from_ini(Overrides *o, const Ini *d);

/* File name for chapter output: "03 - Title.wav"; invalid characters replaced, length bounded. */
void chapter_filename(wchar_t *out, size_t cap, int ordinal, const wchar_t *title);

/* ---------------------------------------------------------------- bookmarks */
typedef struct Bookmark {
    wchar_t *label;
    int chapter, para_in_ch, sent_in_para;   /* chapter -1 = before the first chapter */
    wchar_t *snippet;                        /* opening words of the sentence */
    int occurrence;                          /* which sentence with this snippet (0-based) */
    int kind;                                /* MarkKind: 0 = plain bookmark, 1.. = typed flags */
    wchar_t *note;                           /* optional one-line note, may be NULL */
} Bookmark;
typedef enum { MK_BOOKMARK = 0, MK_REVISIT, MK_IMPORTANT, MK_UNCLEAR, MK_QUESTION, MK_DOUBT, MK_COUNT } MarkKind;
const wchar_t *mark_kind_name(int kind);            /* "Bookmark", "Revisit", ... */
const wchar_t *mark_kind_glyph(int kind);           /* one character for the gutter */
typedef struct BookmarkList { Bookmark *b; int n, cap; } BookmarkList;

void bm_free(BookmarkList *l);
void bm_clear(BookmarkList *l);
/* Create a bookmark/anchor for sentence `sent`. `label` NULL -> automatic. Returns index or -1. */
int  bm_add(BookmarkList *l, const Document *doc, const ChapterList *chs, int sent, const wchar_t *label);
int  bm_make(Bookmark *out, const Document *doc, const ChapterList *chs, int sent, const wchar_t *label);
void bm_destroy(Bookmark *b);
/* Sentence the bookmark points at now, or -1 if it can no longer be found. */
int  bm_resolve(const Bookmark *b, const Document *doc, const ChapterList *chs);
void bm_remove(BookmarkList *l, int i);
/* Index of the nearest bookmark strictly after (dir>0) / before (dir<0) `from_sent` that still resolves; -1 none.
 * Wraps around the document when nothing lies further in that direction. */
int  bm_next(const BookmarkList *l, const Document *doc, const ChapterList *chs, int from_sent, int dir);
/* Typed marks. At most one mark of a given kind per sentence: adding it again returns -2 (caller may toggle it off). */
int  bm_add_kind(BookmarkList *l, const Document *doc, const ChapterList *chs, int sent, int kind, const wchar_t *note);
int  bm_find_kind_at(const BookmarkList *l, const Document *doc, const ChapterList *chs, int sent, int kind);   /* index or -1 */
int  bm_set_note(Bookmark *b, const wchar_t *note);      /* newlines become spaces; 1 ok */
/* Like bm_next but only kinds whose bit (1<<kind) is in mask. */
int  bm_next_mask(const BookmarkList *l, const Document *doc, const ChapterList *chs, int from_sent, int dir, unsigned mask);
int  bm_find_at(const BookmarkList *l, const Document *doc, const ChapterList *chs, int sent);
void bm_to_ini(const BookmarkList *l, Ini *d);
void bm_from_ini(BookmarkList *l, const Ini *d);
void bm_anchor_to_ini(const Bookmark *b, Ini *d, const wchar_t *section);     /* single anchor (resume position) */
int  bm_anchor_from_ini(Bookmark *b, const Ini *d, const wchar_t *section);
#endif
