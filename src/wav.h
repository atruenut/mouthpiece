/* wav.h - minimal RIFF/WAVE (PCM) reading and writing on stdio FILE*, plus CRC-32. PURE C. */
#ifndef WAV_H
#define WAV_H
#include <stdint.h>
#include <stdio.h>

typedef struct WavInfo {
    uint32_t rate, data_bytes;
    uint16_t channels, bits;
    long data_offset;               /* absolute offset of the first sample byte */
} WavInfo;

/* Parse the header of a PCM WAV (skips unknown chunks). Returns 1 only for plain PCM with a
 * data chunk that fits inside the file; 0 for anything else, including truncated files. */
int wav_read_info(FILE *f, WavInfo *w);
/* Write the canonical 44-byte header at the current position. */
int wav_write_header(FILE *f, uint32_t rate, uint16_t channels, uint16_t bits, uint32_t data_bytes);

uint32_t crc32_update(uint32_t crc, const void *p, size_t n);   /* start with crc = 0 */
#endif
