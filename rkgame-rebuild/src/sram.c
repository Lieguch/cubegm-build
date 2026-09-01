/* ============================================================
 * rkgame-rebuild — SRAM 存档持久化
 * ============================================================
 *
 * 根因（反编译实证）：
 *   - rkgame 全文 `retro_get_memory_data` / `retro_get_memory_size` 出现次数 = 0
 *   - 28 个 libemu_*.so 核心全部导出 retro_get_memory_data/size（核心侧就绪）
 *   - 缺口 100% 在前端 rkgame
 *
 * 标准 libretro 存档：
 *   - retro_get_memory_data(RETRO_MEMORY_SAVE_RAM=0) 返回 SRAM 缓冲区指针
 *   - retro_get_memory_size(RETRO_MEMORY_SAVE_RAM=0) 返回缓冲区大小
 *   - 持久化到 <save_dir>/<rom_basename>.srm
 *
 * 调用时机：
 *   - retro_load_game 成功后：sram_load() 读取 .srm 并写入核心
 *   - retro_unload_game 前：    sram_save_to_file() 落盘
 *   - 周期性（10s 间隔）：      sram_save_to_file() fsync（后台线程避免卡顿）
 *
 * ABI 铁律：
 *   - retro_get_memory_data(0) 是正确参数；用 2/3 会取到 SYSTEM_RAM/VIDEO_RAM
 *   - pthread_* 必须 .symver 锁 GLIBC_2.4（设备 glibc 2.29）
 *   - clock_gettime@GLIBC_2.17 可用（设备 glibc 2.29 支持）
 * ============================================================ */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#include "rkgame.h"

/* sram_state 定义在 main.c，本文件只作为外部引用 */

/* ---- 后台落盘线程 ---- */

static volatile sig_atomic_t g_flush_req = 0;
static pthread_t    g_flush_thread;
static pthread_mutex_t g_flush_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_flush_cond  = PTHREAD_COND_INITIALIZER;
static volatile sig_atomic_t g_flush_stop = 0;

/* ---- 版本化符号声明（锁定到设备 glibc 2.29 导出的版本） ---- */

extern int  pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                           void *(*start)(void *), void *arg)
    __asm__("pthread_create@GLIBC_2.4");
extern int  pthread_join(pthread_t thread, void **value)
    __asm__("pthread_join@GLIBC_2.4");
extern int  pthread_mutex_lock(pthread_mutex_t *m)
    __asm__("pthread_mutex_lock@GLIBC_2.4");
extern int  pthread_mutex_unlock(pthread_mutex_t *m)
    __asm__("pthread_mutex_unlock@GLIBC_2.4");
extern int  pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
    __asm__("pthread_cond_wait@GLIBC_2.4");
extern int  clock_gettime(clockid_t clk, struct timespec *ts)
    __asm__("clock_gettime@GLIBC_2.17");

/* ---- 快照与信号 ---- */

static void sram_write_file(void);
static void sram_snapshot_and_signal(void)
{
    if (!sram_state.active || !sram_state.dirty) return;
    if (!ctx.retro_get_memory_data || !ctx.retro_get_memory_size) return;

    size_t size = ctx.retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (size == 0 || size > SRAMSHIM_MAX_SIZE) {
        ERR("sram: invalid SRAM size %zu", size);
        return;
    }

    void *data = ctx.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!data) return;

    pthread_mutex_lock(&g_flush_mutex);
    /* 更新缓冲区（如果大小变了则 realloc） */
    if (sram_state.size != size) {
        free(sram_state.buf);
        sram_state.buf = malloc(size);
        if (!sram_state.buf) {
            sram_state.size = 0;
            pthread_mutex_unlock(&g_flush_mutex);
            return;
        }
        sram_state.size = size;
    }
    memcpy(sram_state.buf, data, size);
    sram_state.dirty = true;
    g_flush_req = 1;
    pthread_cond_wait(&g_flush_cond, &g_flush_mutex);  /* 等落盘完成 */
    pthread_mutex_unlock(&g_flush_mutex);
}

/* ---- 后台落盘线程 ---- */

static void *sram_flush_thread_fn(void *arg)
{
    (void)arg;
    struct timespec ts;
    int64_t next_save = 0;

    while (!g_flush_stop) {
        /* 等待 flush 请求或超时 */
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now = ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
        int64_t timeout_us;
        if (next_save > 0) {
            timeout_us = next_save - now;
            if (timeout_us < 0) timeout_us = 0;
        } else {
            timeout_us = 100000;  /* 100ms 轮询间隔 */
        }

        /* 用 cond_wait 等待，但设置超时避免死锁 */
        struct timespec to;
        clock_gettime(CLOCK_REALTIME, &to);
        to.tv_sec  += (time_t)(timeout_us / 1000000);
        to.tv_nsec += (long)((timeout_us % 1000000) * 1000);
        if (to.tv_nsec >= 1000000000L) {
            to.tv_sec++; to.tv_nsec -= 1000000000L;
        }

        pthread_mutex_lock(&g_flush_mutex);
        if (!g_flush_req && next_save <= now) {
            /* 没有请求且未到定期保存时间，等待 */
            pthread_mutex_unlock(&g_flush_mutex);
            usleep(50000);
            continue;
        }
        pthread_mutex_unlock(&g_flush_mutex);

        if (g_flush_req) {
            pthread_mutex_lock(&g_flush_mutex);
            g_flush_req = 0;
            pthread_mutex_unlock(&g_flush_mutex);
            sram_write_file();
        }

        pthread_mutex_lock(&g_flush_mutex);
        if (sram_state.dirty && sram_state.interval_sec > 0) {
            clock_gettime(CLOCK_MONOTONIC, &ts);
            int64_t now2 = ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
            if (next_save == 0 || now2 >= next_save) {
                next_save = now2 + (int64_t)sram_state.interval_sec * 1000000;
            }
        }
        pthread_mutex_unlock(&g_flush_mutex);
    }
    return NULL;
}

/* ---- SRAM 文件写入 ---- */

static void sram_write_file(void)
{
    if (!sram_state.buf || !sram_state.size || !sram_state.path[0]) return;

    /* 确保目录存在 */
    char dir[512];
    strncpy(dir, sram_state.path, sizeof(dir));
    char *last = strrchr(dir, '/');
    if (last) {
        *last = '\0';
        mkdir(dir, 0755);
    }

    /* 写入 */
    FILE *fp = fopen(sram_state.path, "wb");
    if (!fp) {
        ERR("sram: cannot open %s for writing: %s", sram_state.path, strerror(errno));
        return;
    }
    size_t n = fwrite(sram_state.buf, 1, sram_state.size, fp);
    if (n != sram_state.size) {
        ERR("sram: short write %zu/%zu", n, sram_state.size);
    }
    int fd = fileno(fp);
    if (fd >= 0) fsync(fd);
    fclose(fp);
    LOG("sram: saved %s (%zu bytes)", sram_state.path, sram_state.size);
}

/* ---- 公开 API ---- */

void sram_init(void)
{
    /* 初始化全局 sram_state 默认值 */
    memset(&sram_state, 0, sizeof(sram_state));
    sram_state.interval_sec = SRAMSHIM_DEFAULT_INTERVAL;
    sram_state.save_fd = -1;
    sram_state.dirty = false;
    sram_state.active = false;
    sram_state.size = 0;
    sram_state.buf = NULL;
    char *dir_env = getenv("SRAMSHIM_DIR");
    if (dir_env) {
        snprintf(save_directory, sizeof(save_directory), "%s", dir_env);
    } else {
        snprintf(save_directory, sizeof(save_directory), SRAMSHIM_DEFAULT_DIR);
    }

    char *interval_env = getenv("SRAMSHIM_INTERVAL");
    if (interval_env) {
        sram_state.interval_sec = atoi(interval_env);
    }

    LOG("sram_init: dir=%s interval=%ds", save_directory, sram_state.interval_sec);
}

void sram_build_path(const char *rom_path)
{
    if (!rom_path) return;

    /* 提取 ROM 父目录名和文件名（无扩展名） */
    char dir_name[256] = "";
    char file_base[256] = "";

    const char *slash = strrchr(rom_path, '/');
    if (slash) {
        size_t dirlen = slash - rom_path;
        if (dirlen > 0) {
            const char *prev = slash - 1;
            while (prev > rom_path && *prev != '/') prev--;
            if (prev > rom_path) {
                size_t n = slash - prev - 1;
                if (n < sizeof(dir_name)) {
                    memcpy(dir_name, prev + 1, n);
                    dir_name[n] = '\0';
                }
            }
        }
    }

    /* 提取文件名（去掉 .zip / .srm / .bin 等扩展名） */
    char *name = strrchr(rom_path, '/');
    if (!name) name = (char *)rom_path;
    else name++;
    strncpy(file_base, name, sizeof(file_base) - 1);
    /* 去掉扩展名 */
    char *dot = strrchr(file_base, '.');
    if (dot) *dot = '\0';

    snprintf(sram_state.path, sizeof(sram_state.path),
             "%s/%s/%s.srm", save_directory, dir_name, file_base);
    LOG("sram: path=%s", sram_state.path);
}

void sram_load(void)
{
    if (!sram_state.path[0]) return;
    if (!ctx.retro_get_memory_data || !ctx.retro_get_memory_size) {
        ERR("sram_load: core missing retro_get_memory_data");
        return;
    }

    FILE *fp = fopen(sram_state.path, "rb");
    if (!fp) {
        LOG("sram_load: no existing file %s (new game)", sram_state.path);
        return;
    }

    size_t size = ctx.retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (size == 0 || size > SRAMSHIM_MAX_SIZE) {
        ERR("sram_load: invalid SRAM size %zu", size);
        fclose(fp);
        return;
    }

    uint8_t *buf = malloc(size);
    if (!buf) { fclose(fp); return; }

    size_t n = fread(buf, 1, size, fp);
    fclose(fp);

    if (n != size) {
        ERR("sram_load: short read %zu/%zu", n, size);
        free(buf);
        return;
    }

    void *dest = ctx.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (dest) {
        memcpy(dest, buf, size);
        LOG("sram_load: loaded %s (%zu bytes) into core", sram_state.path, size);
    }
    free(buf);

    sram_state.active = true;
    sram_state.dirty = false;
}

void sram_save(void)
{
    if (!sram_state.active) return;
    if (!ctx.retro_get_memory_data || !ctx.retro_get_memory_size) return;

    size_t size = ctx.retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (size == 0) return;

    void *data = ctx.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!data) return;

    pthread_mutex_lock(&g_flush_mutex);
    if (sram_state.size != size) {
        free(sram_state.buf);
        sram_state.buf = malloc(size);
        if (!sram_state.buf) {
            sram_state.size = 0;
            pthread_mutex_unlock(&g_flush_mutex);
            return;
        }
        sram_state.size = size;
    }
    memcpy(sram_state.buf, data, size);
    sram_state.dirty = true;
    pthread_mutex_unlock(&g_flush_mutex);
}

void sram_save_to_file(void)
{
    sram_save();
    if (sram_state.dirty) {
        sram_write_file();
        sram_state.dirty = false;
    }
}

void sram_unload(void)
{
    sram_save_to_file();
    pthread_mutex_lock(&g_flush_mutex);
    free(sram_state.buf);
    sram_state.buf = NULL;
    sram_state.size = 0;
    sram_state.active = false;
    pthread_mutex_unlock(&g_flush_mutex);
}

void *sram_get_data(void)
{
    return sram_state.buf;
}

size_t sram_get_size(void)
{
    return sram_state.size;
}

/*
 * sram_get_data_from_core：直接从 core 取 SRAM（用于调试/验证）。
 * 调用时必须在 core 已加载状态下。
 */
void *sram_get_data_from_core(size_t *out_size)
{
    if (!ctx.retro_get_memory_data || !ctx.retro_get_memory_size)
        return NULL;
    size_t size = ctx.retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (out_size) *out_size = size;
    return ctx.retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
}
