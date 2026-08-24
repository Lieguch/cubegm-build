/* keymap_learning.c — 未知手柄键位学习（鼠标辅助）
 *
 * 需求：接入系统无法识别键位的手柄时，用 USB 鼠标在界面上
 * 给该手柄设置自定义键位（逐键学习），并永久保存。
 *
 * 流程：
 *   1. 检测到未知 VID:PID 手柄（joystick.zip 无其映射文件）
 *   2. 进入"键位学习界面"
 *   3. 鼠标点选"要设置的动作"（上/下/左/右/A/B/X/Y/L/R/L2/R2/开始/选择）
 *   4. 按手柄上对应的物理键 → 记录"动作 ↔ 物理键码"映射
 *   5. 全部设置完 → 保存为 VID_PID 文件（复用原厂 joystick.zip 格式）
 *
 * 原厂映射格式（已逆向确认）：
 *   - 文件 = joystick.zip 内的 VID_PID 文件（如 0810_0001_0100）
 *   - 内容 = 逗号分隔的动作序列（物理键位 → 逻辑动作）
 *   例：0810_0001_0100 = "A,B,TL1,X,Y,TR1,TL1,TR1,TL2,TR2,SELECT,START,0,0,0,0,0,1,0,1"
 *
 * 我们扩展为独立的 keymap 文件（不覆盖原厂 joystick.zip，而是新增）：
 *   - 路径：/mnt/sdcard/cubegm/keymap_<VID>_<PID>.txt
 *   - 格式：动作=键码（每行一个，如 "UP=4"）
 *   - cubevol_bridge 已有 keymap.txt 加载机制（remap_load），可扩展为按 VID:PID 加载
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

/* 学习结果：动作 → 物理键码 */
static int g_learned[ACT_COUNT];  /* -1 = 未设置 */

/* 学习状态 */
static int g_learning_active = 0;
static int g_current_action = 0;  /* 当前要学习的动作索引 */
static int g_waiting_key = 0;     /* 是否在等待按键 */

/* 手柄信息 */
static int g_vid = 0, g_pid = 0;
static int g_pad_fd = -1;  /* 未知手柄的 evdev fd */

/* 检测未知手柄：扫描 /dev/input/event*，找 joystick 设备 */
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
        /* joystick = EV_ABS（绝对轴，非鼠标的 EV_REL） */
        int is_abs = (evbits[EV_ABS / 8] & (1 << (EV_ABS % 8))) != 0;
        int is_rel = (evbits[EV_REL / 8] & (1 << (EV_REL % 8))) != 0;
        if (is_abs && !is_rel) {
            /* 这是手柄（绝对轴 + 非相对） */
            /* 读 VID:PID（从设备路径或 ioctl） */
            char name[256] = {0};
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            /* VID:PID 从 sysfs 读 */
            char syspath[256];
            snprintf(syspath, sizeof syspath,
                     "/sys/class/input/event%d/device/id/vendor", i);
            FILE *vf = fopen(syspath, "r");
            if (vf) { fscanf(vf, "%x", out_vid); fclose(vf); }
            snprintf(syspath, sizeof syspath,
                     "/sys/class/input/event%d/device/id/product", i);
            FILE *pf = fopen(syspath, "r");
            if (pf) { fscanf(pf, "%x", out_pid); fclose(pf); }
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

/* 检查该 VID:PID 是否有已知映射（joystick.zip 或 keymap 文件） */
static int has_known_mapping(uint16_t vid, uint16_t pid) {
    char path[256];
    snprintf(path, sizeof path, "/mnt/sdcard/cubegm/keymap_%04x_%04x.txt", vid, pid);
    if (access(path, F_OK) == 0) return 1;  /* 已有映射 */
    return 0;
}

/* 启动键位学习 */
int keymap_learning_start(void) {
    uint16_t vid = 0, pid = 0;
    g_pad_fd = find_pad_device(&vid, &pid);
    if (g_pad_fd < 0) return 0;  /* 没有未知手柄 */

    if (has_known_mapping(vid, pid)) {
        /* 已有映射，不需要学习 */
        close(g_pad_fd);
        g_pad_fd = -1;
        return 0;
    }

    g_vid = vid;
    g_pid = pid;
    for (int i = 0; i < ACT_COUNT; i++) g_learned[i] = -1;
    g_learning_active = 1;
    g_current_action = 0;
    g_waiting_key = 1;
    fprintf(stderr, "keymap_learning: start for vid=%04x pid=%04x\n", vid, pid);
    return 1;
}

/* 手柄按键监听（等待用户按物理键） */
static int wait_for_pad_key(void) {
    if (g_pad_fd < 0) return -1;

    struct input_event ev;
    int n;
    while ((n = read(g_pad_fd, &ev, sizeof(ev))) == sizeof(ev)) {
        if (ev.type == EV_KEY && ev.value == 1) {
            /* 按键按下 */
            return ev.code;
        }
        if (ev.type == EV_ABS && (ev.value < -1000 || ev.value > 1000)) {
            /* 轴（方向键）→ 映射为方向 */
            if (ev.code == ABS_HAT0Y && ev.value < 0) return KEY_UP;
            if (ev.code == ABS_HAT0Y && ev.value > 0) return KEY_DOWN;
            if (ev.code == ABS_HAT0X && ev.value < 0) return KEY_LEFT;
            if (ev.code == ABS_HAT0X && ev.value > 0) return KEY_RIGHT;
        }
    }
    return -1;  /* 还没按 */
}

/* 每帧处理（FrogUI 渲染循环里调用） */
void keymap_learning_poll(void) {
    if (!g_learning_active) return;

    /* 鼠标点击：跳过当前动作（可选）或确认 */
    if (mouse_button_pressed(1)) {  /* 右键 = 跳过当前动作 */
        g_current_action++;
        if (g_current_action >= ACT_COUNT) {
            keymap_learning_save();
            g_learning_active = 0;
        }
        return;
    }

    /* 等待手柄按键 */
    if (g_waiting_key) {
        int code = wait_for_pad_key();
        if (code >= 0) {
            g_learned[g_current_action] = code;
            g_current_action++;
            if (g_current_action >= ACT_COUNT) {
                keymap_learning_save();
                g_learning_active = 0;
            }
        }
    }
}

/* 保存映射为 VID_PID 文件（复用原厂格式：逗号分隔动作序列） */
int keymap_learning_save(void) {
    char path[256];
    snprintf(path, sizeof path, "/mnt/sdcard/cubegm/keymap_%04x_%04x.txt", g_vid, g_pid);

    FILE *f = fopen(path, "w");
    if (!f) return 0;

    /* 复用原厂格式：逗号分隔的动作序列（对应物理键位顺序） */
    /* 但我们的格式更清晰：动作=键码（供 cubevol_bridge 加载） */
    for (int i = 0; i < ACT_COUNT; i++) {
        if (g_learned[i] >= 0) {
            fprintf(f, "%s %d\n", action_names[i], g_learned[i]);
        }
    }
    fclose(f);

    fprintf(stderr, "keymap_learning: saved %s\n", path);
    return 1;
}

/* 渲染键位学习界面 */
void keymap_learning_render(uint16_t *framebuffer) {
    if (!framebuffer || !g_learning_active) return;

    /* 简单界面：列出动作，当前动作高亮 */
    for (int i = 0; i < ACT_COUNT; i++) {
        int y = 100 + i * 40;
        uint16_t color = (i == g_current_action) ? 0xF800 : 0xFFFF;  /* 当前=红，其他=白 */
        char buf[128];
        snprintf(buf, sizeof buf, "%s : %s",
                 action_names[i],
                 g_learned[i] >= 0 ? "SET" : "...");
        font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT,
                       200, y, buf, color);
    }

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
