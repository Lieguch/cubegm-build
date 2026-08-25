/*
 * stock_dat.h -- read stock CubeGM category .dat containers (covers + game list)
 *
 * Stock layout (verified 2026-08-25 on 原厂SD卡根目录结构/000-008):
 *   - 000-008/*.dat = "WQW\x03"-magic obfuscated ZIP container
 *   - Each entry: [WQW\x03][zip-local-header]...[zlib data]
 *       usize 307200  -> 480x320 RGB565 cover (one per ROM, ORDER == list order)
 *       10th/.. entry -> game list text "rom.zip;English;中文" GBK (with 008.dat:
 *       9 covers + 1 text, list rows map 1:1 to cover indices)
 *   - 000.dat = 3905 covers + 1 list (8514 rows)   (240MB -> seek/stream read!)
 *
 * Device-side constraints (244MB RAM): never load the whole .dat. Build-time
 * generator extracts a tiny index (covers/*.idx) + UTF-8 list (gamelist/*.txt)
 * into the payload; this reader fseeks to the cover offset and zlib-inflates
 * ONLY the requested 480x320 frame (307200B).
 */
#ifndef STOCK_DAT_H
#define STOCK_DAT_H

#include <stdint.h>
#include <stddef.h>

#define STOCK_CAT_COUNT 9
#define STOCK_COVER_W   480
#define STOCK_COVER_H   320
#define STOCK_COVER_BYTES (480*320*2)   /* 307200 */
#define STOCK_DAT_BASE  "/mnt/sdcard"   /* SD root holds 000..008 dirs */

/* Cover-index entry (payload file covers/XXX.idx; struct must be 16B) */
typedef struct {
    uint32_t dat_offset;   /* absolute offset of this entry in XXX.dat */
    uint32_t csize;        /* compressed size (bytes) */
    uint32_t usize;        /* uncompressed size (307200) */
    uint32_t reserved;
} StockCoverIdx;

/* Game list entry (payload file gamelist/XXX.txt, UTF-8, "rom|en|cn") */
typedef struct {
    char rom[160];
    char en[160];
    char cn[160];
} StockGameEntry;

/* --- cover loading ------------------------------------------------------- */
typedef struct { int fd; const char *dat_path; } StockDatHandle;

/* Open category .dat (path may be NULL -> auto /mnt/sdcard/XXX/XXX.dat).
 * Returns 0 ok, -1 open error. Caller closes via stock_dat_close(). */
int  stock_dat_open(StockDatHandle *h, int cat);
void stock_dat_close(StockDatHandle *h);

/* Load cover index for category into caller buffer (capacity 1MB). Returns
 * count of covers, -1 on error. idx must hold count*16 bytes. */
int  stock_cover_index_load(int cat, StockCoverIdx *idx, int capacity);

/* Inflate cover #n of open handle into out (must be STOCK_COVER_BYTES).
 * Returns 0 ok; -1 seek/read; -2 inflate. */
int  stock_cover_load(StockDatHandle *h, const StockCoverIdx *idx, int n,
                      uint8_t *out);

/* --- game list ----------------------------------------------------------- */
/* Load UTF-8 game list for category (read full text file), returns:

 * number of lines, -1 error. Parses "rom|en|cn" into entries (capacity cap). */
int  stock_list_load(int cat, StockGameEntry *entries, int capacity);

#endif /* STOCK_DAT_H */