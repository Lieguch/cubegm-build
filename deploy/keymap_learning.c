/* keymap_learning.c — 未知手柄键位学习（鼠标辅助）
 *
 * 需求：接入系统无法识别键位的手柄时，用 USB 鼠标在界面上
 * 给该手柄设置自定义键位（逐键学习），并永久保存。
 *
 * 流程：
 *   1. 检测到未知 VID:PID 手柄（keymap.txt 无其映射记录）
 *   2. 进入"键位学习界面"
 *   3. 鼠标左键点选"要设置的动作"（上/下/左/右/A/B/X/Y/L/R/L2/R2/开始/选择）
 *   4. 按手柄上对应的物理键 → 记录"动作 ↔ evdev 键码"映射
 *   5. 全部设置完 → 保存到 /mnt/sdcard/cubegm/keymap.txt
 *
 * 关键事实（逆向 cubevol_bridge.c 确认）：
 *   - cubevol_bridge.c 的 KEYMAP_FILE = /mnt/sdcard/cubegm/keymap.txt
 *   - 格式："ACTION code"（空格分隔，每行一个），如 "UP 288"
 *     （sscanf "%23s %d" 解析；动作 UP/DOWN/LEFT/RIGHT/A/B/X/Y/L/R/L2/R2/SELECT/START）
 *   - code = evdev 键码（BTN_TRIGGER=0x120=288 等），remap_load() 启动时读取。
 *   → 学习完成后重启（cubevol_bridge 重新加载）即生效，实现"下次接入自动加载"。
 *
 * 注意：原厂 joystick.zip 是 VID_PID 文件（逗号分隔），但本开源链路的
 * cubevol_bridge 只读单一 keymap.txt（"ACTION code" 格式），因此学习结果
 * 保存为 keymap.txt 以真正生效（而非写到无人读取的 keymap_<VID>_<PID>.txt）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include "mouse_input.h"   /* 鼠标点击 */
#include "render.h"
#include "font.h"

/* 要学习的动作（14 个） */
typedef enum {
    ACT_UP = 0, ACT_DOWN, ACT_LEFT, ACT_RIGHT,
    ACT_A, ACT_B, ACT_X, ACT_Y,
    ACT_L, ACT_R, ACT_L2, ACT_R2,
    ACT_SELECT, ACT_START,
    ACT_COUNT
} Action;

static const char *action_names[ACT_COUNT] = {
    "UP", "DOWN", "LEFT", "RIGHT",
    "A", "B", "X", "Y",
    "L", "R", "L2", "R2",
    "SELECT", "START",
};

/* 学习结果：动作 → evdev 键码（-1 = 未设置） */
static int g_learned[ACT_COUNT];

/* 学习状态 */
static int g_learning_active = 0;
static int g_current_action = 0;  /* 当前要学习的动作索引 */
static int g_waiting_key = 0;     /* 是否在等待按键 */

/* 手柄信息 */
static int g_vid = 0, g_pid = 0;
static int g_pad_fd = -1;  /* 未知手柄的 evdev fd */

/* 保存路径：cubevol_bridge 的 KEYMAP_FILE（"ACTION code" 格式） */
#define KEYMAP_FILE "/mnt/sdcard/cubegm/keymap.txt"

/* forward 声明：keymap_learning_poll() 先于定义调用 keymap_learning_save() */
int keymap_learning_save(void);

/* 键位学习界面首行 y 坐标 + 行高（与 keymap_learning_render 一致，供点击命中） */
#define LEARN_Y0 100
#define LEARN_ROW_H 40

/* 检测手柄：扫描 /dev/input/event*，找 joystick 设备（EV_ABS 绝对轴，非鼠标 EV_REL） */
static int find_pad_device(uint16_t *out_vid, uint16_t *out_pid) {
    char path[64];
    for (int i = 0; i < 16; i++) {
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        unsigned long evbits[(EV_MAX / 8 / sizeof(long)) + 1] = {0};
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) {
            close(fd);
            continue;
        }
        int is_abs = (evbits[EV_ABS / 8] & (1 << (EV_ABS % 8))) != 0;
        int is_rel = (evbits[EV_REL / 8] & (1 << (EV_REL % 8))) != 0;
        if (is_abs && !is_rel) {
            char name[256] = {0};
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            char syspath[256];
            snprintf(syspath, sizeof syspath,
                     "/sys/class/input/event%d/device/id/vendor", i);
            FILE *vf = fopen(syspath, "r");
            if (vf) { fscanf(vf, "%hx", out_vid); fclose(vf); }
            snprintf(syspath, sizeof syspath,
                     "/sys/class/input/event%d/device/id/product", i);
            FILE *pf = fopen(syspath, "r");
            if (pf) { fscanf(pf, "%hx", out_pid); fclose(pf); }
            *out_vid = *out_vid & 0xFFFF;
            *out_pid = *out_pid & 0xFFFF;

            fprintf(stderr, "keymap_learning: pad event%d name=%s vid=%04x pid=%04x\n",
                    i, name, *out_vid, *out_pid);
            return fd;
        }
        close(fd);
    }
    return -1;
}

/* 启动键位学习 */
int keymap_learning_start(void) {
    uint16_t vid = 0, pid = 0;
    g_pad_fd = find_pad_device(&vid, &pid);
    if (g_pad_fd < 0) return 0;  /* 没有手柄 */

    g_vid = vid;
    g_pid = pid;
    for (int i = 0; i < ACT_COUNT; i++) g_learned[i] = -1;
    g_learning_active = 1;
    g_current_action = 0;
    g_waiting_key = 1;
    fprintf(stderr, "keymap_learning: start for vid=%04x pid=%04x\n", vid, pid);
    return 1;
}

/* 手柄按键监听（等待用户按物理键），返回 evdev 键码（或 -1 未按）。
 * v1.1 修复：ABS_HAT0X/Y 是 D-pad 数字方向（值 -1/0/1），不能用
 * "|value|>1000" 的模拟摇杆阈值过滤，否则方向键学习必然失效。 */
static int wait_for_pad_key(void) {
    if (g_pad_fd < 0) return -1;

    struct input_event ev;
    int n;
    while ((n = read(g_pad_fd, &ev, sizeof(ev))) == sizeof(ev)) {
        if (ev.type == EV_KEY && ev.value == 1) {
            return ev.code;  /* 按键按下 → 返回键码 */
        }
        if (ev.type == EV_ABS) {
            /* D-pad hat：值 -1/0/1，直接映射为方向键码 */
            if (ev.code == ABS_HAT0Y) {
                if (ev.value < 0) return KEY_UP;
                if (ev.value > 0) return KEY_DOWN;
            } else if (ev.code == ABS_HAT0X) {
                if (ev.value < 0) return KEY_LEFT;
                if (ev.value > 0) return KEY_RIGHT;
            } else if (ev.code == ABS_X || ev.code == ABS_Y) {
                /* 模拟摇杆（若手柄方向用 X/Y 轴而非 HAT）：阈值 ±500 */
                if (ev.value < -500) return (ev.code == ABS_Y) ? KEY_UP : KEY_LEFT;
                if (ev.value >  500) return (ev.code == ABS_Y) ? KEY_DOWN : KEY_RIGHT;
            }
        }
    }
    return -1;  /* 还没按 */
}

/* 每帧处理（FrogUI 渲染循环调用）：
 * - 鼠标左键点选某一行动作 → 设为当前动作
 * - 之后按手柄对应物理键 → 记录映射，并移到下一个未设置动作 */
void keymap_learning_poll(void) {
    if (!g_learning_active) return;

    /* 鼠标左键：点选动作行（基于 LEARN_Y0 + i*LEARN_ROW_H 的命中检测） */
    if (mouse_button_pressed(0)) {
        int y = mouse_get_y();
        int i = (y - LEARN_Y0) / LEARN_ROW_H;
        if (i >= 0 && i < ACT_COUNT) {
            g_current_action = i;
            g_waiting_key = 1;
        }
        /* 右键：跳过（结束学习并保存） */
    } else if (mouse_button_pressed(1)) {
        keymap_learning_save();
        g_learning_active = 0;
        return;
    }

    /* 等待手柄按键 */
    if (g_waiting_key) {
        int code = wait_for_pad_key();
        if (code >= 0) {
            g_learned[g_current_action] = code;
            /* 移到下一个未设置的动作（自动顺序兜底） */
            int next = -1;
            for (int i = 0; i < ACT_COUNT; i++) {
                int idx = (g_current_action + 1 + i) % ACT_COUNT;
                if (g_learned[idx] < 0) { next = idx; break; }
            }
            if (next < 0) {  /* 全部设置完 → 保存 */
                keymap_learning_save();
                g_learning_active = 0;
            } else {
                g_current_action = next;
            }
        }
    }
}

/* 保存映射到 cubevol_bridge 的 keymap.txt（"ACTION code" 格式，真被消费） */
int keymap_learning_save(void) {
    FILE *f = fopen(KEYMAP_FILE, "w");
    if (!f) return 0;

    for (int i = 0; i < ACT_COUNT; i++) {
        if (g_learned[i] >= 0) {
            fprintf(f, "%s %d\n", action_names[i], g_learned[i]);
        }
    }
    fclose(f);

    fprintf(stderr, "keymap_learning: saved %s (vid=%04x pid=%04x)\n",
            KEYMAP_FILE, g_vid, g_pid);
    return 1;
}

/* 渲染键位学习界面：列出动作，当前动作高亮；提示鼠标点选 + 手柄按键 */
void keymap_learning_render(uint16_t *framebuffer) {
    if (!framebuffer || !g_learning_active) return;

    for (int i = 0; i < ACT_COUNT; i++) {
        int y = LEARN_Y0 + i * LEARN_ROW_H;
        uint16_t color = (i == g_current_action) ? 0xF800 : 0xFFFF;  /* 当前=红，其他=白 */
        char buf[128];
        snprintf(buf, sizeof buf, "%s : %s",
                 action_names[i],
                 g_learned[i] >= 0 ? "SET" : "...");
        font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT,
                       200, y, buf, color);
    }

    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT,
                   20, 20, "Click action, then press pad key. Right-click=save",
                   0xFFFF);

    /* 鼠标光标 */
    mouse_render_cursor(framebuffer);
}

/* 是否正在学习 */
int keymap_learning_active(void) { return g_learning_active; }

/* 关闭 */
void keymap_learning_stop(void) {
    if (g_pad_fd >= 0) { close(g_pad_fd); g_pad_fd = -1; }
    g_learning_active = 0;
}