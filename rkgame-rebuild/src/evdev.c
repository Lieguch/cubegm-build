/* ============================================================
 * rkgame-rebuild — evdev 手柄自动识别
 * ============================================================
 *
 * 原版实现（反编译实证）：
 *   ReadUSBJoy(port):
 *     1. access(JOYSTICK_DEVNAME[port]) — 硬编码的设备路径
 *     2. open(JOYSTICK_DEVNAME[port], O_RDONLY)
 *     3. read(fd, buf, 8) — evdev event
 *     4. GetInputInfo("js%d", param) — 读取 /proc/bus/input/devices
 *     5. GetJoystickConfig(USB_Table[port], vid, pid, rev) — 查硬编码映射表
 *
 * 核心问题：
 *   - JOYSTICK_DEVNAME[] 是静态字符数组（如 "/dev/input/js0"）
 *   - USB_Table[] 是静态映射表（按 VID/PID/REV 匹配按键位）
 *   - 不支持即插即用：手柄变化需改代码重新编译
 *
 * 重构方案：
 *   1. 扫描 /dev/input/event* 枚举所有设备
 *   2. ioctl(EVIOCGID) 获取 VID/PID/REV
 *   3. ioctl(EVIOCGBIT) 获取按键/摇杆能力
 *   4. 与内置常见手柄映射表匹配
 *   5. 不支持时输出诊断日志
 *
 * 设备路径优先级：
 *   /dev/input/eventN > /dev/input/jsN（evdev 接口更现代、信息更全）
 *
 * ABI 铁律：
 *   - 需要 linux/input.h、sys/ioctl.h
 *   - ioctl 不需要特殊 .symver（libc 默认版本可用）
 * ============================================================ */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/input.h>

#include "rkgame.h"

/* joy_devs / joy_dev_count 定义在 main.c */

/* ---- 内置手柄映射表（常见设备） ---- */

/* 映射格式：button_id(0-31) -> rkgame key bitmap 位 */
/* 标准 12 键映射（A/B/X/Y/L1/L2/R1/R2/Start/Select/Up/Down/Left/Right/L3/R3） */

typedef struct {
    uint16_t vid;
    uint16_t pid;
    const char *name_pattern;  /* NULL = 匹配任意 name */
    uint32_t button_map[32];   /* btn_code -> key_mask */
    uint8_t  axis_left;
    uint8_t  axis_right;
    uint8_t  hat;              /* HAT0 = 0, HAT1 = 1, ... */
} joystick_profile_t;

/* 按键位定义（兼容 rkgame key bitmap） */
#define KEY_UP      (1 << 0)   /* 0x01 */
#define KEY_DOWN    (1 << 1)   /* 0x02 */
#define KEY_LEFT    (1 << 2)   /* 0x04 */
#define KEY_RIGHT   (1 << 3)   /* 0x08 */
#define KEY_A       (1 << 4)   /* 0x10 */
#define KEY_B       (1 << 5)   /* 0x20 */
#define KEY_X       (1 << 6)   /* 0x40 */
#define KEY_Y       (1 << 7)   /* 0x80 */
#define KEY_L1      (1 << 8)
#define KEY_R1      (1 << 9)
#define KEY_L2      (1 << 10)
#define KEY_R2      (1 << 11)
#define KEY_START   (1 << 12)
#define KEY_SELECT  (1 << 13)

/* 通用手柄映射（匹配大多数 USB 手柄） */
static const uint32_t generic_button_map[32] = {
    [BTN_A]       = KEY_A,
    [BTN_B]       = KEY_B,
    [BTN_X]       = KEY_X,
    [BTN_Y]       = KEY_Y,
    [BTN_TL]      = KEY_L1,
    [BTN_TR]      = KEY_R1,
    [BTN_TL2]     = KEY_L2,
    [BTN_TR2]     = KEY_R2,
    [BTN_START]   = KEY_START,
    [BTN_SELECT]  = KEY_SELECT,
    [BTN_THUMBL]  = KEY_L1,    /* L3 */
    [BTN_THUMBR]  = KEY_R1,    /* R3 */
};

/* 8BitDo SN30 Pro */
static const uint32_t sn30_button_map[32] = {
    [BTN_A]       = KEY_A,
    [BTN_B]       = KEY_B,
    [BTN_X]       = KEY_X,
    [BTN_Y]       = KEY_Y,
    [BTN_TL]      = KEY_L1,
    [BTN_TR]      = KEY_R1,
    [BTN_TL2]     = KEY_L2,
    [BTN_TR2]     = KEY_R2,
    [BTN_START]   = KEY_START,
    [BTN_SELECT]  = KEY_SELECT,
};

/* 内置 profiles */
static const joystick_profile_t built_in_profiles[] = {
    /* 8BitDo SN30 Pro+ (D-Pad) */
    { .vid = 0x2dc8, .pid = 0x2000, .name_pattern = "8BitDo",
      .button_map = sn30_button_map, .axis_left = 0, .axis_right = 1, .hat = 0 },

    /* XBox 360 Controller */
    { .vid = 0x045e, .pid = 0x028e, .name_pattern = "XBOX",
      .button_map = generic_button_map, .axis_left = 0, .axis_right = 1, .hat = 0 },

    /* PS3 DualShock 3 (蓝牙) */
    { .vid = 0x054c, .pid = 0x0268, .name_pattern = "PLAYSTATION",
      .button_map = generic_button_map, .axis_left = 0, .axis_right = 1, .hat = 0 },

    /* 通用手柄（匹配所有有 BTN_A+BTN_B 的设备） */
    { .vid = 0, .pid = 0, .name_pattern = NULL,
      .button_map = generic_button_map, .axis_left = 0, .axis_right = 1, .hat = 0 },
};

#define PROFILE_COUNT (sizeof(built_in_profiles) / sizeof(built_in_profiles[0]))

/* ---- evdev 探测 ---- */

/*
 * joy_probe_evdev：打开 /dev/input/eventN 并探测设备信息。
 * 返回 0 = 成功，-1 = 失败。
 */
int joy_open(const char *path)
{
    if (joy_dev_count >= MAX_DEVICES) return -1;

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return -1;

    /* EVIOCGID — 获取 VID/PID/REV */
    struct input_id id = { 0 };
    if (ioctl(fd, EVIOCGID, &id) < 0) {
        close(fd);
        return -1;
    }

    /* EVIOCGNAME — 获取设备名 */
    char name[128] = { 0 };
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
        snprintf(name, sizeof(name), "unknown");

    /* EVIOCGBIT — 获取按键能力位图 */
    unsigned long key_bits[(KEY_MAX + 1) / sizeof(unsigned long)];
    memset(key_bits, 0, sizeof(key_bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
        close(fd);
        return -1;
    }

    /* 统计按键数 */
    unsigned int button_count = 0;
    for (unsigned int i = BTN_0; i < KEY_MAX; i++) {
        if (i < (KEY_MAX & ~(sizeof(unsigned long) * 8 - 1)))
            break;
        if (key_bits[i / (sizeof(unsigned long) * 8)] & (1U << (i % (sizeof(unsigned long) * 8))))
            button_count++;
    }

    joy_device_t *dev = &joy_devs[joy_dev_count];
    dev->vid = id.vendor;
    dev->pid = id.product;
    dev->revision = id.version;
    strncpy(dev->name, name, sizeof(dev->name) - 1);
    dev->fd = -1;
    dev->event_fd = fd;
    dev->is_evdev = true;
    dev->is_js = false;
    dev->axis_count = 0;
    dev->button_count = button_count;
    memcpy(dev->button_map, generic_button_map, sizeof(dev->button_map));

    joy_dev_count++;

    LOG("joy: probe %s -> vendor=0x%04x pid=0x%04x rev=0x%04x name=\"%s\" buttons=%u",
        path, dev->vid, dev->pid, dev->revision, dev->name, button_count);

    return 0;
}

/*
 * joy_autodetect：扫描 /dev/input/ 自动枚举所有手柄设备。
 */
int joy_autodetect(void)
{
    LOG("joy_autodetect: scanning /dev/input/");

    /* 重置已打开设备 */
    joy_close_all();

    DIR *dir = opendir("/dev/input");
    if (!dir) {
        ERR("joy_autodetect: cannot open /dev/input: %s", strerror(errno));
        return -1;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        if (strncmp(name, "event", 5) == 0) {
            char path[64];
            snprintf(path, sizeof(path), "/dev/input/%s", name);
            joy_open(path);
        }
    }
    closedir(dir);

    LOG("joy_autodetect: found %d devices", joy_dev_count);
    return joy_dev_count;
}

bool joy_probe_evdev(const char *path, uint16_t *out_vid, uint16_t *out_pid)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;

    struct input_id id = { 0 };
    if (ioctl(fd, EVIOCGID, &id) < 0) {
        close(fd);
        return false;
    }

    if (out_vid) *out_vid = id.vendor;
    if (out_pid) *out_pid = id.product;
    close(fd);
    return true;
}

void joy_close_all(void)
{
    for (int i = 0; i < joy_dev_count; i++) {
        if (joy_devs[i].event_fd >= 0) {
            close(joy_devs[i].event_fd);
            joy_devs[i].event_fd = -1;
        }
        if (joy_devs[i].fd >= 0) {
            close(joy_devs[i].fd);
            joy_devs[i].fd = -1;
        }
    }
    joy_dev_count = 0;
}

/*
 * joy_poll：读取所有已连接手柄的事件。
 * 返回 true 如果至少有一个手柄有输入。
 */
bool joy_poll(void)
{
    bool any = false;
    for (int i = 0; i < joy_dev_count; i++) {
        if (joy_devs[i].event_fd < 0) continue;

        struct input_event ev[16];
        ssize_t n = read(joy_devs[i].event_fd, ev, sizeof(ev));
        if (n <= 0) continue;

        size_t count = n / sizeof(struct input_event);
        for (size_t j = 0; j < count; j++) {
            struct input_event *e = &ev[j];
            if (e->type == EV_KEY && e->value > 0) {
                /* 按键按下 */
                unsigned int code = (unsigned int)e->code;
                if (code < 32 && joy_devs[i].button_map[code]) {
                    any = true;
                }
            } else if (e->type == EV_ABS) {
                /* 摇杆/按键释放 */
                any = true;
            }
        }
    }
    return any;
}

/*
 * joy_get_key：查询手柄按键状态。
 * player: 0-1 (对应 rkgame 的两个 player 槽位)
 * key_id: 按键位掩码（如 KEY_A = 0x10）
 * 返回 1 = 按下，0 = 未按下
 */
int joy_get_key(uint8_t player, uint32_t key_id)
{
    if (player >= (uint8_t)joy_dev_count) return 0;

    joy_device_t *dev = &joy_devs[player];
    if (dev->event_fd < 0) return 0;

    /* 读取所有待处理事件（非阻塞） */
    struct input_event ev[32];
    ssize_t n = read(dev->event_fd, ev, sizeof(ev));
    if (n <= 0) return 0;

    size_t count = n / sizeof(struct input_event);
    for (size_t j = 0; j < count; j++) {
        struct input_event *e = &ev[j];
        if (e->type == EV_KEY && e->value > 0) {
            unsigned int code = (unsigned int)e->code;
            if (code < 32 && dev->button_map[code] == key_id)
                return 1;
        }
    }
    return 0;
}

/*
 * joy_init：初始化手柄子系统，调用自动探测。
 */
void joy_init(void)
{
    LOG("joy_init: initializing");
    joy_dev_count = 0;

    /* 自动探测 */
    int count = joy_autodetect();
    if (count < 0) {
        ERR("joy_init: autodetect failed, falling back to static devices");
        /* 回退到静态设备路径（兼容原版） */
        static const char *fallback_devices[] = {
            "/dev/input/event0", "/dev/input/event1",
            "/dev/input/js0", "/dev/input/js1",
            NULL
        };
        for (int i = 0; fallback_devices[i] != NULL; i++) {
            if (access(fallback_devices[i], F_OK) == 0) {
                joy_open(fallback_devices[i]);
            }
        }
    }

    LOG("joy_init: %d device(s) ready", joy_dev_count);
}

/*
 * joy_print_diag：打印所有已连接手柄的诊断信息。
 * 用于调试 /sdcard/cubegm/ 下的 sramshim.log。
 */
void joy_print_diag(void)
{
    LOG("joy_diag: ===");
    for (int i = 0; i < joy_dev_count; i++) {
        joy_device_t *d = &joy_devs[i];
        LOG("  [%d] %s vid=0x%04x pid=0x%04x rev=0x%04x evdev=%s fd=%d buttons=%u",
            i, d->name, d->vid, d->pid, d->revision,
            d->is_evdev ? "yes" : "no", d->event_fd, d->button_count);
    }
    LOG("joy_diag: ===");
}
