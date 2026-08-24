/* mouse_input.c — USB 鼠标输入（UI 全面使用）
 *
 * 用 evdev 读 USB 鼠标：
 *   - REL_X / REL_Y：相对移动 → 累加成光标坐标
 *   - BTN_LEFT / BTN_RIGHT / BTN_MIDDLE：点击
 *   - REL_WHEEL：滚轮（可选，映射为上下导航）
 *
 * 关键约束（用户明确）：
 *   - 鼠标只在 UI 界面使用（导航/选择/搜索/设置/键位学习）
 *   - 游戏内不使用鼠标（游戏仍手柄操作）
 *
 * 设计：独立模块，FrogUI 直接调用（不通过共享内存，因为需要光标坐标）
 *
 * 集成：
 *   1. mouse_init() 打开 /dev/input/event* 里的鼠标设备（EV_REL 能力位）
 *   2. 每帧 mouse_poll() 读事件，更新光标 + 按键状态
 *   3. FrogUI 渲染循环里渲染光标 + 处理点击
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include "render.h"   /* SCREEN_WIDTH/SCREEN_HEIGHT */

#define MAX_MICE 4

/* 鼠标设备文件描述符 */
static int g_mouse_fds[MAX_MICE];
static int g_mouse_count = 0;

/* 光标状态 */
static int g_cursor_x = 0;
static int g_cursor_y = 0;
static int g_cursor_visible = 0;

/* 按键状态（bit 0=左, 1=右, 2=中） */
static uint8_t g_buttons = 0;
static uint8_t g_buttons_prev = 0;

/* 滚轮 */
static int g_wheel = 0;

/* 检测设备是否是鼠标（有 EV_REL + REL_X/REL_Y） */
static int is_mouse_device(const char *path) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return 0;

    unsigned long evbits[(EV_MAX / 8 / sizeof(long)) + 1] = {0};
    unsigned long relbits[(REL_MAX / 8 / sizeof(long)) + 1] = {0};

    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) {
        close(fd);
        return 0;
    }

    /* 必须支持 EV_REL（相对移动 = 鼠标特征） */
    if (!(evbits[EV_REL / 8] & (1 << (EV_REL % 8)))) {
        close(fd);
        return 0;
    }

    if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits) < 0) {
        close(fd);
        return 0;
    }

    /* 必须支持 REL_X 和 REL_Y（鼠标移动） */
    int has_x = (relbits[REL_X / 8] & (1 << (REL_X % 8))) != 0;
    int has_y = (relbits[REL_Y / 8] & (1 << (REL_Y % 8))) != 0;

    close(fd);
    return has_x && has_y;
}

/* 初始化：扫描 /dev/input/event* 找鼠标 */
int mouse_init(void) {
    g_mouse_count = 0;
    g_cursor_x = SCREEN_WIDTH / 2;
    g_cursor_y = SCREEN_HEIGHT / 2;

    char path[64];
    for (int i = 0; i < 16 && g_mouse_count < MAX_MICE; i++) {
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        if (!is_mouse_device(path)) continue;

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            g_mouse_fds[g_mouse_count++] = fd;
            fprintf(stderr, "mouse: opened %s\n", path);
            g_cursor_visible = 1;
        }
    }

    if (g_mouse_count == 0) {
        fprintf(stderr, "mouse: no mouse device found (UI mouse disabled)\n");
    }
    return g_mouse_count;
}

/* 轮询鼠标事件，更新光标 + 按键 */
void mouse_poll(void) {
    if (g_mouse_count == 0) return;

    g_buttons_prev = g_buttons;
    g_wheel = 0;

    for (int i = 0; i < g_mouse_count; i++) {
        struct input_event ev;
        int n;
        while ((n = read(g_mouse_fds[i], &ev, sizeof(ev))) == sizeof(ev)) {
            if (ev.type == EV_REL) {
                if (ev.code == REL_X) {
                    g_cursor_x += ev.value;
                } else if (ev.code == REL_Y) {
                    g_cursor_y += ev.value;
                } else if (ev.code == REL_WHEEL) {
                    g_wheel += ev.value;
                }
            } else if (ev.type == EV_KEY) {
                if (ev.code == BTN_LEFT) {
                    if (ev.value) g_buttons |= 0x01; else g_buttons &= ~0x01;
                } else if (ev.code == BTN_RIGHT) {
                    if (ev.value) g_buttons |= 0x02; else g_buttons &= ~0x02;
                } else if (ev.code == BTN_MIDDLE) {
                    if (ev.value) g_buttons |= 0x04; else g_buttons &= ~0x04;
                }
            }
        }
    }

    /* 光标边界钳制 */
    if (g_cursor_x < 0) g_cursor_x = 0;
    if (g_cursor_x >= SCREEN_WIDTH) g_cursor_x = SCREEN_WIDTH - 1;
    if (g_cursor_y < 0) g_cursor_y = 0;
    if (g_cursor_y >= SCREEN_HEIGHT) g_cursor_y = SCREEN_HEIGHT - 1;
}

/* 光标坐标访问器 */
int mouse_get_x(void) { return g_cursor_x; }
int mouse_get_y(void) { return g_cursor_y; }
int mouse_is_visible(void) { return g_cursor_visible; }

/* 按键状态访问器（边沿检测） */
int mouse_button_down(int btn) {  /* 0=左 1=右 2=中 */
    return (g_buttons >> btn) & 1;
}
int mouse_button_pressed(int btn) {  /* 边沿：按下瞬间 */
    return ((g_buttons >> btn) & 1) && !((g_buttons_prev >> btn) & 1);
}
int mouse_button_released(int btn) {
    return !((g_buttons >> btn) & 1) && ((g_buttons_prev >> btn) & 1);
}

/* 滚轮（累计值，调用后清零） */
int mouse_get_wheel(void) { return g_wheel; }

/* 渲染光标（十字或箭头，简单实现） */
void mouse_render_cursor(uint16_t *framebuffer) {
    if (!framebuffer || !g_cursor_visible) return;

    int x = g_cursor_x, y = g_cursor_y;
    uint16_t white = 0xFFFF;  /* 白色光标 */
    uint16_t black = 0x0000;  /* 黑色描边 */

    /* 简单箭头光标（9×9） */
    for (int i = 0; i < 9; i++) {
        /* 对角线 */
        if (y + i < SCREEN_HEIGHT && x + i < SCREEN_WIDTH) {
            framebuffer[(y + i) * SCREEN_WIDTH + (x + i)] = white;
        }
        /* 竖线 */
        if (y + i < SCREEN_HEIGHT && x < SCREEN_WIDTH) {
            framebuffer[(y + i) * SCREEN_WIDTH + x] = white;
        }
        /* 横线 */
        if (y < SCREEN_HEIGHT && x + i < SCREEN_WIDTH) {
            framebuffer[y * SCREEN_WIDTH + (x + i)] = white;
        }
    }
}

/* 关闭 */
void mouse_deinit(void) {
    for (int i = 0; i < g_mouse_count; i++) {
        close(g_mouse_fds[i]);
    }
    g_mouse_count = 0;
    g_cursor_visible = 0;
}
