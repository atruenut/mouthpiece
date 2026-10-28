#include "wav.h"
#include <string.h>

static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

int wav_read_info(FILE *f, WavInfo *w)
{
    unsigned char h[12], ck[8];
    if (fseek(f, 0, SEEK_END) != 0) return 0;
    long size = ftell(f);
    if (size < 44 || fseek(f, 0, SEEK_SET) != 0) return 0;
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) return 0;
    int have_fmt = 0;
    memset(w, 0, sizeof *w);
    long pos = 12;
    while (pos + 8 <= size) {
        if (fseek(f, pos, SEEK_SET) != 0 || fread(ck, 1, 8, f) != 8) return 0;
        uint32_t len = rd32(ck + 4);
        if (memcmp(ck, "fmt ", 4) == 0) {
            unsigned char fm[16];
            if (len < 16 || fread(fm, 1, 16, f) != 16) return 0;
            if (rd16(fm) != 1) return 0;                           /* PCM only */
            w->channels = rd16(fm + 2);
            w->rate = rd32(fm + 4);
            w->bits = rd16(fm + 14);
            have_fmt = 1;
        } else if (memcmp(ck, "data", 4) == 0) {
            if (!have_fmt) return 0;
            long off = pos + 8;
            if ((long long)off + (long long)len > (long long)size) return 0;     /* truncated */
            w->data_offset = off;
            w->data_bytes = len;
            return w->channels >= 1 && w->bits == 16 && w->rate > 0;
        }
        pos += 8 + (long)len + (long)(len & 1);
    }
    return 0;
}

int wav_write_header(FILE *f, uint32_t rate, uint16_t ch, uint16_t bits, uint32_t data_bytes)
{
    unsigned char h[44];
    uint32_t block = (uint32_t)ch * bits / 8, avg = rate * block, riff = 36 + data_bytes;
#define W32(o, v) do { h[o] = (unsigned char)(v); h[o+1] = (unsigned char)((v) >> 8); h[o+2] = (unsigned char)((v) >> 16); h[o+3] = (unsigned char)((v) >> 24); } while (0)
    memcpy(h, "RIFF", 4); W32(4, riff); memcpy(h + 8, "WAVEfmt ", 8); W32(16, 16u);
    h[20] = 1; h[21] = 0; h[22] = (unsigned char)ch; h[23] = 0;
    W32(24, rate); W32(28, avg);
    h[32] = (unsigned char)block; h[33] = 0; h[34] = (unsigned char)bits; h[35] = 0;
    memcpy(h + 36, "data", 4); W32(40, data_bytes);
#undef W32
    return fwrite(h, 1, 44, f) == 44;
}

uint32_t crc32_update(uint32_t crc, const void *p, size_t n)
{
    static uint32_t table[256];
    static int init;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = 1;
    }
    const unsigned char *s = (const unsigned char *)p;
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ s[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
