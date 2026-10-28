/* ui_speech.h - pronunciation editor and speech-text preview dialogs (Milestone 4). */
#ifndef UI_SPEECH_H
#define UI_SPEECH_H
#include <windows.h>
#include "speech.h"
#include "pronscope.h"

typedef void (*SpeakFn)(void *ctx, const wchar_t *text);

typedef struct SpeechUi {
    HFONT font;
    int dpi;
    PronDict *dict;               /* edited in place; changed() is called after every edit */
    SpeechOptions *opts;          /* edited by the preview dialog when "Use these settings" is pressed */
    const Document *doc;          /* the cleaned document (preview only) */
    const wchar_t *scope_chapter;     /* ready-made scope strings for "this chapter" / "the current selection"; */
    const wchar_t *scope_selection;   /* NULL when there is none (no chapter here / nothing selected) */
    const wchar_t *chapter_label;     /* chapter title, for the radio button */
    SpeakFn speak;  void *speak_ctx;       /* speak `text` now with the current voice settings */
    void (*changed)(void *ctx);  void *changed_ctx;   /* persist dictionary/options */
} SpeechUi;

/* Modal editor. `initial_word` (may be NULL/empty) prefills the Word field. */
void pron_dialog(HWND parent, const SpeechUi *ui, const wchar_t *initial_word);

/* Modal ORIGINAL-vs-SPOKEN preview of every sentence the speech stage changes.
 * Returns 1 if the normalization options were changed and accepted. */
int speech_preview_dialog(HWND parent, const SpeechUi *ui);

#endif
