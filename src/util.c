#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void util_format_win32(wchar_t *buf, size_t cap, const wchar_t *context, DWORD code)
{
    wchar_t sys[256];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, code, 0, sys, 256, NULL);
    while (n > 0 && (sys[n - 1] == L'\r' || sys[n - 1] == L'\n' || sys[n - 1] == L' '))
        n--;
    sys[n] = 0;
    if (n)
        _snwprintf(buf, cap, L"%ls: %ls (0x%08lX)", context, sys, (unsigned long)code);
    else
        _snwprintf(buf, cap, L"%ls (0x%08lX)", context, (unsigned long)code);
    buf[cap - 1] = 0;
}

void util_format_hresult(wchar_t *buf, size_t cap, const wchar_t *context, HRESULT hr)
{
    util_format_win32(buf, cap, context, (DWORD)hr);
}

#define MAX_TEXT_FILE (256u * 1024u * 1024u)

int util_load_text_file(const wchar_t *path, wchar_t **out_text,
                        wchar_t *err, size_t errcap)
{
    *out_text = NULL;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        util_format_win32(err, errcap, L"Unable to open file", GetLastError());
        return 0;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) {
        util_format_win32(err, errcap, L"Unable to get file size", GetLastError());
        CloseHandle(h);
        return 0;
    }
    if (sz.QuadPart > MAX_TEXT_FILE) {
        _snwprintf(err, errcap, L"File is larger than %u MB.", MAX_TEXT_FILE >> 20);
        err[errcap - 1] = 0;
        CloseHandle(h);
        return 0;
    }
    DWORD size = (DWORD)sz.QuadPart;
    unsigned char *buf = (unsigned char *)malloc((size_t)size + 4);
    if (!buf) {
        _snwprintf(err, errcap, L"Out of memory reading file.");
        CloseHandle(h);
        return 0;
    }
    DWORD got = 0, total = 0;
    while (total < size && ReadFile(h, buf + total, size - total, &got, NULL) && got)
        total += got;
    CloseHandle(h);
    if (total != size) {
        util_format_win32(err, errcap, L"Unable to read file", GetLastError());
        free(buf);
        return 0;
    }
    memset(buf + size, 0, 4);

    /* ---- decode to UTF-16 ---- */
    wchar_t *w = NULL;
    size_t   wlen = 0;
    if (size >= 2 && buf[0] == 0xFF && buf[1] == 0xFE) {
        wlen = (size - 2) / 2;
        w = (wchar_t *)malloc((wlen + 1) * sizeof(wchar_t));
        if (w) memcpy(w, buf + 2, wlen * sizeof(wchar_t));
    } else {
        const char *src = (const char *)buf;
        int n = (int)size;
        if (n >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) { src += 3; n -= 3; }
        UINT cp = CP_UTF8;
        DWORD flags = MB_ERR_INVALID_CHARS;
        int need = n ? MultiByteToWideChar(cp, flags, src, n, NULL, 0) : 0;
        if (n && need == 0) {            /* not valid UTF-8: assume legacy ANSI */
            cp = CP_ACP; flags = 0;
            need = MultiByteToWideChar(cp, flags, src, n, NULL, 0);
        }
        wlen = (size_t)need;
        w = (wchar_t *)malloc((wlen + 1) * sizeof(wchar_t));
        if (w && need) MultiByteToWideChar(cp, flags, src, n, w, need);
    }
    free(buf);
    if (!w) {
        _snwprintf(err, errcap, L"Out of memory decoding file.");
        return 0;
    }
    w[wlen] = 0;

    /* ---- LF -> CRLF for the EDIT control ---- */
    size_t extra = 0;
    for (size_t i = 0; i < wlen; i++)
        if (w[i] == L'\n' && (i == 0 || w[i - 1] != L'\r')) extra++;
    wchar_t *out = (wchar_t *)malloc((wlen + extra + 1) * sizeof(wchar_t));
    if (!out) {
        free(w);
        _snwprintf(err, errcap, L"Out of memory converting line endings.");
        return 0;
    }
    size_t j = 0;
    for (size_t i = 0; i < wlen; i++) {
        if (w[i] == L'\n' && (i == 0 || w[i - 1] != L'\r')) out[j++] = L'\r';
        out[j++] = w[i];
    }
    out[j] = 0;
    free(w);
    *out_text = out;
    return 1;
}

int util_read_file(const wchar_t *path, unsigned char **buf, size_t *len, int *missing, wchar_t *err, size_t errcap)
{
    *buf = NULL; *len = 0;
    if (missing) *missing = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (missing && (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)) *missing = 1;
        util_format_win32(err, errcap, L"Unable to open file", e);
        return 0;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 64LL * 1024 * 1024) {
        _snwprintf(err, errcap, L"File is unreadable or too large."); err[errcap - 1] = 0;
        CloseHandle(h);
        return 0;
    }
    unsigned char *b = (unsigned char *)malloc((size_t)sz.QuadPart + 4);
    DWORD got = 0, total = 0;
    if (!b) { CloseHandle(h); _snwprintf(err, errcap, L"Out of memory."); return 0; }
    while (total < (DWORD)sz.QuadPart && ReadFile(h, b + total, (DWORD)sz.QuadPart - total, &got, NULL) && got) total += got;
    CloseHandle(h);
    memset(b + total, 0, 4);
    *buf = b; *len = total;
    return 1;
}

int util_write_file(const wchar_t *path, const void *data, size_t len, wchar_t *err, size_t errcap)
{
    wchar_t tmp[MAX_PATH + 8];
    _snwprintf(tmp, MAX_PATH + 8, L"%ls.tmp", path);
    tmp[MAX_PATH + 7] = 0;
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        wchar_t why[256];
        util_format_win32(why, 256, L"", GetLastError());
        _snwprintf(err, errcap, L"Unable to write file:\n%ls\n\nReason:\n%ls", path, why); err[errcap - 1] = 0;
        return 0;
    }
    const unsigned char *p = (const unsigned char *)data;
    size_t done = 0;
    int ok = 1;
    while (done < len) {
        DWORD chunk = (DWORD)((len - done) > 0x100000 ? 0x100000 : (len - done)), w = 0;
        if (!WriteFile(h, p + done, chunk, &w, NULL) || !w) { ok = 0; break; }
        done += w;
    }
    if (ok) ok = FlushFileBuffers(h) != 0;
    DWORD e = GetLastError();
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) {
        if (!e) e = GetLastError();
        DeleteFileW(tmp);
        wchar_t why[256];
        util_format_win32(why, 256, L"", e ? e : GetLastError());
        _snwprintf(err, errcap, L"Unable to write file:\n%ls\n\nReason:\n%ls", path, why); err[errcap - 1] = 0;
        return 0;
    }
    return 1;
}
