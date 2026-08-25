/*
 * stock_ui.c -- 原厂 CubeGM UI 渲染器实现（操作模型复刻原厂）
 *
 * 与 frogui_libretro.c 的关系：本模块负责原厂界面的渲染+交互；
 * frogui 负责 libretro 启动/游戏 fork/键位学习/设置持久化。通过
 * stock_ui_handle_input() + stock_ui_render() 接入，A 启动游戏时
 * 取 stock_ui_selected_rom() 交给 frogui 的 request_game_launch()。
 */
#include "stock_ui.h"
#include "stock_dat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* ---------- 按键位（对齐 frogui CV_*） ---------- */
#define K_UP     4
#define K_DOWN   6
#define K_LEFT   7
#define K_RIGHT  5
#define K_A     13
#define K_B     14
#define K_X     12
#define K_Y     15
#define K_L     10
#define K_R     11
#define K_SEL    0
#define K_START  3

#define BTN(s, b) (((s) >> (b)) & 1)

/* ---------- 常量 ---------- */
const char *stock_cat_names[STOCK_CAT_COUNT] = {
    "ARCADE", "FC", "SFC", "MD", "GBA", "GB", "GBC", "PS", "ATARI"
};
const char *stock_cat_cn[STOCK_CAT_COUNT] = {
    "街机", "红白机", "超任", "世嘉MD", "GBA", "GAMEBOY", "GB彩色", "索尼PS", "雅达利"
};

/* 分类 → libretro 核心（对齐 FrogUI console_mappings，原厂 000-008 顺序） */
const char *stock_cat_cores[STOCK_CAT_COUNT] = {
    "/mnt/sdcard/cubegm/cores/fbalpha2012_libretro.so",   /* 000 ARCADE */
    "/mnt/sdcard/cubegm/cores/fceumm_libretro.so",        /* 001 FC */
    "/mnt/sdcard/cubegm/cores/snes9x2005_libretro.so",    /* 002 SFC */
    "/mnt/sdcard/cubegm/cores/picodrive_libretro.so",     /* 003 MD */
    "/mnt/sdcard/cubegm/cores/mgba_libretro.so",          /* 004 GBA */
    "/mnt/sdcard/cubegm/cores/gambatte_libretro.so",      /* 005 GB */
    "/mnt/sdcard/cubegm/cores/gambatte_libretro.so",      /* 006 GBC */
    "/mnt/sdcard/cubegm/cores/pcsx_rearmed_libretro.so",  /* 007 PS */
    "/mnt/sdcard/cubegm/cores/stella2014_libretro.so",    /* 008 ATARI */
};

const char *stock_cat_core_for_rom(const char *rom_path) {
    if (!rom_path) return NULL;
    /* 解析 /mnt/sdcard/NNN/xxx */
    const char *p = strrchr(rom_path, '/');
    if (!p || p == rom_path) return NULL;
    const char *dir = p + 1;   /* 文件名 */
    (void)dir;
    /* 找倒数第二段 = 分类目录名 */
    char path[320]; snprintf(path, sizeof path, "%s", rom_path);
    char *sl = strrchr(path, '/'); if (!sl) return NULL;
    *sl = 0;
    char *sl2 = strrchr(path, '/');
    const char *cat = sl2 ? sl2 + 1 : path;
    if (strlen(cat) != 3) return NULL;
    int n = atoi(cat);
    if (n < 0 || n >= STOCK_CAT_COUNT) return NULL;
    return stock_cat_cores[n];
}

static const char *STOCK_TAGS[STOCK_PAGE_COUNT] = {
    "列表", "分类", "历史", "收藏", "搜索"
};

/* 背景资源路径（payload res/ 下，5 张原厂整屏） */
static const char *BG_FILES[STOCK_PAGE_COUNT] = {
    "/mnt/sdcard/cubegm/res/menu.rgb565",
    "/mnt/sdcard/cubegm/res/type.rgb565",
    "/mnt/sdcard/cubegm/res/menu.rgb565",
    "/mnt/sdcard/cubegm/res/menu.rgb565",
    "/mnt/sdcard/cubegm/res/search.rgb565",
};

/* ---------- 状态 ---------- */
static bool g_init = false;
static bool g_active = true;          /* stock UI 接管（暂停 FrogUI 文件夹 UI） */
static int  g_page = STOCK_PAGE_LIST;
static int  g_cat  = 0;               /* 当前分类（0-8） */
static int  g_sel  = 0;               /* 当前选中行 */
static int  g_scroll = 0;
static int  g_fav_sel = 0, g_fav_scroll = 0;
static char g_search[64];             /* 搜索关键字 */
static int  g_search_sel = 0, g_search_scroll = 0;

/* 当前分类游戏列表（StockEntry 数组） */
static StockEntry *g_list = NULL;
static int g_list_count = 0;
static int g_list_cap = 0;

/* 聚合总表（搜索/历史/收藏用） */
static StockEntry *g_all = NULL;
static int g_all_count = 0;
static int g_all_cap = 0;

/* 收藏列表（行号索引） */
static int *g_fav = NULL; static int g_fav_count = 0, g_fav_cap = 0;

/* 历史列表（最多 30） */
static char g_history[30][160]; static int g_history_count = 0;

/* 封面缓冲 */
static uint8_t *g_cover_buf = NULL;
static bool g_cover_valid = false;

/* 背景缓冲（5 张全尺寸 = 9.2MB，一次性载入） */
static uint16_t *g_bg[STOCK_PAGE_COUNT] = {0};

/* 启动请求（边沿时置 1；frogui 轮询并清零） */
int g_launch_req = 0;

/* ---------- 内部 ---------- */
static void add_entry(StockEntry **arr, int *cnt, int *cap, const StockEntry *e) {
    if (*cnt >= *cap) {
        int nc = *cap ? *cap * 2 : 256;
        StockEntry *na = realloc(*arr, (size_t)nc * sizeof(StockEntry));
        if (!na) return;
        *arr = na; *cap = nc;
    }
    (*arr)[*cnt] = *e;
    (*cnt)++;
}

static int load_bg(int page) {
    if (g_bg[page]) return 0;
    FILE *f = fopen(BG_FILES[page], "rb");
    if (!f) return -1;
    g_bg[page] = malloc(STOCK_SCREEN_W * STOCK_SCREEN_H * 2);
    if (!g_bg[page]) { fclose(f); return -1; }
    size_t r = fread(g_bg[page], 2, STOCK_SCREEN_W * STOCK_SCREEN_H, f);
    fclose(f);
    if (r != (size_t)(STOCK_SCREEN_W * STOCK_SCREEN_H)) { free(g_bg[page]); g_bg[page]=NULL; return -1; }
    return 0;
}

static void load_cat(int cat) {
    g_cat = cat; g_sel = 0; g_scroll = 0;
    StockGameEntry se[512];
    int n = stock_list_load(cat, se, 512);
    /* 重新建当前分类列表 */
    g_list_count = 0;
    if (g_list_cap < 600) { free(g_list); g_list = calloc(600, sizeof(StockEntry)); g_list_cap = 600; }
    if (n > 0) {
        for (int i = 0; i < n && i < 600; i++) {
            g_list[g_list_count].cat = cat;
            snprintf(g_list[g_list_count].rom, sizeof(g_list[0].rom), "%s", se[i].rom);
            snprintf(g_list[g_list_count].en, sizeof(g_list[0].en), "%s", se[i].en);
            snprintf(g_list[g_list_count].cn, sizeof(g_list[0].cn), "%s", se[i].cn);
            g_list_count++;
        }
    }
}

static void load_all(void) {
    g_all_count = 0;
    for (int c = 0; c < STOCK_CAT_COUNT; c++) {
        StockGameEntry se[512];
        int n = stock_list_load(c, se, 512);
        for (int i = 0; i < n && i < 512; i++) {
            StockEntry e; memset(&e, 0, sizeof e);
            e.cat = c;
            snprintf(e.rom, sizeof e.rom, "%s", se[i].rom);
            snprintf(e.en,  sizeof e.en,  "%s", se[i].en);
            snprintf(e.cn,  sizeof e.cn,  "%s", se[i].cn);
            add_entry(&g_all, &g_all_count, &g_all_cap, &e);
        }
    }
}

static void load_favs(void) {
    g_fav_count = 0;
    FILE *f = fopen("/mnt/sdcard/cubegm/favorites.lst", "r");
    if (!f) return;
    char line[512];
    while (g_fav_count < 100 && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        /* 原厂格式：分类/文件;英文;大写;中文;缩写 或 文件 */
        /* 简化：记录 rom 名 */
        if (g_fav_count >= g_fav_cap) { g_fav_cap = g_fav_cap ? g_fav_cap*2 : 32; g_fav = realloc(g_fav ? g_fav : NULL, g_fav_cap*sizeof(int)); }
        /* 在 all 中找 */
        char *sl = strrchr(line, '/');
        const char *rom = sl ? sl+1 : line;
        char *sc = strchr(rom, ';'); if (sc) *sc = 0;
        for (int i = 0; i < g_all_count; i++) {
            if (strcmp(g_all[i].rom, rom) == 0) { g_fav[g_fav_count++] = i; break; }
        }
    }
    fclose(f);
}

static void save_favs(void) {
    FILE *f = fopen("/mnt/sdcard/cubegm/favorites.lst", "w");
    if (!f) return;
    for (int i = 0; i < g_fav_count; i++) {
        fprintf(f, "%03d/%s;%s;%s\n", g_all[g_fav[i]].cat, g_all[g_fav[i]].rom,
                g_all[g_fav[i]].en, g_all[g_fav[i]].cn);
    }
    fclose(f);
}

static void push_history(int idx) {
    if (idx < 0 || idx >= g_all_count) return;
    /* shift */
    for (int i = 29; i > 0; i--) strcpy(g_history[i], g_history[i-1]);
    snprintf(g_history[0], 160, "%03d/%s", g_all[idx].cat, g_all[idx].rom);
    FILE *f = fopen("/mnt/sdcard/cubegm/recent.lst", "w");
    if (f) { for (int i = 0; i < 30 && g_history[i][0]; i++) fprintf(f, "%s\n", g_history[i]); fclose(f); }
}

/* 封面预载：idx 为 all 全局索引 */
static void preload_cover(int idx) {
    g_cover_valid = false;
    if (idx < 0 || idx >= g_all_count) return;
    int cat = g_all[idx].cat;
    StockDatHandle h;
    if (stock_dat_open(&h, cat) != 0) return;
    StockCoverIdx cidx[4000];
    int nc = stock_cover_index_load(cat, cidx, 4000);
    if (nc > 0) {
        /* 封面索引顺序 == 列表顺序；当前游戏行号 = all 中该分类内的行号 */
        int row = 0;
        for (int i = 0; i < idx; i++) if (g_all[i].cat == cat) row++;
        if (row < nc && row >= 0) {
            if (!g_cover_buf) g_cover_buf = malloc(STOCK_COVER_BYTES);
            if (stock_cover_load(&h, cidx, row, g_cover_buf) == 0) g_cover_valid = true;
        }
    }
    stock_dat_close(&h);
}

/* 当前选中游戏在 all 中的索引（分类列表 g_sel -> all 索引） */
static int sel_all_idx(void) {
    if (g_page != STOCK_PAGE_LIST || g_sel >= g_list_count) return -1;
    const char *rom = g_list[g_sel].rom;
    for (int i = 0; i < g_all_count; i++) {
        if (g_all[i].cat == g_cat && strcmp(g_all[i].rom, rom) == 0) return i;
    }
    return -1;
}

static void preload_cover_local(void) {
    int idx = sel_all_idx();
    static int last_idx = -2;
    if (idx != last_idx) { last_idx = idx; preload_cover(idx); }
}

/* ---------- 渲染原语（小工具） ---------- */
static void blit_bg(uint16_t *fb, int page) {
    if (g_bg[page]) memcpy(fb, g_bg[page], STOCK_SCREEN_W * STOCK_SCREEN_H * 2);
    else memset(fb, 0, STOCK_SCREEN_W * STOCK_SCREEN_H * 2);
}

static void fill(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x + w > STOCK_SCREEN_W) w = STOCK_SCREEN_W - x;
    if (y + h > STOCK_SCREEN_H) h = STOCK_SCREEN_H - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        uint16_t *row = fb + yy * STOCK_SCREEN_W + x;
        for (int xx = 0; xx < w; xx++) row[xx] = c;
    }
}

extern void font_draw_text(uint16_t *fb, int sw, int sh, int x, int y,
                           const char *text, uint16_t color);

static void draw_text(uint16_t *fb, int x, int y, const char *t, uint16_t c) {
    if (t && t[0]) font_draw_text(fb, STOCK_SCREEN_W, STOCK_SCREEN_H, x, y, t, c);
}

#define C_TAG_SEL    0x13DF   /* 原厂蓝高亮 */
#define C_TAG_TXT    0xFFFF
#define C_ROW_HL     0x1BDE   /* 选中条目高亮（半透明蓝） */
#define C_ROW_BG     0x5AEB
#define C_TXT        0xFFFF
#define C_TXT_DIM    0xBDF7
#define C_BOX        0x7BEF
#define C_SRC        0xFFE0

/* 顶部 5 标签 */
static void draw_tags(uint16_t *fb) {
    int x = STOCK_TAG_FIRST_X;
    for (int i = 0; i < STOCK_PAGE_COUNT; i++) {
        int w = 140;
        if (i == g_page) {
            fill(fb, x, STOCK_TAG_Y, w, STOCK_TAG_H, C_TAG_SEL);
            draw_text(fb, x + 30, STOCK_TAG_Y + 18, STOCK_TAGS[i], 0x0000);
        } else {
            fill(fb, x, STOCK_TAG_Y, w, STOCK_TAG_H, 0x2104);
            draw_text(fb, x + 30, STOCK_TAG_Y + 18, STOCK_TAGS[i], C_TAG_TXT);
        }
        x += w + STOCK_TAG_GAP;
    }
}

/* 左侧列表（当前分类） */
static void draw_list(uint16_t *fb) {
    int y = STOCK_LIST_Y0;
    for (int i = 0; i < 10; i++) {
        int idx = g_scroll + i;
        if (idx >= g_list_count) break;
        bool sel = (idx == g_sel);
        fill(fb, STOCK_LIST_X, y, STOCK_LIST_W, STOCK_LIST_H,
             sel ? C_ROW_HL : C_ROW_BG);
        /* 游戏名：优先中文，其次英文 */
        const char *nm = g_list[idx].cn[0] ? g_list[idx].cn : g_list[idx].en;
        draw_text(fb, STOCK_LIST_X + 16, y + 16, nm, sel ? 0x0000 : C_TXT);
        y += STOCK_LIST_H + STOCK_LIST_GAP;
    }
}

/* 当前选中封面 */
static void draw_cover(uint16_t *fb) {
    fill(fb, STOCK_COVER_X - 4, STOCK_COVER_Y - 4,
         STOCK_COVER_W + 8, STOCK_COVER_H + 8, C_BOX);
    fill(fb, STOCK_COVER_X, STOCK_COVER_Y, STOCK_COVER_W, STOCK_COVER_H, 0x0000);
    if (g_cover_valid) {
        for (int yy = 0; yy < STOCK_COVER_H; yy++) {
            const uint16_t *src = (const uint16_t *)g_cover_buf + yy * STOCK_COVER_W;
            uint16_t *dst = fb + (STOCK_COVER_Y + yy) * STOCK_SCREEN_W + STOCK_COVER_X;
            memcpy(dst, src, STOCK_COVER_W * 2);
        }
    } else {
        if (g_sel < g_list_count) {
            const char *nm = g_list[g_sel].cn[0] ? g_list[g_sel].cn : g_list[g_sel].en;
            draw_text(fb, STOCK_COVER_X + 80, STOCK_COVER_Y + 150, nm, 0x8410);
        }
    }
}

/* 分类页 9 卡片 */
static void draw_type(uint16_t *fb) {
    int y = 110;
    for (int i = 0; i < STOCK_CAT_COUNT; i++) {
        bool sel = (i == g_cat);
        fill(fb, 100, y, 520, 62, sel ? 0x07FF : 0x2104);
        draw_text(fb, 240, y + 20, stock_cat_cn[i], sel ? 0x0000 : C_TAG_TXT);
        draw_text(fb, 350, y + 20, stock_cat_names[i], sel ? 0x0000 : C_TAG_TXT);
        /* 封面数 */
        char cnt[32]; snprintf(cnt, sizeof cnt, "%d 款", g_list_count > 0 && i == g_cat ? g_list_count : 0);
        if (i == g_cat) draw_text(fb, 470, y+20, cnt, 0x0000);
        y += 68;
    }
}

/* 软键盘布局 */
static const char SEARCH_KEYS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -.";
static const int SEARCH_KEY_ROWS = 6;
static int key_rows[6] = {7, 7, 7, 7, 7, 5};

/* 渲染键盘（简化 5xN 网格） */
static void draw_keyboard(uint16_t *fb, int kx, int ky) {
    int k = 0;
    int kw = 72, kh = 52, gap = 8;
    for (int r = 0; r < 6; r++) {
        for (int c = 0; c < key_rows[r] && SEARCH_KEYS[k]; c++, k++) {
            int x = kx + c * (kw + gap);
            int y = ky + r * (kh + gap);
            fill(fb, x, y, kw, kh, 0x2945);
            char s[2] = {SEARCH_KEYS[k], 0};
            draw_text(fb, x + 22, y + 14, s, 0xFFFF);
            if (r == 5) draw_text(fb, x + 8, y + 14, c < 4 ? (c==0?"<":SEARCH_KEYS[k]=='.'?".":"0") : "OK", 0xFFFF);
        }
    }
}

static void draw_search(uint16_t *fb) {
    /* 输入框 */
    fill(fb, 60, 90, 560, 56, 0x1082);
    draw_text(fb, 80, 106, g_search[0] ? g_search : "输入游戏名", 0xE71C);
    /* 左侧匹配结果 */
    int y = 170;
    int shown = 0;
    for (int i = 0; i < g_all_count && shown < 10; i++) {
        const char *nm = g_all[i].cn[0] ? g_all[i].cn : g_all[i].en;
        if (g_search[0] && !strstr(nm, g_search) && !strstr(g_all[i].rom, g_search)) continue;
        if (g_search[0] == 0) break;
        bool sel = (g_search_sel == i);
        fill(fb, 60, y, 560, 44, sel ? C_ROW_HL : 0x3186);
        draw_text(fb, 80, y + 12, nm, sel ? 0x0000 : 0xFFFF);
        y += 50; shown++;
    }
    draw_keyboard(fb, 640, 120);
}

static void draw_history(uint16_t *fb) {
    int y = 110;
    for (int i = 0; i < 30 && g_history[i][0]; i++) {
        bool sel = (i == g_sel);
        fill(fb, 60, y, 560, 44, sel ? C_ROW_HL : 0x3186);
        draw_text(fb, 80, y + 12, g_history[i], sel ? 0x0000 : 0xFFFF);
        y += 50;
    }
}

static void draw_fav(uint16_t *fb) {
    int y = 110;
    int shown = 0;
    for (int i = 0; i < g_fav_count && shown < 10; i++) {
        int idx = g_fav[i];
        bool sel = (i == g_fav_sel);
        fill(fb, 60, y, 560, 44, sel ? C_ROW_HL : 0x3186);
        const char *nm = g_all[idx].cn[0] ? g_all[idx].cn : g_all[idx].en;
        draw_text(fb, 80, y + 12, nm, sel ? 0x0000 : 0xFFFF);
        y += 50; shown++;
    }
}

/* ---------- 输入 ---------- */
static void page_list_key(uint32_t k) {
    if (g_list_count == 0) return;
    if (BTN(k, K_UP) ) { g_sel = (g_sel - 1 + g_list_count) % g_list_count; if (g_sel < g_scroll) g_scroll = g_sel; }
    if (BTN(k, K_DOWN)) { g_sel = (g_sel + 1) % g_list_count; if (g_sel >= g_scroll + 10) g_scroll = g_sel - 9; }
    if (BTN(k, K_LEFT)) { if (g_sel > 0) g_sel--; if (g_sel < g_scroll) g_scroll = g_sel; }
    if (BTN(k, K_RIGHT)) { if (g_sel < g_list_count-1) g_sel++; if (g_sel >= g_scroll + 10) g_scroll = g_sel - 9; }
    if (BTN(k, K_A)) { /* 运行：存入 launch 请求状态（frogui 读） */ g_launch_req = 1; }
    if (BTN(k, K_R)) { g_page = STOCK_PAGE_TYPE; }
    if (BTN(k, K_L)) { g_launch_req = 1; }  /* 预留：游戏内设置 */
    if (BTN(k, K_SEL)) { /* 收藏当前分类行 */ stock_ui_toggle_fav(); }
}

static void page_type_key(uint32_t k) {
    if (BTN(k, K_UP))   g_cat = (g_cat - 1 + STOCK_CAT_COUNT) % STOCK_CAT_COUNT;
    if (BTN(k, K_DOWN)) g_cat = (g_cat + 1) % STOCK_CAT_COUNT;
    if (BTN(k, K_A))    { load_cat(g_cat); g_page = STOCK_PAGE_LIST; }
    if (BTN(k, K_L))    { g_page = STOCK_PAGE_LIST; }
    if (BTN(k, K_R))    { g_page = STOCK_PAGE_HISTORY; }
}

static void page_history_key(uint32_t k) {
    if (BTN(k, K_A) && g_history_count > 0) { /* 运行历史项（简化：进列表） */ g_page = STOCK_PAGE_LIST; }
    if (BTN(k, K_L)) { g_page = STOCK_PAGE_LIST; }
    if (BTN(k, K_R)) { g_page = STOCK_PAGE_FAV; }
    if (BTN(k, K_UP)) { if (g_sel>0) g_sel--; }
    if (BTN(k, K_DOWN)) { g_sel++; }
}

static void page_fav_key(uint32_t k) {
    if (g_fav_count == 0) return;
    if (BTN(k, K_UP))   { if (g_fav_sel>0) g_fav_sel--; }
    if (BTN(k, K_DOWN)) { g_fav_sel++; }
    if (BTN(k, K_A))    { int idx = g_fav[g_fav_sel]; g_all[idx].fav = false; /* 简化为移除 */ }
    if (BTN(k, K_L))    { g_page = STOCK_PAGE_HISTORY; }
    if (BTN(k, K_R))    { g_page = STOCK_PAGE_SEARCH; }
}

static void page_search_key(uint32_t k) {
    int len = (int)strlen(g_search);
    if (BTN(k, K_B)) { if (len > 0) g_search[--g_search_sel < 0 ? 0 : g_search_sel] = 0; g_search[len-1] = 0; }
    if (BTN(k, K_X)) { g_search[0] = 0; }
    if (BTN(k, K_L)) { g_page = STOCK_PAGE_FAV; }
    if (BTN(k, K_R)) { g_page = STOCK_PAGE_FAV; }
    if (BTN(k, K_UP)) { g_search_sel = g_search_sel > 0 ? g_search_sel - 1 : 0; }
    if (BTN(k, K_DOWN)) { g_search_sel++; }
    /* 简化输入：A 附加一个字母（循环） */
    if (BTN(k, K_A)) {
        int len = (int)strlen(g_search);
        if (len < 60) {
            g_search[len] = SEARCH_KEYS[g_search_sel >= 0 && g_search_sel < (int)sizeof(SEARCH_KEYS) - 1 ? g_search_sel : 0];
            g_search[len + 1] = 0;
        }
    }
    if (g_search_sel < 0) g_search_sel = 0;
}

/* ---------- 公开 API ---------- */
void stock_ui_init(void) {
    if (g_init) return;
    memset(g_history, 0, sizeof g_history);
    FILE *f = fopen("/mnt/sdcard/cubegm/recent.lst", "r");
    if (f) {
        char line[512]; int n = 0;
        while (n < 30 && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0]) { snprintf(g_history[n++], 160, "%s", line); }
        }
        g_history_count = n;
        fclose(f);
    }
    for (int i = 0; i < STOCK_PAGE_COUNT; i++) load_bg(i);
    load_all();
    load_favs();
    load_cat(0);
    if (!g_cover_buf) g_cover_buf = malloc(STOCK_COVER_BYTES);
    g_init = true;
    g_active = true;
}

void stock_ui_deinit(void) {
    g_init = false;
}

void stock_ui_handle_input(uint32_t keys) {
    if (!g_init) return;
    /* 边沿：只需处理新按下（frogui 已有 last 跟踪，这里简化：按键 keep + 语义幂等） */
    switch (g_page) {
        case STOCK_PAGE_LIST:    page_list_key(keys); break;
        case STOCK_PAGE_TYPE:    page_type_key(keys); break;
        case STOCK_PAGE_HISTORY: page_history_key(keys); break;
        case STOCK_PAGE_FAV:     page_fav_key(keys); break;
        case STOCK_PAGE_SEARCH:  page_search_key(keys); break;
        default: break;
    }
}

void stock_ui_render(uint16_t *fb) {
    if (!g_init) return;
    int bg = (g_page == STOCK_PAGE_TYPE) ? STOCK_PAGE_TYPE
            : (g_page == STOCK_PAGE_SEARCH) ? STOCK_PAGE_SEARCH
            : STOCK_PAGE_LIST;
    blit_bg(fb, bg);
    draw_tags(fb);
    switch (g_page) {
        case STOCK_PAGE_LIST:
            draw_list(fb);
            preload_cover_local();
            draw_cover(fb);
            break;
        case STOCK_PAGE_TYPE:
            draw_type(fb);
            break;
        case STOCK_PAGE_SEARCH:
            draw_search(fb);
            break;
        case STOCK_PAGE_HISTORY:
            draw_history(fb);
            break;
        case STOCK_PAGE_FAV:
            draw_fav(fb);
            break;
        default: break;
    }
    /* 底部按键提示 */
    draw_text(fb, 60, 665, "↑↓选择  ←→翻页  A运行  B返回  L1设置  R1分类  SELECT收藏", 0xBDF7);
}

bool stock_ui_active(void) { return g_active; }

const char *stock_ui_selected_rom(const char **core_out) {
    *core_out = NULL;
    if (g_page == STOCK_PAGE_LIST && g_sel < g_list_count && g_list_count > 0) {
        static char rom_path[512];
        snprintf(rom_path, sizeof rom_path, "/mnt/sdcard/%03d/%s",
                 g_cat, g_list[g_sel].rom);
        return rom_path;
    }
    return NULL;
}

void stock_ui_game_exited(void) {
}

void stock_ui_toggle_fav(void) {
    if (g_page != STOCK_PAGE_LIST || g_sel >= g_list_count) return;
    const char *rom = g_list[g_sel].rom;
    /* 在 all 中找 */
    for (int i = 0; i < g_all_count; i++) {
        if (g_all[i].cat == g_cat && strcmp(g_all[i].rom, rom) == 0) {
            g_all[i].fav = !g_all[i].fav;
            load_favs();
            save_favs();
            break;
        }
    }
}