/*
 * caplooks.h - named caption looks the user saves and deletes (font, size, colours, opacity, outline, width).
 * PURE C. Position, "enabled" and "hide when paused" are deliberately NOT part of a look: choosing a look changes how the
 * caption appears, never where it is or whether it is on. Stored in settings.txt, section [CaptionLooks].
 */
#ifndef CAPLOOKS_H
#define CAPLOOKS_H
#include <stddef.h>
#include "ini.h"

#define LOOK_MAX 24
#define LOOK_NAME 40
typedef struct CaptionLook {
    wchar_t name[LOOK_NAME];
    wchar_t face[32];
    int font_pt, bold;
    unsigned text_color, bg_color;
    int text_opacity, bg_opacity, outline, width_pct;
} CaptionLook;
typedef struct CaptionLooks { int n; CaptionLook l[LOOK_MAX]; } CaptionLooks;

/* Name rules: 1..39 characters after trimming, no '=' or control characters. Returns 1 if usable; `out` receives the trimmed name. */
int  looks_clean_name(const wchar_t *in, wchar_t *out, size_t cap);
int  looks_find(const CaptionLooks *s, const wchar_t *name);          /* case-insensitive; -1 if absent */
/* Add, or replace the look of the same name. Returns the index, or -1 (bad name, or the list is full). */
int  looks_put(CaptionLooks *s, const CaptionLook *look);
void looks_remove(CaptionLooks *s, int i);
int  looks_same(const CaptionLook *a, const CaptionLook *b);          /* every field except the name */
void looks_to_ini(const CaptionLooks *s, Ini *d);
void looks_from_ini(CaptionLooks *s, const Ini *d);
#endif
