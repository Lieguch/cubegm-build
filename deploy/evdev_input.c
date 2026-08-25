/*
 * evdev_input.c — 直接 evdev 输入读取，替代 cubevol_bridge + shm
 *
 * 读取 /dev/input/event* 中第一个手柄设备，构建 CV_* 位掩码
 * 供 stock_ui_handle_input() 消费。
 *
 * 按键映射（对齐 FrogUI 的 CV_* 定义）：
 *   UP=4 DOWN=6 LEFT=7 RIGHT=5 A=13 B=14 X=12 Y=15
 *   L=10 R=11 L2=8 R2=9 SELECT=0 START=3
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>

#define MAX_DEV 16
#define KEY_BIT(n) (1u << (n))

/* 按键码 → CV_* 位映射表（14 项，匹配 sf3000_keymap.txt 顺序） */
static const struct { unsigned code; int bit; } keymap[] = {
    { 4, 4 },  /* UP */     { 6, 6 },  /* DOWN */
    { 7, 7 },  /* LEFT */   { 5, 5 },  /* RIGHT */
    { 13, 13 },{ 14, 14 }, /* A B */
    { 12, 12 },{ 15, 15 }, /* X Y */
    { 10, 10 },{ 11, 11 }, /* L R */
    { 8, 8 },  { 9, 9 },   /* L2 R2 */
    { 0, 0 },  { 3, 3 },   /* SELECT START */
};
#define KEYMAP_N (sizeof(keymap)/sizeof(keymap[0]))

static int g_fd = -1;          /* 打开的 evdev fd */
static uint32_t g_keys = 0;    /* 当前按键位掩码 */
static uint32_t g_prev = 0;

/* ABS 方向键 → 位掩码 */
static uint32_t abs_to_bits(int code, int val) {
    if (val < -1000) return 0;  /* 模拟摇杆反向 */
    if (val > 1000)  return 0;  /* 模拟摇杆正向 */
    if (code == ABS_HAT0X) return val < 0 ? KEY_BIT(7) : (val > 0 ? KEY_BIT(5) : 0);
    if (code == ABS_HAT0Y) return val < 0 ? KEY_BIT(4) : (val > 0 ? KEY_BIT(6) : 0);
    /* 摇杆模拟（阈值 500） */
    if (code == ABS_X)     return val < 128 ? KEY_BIT(7) : (val > 128 ? KEY_BIT(5) : 0);
    if (code == ABS_Y)     return val < 128 ? KEY_BIT(4) : (val > 128 ? KEY_BIT(6) : 0);
    return 0;
}

int evdev_init(void) {
    /* 找第一个手柄设备 */
    for (int e = 0; e < MAX_DEV; e++) {
        char p[64];
        snprintf(p, sizeof p, "/dev/input/event%d", e);
        int fd = open(p, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        /* 检查是否为游戏手柄（有 KEY 和 ABS 能力） */
        unsigned long evbits[2] = {0};
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) { close(fd); continue; }
        if (!(evbits[0] & (1 << EV_KEY)) || !(evbits[0] & (1 << EV_ABS))) { close(fd); continue; }
        /* 检查 ABS 刚好是 HAT 或摇杆 */
        unsigned long absbits[64 / 8] = {0};
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
        if (absbits[ABS_HAT0X / 8] & (1 << (ABS_HAT0X % 8))) {
            g_fd = fd;
            fprintf(stderr, "evdev: opened %s (gamepad)\n", p);
            return 0;
        }
        close(fd);
    }
    return -1;
}

uint32_t evdev_read_keys(void) {
    if (g_fd < 0) return 0;
    g_prev = g_keys;

    struct input_event ev[16];
    ssize_t rd;
    while ((rd = read(g_fd, ev, sizeof(ev))) > 0) {
        int n = rd / (int)sizeof(struct input_event);
        for (int i = 0; i < n; i++) {
            if (ev[i].type == EV_KEY) {
                if (ev[i].value == 0) {
                    /* 释放 → 清位 */
                    for (unsigned j = 0; j < KEYMAP_N; j++)
                        if (keymap[j].code == ev[i].code)
                            g_keys &= ~KEY_BIT(keymap[j].bit);
                } else if (ev[i].value == 1) {
                    /* 按下 → 置位 */
                    for (unsigned j = 0; j < KEYMAP_N; j++)
                        if (keymap[j].code == ev[i].code)
                            g_keys |= KEY_BIT(keymap[j].bit);
                }
            } else if (ev[i].type == EV_ABS) {
                /* 方向键 */
                uint32_t bits = abs_to_bits(ev[i].code, ev[i].value);
                /* 清当前轴 */
                unsigned axes[] = {ABS_HAT0X, ABS_HAT0Y, ABS_X, ABS_Y};
                for (unsigned a = 0; a < 4; a++) {
                    if (ev[i].code == axes[a]) {
                        g_keys &= ~(KEY_BIT(4) | KEY_BIT(5) | KEY_BIT(6) | KEY_BIT(7));
                        break;
                    }
                }
                g_keys |= bits;
            }
        }
    }
    return g_keys;
}