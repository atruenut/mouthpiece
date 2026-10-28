#include "hash.h"
#include "utf8.h"
#include <stdlib.h>

static uint64_t fmix(uint64_t k)
{
    k ^= k >> 33; k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

void hash_init(HashState *h)
{
    h->a = 0xcbf29ce484222325ULL;
    h->b = 0x9E3779B97F4A7C15ULL;
    h->len = 0;
}

void hash_bytes(HashState *h, const void *p, size_t n)
{
    const unsigned char *s = (const unsigned char *)p;
    uint64_t a = h->a, b = h->b;
    for (size_t i = 0; i < n; i++) {
        a = (a ^ s[i]) * 0x100000001b3ULL;                    /* FNV-1a */
        b = (b + s[i] + 1) * 0x9E3779B97F4A7C15ULL;           /* independent multiplicative stream */
        b ^= b >> 29;
    }
    h->a = a; h->b = b; h->len += n;
}

void hash_u32(HashState *h, uint32_t v)
{
    unsigned char t[4] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    hash_bytes(h, t, 4);
}

void hash_wstr(HashState *h, const wchar_t *s, size_t n)
{
    size_t bytes = utf8_from_wide(s, n, NULL);
    char small[256];
    char *buf = bytes <= sizeof small ? small : (char *)malloc(bytes);
    if (!buf) { hash_bytes(h, "?", 1); return; }
    utf8_from_wide(s, n, buf);
    hash_bytes(h, buf, bytes);
    if (buf != small) free(buf);
    hash_bytes(h, "\0", 1);
}

Hash128 hash_final(const HashState *h)
{
    Hash128 r;
    uint64_t a = fmix(h->a ^ h->len), b = fmix(h->b + h->len);
    r.a = fmix(a ^ (b << 1));
    r.b = fmix(b ^ (a >> 1) ^ 0xA5A5A5A5A5A5A5A5ULL);
    return r;
}

int hash_eq(const Hash128 *x, const Hash128 *y) { return x->a == y->a && x->b == y->b; }

void hash_hex(const Hash128 *h, char out[33])
{
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i] = H[(h->a >> (60 - 4 * i)) & 15];
        out[16 + i] = H[(h->b >> (60 - 4 * i)) & 15];
    }
    out[32] = 0;
}

int hash_from_hex(const char *s, Hash128 *h)
{
    uint64_t v[2] = { 0, 0 };
    for (int i = 0; i < 32; i++) {
        char c = s[i];
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        v[i / 16] = (v[i / 16] << 4) | (uint64_t)d;
    }
    h->a = v[0]; h->b = v[1];
    return 1;
}
