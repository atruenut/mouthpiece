/*
 * navmath.h - small pure helpers for moving around a long document by ear. PURE C.
 *   PosHist   : "back / forward" through places you jumped from (like a browser).
 *   timeline  : estimated listening time, to skip "30 seconds" or jump to "40%".
 *   goto spec : parser for the Go to box ("40%", "c12", "p300", "s1200", "m3", "+30s", "-2m").
 *   caption chunks : split a sentence into caption-sized pieces.
 */
#ifndef NAVMATH_H
#define NAVMATH_H
#include <stddef.h>
#include <wchar.h>
#include "doc.h"

#define POSHIST_MAX 64
typedef struct PosHist { int back[POSHIST_MAX], fwd[POSHIST_MAX]; int nb, nf; } PosHist;
void poshist_clear(PosHist *h);
void poshist_jump(PosHist *h, int from);                 /* record `from` before jumping away from it */
int  poshist_back(PosHist *h, int cur);                  /* target sentence, or -1; `cur` goes on the forward list */
int  poshist_forward(PosHist *h, int cur);

/* Estimated speaking time. chars_per_sec at rate 0 is ~15; each rate step changes it by ~7%. */
double nav_chars_per_sec(int rate);
double nav_sentence_seconds(const Document *d, int sent, double cps);          /* includes a small pause */
double nav_seconds_before(const Document *d, int sent, double cps);
double nav_total_seconds(const Document *d, double cps);
int    nav_skip_seconds(const Document *d, int from, double seconds, double cps);   /* negative = back; clamped */
int    nav_at_fraction(const Document *d, double frac, double cps);                  /* 0..1 */

typedef enum { GT_NONE = 0, GT_PERCENT, GT_CHAPTER, GT_PARAGRAPH, GT_SENTENCE, GT_MARK, GT_SKIP } GotoKind;
typedef struct GotoSpec { int kind; double value; } GotoSpec;     /* value: percent, 1-based number, or signed seconds */
int  goto_parse(const wchar_t *text, GotoSpec *out);             /* 1 = understood */

/* Caption chunks: break `text` (n chars) at spaces into pieces of at most `maxc` chars (longer words stay whole).
 * Fills starts[]/lens[] (up to cap) and returns the count. */
int  caption_chunks(const wchar_t *text, size_t n, size_t maxc, size_t *starts, size_t *lens, int cap);

/* Where to resume speaking a sentence of n characters after `frac` (0..1) of it has been spoken: the start of a word at or a
 * little before that point (about 4 characters of overlap so a word is not cut in half). Returns 0 when frac is small, n when
 * the sentence is practically over (caller then continues with the next one). */
size_t text_resume_offset(const wchar_t *t, size_t n, double frac);
#endif
