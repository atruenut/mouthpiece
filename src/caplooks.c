#include "caplooks.h"
#include <string.h>
#include <stdio.h>
#include <wchar.h>

static wchar_t lower(wchar_t c) { return (c >= 'A' && c <= 'Z') ? (wchar_t)(c + 32) : c; }
static int ieq(const wchar_t *a, const wchar_t *b)
{
    while (*a && *b) { if (lower(*a) != lower(*b)) return 0; a++; b++; }
    return *a == *b;
}

int looks_clean_name(const wchar_t *in, wchar_t *out, size_t cap)
{
    if (!in || cap == 0) return 0;
    out[0] = 0;
    while (*in == ' ' || *in == '\t') in++;
    size_t n = wcslen(in);
    while (n && (in[n - 1] == ' ' || in[n - 1] == '\t')) n--;
    if (n == 0 || n >= LOOK_NAME || n >= cap) return 0;
    for (size_t i = 0; i < n; i++) if (in[i] == '=' || in[i] < 32) return 0;
    memcpy(out, in, n * sizeof(wchar_t));
    out[n] = 0;
    return 1;
}

int looks_find(const CaptionLooks *s, const wchar_t *name)
{
    for (int i = 0; i < s->n; i++) if (ieq(s->l[i].name, name)) return i;
    return -1;
}

int looks_put(CaptionLooks *s, const CaptionLook *look)
{
    CaptionLook c = *look;
    if (!looks_clean_name(look->name, c.name, LOOK_NAME)) return -1;
    c.face[31] = 0;
    int i = looks_find(s, c.name);
    if (i < 0) {
        if (s->n >= LOOK_MAX) return -1;
        i = s->n++;
    }
    s->l[i] = c;
    return i;
}

void looks_remove(CaptionLooks *s, int i)
{
    if (i < 0 || i >= s->n) return;
    for (int k = i; k + 1 < s->n; k++) s->l[k] = s->l[k + 1];
    s->n--;
}

int looks_same(const CaptionLook *a, const CaptionLook *b)
{
    return !wcscmp(a->face, b->face) && a->font_pt == b->font_pt && !a->bold == !b->bold && a->text_color == b->text_color &&
           a->bg_color == b->bg_color && a->text_opacity == b->text_opacity && a->bg_opacity == b->bg_opacity &&
           !a->outline == !b->outline && a->width_pct == b->width_pct;
}

static int clampi(long v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : (int)v; }

void looks_to_ini(const CaptionLooks *s, Ini *d)
{
    ini_remove_section(d, L"CaptionLooks");
    ini_set_int(d, L"CaptionLooks", L"Count", s->n);
    for (int i = 0; i < s->n; i++) {
        const CaptionLook *l = &s->l[i];
        wchar_t k[40];
#define KEY(f) swprintf(k, 40, L"%d." f, i)
        KEY(L"Name"); ini_set(d, L"CaptionLooks", k, l->name);
        KEY(L"Font"); ini_set(d, L"CaptionLooks", k, l->face);
        KEY(L"FontPt"); ini_set_int(d, L"CaptionLooks", k, l->font_pt);
        KEY(L"Bold"); ini_set_int(d, L"CaptionLooks", k, l->bold);
        KEY(L"TextColor"); ini_set_int(d, L"CaptionLooks", k, (long)l->text_color);
        KEY(L"BackColor"); ini_set_int(d, L"CaptionLooks", k, (long)l->bg_color);
        KEY(L"TextOpacity"); ini_set_int(d, L"CaptionLooks", k, l->text_opacity);
        KEY(L"BackOpacity"); ini_set_int(d, L"CaptionLooks", k, l->bg_opacity);
        KEY(L"Outline"); ini_set_int(d, L"CaptionLooks", k, l->outline);
        KEY(L"Width"); ini_set_int(d, L"CaptionLooks", k, l->width_pct);
#undef KEY
    }
}

void looks_from_ini(CaptionLooks *s, const Ini *d)
{
    memset(s, 0, sizeof *s);
    int cnt = clampi(ini_get_int(d, L"CaptionLooks", L"Count", 0), 0, LOOK_MAX);
    for (int i = 0; i < cnt; i++) {
        CaptionLook l;
        memset(&l, 0, sizeof l);
        wchar_t k[40];
#define KEY(f) swprintf(k, 40, L"%d." f, i)
        KEY(L"Name");
        const wchar_t *nm = ini_get(d, L"CaptionLooks", k);
        if (!nm) continue;
        wcsncpy(l.name, nm, LOOK_NAME - 1);
        KEY(L"Font");
        const wchar_t *f = ini_get(d, L"CaptionLooks", k);
        wcsncpy(l.face, f && *f ? f : L"Segoe UI", 31);
        KEY(L"FontPt"); l.font_pt = clampi(ini_get_int(d, L"CaptionLooks", k, 24), 12, 72);
        KEY(L"Bold"); l.bold = ini_get_int(d, L"CaptionLooks", k, 1) != 0;
        KEY(L"TextColor"); l.text_color = (unsigned)ini_get_int(d, L"CaptionLooks", k, 0xFFFFFF) & 0xFFFFFF;
        KEY(L"BackColor"); l.bg_color = (unsigned)ini_get_int(d, L"CaptionLooks", k, 0) & 0xFFFFFF;
        KEY(L"TextOpacity"); l.text_opacity = clampi(ini_get_int(d, L"CaptionLooks", k, 100), 20, 100);
        KEY(L"BackOpacity"); l.bg_opacity = clampi(ini_get_int(d, L"CaptionLooks", k, 85), 0, 100);
        KEY(L"Outline"); l.outline = ini_get_int(d, L"CaptionLooks", k, 0) != 0;
        KEY(L"Width"); l.width_pct = clampi(ini_get_int(d, L"CaptionLooks", k, 70), 30, 95);
#undef KEY
        looks_put(s, &l);
    }
}
