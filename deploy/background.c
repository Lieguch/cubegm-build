/* background.c — 原厂背景图加载（复刻原厂 UI）
 *
 * 让 FrogUI 从"纯色背景"升级为"原厂背景图"：
 *   - 加载 /mnt/sdcard/cubegm/res/*.rgb565（原厂背景转成的 RGB565 raw）
 *   - 1280×720 整屏背景，替代 render_clear_screen 的纯色填充
 *
 * 与 render.c 的 load_raw_rgb565() 机制一致（RGB565 raw，little-endian）。
 *
 * 集成方式：
 *   1. 在 frogui_libretro.c 的 retro_init() 里调用 background_init()
 *   2. 在渲染主循环里调用 background_render(framebuffer) 替代 render_clear_screen
 *
 * 内存：1280×720 RGB565 = 1.8MB（静态分配，设备 1GB 内存足够）
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "render.h"   /* SCREEN_WIDTH/SCREEN_HEIGHT */
#include "font.h"

#define RES_DIR "/mnt/sdcard/cubegm/res"

/* 5 个界面的背景图文件名 */
typedef enum {
    BG_MENU = 0,
    BG_SEARCH,
    BG_SETTING,
    BG_TYPE,
    BG_GAME,
    BG_COUNT
} BackgroundType;

static const char *bg_files[BG_COUNT] = {
    "menu.rgb565",
    "search.rgb565",
    "setting.rgb565",
    "type.rgb565",
    "game.rgb565",
};

/* 静态背景图缓冲（1280×720 RGB565 = 1.8MB） */
static uint16_t *g_background = NULL;
static BackgroundType g_current_bg = BG_MENU;
static int g_bg_loaded = 0;

/* 加载背景图 */
int background_load(BackgroundType type) {
    if (type < 0 || type >= BG_COUNT) return 0;

    char path[256];
    snprintf(path, sizeof path, "%s/%s", RES_DIR, bg_files[type]);

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "background: cannot open %s\n", path);
        return 0;
    }

    /* 检查文件大小 = 1280×720×2 */
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size != SCREEN_WIDTH * SCREEN_HEIGHT * 2) {
        fprintf(stderr, "background: %s size %ld != expected %d\n",
                path, size, SCREEN_WIDTH * SCREEN_HEIGHT * 2);
        fclose(fp);
        return 0;
    }

    /* 分配背景缓冲（只分配一次） */
    if (!g_background) {
        g_background = (uint16_t *)malloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));
        if (!g_background) {
            fprintf(stderr, "background: malloc failed\n");
            fclose(fp);
            return 0;
        }
    }

    size_t read_bytes = fread(g_background, 1, size, fp);
    fclose(fp);

    if (read_bytes != (size_t)size) {
        fprintf(stderr, "background: read %zu != %ld\n", read_bytes, size);
        return 0;
    }

    g_current_bg = type;
    g_bg_loaded = 1;
    return 1;
}

/* 渲染背景到 framebuffer（替代 render_clear_screen 的纯色填充） */
void background_render(uint16_t *framebuffer) {
    if (!framebuffer) return;

    if (g_bg_loaded && g_background) {
        /* blit 背景图 */
        memcpy(framebuffer, g_background,
               SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));
    } else {
        /* 回退：纯色填充（原行为） */
        render_clear_screen(framebuffer);
    }
}

/* 初始化：加载默认背景（菜单） */
void background_init(void) {
    background_load(BG_MENU);
}

/* 切换背景（进入搜索/设置/分类/游戏界面时调用） */
void background_switch(BackgroundType type) {
    if (type != g_current_bg) {
        background_load(type);
    }
}

/* 释放 */
void background_deinit(void) {
    if (g_background) {
        free(g_background);
        g_background = NULL;
    }
    g_bg_loaded = 0;
}
