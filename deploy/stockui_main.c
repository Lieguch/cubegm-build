/*
 * stockui_main.c — 独立原厂 UI 程序（根源方案，弃用 FrogUI 打补丁）
 *
 * 架构：icube_replacement → exec ./stockui
 *   stockui 直接 DRM 渲染（hwdisp）+ evdev 输入 + 游戏 fork/exec
 *   不再依赖 frogui_libretro.so、cubevol_bridge、shm 中转
 *
 * 编译：arm-gcc -O2 -Wall -march=armv7-a -mfpu=neon-vfpv4
 *       -mfloat-abi=hard --sysroot=$SYSROOT -fPIC
 *       -I$HERE/drm_headers -I$SYSROOT/usr/include
 *       stockui_main.c hwdisp.c font.c stock_ui.c stock_dat.c evdev_input.c
 *       -o $DST/stockui -ldl -lz -lm -lasound
 */
#include "stock_ui.h"
#include "hwdisp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <time.h>

/* 输入位掩码（对齐 CV_* 定义，从 evdev 映射） */
extern uint32_t evdev_read_keys(void);
extern int evdev_init(void);

/* stock_ui.c 定义的启动请求标志 */
extern int g_launch_req;

/* 帧缓冲 */
static uint16_t *fb = NULL;

/* 启动游戏 */
static void launch_game(const char *rom_path) {
    if (!rom_path) return;
    const char *core = stock_cat_core_for_rom(rom_path);
    if (!core) { fprintf(stderr, "stockui: no core for %s\n", rom_path); return; }

    /* 释放音频（子进程重新打开） */
    /* 暂略：ALSA 释放 */

    pid_t pid = fork();
    if (pid == 0) {
        /* 子进程：exec picoarch */
        execl("/mnt/sdcard/cubegm/picoarch", "picoarch", core, rom_path, (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        /* 父进程：等待游戏结束 */
        int status;
        waitpid(pid, &status, 0);
        /* 恢复 DRM 显示 */
        hwdisp_restore();
        stock_ui_game_exited();
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* 1. DRM 显示初始化 */
    if (hwdisp_init() != 0) {
        fprintf(stderr, "stockui: hwdisp_init FAILED\n");
        return 1;
    }
    fb = malloc(1280 * 720 * 2);
    if (!fb) return 1;

    /* 2. 输入初始化 */
    if (evdev_init() != 0) {
        fprintf(stderr, "stockui: evdev_init FAILED (fallback to dummy)\n");
    }

    /* 3. 原厂 UI 初始化（加载列表、背景、字体） */
    stock_ui_init();

    /* 4. 主循环 */
    while (1) {
        /* 读输入 */
        uint32_t keys = evdev_read_keys();
        stock_ui_handle_input(keys);

        /* 渲染 */
        stock_ui_render(fb);
        hwdisp_present(fb, 1280, 720, 1280 * 2);

        /* 检查启动请求 */
        if (g_launch_req) {
            g_launch_req = 0;
            const char *rom = stock_ui_selected_rom(NULL);
            if (rom) launch_game(rom);
        }

        /* 帧率控制：~60fps */
        usleep(16000);
    }

    return 0;
}