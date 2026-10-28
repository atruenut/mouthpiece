#include "tts_remote.h"
#include "hexcodec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOST_EXE L"MouthpieceVoiceHost32.exe"
#define LINE_MAX_BYTES (1 << 20)
#define CALL_TIMEOUT_MS 30000

struct RemoteHost {
    HANDLE proc, to, from;       /* process, its stdin (we write), its stdout (we read) */
    int dead;
    char *rbuf; size_t rlen, rcap;   /* bytes read but not yet consumed */
};

int remote_host_path(wchar_t *out, size_t cap)
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    wchar_t *sl = wcsrchr(exe, L'\\');
    if (!sl) return 0;
    *sl = 0;
    _snwprintf(out, cap, L"%ls\\%ls", exe, HOST_EXE);
    out[cap - 1] = 0;
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

RemoteHost *remote_open(wchar_t *err, size_t errcap)
{
    wchar_t path[MAX_PATH + 4];
    if (!remote_host_path(path, MAX_PATH)) {
        _snwprintf(err, errcap, L"The 32-bit voice helper (%ls) is not next to the program.", HOST_EXE);
        return NULL;
    }
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE inR = NULL, inW = NULL, outR = NULL, outW = NULL;
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0)) {
        _snwprintf(err, errcap, L"Unable to create pipes for the voice helper.");
        return NULL;
    }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    wchar_t cmd[MAX_PATH + 8];
    _snwprintf(cmd, MAX_PATH + 8, L"\"%ls\"", path);
    cmd[MAX_PATH + 7] = 0;
    BOOL ok = CreateProcessW(path, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(inR);
    CloseHandle(outW);
    if (!ok) {
        wchar_t why[200];
        _snwprintf(why, 200, L"error %lu", GetLastError());
        _snwprintf(err, errcap, L"Unable to start the 32-bit voice helper (%ls).", why);
        CloseHandle(inW); CloseHandle(outR);
        return NULL;
    }
    CloseHandle(pi.hThread);
    RemoteHost *h = (RemoteHost *)calloc(1, sizeof *h);
    if (!h) { TerminateProcess(pi.hProcess, 1); CloseHandle(pi.hProcess); CloseHandle(inW); CloseHandle(outR); return NULL; }
    h->proc = pi.hProcess; h->to = inW; h->from = outR;
    return h;
}

int remote_alive(RemoteHost *h) { return h && !h->dead && WaitForSingleObject(h->proc, 0) == WAIT_TIMEOUT; }

void remote_close(RemoteHost *h)
{
    if (!h) return;
    if (!h->dead) {
        DWORD n;
        WriteFile(h->to, "E\n", 2, &n, NULL);
        if (WaitForSingleObject(h->proc, 1500) != WAIT_OBJECT_0) TerminateProcess(h->proc, 0);
    }
    CloseHandle(h->to); CloseHandle(h->from); CloseHandle(h->proc);
    free(h->rbuf);
    free(h);
}

static void kill_host(RemoteHost *h)
{
    if (!h->dead) { TerminateProcess(h->proc, 1); h->dead = 1; }
}

/* Reads one line (without the newline) into a malloc'd string. timeout_ms < 0 = wait forever.
 * Returns NULL on failure/timeout/cancel (*why: 0 fail, 1 cancelled). */
static char *read_line(RemoteHost *h, int timeout_ms, volatile LONG *cancel, int *why)
{
    DWORD start = GetTickCount();
    *why = 0;
    for (;;) {
        for (size_t i = 0; i < h->rlen; i++) {
            if (h->rbuf[i] == '\n') {
                char *line = (char *)malloc(i + 1);
                if (!line) return NULL;
                memcpy(line, h->rbuf, i);
                line[i] = 0;
                if (i && line[i - 1] == '\r') line[i - 1] = 0;
                memmove(h->rbuf, h->rbuf + i + 1, h->rlen - i - 1);
                h->rlen -= i + 1;
                return line;
            }
        }
        if (cancel && *cancel) { *why = 1; return NULL; }
        DWORD avail = 0;
        if (!PeekNamedPipe(h->from, NULL, 0, NULL, &avail, NULL)) return NULL;     /* helper gone */
        if (avail == 0) {
            if (WaitForSingleObject(h->proc, 0) == WAIT_OBJECT_0) return NULL;
            if (timeout_ms >= 0 && (int)(GetTickCount() - start) > timeout_ms) return NULL;
            Sleep(5);
            continue;
        }
        if (h->rlen + avail + 1 > h->rcap) {
            size_t nc = h->rcap ? h->rcap : 4096;
            while (nc < h->rlen + avail + 1) nc *= 2;
            if (nc > LINE_MAX_BYTES * 2) return NULL;
            char *nb = (char *)realloc(h->rbuf, nc);
            if (!nb) return NULL;
            h->rbuf = nb; h->rcap = nc;
        }
        DWORD got = 0;
        if (!ReadFile(h->from, h->rbuf + h->rlen, avail, &got, NULL) || got == 0) return NULL;
        h->rlen += got;
    }
}

static int send_line(RemoteHost *h, const char *line)
{
    size_t n = strlen(line);
    char *b = (char *)malloc(n + 1);
    if (!b) return 0;
    memcpy(b, line, n);
    b[n] = '\n';
    DWORD w = 0;
    BOOL ok = WriteFile(h->to, b, (DWORD)(n + 1), &w, NULL) && w == n + 1;
    free(b);
    return ok;
}

static void set_reply_error(const char *line, wchar_t *err, size_t errcap)
{
    wchar_t *m = NULL;
    if (line && !strncmp(line, "ERR ", 4)) m = hex_decode(line + 4, strlen(line + 4));
    _snwprintf(err, errcap, L"%ls", m ? m : L"The voice helper reported an error.");
    err[errcap - 1] = 0;
    free(m);
}

int remote_call(RemoteHost *h, const char *line, char *reply, size_t replycap, wchar_t *err, size_t errcap)
{
    int why;
    if (h->dead) { _snwprintf(err, errcap, L"The 32-bit voice helper has stopped."); return 0; }
    if (!send_line(h, line)) { kill_host(h); _snwprintf(err, errcap, L"Lost contact with the 32-bit voice helper."); return 0; }
    char *r = read_line(h, CALL_TIMEOUT_MS, NULL, &why);
    if (!r) { kill_host(h); _snwprintf(err, errcap, L"The 32-bit voice helper did not answer."); return 0; }
    int ok = !strncmp(r, "OK", 2);
    if (ok) {
        if (reply && replycap) {
            const char *p = r + 2;
            if (*p == ' ') p++;
            strncpy(reply, p, replycap - 1);
            reply[replycap - 1] = 0;
        }
    } else set_reply_error(r, err, errcap);
    free(r);
    return ok;
}

int remote_render(RemoteHost *h, const char *line, volatile LONG *cancel, wchar_t *err, size_t errcap)
{
    int why;
    if (h->dead) { _snwprintf(err, errcap, L"The 32-bit voice helper has stopped."); return 0; }
    if (!send_line(h, line)) { kill_host(h); _snwprintf(err, errcap, L"Lost contact with the 32-bit voice helper."); return 0; }
    char *r = read_line(h, -1, cancel, &why);
    if (!r) {
        kill_host(h);                                    /* cancelling = stopping the helper; a new one starts when needed */
        if (why == 1) return -1;
        _snwprintf(err, errcap, L"The 32-bit voice helper stopped while rendering.");
        return 0;
    }
    int ok = !strncmp(r, "OK", 2);
    if (!ok) set_reply_error(r, err, errcap);
    free(r);
    return ok;
}

int remote_list_voices(RemoteHost *h, TTSVoiceList *out, wchar_t *err, size_t errcap)
{
    char head[64];
    int why;
    out->items = NULL;
    out->count = 0;
    if (!remote_call(h, "V", head, sizeof head, err, errcap)) return 0;
    int n = atoi(head);
    if (n < 0 || n > 2000) return 0;
    if (n == 0) return 1;
    out->items = (TTSVoice *)calloc((size_t)n, sizeof(TTSVoice));
    if (!out->items) return 0;
    for (int i = 0; i < n; i++) {
        char *l = read_line(h, CALL_TIMEOUT_MS, NULL, &why);
        if (!l) { kill_host(h); tts_voices_free(out); _snwprintf(err, errcap, L"The 32-bit voice helper stopped while listing voices."); return 0; }
        char *sp = strchr(l, ' ');
        if (sp) {
            *sp = 0;
            wchar_t *id = hex_decode(l, strlen(l)), *nm = hex_decode(sp + 1, strlen(sp + 1));
            if (id && nm && wcslen(id) < TTS_ID_MAX) {
                wcsncpy(out->items[out->count].id, id, TTS_ID_MAX - 1);
                wcsncpy(out->items[out->count].name, nm, TTS_NAME_MAX - 1);
                out->count++;
            }
            free(id); free(nm);
        }
        free(l);
    }
    return 1;
}
