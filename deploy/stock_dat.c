/*
 * stock_dat.c -- device-side reader for stock CubeGM category .dat containers
 *
 * Cover+list extraction pipeline:
 *   1. Build time (host, Python): generate covers/XXX.idx + gamelist/XXX.txt
 *   2. Device: this file fseeks a single cover (zlib inflate, no slurp)
 *
 * Compile: arm-gcc -O2 -Wall -march=armv7-a -mfpu=neon-vfpv4
 *          -mfloat-abi=hard --sysroot=$SYSROOT -fPIC -c stock_dat.c -o stock_dat.o
 */
#include "stock_dat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

static void cat_path(int cat, const char *kind, char *buf, size_t n) {
    (void)cat; (void)kind; (void)buf; (void)n;
}

int stock_dat_open(StockDatHandle *h, int cat) {
    if (!h) return -1;
    char p[256];
    snprintf(p, sizeof p, STOCK_DAT_BASE "/%02d/%02d.dat", cat, cat);
    h->fd = open(p, O_RDONLY);
    h->dat_path = strdup(p);
    return h->fd >= 0 ? 0 : -1;
}

void stock_dat_close(StockDatHandle *h) {
    if (!h) return;
    if (h->fd >= 0) close(h->fd);
    if (h->dat_path) free((void *)h->dat_path);
    h->fd = -1; h->dat_path = NULL;
}

int stock_cover_index_load(int cat, StockCoverIdx *idx, int capacity) {
    char p[256];
    snprintf(p, sizeof p, STOCK_DAT_BASE "/cubegm/covers/%02d.idx", cat);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    long sz;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    int n = (int)(sz / sizeof(StockCoverIdx));
    if (n > capacity) n = capacity;
    if (n > 0 && fread(idx, sizeof(StockCoverIdx), n, f) != (size_t)n) {
        fclose(f); return -1;
    }
    fclose(f);
    return n;
}

int stock_cover_load(StockDatHandle *h, const StockCoverIdx *idx, int n,
                     uint8_t *out) {
    if (!h || h->fd < 0 || !idx || !out) return -1;
    const StockCoverIdx *e = &idx[n];
    if (e->usize != STOCK_COVER_BYTES) return -2;

    if (lseek(h->fd, (off_t)e->dat_offset, SEEK_SET) < 0) return -1;

    /* Zip local header after "WQW\x03":
     *   [ver2 flags2 meth2 time2 date2 crc4 csize4 usize4 fnlen2 exlen2] = 26B
     *   then fname(fnlen) + extra(exlen), then zlib data.
     * We only need fnlen/exlen at offset 26..29. */
    unsigned char lens[4];
    if (lseek(h->fd, (off_t)e->dat_offset + 26, SEEK_SET) < 0) return -1;
    if (read(h->fd, lens, 4) != 4) return -1;
    uint16_t fnlen = lens[0] | (lens[1] << 8);
    uint16_t exlen = lens[2] | (lens[3] << 8);
    if (lseek(h->fd, (off_t)e->dat_offset + 30 + fnlen + exlen, SEEK_SET) < 0) return -1;

    uint8_t *comp = malloc(e->csize > 0 ? e->csize : 1);
    if (!comp) return -2;
    ssize_t r = read(h->fd, comp, e->csize);
    if (r != (ssize_t)e->csize) { free(comp); return -1; }

    uLongf dlen = STOCK_COVER_BYTES;
    int rc = uncompress(out, &dlen, comp, e->csize);
    free(comp);
    return (rc == Z_OK && dlen == STOCK_COVER_BYTES) ? 0 : -2;
}

int stock_list_load(int cat, StockGameEntry *entries, int capacity) {
    char p[256];
    snprintf(p, sizeof p, STOCK_DAT_BASE "/cubegm/gamelist/%02d.txt", cat);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;

    int n = 0;
    char line[512];
    while (n < capacity && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        char *p1 = strchr(line, '|');
        if (!p1) continue;
        *p1 = 0;
        char *p2 = strchr(p1 + 1, '|');
        if (p2) *p2 = 0;
        snprintf(entries[n].rom, sizeof entries[n].rom, "%s", line);
        snprintf(entries[n].en,  sizeof entries[n].en,  "%s", p1 + 1);
        snprintf(entries[n].cn,  sizeof entries[n].cn,  "%s", p2 ? p2 + 1 : "");
        n++;
    }
    fclose(f);
    return n;
}