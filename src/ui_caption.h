/*
 * ui_caption.h - the on-screen caption: a borderless, always-on-top bar that shows the sentence being read, like closed
 * captions on TV. Mouse clicks pass straight through it to the program underneath and it never takes focus, except in
 * "move mode" where it can be dragged anywhere on any monitor. Drawn with per-pixel alpha, so the text opacity and the
 * background opacity are independent. Long sentences are split into caption-sized pieces that advance on a timer.
 * Also shows short notices ("Flagged: Question") even when captions are switched off.
 */
#ifndef UI_CAPTION_H
#define UI_CAPTION_H
#include <windows.h>

typedef struct CaptionCfg {
    int enabled;
    wchar_t face[32];     /* font family */
    int font_pt;          /* 12..72 */
    int bold;
    unsigned text_color;  /* COLORREF */
    unsigned bg_color;
    int text_opacity;     /* 20..100 percent */
    int bg_opacity;       /* 0..100 percent */
    int outline;          /* thin outline in a contrasting colour */
    int position;         /* 0 bottom, 1 top, 2 custom (dragged) */
    int cx, cy;           /* custom position: centre of the bar in screen pixels */
    int width_pct;        /* 30..95 of the screen width */
    int hide_when_paused; /* hide a few seconds after playback pauses or stops */
} CaptionCfg;
void caption_default(CaptionCfg *c);
void caption_cfg_from_ini(CaptionCfg *c, const void *ini);     /* const Ini * */
void caption_cfg_to_ini(const CaptionCfg *c, void *ini);       /* Ini * */

typedef struct CaptionPreset { const wchar_t *name; unsigned text, bg; int bg_opacity, outline; } CaptionPreset;
const CaptionPreset *caption_presets(int *n);
int  caption_preset_of(const CaptionCfg *c);                   /* index of the matching preset, or -1 */
void caption_apply_preset(CaptionCfg *c, int i);

int  caption_init(HINSTANCE inst);
void caption_shutdown(void);
void caption_configure(const CaptionCfg *c);
/* Called when the user finishes dragging (position == custom) or ends move mode; the app saves the settings. */
void caption_on_moved(void (*fn)(const CaptionCfg *cfg, void *ctx), void *ctx);
void caption_move_mode(int on);                // let the bar be dragged; double-click on it ends the mode
int  caption_moving(void);
void caption_preview(int on);                  // show sample text while a settings window is open
/* The sentence being spoken now. `seconds` = estimated time to speak it (drives paging of long sentences). */
void caption_set(const wchar_t *text, size_t n, double seconds);
/* The voice's real position in the sentence set by caption_set (0..1, from its own word events). Pages follow the spoken words
 * instead of the estimate, and turn a little BEFORE the first word of the next page is spoken. Never goes back. */
void caption_progress(double frac);
void caption_idle(void);                       /* playback paused/stopped: fade out later if configured */
void caption_notice(const wchar_t *text, int ms);   /* shown regardless of `enabled` */
void caption_hide(void);
int  caption_visible(void);
#endif
