/*
 * speech.h - SPEECH TEXT stage (Milestone 4). PURE C.
 *
 *   cleaned sentence  --speech_render-->  text handed to the TTS engine
 *
 * One left-to-right scan. At each position, in priority order:
 *   1. the user's pronunciation dictionary (longest entry wins)
 *   2. built-in normalizers (each switchable): abbreviations (Dr. -> Doctor),
 *      numbers (cardinals, decimals, ordinals, years, currency, percent),
 *      URLs/e-mail ("web address", "email address"), technical terms (C++ ...)
 *   3. otherwise the character is copied.
 * Text produced by a rule is never re-scanned, so rules cannot cascade or loop.
 * Deterministic: same input + dictionary + options -> same output.
 *
 * Not handled (left as written): times (10:30), dates (12/05/2020), section
 * numbers (3.2.1), phone numbers, numbers glued to letters (x86, 5km, mp3).
 */
#ifndef SPEECH_H
#define SPEECH_H
#include <stddef.h>
#include <wchar.h>
#include "pron.h"
#include "doc.h"

typedef struct SpeechOptions {
    int abbreviations, numbers, urls, technical;
} SpeechOptions;

typedef struct Speech {
    const PronDict *dict;        /* borrowed, may be NULL */
    SpeechOptions   opts;
    int             sentence1;   /* 1 + index of the document sentence being rendered; 0 = none (scoped entries off) */
} Speech;

typedef enum { SPAN_DICT = 1, SPAN_NORM = 2 } SpanKind;

/* A rewritten region: in[in_start, in_start+in_len) became out[out_start, out_start+out_len). */
typedef struct SpeechSpan { int kind; size_t in_start, in_len, out_start, out_len; } SpeechSpan;

typedef struct SpeechText {
    wchar_t *text; size_t len;
    SpeechSpan *spans; int nspans;       /* only when want_spans */
    size_t cap; int span_cap;            /* private */
} SpeechText;

void speech_default_options(SpeechOptions *o);
int  speech_render(const Speech *sp, const wchar_t *in, size_t n, int want_spans, SpeechText *out);  /* 1 ok, 0 OOM */
void speech_text_free(SpeechText *t);

/* Sentence `sent` of `doc`, whitespace-collapsed and rendered. malloc'd; NULL on OOM. */
wchar_t *speech_sentence_text(const Speech *sp, const Document *doc, int sent);

/* Exposed for tests/UI. Appends words to a malloc'd buffer owned by the caller. */
wchar_t *speech_number_words(unsigned long long n);            /* 1984 -> "one thousand nine hundred eighty-four" */

#endif
