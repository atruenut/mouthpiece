/*
 * analyze.h - dry-run analysis of a document: totals and the places a speech engine is likely to stumble. PURE C.
 * Nothing is synthesized. An issue groups every occurrence of the same token. `handled` says the program already
 * deals with it (your pronunciation dictionary has the word, or the matching built-in speech rule is on).
 */
#ifndef ANALYZE_H
#define ANALYZE_H
#include "doc.h"
#include "chapters.h"
#include "pron.h"
#include "speech.h"

typedef enum { AK_URL = 1, AK_EMAIL, AK_TECH, AK_INITIALS, AK_NUMBER, AK_ACRONYM, AK_OCR, AK_NAME, AK_LONG } AnalysisKind;

typedef struct Issue {
    int kind;
    wchar_t token[48];
    int count;
    int first_sent;
    int handled;
} Issue;

typedef struct Analysis {
    size_t words;
    int paragraphs, sentences, chapters;
    Issue *issues; int n;
} Analysis;

int  analyze_document(Analysis *a, const Document *doc, const ChapterList *chs, const PronDict *dict, const SpeechOptions *opts);
void analysis_free(Analysis *a);
const wchar_t *analysis_kind_name(int kind);        /* "URL", "Acronym", ... */
const wchar_t *analysis_kind_hint(int kind);        /* one line explaining what to do about it */
#endif
