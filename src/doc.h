/*
 * doc.h - document model (Milestone 2).  PURE C: no windows.h, no UI, no SAPI,
 * so it is unit-testable anywhere and reusable by the future MouthpieceCLI.exe CLI.
 *
 * Document
 *   text            owned copy of the text exactly as given (UTF-16 on Windows)
 *   paragraphs[]    spans into text, in order
 *   sentences[]     spans into text, in order, each tagged with its paragraph
 *
 * Every unit is a (start,len) span into `text`, so anything can be mapped back
 * to the source. In Milestone 2 NORMALIZED == SOURCE; Milestone 3 inserts the
 * cleanup stage and an offset map between the two.
 *
 * Spans never include leading/trailing whitespace, but may contain internal
 * line breaks (PDF-style hard-wrapped lines). doc_speech_text() collapses
 * them, so the source is never modified.
 *
 * Ownership: doc_build() copies the input; doc_free() releases everything.
 * A Document is not thread-safe; share it read-only or give each thread its own.
 */
#ifndef DOC_H
#define DOC_H
#include <stddef.h>
#include <wchar.h>

typedef struct Sentence  { size_t start, len; int para; } Sentence;
typedef struct Paragraph { size_t start, len; int first_sentence, sentence_count; } Paragraph;

typedef struct Document {
    wchar_t   *text;
    size_t     len;
    Paragraph *paras;  int npara;
    Sentence  *sents;  int nsent;
    int para_cap, sent_cap;            /* private */
} Document;

#define DOC_MAX_SENTENCE 800           /* longer sentences are split (cache granularity) */

void doc_init(Document *d);
int  doc_build(Document *d, const wchar_t *text, size_t len);  /* 1 = ok, 0 = out of memory (d left empty) */
/* Same, but takes ownership of a malloc'd, NUL-terminated buffer (no copy). */
int  doc_build_take(Document *d, wchar_t *text, size_t len);
void doc_free(Document *d);

/* Sentence containing `offset` (end inclusive); if the offset is in the gap
 * between two sentences, the NEXT sentence. -1 if the document has none. */
int  doc_sentence_at(const Document *d, size_t offset);

/* malloc'd, NUL-terminated text to hand to a TTS engine: internal whitespace
 * runs collapsed to one space, zero-width/soft-hyphen characters dropped. */
wchar_t *doc_speech_text(const Document *d, int sentence);

#endif
