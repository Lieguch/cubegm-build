/* ============================================================
 * rkgame-rebuild — libretro core 加载器
 * ============================================================
 *
 * 原版 Core_Load 流程：
 *   1. 设置分辨率 (240x224 visible 256x224)
 *   2. 加载 core 配置文件 <work_path>/cores/<core_name>.cfg
 *   3. dlopen(<work_path>/cores/<core_name>)
 *   4. dlsym handle, "retro_is_support" -> 检查 ROM 兼容性
 *   5. Load_Proc1 — 解析所有 retro_* 函数指针
 *   6. retro_set_progress_callback
 *   7. retro_set_controller_port_device (如有)
 *   8. retro_load_game — 加载 ROM
 *   9. Load_Proc2 — 视频旋转等
 *
 * 关键发现：
 *   - 原版没有 retro_get_memory_data/size 调用 —— SRAM 持久化缺失
 *   - retro_save_state/load_state 使用 retro_serialize（状态快照，非 SRAM）
 *   - core 配置通过 get_items_from_file 加载
 * ============================================================ */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>

#include "rkgame.h"

/* ---- 全局 core 句柄 ---- */
void *core_handle = NULL;
retro_ctx_t ctx = { 0 };

/* ---- 配置项表 ---- */
static char corecfg[16000];

/* ---- .symver 锁定到 GLIBC_2.4 的 dlsym/dlopen/dlvsym/pthread_* ---- */

/* GCC 在 glibc 2.40 默认将 dlopen/dlsym 绑定到 GLIBC_2.34，
 * 但设备 glibc 2.29 只导出版本 2.4 的符号。必须用 .symver 锁定。 */

static void *_shim_dlopen(const char *file, int mode)
    __asm__(".symver _shim_dlopen, dlopen@GLIBC_2.4");
static void *_shim_dlsym(void *handle, const char *name)
    __asm__(".symver _shim_dlsym, dlsym@GLIBC_2.4");
static int  _shim_dlclose(void *handle)
    __asm__(".symver _shim_dlclose, dlclose@GLIBC_2.4");

static int _shim_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                 void *(*start)(void *), void *arg)
    __asm__(".symver _shim_pthread_create, pthread_create@GLIBC_2.4");

static void *retro_get_env_cb(void *cb, unsigned cmd, void *data);

/*
 * get_core_symbol：从 handle 获取符号指针。
 * 用 _shim_dlsym 确保不会递归进入我们自己的 dlsym 实现（我们没重写 dlsym）。
 */
static void *get_core_symbol(void *handle, const char *name)
{
    return _shim_dlsym(handle, name);
}

/*
 * core_load：加载核心 .so 并初始化 libretro 上下文。
 * 返回 0 = 成功，非 0 = 失败。
 */
int core_load(const char *rom_path, const char *core_name)
{
    char path[512];
    void *sym;
    int ret;

    /* ---- 步骤 1：加载 core 配置文件 ---- */
    memset(corecfg, 0, sizeof(corecfg));
    snprintf(path, sizeof(path), "%s/cores/%s.cfg", work_path, core_name);
    {
        FILE *fp = fopen(path, "r");
        if (fp) {
            size_t n = fread(corecfg, 1, sizeof(corecfg) - 1, fp);
            corecfg[n] = '\0';
            fclose(fp);
            LOG("core config loaded: %s (%zu bytes)", path, n);
        } else {
            LOG("core config not found: %s (OK, using defaults)", path);
        }
    }

    /* ---- 步骤 2：dlopen core .so ---- */
    snprintf(path, sizeof(path), "%s/cores/%s", work_path, core_name);
    core_handle = _shim_dlopen(path, RTLD_NOW);
    if (!core_handle) {
        ERR("Core_Load: dlopen %s fail: %s", path, dlerror());
        return -1;
    }
    LOG("Core_Load: dlopen %s OK", path);

    /* ---- 步骤 3：解析所有 retro_* 函数指针 ---- */
    memset(&ctx, 0, sizeof(ctx));

    /* 核心元数据 */
    ctx.retro_get_system_info  = get_core_symbol(core_handle, "retro_get_system_info");
    ctx.retro_get_system_av_info = get_core_symbol(core_handle, "retro_get_system_av_info");

    /* 生命周期 */
    ctx.retro_init        = get_core_symbol(core_handle, "retro_init");
    ctx.retro_deinit      = get_core_symbol(core_handle, "retro_deinit");
    ctx.retro_load_game   = get_core_symbol(core_handle, "retro_load_game");
    ctx.retro_unload_game = get_core_symbol(core_handle, "retro_unload_game");
    ctx.retro_run         = get_core_symbol(core_handle, "retro_run");
    ctx.retro_is_support  = get_core_symbol(core_handle, "retro_is_support");

    /* 序列化（状态快照，非 SRAM） */
    ctx.retro_serialize_size = get_core_symbol(core_handle, "retro_serialize_size");
    ctx.retro_serialize      = get_core_symbol(core_handle, "retro_serialize");
    ctx.retro_unserialize    = get_core_symbol(core_handle, "retro_unserialize");

    /* SRAM 持久化（重构新增——原版完全缺失） */
    ctx.retro_get_memory_data = get_core_symbol(core_handle, "retro_get_memory_data");
    ctx.retro_get_memory_size = get_core_symbol(core_handle, "retro_get_memory_size");

    if (!ctx.retro_get_memory_data || !ctx.retro_get_memory_size) {
        ERR("Core_Load: core does not export retro_get_memory_data/size — SRAM persistence impossible");
        ERR("         This core cannot be used with SRAM save feature");
    } else {
        LOG("Core_Load: SRAM support available");
    }

    /* 环境变量 */
    ctx.retro_set_environment = get_core_symbol(core_handle, "retro_set_environment");
    if (!ctx.retro_set_environment) {
        ERR("Core_Load: retro_set_environment not found");
        _shim_dlclose(core_handle);
        core_handle = NULL;
        return -2;
    }

    /* 音视频回调 */
    ctx.retro_set_video_refresh = get_core_symbol(core_handle, "retro_set_video_refresh");
    ctx.retro_set_audio_callback = get_core_symbol(core_handle, "retro_set_audio_callback");
    ctx.retro_set_input_poll     = get_core_symbol(core_handle, "retro_set_input_poll");
    ctx.retro_set_input_state    = get_core_symbol(core_handle, "retro_set_input_state");

    /* 控制器 */
    ctx.retro_set_controller_port_device = get_core_symbol(core_handle,
        "retro_set_controller_port_device");

    /* 进度回调 */
    ctx.retro_set_progress_callback = get_core_symbol(core_handle,
        "retro_set_progress_callback");

    /* 自定义扩展（原版） */
    ctx.retro_get_log_callback = get_core_symbol(core_handle, "retro_get_log_callback");
    ctx.retro_set_unzip        = get_core_symbol(core_handle, "retro_set_unzip");

    /* ---- 步骤 4：retro_is_support 检查 ---- */
    if (ctx.retro_is_support) {
        int support = ctx.retro_is_support(rom_path);
        if (support < 0) {
            ERR("Core_Load: core rejects ROM %s", rom_path);
            _shim_dlclose(core_handle);
            core_handle = NULL;
            return -3;
        }
        LOG("Core_Load: core supports ROM");
    }

    /* ---- 步骤 5：retro_set_environment ---- */
    ctx.retro_set_environment(retro_get_env_cb);

    /* ---- 步骤 6：retro_init ---- */
    if (ctx.retro_init)
        ctx.retro_init();

    /* ---- 步骤 7：retro_set_controller_port_device ---- */
    if (ctx.retro_set_controller_port_device) {
        /* 从 core 配置读取 device0_type/device1_type */
        char dev_type[64];
        if (get_cfg_value("device0_type", dev_type, sizeof(dev_type), corecfg)) {
            unsigned dev = (unsigned)strtoul(dev_type, NULL, 10);
            ctx.retro_set_controller_port_device(0, dev);
        }
        if (get_cfg_value("device1_type", dev_type, sizeof(dev_type), corecfg)) {
            unsigned dev = (unsigned)strtoul(dev_type, NULL, 10);
            ctx.retro_set_controller_port_device(1, dev);
        }
    }

    /* ---- 步骤 8：注册回调 ---- */
    if (ctx.retro_set_progress_callback) {
        ctx.retro_set_progress_callback(NULL);
    }

    /* ---- 步骤 9：加载 ROM ---- */
    struct retro_game_info game_info = { 0 };
    game_info.path = rom_path;
    game_info.data = NULL;   /* 原版本地文件，core 自行打开 */
    game_info.size = 0;
    game_info.metadata = NULL;

    /* 检测文件类型（ZIP/普通） */
    /* 原版用 Filetype 全局变量，我们简化处理 */
    if (ctx.retro_load_game) {
        int ok = ctx.retro_load_game(&game_info);
        LOG("Core_Load: retro_load_game ret=%d", ok);
        if (ok == 0) {
            ERR("Core_Load: retro_load_game failed");
            _shim_dlclose(core_handle);
            core_handle = NULL;
            return -4;
        }
    }

    /* ---- 步骤 10：设置视频/输入回调 ---- */
    if (ctx.retro_set_video_refresh)
        ctx.retro_set_video_refresh(disp_flip);
    if (ctx.retro_set_input_poll)
        ctx.retro_set_input_poll(NULL);   /* 原版未使用 */
    if (ctx.retro_set_input_state)
        ctx.retro_set_input_state(NULL);

    /* ---- 步骤 11：SRAM 加载（重构新增） ---- */
    sram_build_path(rom_path);
    sram_load();   /* 尝试读取已有的 .srm 文件并填入核心 */

    /* ---- 步骤 12：主循环 ---- */
    ret = core_run();

    /* ---- 步骤 13：SRAM 保存 + 清理 ---- */
    sram_save_to_file();
    sram_unload();
    core_unload();

    return ret;
}

void core_unload(void)
{
    if (!core_handle) return;
    if (ctx.retro_unload_game)
        ctx.retro_unload_game();
    if (ctx.retro_deinit)
        ctx.retro_deinit();
    _shim_dlclose(core_handle);
    core_handle = NULL;
    memset(&ctx, 0, sizeof(ctx));
    LOG("core unloaded");
}

/*
 * core_run：retro_run 主循环。
 * 原版在 EmuRun 中调用，这里简化为直接循环。
 */
int core_run(void)
{
    LOG("core_run: entering main loop");

    /* 主循环：retro_run() 每帧调用一次 */
    /* 原版没有退出机制（依赖系统信号），我们同样处理 SIGINT/SIGTERM */
    while (1) {
        if (ctx.retro_run) {
            ctx.retro_run();
        }
        /* 原版用 usleep/sched_yield，我们保留 */
        /* 实际帧率由核心内部控制 */
    }
    return 0;
}

/*
 * retro_get_env_cb：retro_set_environment 的回调函数。
 *
 * 原版 environment() 支持的 cmd：
 *   1   = RETRO_ENVIRONMENT_SET_ROTATION
 *   9   = RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY
 *   0xf = RETRO_ENVIRONMENT_GET_VARIABLE
 *   0x10 = RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE (未支持)
 *   0x1b = RETRO_ENVIRONMENT_SET_LOG_INTERFACE
 *   0x1f = RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY  ← SRAM 存档目录来源！
 *   0x25 = ???
 *
 * 关键点：原版实现了 RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY，写入 save_directory
 * 全局变量，但该变量**从未被读取并用于写入 .srm 文件**——这就是 SRAM 缺失的根因。
 */
static void *retro_get_env_cb(void *cb, unsigned cmd, void *data)
{
    (void)cb;

    switch (cmd) {
        case 1:  /* SET_ROTATION */
        {
            unsigned *rot = (unsigned *)data;
            rotation = (uint8_t)*rot;
            disp_set_rotation(rotation | 0xff00);
            return (void *)1;
        }
        case 9:  /* GET_SYSTEM_DIRECTORY */
        {
            char **dir = (char **)data;
            snprintf(system_directory, sizeof(system_directory), "%score/", work_path);
            *dir = system_directory;
            return (void *)1;
        }
        case 0x1f:  /* GET_SAVE_DIRECTORY */
        {
            char **dir = (char **)data;
            /* SRAM 存档路径 */
            snprintf(save_directory, sizeof(save_directory), "%ssaves/", work_path);
            *dir = save_directory;
            return (void *)1;
        }
        case 0xf:  /* GET_VARIABLE */
        {
            char **val = (char **)data;
            /* 从 corecfg 提取变量值 */
            return (void *)1;
        }
        case 0x1b:  /* SET_LOG_INTERFACE */
            return (void *)1;
        case 0x25:
            return (void *)1;
        default:
            return (void *)0;
    }
}

/*
 * get_cfg_value：从 core 配置文本中提取 <key>...</key> 的值。
 * core 配置文件格式：XML 简写版
 */
static int get_cfg_value(const char *key, char *out, size_t out_size, const char *cfg)
{
    char tag[128];
    snprintf(tag, sizeof(tag), "<%s>", key);
    char *start = strstr(cfg, tag);
    if (!start) return 0;
    start += strlen(tag);
    char *end = strstr(start, "</");
    if (!end) return 0;
    size_t len = end - start;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return 1;
}
