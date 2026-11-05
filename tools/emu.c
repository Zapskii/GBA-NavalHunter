/* The real ROM in libmgba with no window, driven over stdin so tools/e2e.py can read
 * the game's state and press its buttons. One command a line, one reply line each:
 *   run N KEYS       N frames with the KEYS mask held (GBA bits: A 1, B 2, START 8...)
 *   peek ADDR N      N bytes as hex
 *   poke ADDR HEX    write bytes
 *   shot PATH        the screen, as a binary PPM
 *   reset            power-cycle the ROM; no save file is loaded, and libmgba keeps SRAM
 *   audio            the mixer's left output since the last audio: "samples peak rms"
 *   record PATH      also write that output to PATH, as raw signed 16-bit; "record -" stops
 */
#include <mgba/core/blip_buf.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void quiet(struct mLogger *l, int cat, enum mLogLevel lv, const char *f, va_list a)
{
    (void)l; (void)cat; (void)lv; (void)f; (void)a;
}

/* Take what the mixer made this frame. The buffers must be emptied anyway or they fill up. */
static unsigned long pcm_n;
static double pcm_sq;
static int pcm_peak;
static FILE *pcm_rec;
static void pcm_drain(struct mCore *core)
{
    static short l[8192], r[8192];
    int n = blip_samples_avail(core->getAudioChannel(core, 0)), i;
    if (n > 8192) n = 8192;
    blip_read_samples(core->getAudioChannel(core, 0), l, n, 0);
    blip_read_samples(core->getAudioChannel(core, 1), r, n, 0);
    for (i = 0; i < n; i++) {
        int a = abs(l[i]);
        if (a > pcm_peak) pcm_peak = a;
        pcm_sq += (double)l[i] * l[i];
    }
    pcm_n += n;
    if (pcm_rec) fwrite(l, sizeof *l, n, pcm_rec);
}

int main(int argc, char **argv)
{
    static struct mLogger log = { .log = quiet };
    struct mCore *core;
    unsigned w, h;
    color_t *buf;
    char line[8192];

    if (argc != 2) return 2;
    mLogSetDefaultLogger(&log);
    core = mCoreFind(argv[1]);
    if (!core || !core->init(core)) return 1;
    mCoreInitConfig(core, NULL);
    core->desiredVideoDimensions(core, &w, &h);
    buf = malloc(w * h * sizeof *buf);
    core->setVideoBuffer(core, buf, w);
    core->setAudioBufferSize(core, 2048);
    if (!mCoreLoadFile(core, argv[1])) return 1;
    core->reset(core);

    setvbuf(stdout, NULL, _IOLBF, 0);
    while (fgets(line, sizeof line, stdin)) {
        char path[4096], hex[8000];
        unsigned a, n, i, keys;
        if (!strcmp(line, "reset\n")) {
            core->reset(core);
            puts("ok");
        } else if (sscanf(line, "run %u %u", &n, &keys) == 2) {
            core->setKeys(core, keys);
            for (i = 0; i < n; i++) { core->runFrame(core); pcm_drain(core); }
            puts("ok");
        } else if (!strcmp(line, "audio\n")) {
            printf("%lu %d %.1f\n", pcm_n, pcm_peak, pcm_n ? sqrt(pcm_sq / pcm_n) : 0.0);
            pcm_n = 0; pcm_sq = 0; pcm_peak = 0;
        } else if (sscanf(line, "record %4095s", path) == 1) {
            if (pcm_rec) fclose(pcm_rec);
            pcm_rec = strcmp(path, "-") ? fopen(path, "wb") : NULL;
            puts("ok");
        } else if (sscanf(line, "peek %x %u", &a, &n) == 2) {
            for (i = 0; i < n; i++) printf("%02x", core->busRead8(core, a + i));
            puts("");
        } else if (sscanf(line, "poke %x %7999s", &a, hex) == 2) {
            for (i = 0; hex[i * 2]; i++) {
                unsigned b;
                sscanf(hex + i * 2, "%2x", &b);
                core->busWrite8(core, a + i, (uint8_t)b);
            }
            puts("ok");
        } else if (sscanf(line, "shot %4095s", path) == 1) {
            FILE *f = fopen(path, "wb");
            unsigned x, y;
            if (!f) { puts("err"); continue; }
            fprintf(f, "P6\n%u %u\n255\n", w, h);
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++) {
                    color_t c = buf[y * w + x];     /* mGBA's desktop builds: XBGR8 */
                    fputc(c & 0xFF, f); fputc(c >> 8 & 0xFF, f); fputc(c >> 16 & 0xFF, f);
                }
            fclose(f);
            puts("ok");
        } else puts("err");
    }
    return 0;
}
