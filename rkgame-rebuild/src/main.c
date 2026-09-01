/* ============================================================
 * rkgame-rebuild — 入口
 * ============================================================
 *
 * 原版 main() 流程：
 *   puts("rkgame v1.42")
 *   get_executable_path(work_path)
 *   resource_path = work_path + "resource"
 *   GetConfig()        — 读 config.xml
 *   dispmeninfo()      — 输出分辨率信息
 *   InitDisplay()
 *   InitSound()
 *   InitJoystick()     — GPIO + RF
 *   sfc_init()
 *   spi_driver_init()  — 触摸屏驱动
 *   if autorunfile[0] == '\0':
 *       main_Menu()
 *   elif autorunfile == "/USBJoystickTest":
 *       TestUSBJoy()
 *   elif autorunfile == "/JoystickTest":
 *       JoystickTest()
 *   else:
 *       autorun(autorunfile, autorundriver)
 *
 * 重构后保持同样入口结构。
 * ============================================================ */

#define _GNU_SOURCE
/* rkgame v1.5.0 — SRAM + evdev rebuild, 2026-09-01 */
/* Build trigger */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>

#include "rkgame.h"

/* ---- 全局变量定义 ---- */

char     work_path[512] = "/sdcard/cubegm/";
char     resource_path[512];
char     autorunfile[1024];
char     autorundriver[128];
char     system_directory[512];
char     save_directory[512];

uint16_t n_input_width       = 0x140;  /* 320 */
uint16_t n_input_height      = 0xe0;   /* 224 */
uint16_t n_input_visible_width  = 0x100;
uint16_t n_input_visible_height = 0xe0;
uint16_t screen_w = 0x100;
uint16_t screen_x = 0;
uint8_t  rotation = 0;
bool     use_rgb_8888 = false;

void *core_handle = NULL;
retro_ctx_t ctx = { 0 };
sram_state_t sram_state = { 0 };
joy_device_t joy_devs[MAX_DEVICES] = { 0 };
int joy_dev_count = 0;
rkgame_config_t g_cfg = { 0 };

/* ---- 日志（rklog 实现） ---- */

void rklog(int level, const char *fmt, ...)
{
    va_list ap;
    const char *prefix;
    switch (level) {
        case RKLOG_ERROR: prefix = "[RK-E]"; break;
        case RKLOG_WARN:  prefix = "[RK-W]"; break;
        case RKLOG_INFO:  prefix = "[RK-I]"; break;
        default:          prefix = "[RK-D]"; break;
    }
    fprintf(stderr, "%s ", prefix);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

/* ---- 显示层占位实现（DRM/KMS 待 Phase 4 实现） ---- */

void disp_init(void)         { LOG("disp_init: placeholder"); }
void disp_shutdown(void)     { LOG("disp_shutdown"); }
void disp_set_rotation(uint32_t rot) { LOG("disp_set_rotation: %u", rot); }
void disp_set_colormode(int mode)    { LOG("disp_set_colormode: %d", mode); }
void disp_flip(const void *buf, unsigned w, unsigned h, size_t p)
{
    (void)buf; (void)w; (void)h; (void)p;
}

/* ---- 音频层占位实现（ALSA 待 Phase 4 实现） ---- */

void audio_init(void)         { LOG("audio_init: placeholder"); }
void audio_shutdown(void)     { LOG("audio_shutdown"); }
void audio_play(const void *buf, size_t frames)
{
    (void)buf; (void)frames;
}

/* ---- 配置保存占位 ---- */

void config_save(void) { LOG("config_save: not implemented"); }

/* ---- 工具函数 ---- */

/*
 * get_executable_path：读取 /proc/self/exe 链接获取自身路径。
 * 原版用 getcwd 替代，但那样拿到的是当前工作目录而非程序路径。
 */
static void get_executable_path(char *out, size_t out_size)
{
    ssize_t len = readlink("/proc/self/exe", out, out_size - 1);
    if (len < 0) {
        /* 退路：用 argv[0] */
        snprintf(out, out_size, "/sdcard/cubegm/rkgame");
        return;
    }
    out[len] = '\0';
    /* 截取到父目录 */
    char *last_slash = strrchr(out, '/');
    if (last_slash) {
        *(last_slash + 1) = '\0';
    }
}

/* ---- 配置加载 ---- */

static void config_load(void)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/config.xml", work_path);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        ERR("config_load: open %s fail (errno=%d: %s)", path, errno, strerror(errno));
        /* 回退到 defaults */
        memset(&g_cfg, 0, sizeof(g_cfg));
        strcpy(g_cfg.autorun_path, "/sdcard/cubegm/roms/fba/");
        return;
    }

    /* 简易 XML 解析：读取 <autorun>...</autorun> 和 <device> 等标签 */
    char *buf = malloc(64 * 1024);
    if (!buf) { fclose(fp); return; }
    size_t n = fread(buf, 1, 64 * 1024 - 1, fp);
    buf[n] = '\0';
    fclose(fp);

    /* 提取 autorun 路径 */
    char *tag = strstr(buf, "<autorun>");
    if (tag) {
        tag += strlen("<autorun>");
        char *end = strstr(tag, "</autorun>");
        if (end) {
            size_t len = end - tag;
            if (len < sizeof(g_cfg.autorun_path))
                memcpy(g_cfg.autorun_path, tag, len);
            g_cfg.autorun_path[len] = '\0';
        }
    }

    /* 提取 core 名 */
    tag = strstr(buf, "<core>");
    if (tag) {
        tag += strlen("<core>");
        char *end = strstr(tag, "</core>");
        if (end) {
            size_t len = end - tag;
            if (len < sizeof(g_cfg.core_name))
                memcpy(g_cfg.core_name, tag, len);
            g_cfg.core_name[len] = '\0';
        }
    }

    /* 提取 device0_type / device1_type */
    tag = strstr(buf, "<device0_type>");
    if (tag) {
        tag += strlen("<device0_type>");
        char *end = strstr(tag, "</device0_type>");
        if (end) {
            size_t len = end - tag;
            if (len < sizeof(g_cfg.device0_type))
                memcpy(g_cfg.device0_type, tag, len);
            g_cfg.device0_type[len] = '\0';
        }
    }
    tag = strstr(buf, "<device1_type>");
    if (tag) {
        tag += strlen("<device1_type>");
        char *end = strstr(tag, "</device1_type>");
        if (end) {
            size_t len = end - tag;
            if (len < sizeof(g_cfg.device1_type))
                memcpy(g_cfg.device1_type, tag, len);
            g_cfg.device1_type[len] = '\0';
        }
    }

    free(buf);
    LOG("config: autorun=%s core=%s dev0=%s dev1=%s",
        g_cfg.autorun_path, g_cfg.core_name, g_cfg.device0_type, g_cfg.device1_type);
}

/* ---- autorun ---- */

void autorun(const char *rom, const char *driver)
{
    LOG("autorun: rom=%s driver=%s", rom, driver ?: "(none)");
    if (driver && *driver) {
        /* 有 driver 参数 — 原版行为：仅 InitScr 后返回 */
        LOG("autorun: driver specified, skipping ROM load");
        return;
    }
    core_load(rom, g_cfg.core_name);
}

/* ---- 菜单占位 ---- */

static void main_menu(void)
{
    LOG("main_menu: not implemented yet");
    /* 留待 Phase 4 实现 */
    ERR("main_menu: 暂不支持，需通过 autorun 启动");
}

/* ---- 入口 ---- */

int main(int argc, char **argv)
{
    LOG("rkgame v1.5.0 (rebuild)");

    /* 获取程序目录 */
    get_executable_path(work_path, sizeof(work_path));
    LOG("work_path = %s", work_path);

    /* 资源路径 */
    snprintf(resource_path, sizeof(resource_path), "%sresource/", work_path);

    /* 加载配置 */
    config_load();

    /* 如果有命令行参数，优先用参数指定 autorun */
    if (argc >= 2) {
        strncpy(autorunfile, argv[1], sizeof(autorunfile) - 1);
    } else if (g_cfg.autorun_path[0]) {
        strncpy(autorunfile, g_cfg.autorun_path, sizeof(autorunfile) - 1);
    } else {
        autorunfile[0] = '\0';
    }
    if (argc >= 3) {
        strncpy(autorundriver, argv[2], sizeof(autorundriver) - 1);
    }

    LOG("autorunfile = %s", autorunfile);

    /* 初始化子系统 */
    disp_init();
    audio_init();
    sram_init();
    joy_init();

    /* 菜单 / autorun */
    if (autorunfile[0] == '\0') {
        main_menu();
    } else if (strcmp(autorunfile, "/USBJoystickTest") == 0) {
        /* 手柄测试模式 */
        LOG("entering USB joystick test mode");
    } else if (strcmp(autorunfile, "/JoystickTest") == 0) {
        LOG("entering joystick test mode");
    } else {
        autorun(autorunfile, autorundriver);
    }

    /* 清理 */
    sram_unload();
    joy_close_all();
    core_unload();
    audio_shutdown();
    disp_shutdown();

    return 0;
}
