/* ui_profiles.h - voice profiles window: named presets of voice, rate and volume. */
#ifndef UI_PROFILES_H
#define UI_PROFILES_H
#include <windows.h>
#include "config.h"

typedef struct ProfileUi {
    HFONT font;
    int dpi;
    ProfileList *list;                              /* edited in place */
    void (*current)(void *ctx, VoiceProfile *out);  /* the voice/rate/volume now selected in the main window */
    int  (*apply)(void *ctx, const VoiceProfile *p);/* 1 ok, 0 if its voice is not installed */
    void (*changed)(void *ctx);                     /* persist the list */
    void *ctx;
} ProfileUi;

void profiles_dialog(HWND parent, const ProfileUi *ui);
#endif
