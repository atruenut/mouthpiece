/* ui_caption_dlg.h - settings window for the on-screen captions (font, colours, contrast, size, position, dragging). */
#ifndef UI_CAPTION_DLG_H
#define UI_CAPTION_DLG_H
#include <windows.h>
#include "ui_caption.h"
#include "caplooks.h"
/* Edits *cfg in place; `apply` is called after every change so the real caption bar shows the result immediately.
 * `looks` is the list of the user's saved looks (edited in place; `looks_changed` is called after each save or delete so the
 * caller can persist it). Returns 1 if anything was changed. */
int caption_dialog(HWND parent, HFONT font, int dpi, CaptionCfg *cfg, void (*apply)(const CaptionCfg *, void *), void *ctx,
                   CaptionLooks *looks, void (*looks_changed)(void *));
#endif
