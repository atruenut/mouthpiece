/* ui_generate.h - "Generate audio" dialog: analysis, assembly settings, background build + export. */
#ifndef UI_GENERATE_H
#define UI_GENERATE_H
#include <windows.h>
#include "ir.h"
#include "cache.h"
#include "chapters.h"
#include "config.h"



typedef struct GenUi {
    HFONT font;
    int dpi;
    Cache *cache;
    const Document *doc;
    const ChapterList *chapters;        /* may be empty */
    const Speech *speech;
    SynthParams synth;                  /* voice, rate, volume, engine, wav_rate set by the caller */
    int first_sent, last_sent;          /* range to build */
    const wchar_t *range_label;         /* "the whole document" / "the selection" */
    GenSettings *settings;              /* edited in place */
    wchar_t out_path[MAX_PATH];         /* in/out: last output file */
    wchar_t out_dir[MAX_PATH];          /* in/out: last output folder (per-chapter mode) */
    void (*changed)(void *ctx);         /* persist settings/out_path */
    void *ctx;
} GenUi;

void generate_dialog(HWND parent, GenUi *ui);
#endif
