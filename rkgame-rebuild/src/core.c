/* ============================================================
 * rkgame-rebuild — libretro core 加载器
 * ============================================================ */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>

/* CI 使用 crosstool-NG glibc-2.29 sysroot。
 * dlopen/dlsym/dlclose 全部为 @GLIBC_2.4，设备 glibc 2.29 完全兼容。
 * 无需 __asm__ 版本锁定或运行时兜底。 */

#include "rkgame.h"

/* ---- 配置项表 ---- */
static char corecfg[16000];

static void *retro_get_env_cb(void *cb, unsigned cmd, void *data);
static int  get_cfg_value(const char *key, char *out, size_t out_size, const char *cfg);

static void *get_core_symbol(void *handle, const char *name)
{
    return dlsym(handle, name);
}

int core_load(const char *rom_path, const char *core_name)
{
    char path[512];
    int ret;

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

    snprintf(path, sizeof(path), "%s/cores/%s", work_path, core_name);
    core_handle = dlopen(path, RTLD_NOW);
    if (!core_handle) {
        ERR("Core_Load: dlopen %s fail", path);
        return -1;
    }
    LOG("Core_Load: dlopen %s OK", path);

    memset(&ctx, 0, sizeof(ctx));
    ctx.retro_get_system_info      = get_core_symbol(core_handle, "retro_get_system_info");
    ctx.retro_get_system_av_info   = get_core_symbol(core_handle, "retro_get_system_av_info");
    ctx.retro_init                 = get_core_symbol(core_handle, "retro_init");
    ctx.retro_deinit               = get_core_symbol(core_handle, "retro_deinit");
    ctx.retro_load_game            = get_core_symbol(core_handle, "retro_load_game");
    ctx.retro_unload_game          = get_core_symbol(core_handle, "retro_unload_game");
    ctx.retro_run                  = get_core_symbol(core_handle, "retro_run");
    ctx.retro_is_support           = get_core_symbol(core_handle, "retro_is_support");
    ctx.retro_serialize_size       = get_core_symbol(core_handle, "retro_serialize_size");
    ctx.retro_serialize            = get_core_symbol(core_handle, "retro_serialize");
    ctx.retro_unserialize          = get_core_symbol(core_handle, "retro_unserialize");
    ctx.retro_get_memory_data      = get_core_symbol(core_handle, "retro_get_memory_data");
    ctx.retro_get_memory_size      = get_core_symbol(core_handle, "retro_get_memory_size");
    ctx.retro_set_environment      = get_core_symbol(core_handle, "retro_set_environment");
    ctx.retro_set_video_refresh    = get_core_symbol(core_handle, "retro_set_video_refresh");
    ctx.retro_set_audio_callback   = get_core_symbol(core_handle, "retro_set_audio_callback");
    ctx.retro_set_input_poll       = get_core_symbol(core_handle, "retro_set_input_poll");
    ctx.retro_set_input_state      = get_core_symbol(core_handle, "retro_set_input_state");
    ctx.retro_set_controller_port_device = get_core_symbol(core_handle,
        "retro_set_controller_port_device");
    ctx.retro_set_progress_callback = get_core_symbol(core_handle,
        "retro_set_progress_callback");
    ctx.retro_get_log_callback     = get_core_symbol(core_handle, "retro_get_log_callback");
    ctx.retro_set_unzip            = get_core_symbol(core_handle, "retro_set_unzip");

    if (!ctx.retro_get_memory_data || !ctx.retro_get_memory_size) {
        ERR("Core_Load: core missing retro_get_memory_data/size — SRAM persistence unavailable");
    } else {
        LOG("Core_Load: SRAM support available");
    }

    if (!ctx.retro_set_environment) {
        ERR("Core_Load: retro_set_environment not found");
        dlclose(core_handle);
        core_handle = NULL;
        return -2;
    }

    if (ctx.retro_is_support) {
        int support = ctx.retro_is_support(rom_path);
        if (support < 0) {
            ERR("Core_Load: core rejects ROM %s", rom_path);
            dlclose(core_handle);
            core_handle = NULL;
            return -3;
        }
        LOG("Core_Load: core supports ROM");
    }

    ctx.retro_set_environment(retro_get_env_cb);

    if (ctx.retro_init)
        ctx.retro_init();

    if (ctx.retro_set_controller_port_device) {
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

    if (ctx.retro_set_progress_callback)
        ctx.retro_set_progress_callback(NULL);

    retro_game_info_t game_info = { 0 };
    game_info.path = rom_path;
    game_info.data = NULL;
    game_info.size = 0;
    game_info.metadata = NULL;

    if (ctx.retro_load_game) {
        ctx.retro_load_game(&game_info);
        LOG("Core_Load: retro_load_game done");
    }

    if (ctx.retro_set_video_refresh)
        ctx.retro_set_video_refresh(disp_flip);
    if (ctx.retro_set_input_poll)
        ctx.retro_set_input_poll(NULL);
    if (ctx.retro_set_input_state)
        ctx.retro_set_input_state(NULL);

    sram_build_path(rom_path);
    sram_load();

    ret = core_run();

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
    dlclose(core_handle);
    core_handle = NULL;
    memset(&ctx, 0, sizeof(ctx));
    LOG("core unloaded");
}

int core_run(void)
{
    LOG("core_run: entering main loop");
    while (1) {
        if (ctx.retro_run) {
            ctx.retro_run();
        }
    }
    return 0;
}

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
            snprintf(save_directory, sizeof(save_directory), "%ssaves/", work_path);
            *dir = save_directory;
            return (void *)1;
        }
        case 0xf:  /* GET_VARIABLE */
            return (void *)1;
        case 0x1b:  /* SET_LOG_INTERFACE */
            return (void *)1;
        case 0x25:
            return (void *)1;
        default:
            return (void *)0;
    }
}

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
