/* ui_goto.h - "Go to" window: a position slider with a live description, and lists of chapters, marks and recent places. */
#ifndef UI_GOTO_H
#define UI_GOTO_H
#include <windows.h>

typedef struct GotoItem { wchar_t label[220]; int sent; } GotoItem;
typedef struct GotoUi {
    HFONT font;
    int dpi;
    GotoItem *chapters; int nch;
    GotoItem *marks; int nmk;
    GotoItem *recent; int nrec;
    int cur;                                        /* sentence we are at now */
    double (*fraction_of)(void *ctx, int sent);     /* 0..1 along the listening time */
    int    (*at_fraction)(void *ctx, double f);     /* sentence at that point */
    void   (*describe)(void *ctx, int sent, wchar_t *out, int cap);   /* "42%  about 1h 12m of 2h 50m  -  Chapter 7: Title" */
    int    (*parse)(void *ctx, const wchar_t *spec, wchar_t *err, int errcap);   /* typed place -> sentence, or -1 with a message */
    void *ctx;
    int out_sent;                                   /* result */
    int out_play;                                   /* start playing from there */
} GotoUi;

/* Returns 1 if the user chose a place (ui->out_sent). */
int goto_dialog(HWND parent, GotoUi *ui);
#endif
