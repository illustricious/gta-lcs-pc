/* Explicit diagnostic: validate the installed WAD and the original recompiled
 * MP3 decoder together, without starting a game or modifying a save. */
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_wad(FILE *f, void *buffer, size_t bytes) {
    int64_t position = _ftelli64(f);
    if (fread(buffer, 1, bytes, f) != bytes) return 0;
    unsigned char *p = buffer;
    for (size_t i = 0; i < bytes; i++) p[i] ^= ((position + i) & 1) ? 0x66 : 0xaf;
    return 1;
}

int radio_probe(CPU *c, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { rt_log("radio-probe: cannot open %s\n", path); return 1; }
    unsigned char header[12];
    uint32_t directories = 0, count = 0;
    if (!read_wad(f, header, sizeof header) || memcmp(header, "DAWL", 4)) {
        fclose(f); return 1;
    }
    _fseeki64(f, 2048, SEEK_SET);
    if (!read_wad(f, &directories, 4) || directories > 100000) { fclose(f); return 1; }
    _fseeki64(f, directories * 8ull, SEEK_CUR);
    if (!read_wad(f, &count, 4) || !count || count > 100000) { fclose(f); return 1; }
    unsigned char *entries = malloc((size_t)count * 48);
    if (!entries || !read_wad(f, entries, (size_t)count * 48)) { free(entries); fclose(f); return 1; }
    unsigned char *mp3 = malloc(256 * 1024);
    int16_t *pcm = malloc(65536);
    int failed = 0;
    if (!mp3 || !pcm || (int32_t)rt_invoke(c, rt_need("mpg123_init"), 0)) {
        free(entries); free(mp3); free(pcm); fclose(f); return 1;
    }
    for (uint32_t i = 0; i < count; i++) {
        unsigned char *entry = entries + i * 48;
        uint32_t hash; uint64_t offset, length;
        memcpy(&hash, entry, 4); memcpy(&offset, entry + 16, 8); memcpy(&length, entry + 24, 8);
        size_t bytes = length < 256 * 1024 ? (size_t)length : 256 * 1024;
        _fseeki64(f, offset, SEEK_SET);
        if (!read_wad(f, mp3, bytes)) { failed++; continue; }
        int error = 0;
        uint64_t decoder = rt_invoke(c, rt_need("mpg123_new"), 2, 0ull, (uint64_t)&error);
        if (!decoder) { rt_log("radio-probe: %08x decoder failed %d\n", hash, error); failed++; continue; }
        int result = (int32_t)rt_invoke(c, rt_need("mpg123_open_feed"), 1, decoder);
        if (!result) result = (int32_t)rt_invoke(c, rt_need("mpg123_feed"), 3, decoder, (uint64_t)mp3, (uint64_t)bytes);
        int peak = 0, channels = 0, encoding = 0;
        int64_t rate = 0;
        size_t total = 0;
        if (!result) for (int attempt = 0; attempt < 16 && !peak; attempt++) {
            uint64_t done = 0;
            result = (int32_t)rt_invoke(c, rt_need("mpg123_read"), 4, decoder, (uint64_t)pcm, 65536ull, (uint64_t)&done);
            if (result == -11) { /* MPG123_NEW_FORMAT */
                rt_invoke(c, rt_need("mpg123_getformat"), 4, decoder, (uint64_t)&rate, (uint64_t)&channels, (uint64_t)&encoding);
                continue;
            }
            if (done > 65536) break;
            total += (size_t)done;
            if (encoding == 0xd0) for (size_t j = 0; j < done / 2; j++) {
                int amplitude = pcm[j] < 0 ? -(int)pcm[j] : pcm[j];
                if (amplitude > peak) peak = amplitude;
            }
            if (result && result != -10) break; /* MPG123_NEED_MORE */
        }
        rt_log("radio-probe: %08x %lld Hz %d ch encoding=%#x decoded=%zu peak=%d %s\n",
               hash, (long long)rate, channels, encoding, total, peak, peak ? "PASS" : "FAIL");
        if (!peak) failed++;
        rt_invoke(c, rt_need("mpg123_delete"), 1, decoder);
    }
    rt_invoke(c, rt_need("mpg123_exit"), 0);
    rt_log("radio-probe: %u entries, %d failed\n", count, failed);
    free(entries); free(mp3); free(pcm); fclose(f);
    return failed ? 1 : 0;
}
