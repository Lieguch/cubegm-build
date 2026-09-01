/* ============================================================
 * rkgame-rebuild — rkgame v1.42 重构版
 * ============================================================
 *
 * 目标：完整替换 /sdcard/cubegm/rkgame，增加：
 *   1. SRAM 存档（retro_get_memory_data/size → .srm 落盘）
 *   2. evdev 手柄 VID/PID 自动识别 + 能力协商
 *   3. 保留原版所有功能（libretro 核心加载、视频/音频、菜单）
 *
 * 构建目标：
 *   ARM32 hard-float, glibc 2.29, e_flags=0x5000400
 *   CFLAGS: -march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4
 *           -mfloat-abi=hard -O2
 *   LDFLAGS: -ldl -lpthread -lm -lz
 *
 * ABI 兼容：所有 dlsym/dlopen/pthread_* 函数指针必须通过 .symver
 *           锁定到 GLIBC_2.4（设备 glibc 2.29 只导出 2.4 版本）
 * ============================================================ */

#ifndef RKGAME_H
#define RKGAME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* libretro 核心所需结构体（不链接 libretro，本地定义） */
typedef struct {
    const char *path;
    const void *data;
    size_t     size;
    const char *metadata;
} retro_game_info_t;

/* ---- 全局状态 ---- */

extern char     work_path[512];    /* /sdcard/cubegm/ */
extern char     resource_path[512]; /* /sdcard/cubegm/resource/ */
extern char     autorunfile[1024]; /* 启动时传入的 ROM 路径 */
extern char     autorundriver[128];
extern char     system_directory[512];
extern char     save_directory[512];

/* 分辨率（原版固定） */
extern uint16_t n_input_width,       n_input_height;
extern uint16_t n_input_visible_width, n_input_visible_height;
extern uint16_t screen_w, screen_x;
extern uint8_t  rotation;
extern bool     use_rgb_8888;

/* libretro core 句柄 */
extern void *core_handle;

/* ---- libretro 函数指针（从 core .so 获取） ---- */

typedef struct {
    /* 核心元数据 */
    void *(*retro_get_system_info)(void);
    void *(*retro_get_system_av_info)(void);

    /* 生命周期 */
    void (*retro_init)(void);
    void (*retro_deinit)(void);
    void (*retro_load_game)(void *);
    void (*retro_unload_game)(void);
    void (*retro_run)(void);
    void (*retro_frame_time)(void);
    int  (*retro_is_support)(const char *);

    /* 序列化（状态快照，非 SRAM） */
    size_t (*retro_serialize_size)(void);
    bool   (*retro_serialize)(void *, size_t);
    bool   (*retro_unserialize)(const void *, size_t);

    /* SRAM 持久化（原版缺失，重构新增） */
    void *(*retro_get_memory_data)(unsigned);
    size_t (*retro_get_memory_size)(unsigned);

    /* 环境变量 */
    bool (*retro_set_environment)(void (*)(unsigned, void *));

    /* 音视频 */
    void (*retro_set_audio_callback)(void *);
    void (*retro_set_video_refresh)(void (*)(const void *, unsigned, unsigned, size_t));
    void (*retro_set_input_poll)(void (*)(void));
    void (*retro_set_input_state)(void (*)(unsigned, unsigned, unsigned, int16_t *));

    /* 设备/控制器 */
    void (*retro_set_controller_port_device)(unsigned, unsigned);
    void (*retro_set_led)(void (*)(unsigned, unsigned));

    /* 进度回调 */
    void (*retro_set_progress_callback)(void (*)(unsigned));

    /* 自定义扩展（原版） */
    void (*retro_get_log_callback)(void);
    int  (*retro_set_unzip)(void (*)(void *, unsigned int, void (*)(unsigned int, void *, void *)));
} retro_ctx_t;

extern retro_ctx_t ctx;

/* ---- SRAM 存档 ---- */

#define RETRO_MEMORY_SAVE_RAM   0
#define RETRO_MEMORY_RTC        1
#define RETRO_MEMORY_SYSTEM_RAM 2
#define RETRO_MEMORY_VIDEO_RAM  3

#define SRAMSHIM_MAX_SIZE       (1 * 1024 * 1024)  /* 1 MB 上限 */
#define SRAMSHIM_DEFAULT_DIR    "/sdcard/cubegm/saves"
#define SRAMSHIM_DEFAULT_INTERVAL 10  /* 秒 */

typedef struct {
    char      path[512];          /* 当前 ROM 的 .srm 文件路径 */
    uint8_t  *buf;                /* 内存中的 SRAM 缓冲区（malloc'd） */
    size_t    size;               /* 缓冲区大小 */
    int       interval_sec;       /* 自动落盘间隔 */
    bool      dirty;              /* 是否有未落盘数据 */
    bool      active;             /* 当前 ROM 已加载 */
    int       save_fd;            /* 落盘时用的 fd */
} sram_state_t;

extern sram_state_t sram_state;

/* sram.h API */
void sram_init(void);
void sram_build_path(const char *rom_path);
void sram_load(void);
void sram_save(void);
void sram_save_to_file(void);
void sram_unload(void);
void *sram_get_data(void);
size_t sram_get_size(void);

/* ---- evdev 手柄 ---- */

#define MAX_JOYSTICKS   2
#define MAX_DEVICES     32

typedef struct {
    uint16_t vid;
    uint16_t pid;
    uint16_t revision;
    char     name[64];
    int      fd;                          /* -1 = 未打开 */
    int      event_fd;                    /* /dev/input/eventN fd */
    bool     is_evdev;
    bool     is_js;                       /* /dev/input/jsN */
    /* 按键映射（button_id -> rkgame key bitmap） */
    uint32_t button_map[32];
    /* 摇杆/手柄映射 */
    uint8_t  axis_count;
    uint8_t  button_count;
} joy_device_t;

extern joy_device_t joy_devs[MAX_DEVICES];
extern int          joy_dev_count;

/* evdev.h API */
void joy_init(void);
int  joy_open(const char *path);
bool joy_poll(void);
void joy_close_all(void);
int  joy_get_key(uint8_t player, uint32_t key_id);
int  joy_autodetect(void);   /* 扫描 /dev/input/ 自动识别 */
bool joy_probe_evdev(const char *path, uint16_t *vid, uint16_t *pid);

/* ---- 配置 ---- */

typedef struct {
    char core_name[128];
    char device0_type[32];
    char device1_type[32];
    char autorun_path[1024];
    char autorun_driver[128];
} rkgame_config_t;

extern rkgame_config_t g_cfg;

void config_load(void);
void config_save(void);

/* ---- 日志 ---- */

#define RKLOG_DEBUG 0
#define RKLOG_INFO  1
#define RKLOG_WARN  2
#define RKLOG_ERROR 3

void rklog(int level, const char *fmt, ...);
#define LOG(fmt, ...) rklog(RKLOG_INFO, fmt, ##__VA_ARGS__)
#define ERR(fmt, ...) rklog(RKLOG_ERROR, fmt, ##__VA_ARGS__)

/* ---- 模块导出 ---- */

/* main.c */
int main(int argc, char **argv);
void autorun(const char *rom, const char *driver);

/* core.c */
int  core_load(const char *rom_path, const char *core_name);
void core_unload(void);
int  core_run(void);

/* sram.c */
/* (see above) */

/* evdev.c */
/* (see above) */

/* disp.c */
void disp_init(void);
void disp_shutdown(void);
void disp_set_rotation(uint32_t rot);
void disp_set_colormode(int mode);
void disp_flip(const void *buf, unsigned width, unsigned height, size_t pitch);

/* audio.c */
void audio_init(void);
void audio_shutdown(void);
void audio_play(const void *buf, size_t frames);

#endif /* RKGAME_H */
