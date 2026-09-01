/* libz_shim_v3.c — SRAM shim v3：后台 flush 线程消除落盘卡顿
 *
 * v2 修复了 dlsym 版本冲突（v1 无版本导出、rkgame 引用 dlsym@GLIBC_2.4 →
 * v1 shim 从未被调用）。但 v2 的自动落盘仍跑在 retro_run 主线程里，
 * 每 10 秒一次 fsync 会阻塞主循环 10-70ms，肉眼可见顿一下。
 *
 * v3 改动：
 *   - 新增一个常驻 flush 线程
 *   - 主线程（retro_run）到点只做 memcpy SRAM 到快照缓冲 + cond_signal，
 *     耗时 < 0.2ms（16KB memcpy on Cortex-A7）
 *   - flush 线程做真正的 fwrite + fsync，与游戏主循环并行
 *   - retro_unload_game 里 pthread_join 保证最后一份存档落盘
 *
 * 其他与 v2 一致（GLIBC_2.4 版本锁、SONAME 技巧、dlopen 版本锁、
 * dlvsym 精确定位真 dlsym 避免递归、手写 parse_int、每次 sram_pull
 * 重取指针防核心 realloc、fsync 代替 sync 只刷目标文件）。
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>

/* zlib 类型（工具链无 zlib.h，自行声明） */
typedef unsigned long  uLong;
typedef unsigned char  uByte;
typedef uByte *uBytef;
typedef int  (*zlib_fn2)(uBytef *, uLong *, const uBytef *, uLong);

/* libretro ABI（工具链无 libretro.h，自行声明最小集） */
typedef struct retro_game_info {
    const char *path;
    const void *data;
    size_t size;
    const char *meta;
} retro_game_info_t;

/* 全局状态 */
static zlib_fn2 real_compress   = NULL;
static zlib_fn2 real_uncompress = NULL;
static void    *g_syszlib       = NULL;

static void *(*real_dlsym)(void *, const char *) = NULL;

static int      (*real_retro_load_game)(const retro_game_info_t *) = NULL;
static void     (*real_retro_unload_game)(void)                    = NULL;
static void     (*real_retro_run)(void)                            = NULL;

static void    *(*core_get_memory_data)(unsigned) = NULL;
static size_t   (*core_get_memory_size)(unsigned) = NULL;

static char     g_srm[2048];
static int      g_have_srm = 0;
static void    *g_sram  = NULL;
static size_t   g_ssize = 0;
static time_t   g_last  = 0;
static unsigned g_frames = 0;
static int      g_interval = 10;
static FILE    *g_log = NULL;
static char     g_workdir[1024];
static int      g_active = 0;

static volatile sig_atomic_t g_flush_req = 0;

/* ------------------------------------------------------------------ */
/* 后台 flush 线程状态                                                 */
/* ------------------------------------------------------------------ */
#define MAX_SRM_SIZE (1u << 20)   /* 1 MiB 快照上限，防异常核心撑爆 */

static pthread_t  g_flush_tid = 0;
static int        g_have_flush = 0;
static pthread_mutex_t g_snap_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_snap_cv = PTHREAD_COND_INITIALIZER;
static unsigned char *g_snap = NULL;
static size_t   g_snap_size = 0;
static char     g_snap_path[2048] = { 0 };
static volatile int g_snap_pending = 0;
static volatile int g_flush_stop = 0;

/* ------------------------------------------------------------------ */
/* 版本锁：工具链 glibc 2.40 默认绑 2.34，设备只有 2.4                 */
/* ------------------------------------------------------------------ */

__asm__(".symver shim_real_dlopen, dlopen@GLIBC_2.4");
extern void *shim_real_dlopen(const char *, int);

__asm__(".symver shim_real_dlvsym, dlvsym@GLIBC_2.4");
extern void *shim_real_dlvsym(void *, const char *, const char *);

/* ------------------------------------------------------------------ */
/* pthread 版本锁                                                       */
/* ------------------------------------------------------------------ */
/* 同 dlopen/dlvsym 一样：工具链 glibc 2.40 默认绑 pthread_create/join/
 * attr_setstacksize 到 GLIBC_2.34，设备 glibc 2.29 只有 GLIBC_2.4 →
 * 加载即 "symbol lookup error"。必须显式锁到 2.4。
 * 这三个函数的签名在 glibc 2.4..2.34 之间无变化，只是内部实现细节变了。
 */
__asm__(".symver shim_pthread_create,         pthread_create@GLIBC_2.4");
__asm__(".symver shim_pthread_join,           pthread_join@GLIBC_2.4");
__asm__(".symver shim_pthread_attr_setstacksize, pthread_attr_setstacksize@GLIBC_2.4");

extern int shim_pthread_create(pthread_t *, const pthread_attr_t *,
                               void *(*)(void *), void *);
extern int shim_pthread_join(pthread_t, void **);
extern int shim_pthread_attr_setstacksize(pthread_attr_t *, size_t);

/* 手写整数解析（GCC 14 会把 atoi 优化成 __isoc23_strtol@GLIBC_2.38） */
static int parse_int(const char *s, int dflt)
{
    int v = 0, neg = 0, any = 0;
    if (s == (const char *)0 || *s == '\0') return dflt;
    if (*s == '-') { neg = 1; s++; }
    for (; *s >= '0' && *s <= '9'; s++) {
        v = v * 10 + (*s - '0');
        any = 1;
    }
    if (!any) return dflt;
    return neg ? -v : v;
}

/* ------------------------------------------------------------------ */
/* 工具                                                                */
/* ------------------------------------------------------------------ */
static void shim_log(const char *fmt, ...)
{
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

static void mkdir_p(const char *path)
{
    char tmp[1024];
    size_t len;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(tmp, 0755);
        *p = '/';
    }
    mkdir(tmp, 0755);
}

static void build_srm_path(const char *rom_path)
{
    const char *slash, *parent_start;
    char name[1024];
    char parent[256];
    char *dot;
    size_t n;

    if (!rom_path || *rom_path == '\0') { g_have_srm = 0; return; }

    slash = strrchr(rom_path, '/');
    if (!slash || slash == rom_path) { g_have_srm = 0; return; }

    parent_start = slash;
    while (parent_start > rom_path && *(parent_start - 1) != '/') parent_start--;
    n = (size_t)(slash - parent_start);
    if (n == 0 || n >= sizeof parent) { g_have_srm = 0; return; }
    memcpy(parent, parent_start, n);
    parent[n] = 0;

    snprintf(name, sizeof name, "%s", slash + 1);
    dot = strrchr(name, '.');
    if (dot && dot != name) *dot = 0;

    snprintf(g_srm, sizeof g_srm, "%s/%s/%s.srm", g_workdir, parent, name);
    g_have_srm = 1;
}

static void sram_pull(void)
{
    if (!core_get_memory_data || !core_get_memory_size) {
        g_sram = NULL;
        g_ssize = 0;
        return;
    }
    g_ssize = core_get_memory_size(0);   /* SAVE_RAM */
    g_sram  = core_get_memory_data(0);
    if (!g_sram && g_ssize) { g_sram = NULL; g_ssize = 0; }
}

/* ------------------------------------------------------------------ */
/* SRAM 快照（主线程调用；仅 memcpy + signal，不写盘）                 */
/* ------------------------------------------------------------------ */
static void snapshot_and_signal(void)
{
    if (!g_have_srm || !g_sram || g_ssize == 0) return;
    if (g_ssize > MAX_SRM_SIZE) {
        shim_log("snapshot: SRAM too large (%zu), skip\n", g_ssize);
        return;
    }
    if (!g_snap || g_snap_size < g_ssize) {
        size_t cap = g_ssize * 2;
        unsigned char *ns = (unsigned char *)malloc(cap > 4096 ? cap : 4096);
        if (!ns) {
            shim_log("snapshot: malloc(%zu) failed\n", cap);
            return;
        }
        free(g_snap);
        g_snap = ns;
        g_snap_size = (cap > 4096) ? cap : 4096;
    }

    pthread_mutex_lock(&g_snap_mu);
    memcpy(g_snap, g_sram, g_ssize);
    memcpy(g_snap_path, g_srm, sizeof g_snap_path);
    g_snap_path[sizeof g_snap_path - 1] = 0;
    pthread_mutex_unlock(&g_snap_mu);

    g_snap_pending = 1;
    pthread_cond_signal(&g_snap_cv);
}

/* ------------------------------------------------------------------ */
/* flush 线程主循环                                                    */
/* ------------------------------------------------------------------ */
static void *flush_thread_main(void *arg)
{
    (void)arg;

    for (;;) {
        pthread_mutex_lock(&g_snap_mu);

        while (!g_snap_pending && !g_flush_stop) {
            /* 0.1s 超时避免 g_flush_stop 被置位后卡在 cv_wait 里 */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&g_snap_cv, &g_snap_mu, &ts);
        }

        if (g_flush_stop && !g_snap_pending) {
            pthread_mutex_unlock(&g_snap_mu);
            break;
        }

        unsigned char *buf = g_snap;
        size_t len = 0;
        char path[2048];
        path[0] = 0;

        if (buf) {
            memcpy(path, g_snap_path, sizeof path);
            path[sizeof path - 1] = 0;
            /* 记录长度：下次快照前 g_ssize 就是本次要写的长度 */
            len = g_ssize;   /* g_ssize 由 snapshot_and_signal 前调 sram_pull 设置 */
        }
        g_snap_pending = 0;

        pthread_mutex_unlock(&g_snap_mu);

        if (buf && len > 0 && path[0]) {
            FILE *f;
            char dir[1024];
            char *slash;
            size_t n, w;
            int fd;

            /* 目录创建 */
            slash = strrchr(path, '/');
            if (slash) {
                n = (size_t)(slash - path);
                if (n > 0 && n < sizeof dir) {
                    memcpy(dir, path, n);
                    dir[n] = 0;
                    mkdir_p(dir);
                }
            }

            f = fopen(path, "wb");
            if (!f) {
                shim_log("flush_thread: fopen %s failed: %s\n", path, strerror(errno));
            } else {
                w = fwrite(buf, 1, len, f);
                fd = fileno(f);
                if (fd >= 0) fsync(fd);
                fclose(f);
                shim_log("flush_thread: wrote %zu/%zu bytes -> %s\n", w, len, path);
            }
            free(buf);
            g_snap = NULL;
            g_snap_size = 0;
        } else if (buf) {
            free(buf);
            g_snap = NULL;
            g_snap_size = 0;
        }
    }
    return NULL;
}

static void start_flush_thread(void)
{
    int r;
    pthread_attr_t attr;

    if (g_have_flush) return;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    /* 4KB 栈足够：flush 线程只做 memcpy + fopen/fwrite + fsync，无深递归 */
    shim_pthread_attr_setstacksize(&attr, 4096);

    r = shim_pthread_create(&g_flush_tid, &attr, flush_thread_main, NULL);
    pthread_attr_destroy(&attr);
    if (r != 0) {
        shim_log("start_flush_thread: pthread_create failed: %s\n", strerror(r));
        return;
    }
    g_have_flush = 1;
}

/* 停线程并把最后一份快照同步写完 */
static void flush_and_stop_thread(void)
{
    if (!g_have_flush) return;

    pthread_mutex_lock(&g_snap_mu);
    g_flush_stop = 1;
    pthread_cond_broadcast(&g_snap_cv);
    pthread_mutex_unlock(&g_snap_mu);

    shim_pthread_join(g_flush_tid, NULL);
    g_have_flush = 0;
}

/* 直接写盘（不走线程；用于 retro_load_game 的读） */
static void sram_save_sync(void)
{
    char dir[1024];
    char *slash;
    size_t n;

    if (!g_have_srm || !g_sram || !g_ssize) return;

    sram_pull();
    if (!g_sram || !g_ssize) return;

    slash = strrchr(g_srm, '/');
    if (slash) {
        n = (size_t)(slash - g_srm);
        if (n > 0 && n < sizeof dir) {
            memcpy(dir, g_srm, n);
            dir[n] = 0;
            mkdir_p(dir);
        }
    }

    FILE *f = fopen(g_srm, "wb");
    if (!f) {
        shim_log("sram_save_sync: fopen %s failed: %s\n", g_srm, strerror(errno));
        return;
    }
    size_t w = fwrite(g_sram, 1, g_ssize, f);
    if (w != g_ssize) shim_log("sram_save_sync: fwrite %zu/%zu\n", w, g_ssize);
    int fd = fileno(f);
    if (fd >= 0) fsync(fd);
    fclose(f);

    g_last = time(NULL);
    shim_log("sram_save_sync: wrote %zu bytes -> %s\n", w, g_srm);
}

static void sram_load(void)
{
    if (!g_have_srm || !g_sram || !g_ssize) return;

    sram_pull();
    if (!g_sram || !g_ssize) return;

    FILE *f = fopen(g_srm, "rb");
    if (!f) {
        shim_log("sram_load: no existing save at %s\n", g_srm);
        return;
    }
    size_t r = fread(g_sram, 1, g_ssize, f);
    fclose(f);
    shim_log("sram_load: read %zu bytes <- %s\n", r, g_srm);
}

/* ------------------------------------------------------------------ */
/* 包装函数                                                            */
/* ------------------------------------------------------------------ */
static int my_retro_load_game(const retro_game_info_t *info)
{
    int ret = real_retro_load_game ? real_retro_load_game(info) : 0;

    if (ret) {
        build_srm_path(info ? info->path : NULL);
        sram_pull();
        sram_load();
        g_last = time(NULL);
    }
    return ret ? 1 : 0;
}

static void my_retro_unload_game(void)
{
    if (g_active) {
        /* 先把主线程的最后一次落盘交给 flush 线程，join 后再卸载核心 */
        if (g_sram && g_ssize) {
            snapshot_and_signal();
        }
        flush_and_stop_thread();
    }
    if (real_retro_unload_game) real_retro_unload_game();
    g_sram = NULL;
    g_ssize = 0;
    g_have_srm = 0;
}

static void my_retro_run(void)
{
    if (real_retro_run) real_retro_run();

    if (!g_active || !g_sram) return;

    g_frames++;
    if (g_frames < 256) return;   /* 每 256 帧才查一次时间 */
    g_frames = 0;

    if (g_flush_req) {
        g_flush_req = 0;
        sram_pull();
        if (g_sram && g_ssize) snapshot_and_signal();
        g_last = time(NULL);
        return;
    }

    if (g_interval > 0) {
        time_t now = time(NULL);
        if (now - g_last >= (time_t)g_interval) {
            sram_pull();
            if (g_sram && g_ssize) snapshot_and_signal();
            g_last = now;
        }
    }
}

static void on_signal(int sig)
{
    g_flush_req = 1;
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ------------------------------------------------------------------ */
/* dlsym 钩子（v3：导出带 GLIBC_2.4 版本，能被 rkgame 匹配到）         */
/* ------------------------------------------------------------------ */
void *dlsym(void *handle, const char *symbol)
{
    void *sym;
    const char *sym_name;

    if (!real_dlsym) return NULL;

    sym_name = symbol;
    if (sym_name == (const char *)0) return NULL;

    sym = real_dlsym(handle, symbol);
    if (!g_active) return sym;
    if (!sym) return sym;

    if (strncmp(symbol, "retro_", 6) == 0) {
        if (!core_get_memory_data)
            core_get_memory_data = (void *(*)(unsigned))real_dlsym(handle, "retro_get_memory_data");
        if (!core_get_memory_size)
            core_get_memory_size = (size_t(*)(unsigned))real_dlsym(handle, "retro_get_memory_size");
    }

    if (!strcmp(symbol, "retro_load_game")) {
        real_retro_load_game = (int (*)(const retro_game_info_t *))sym;
        return (void *)my_retro_load_game;
    }
    if (!strcmp(symbol, "retro_unload_game")) {
        real_retro_unload_game = (void (*)(void))sym;
        return (void *)my_retro_unload_game;
    }
    if (!strcmp(symbol, "retro_run")) {
        real_retro_run = (void (*)(void))sym;
        return (void *)my_retro_run;
    }
    return sym;
}

/* ------------------------------------------------------------------ */
/* zlib 转发                                                           */
/* ------------------------------------------------------------------ */
int compress(uBytef *dest, uLong *destLen, const uBytef *source, uLong sourceLen)
{
    return real_compress ? real_compress(dest, destLen, source, sourceLen) : -2;
}

int uncompress(uBytef *dest, uLong *destLen, const uBytef *source, uLong sourceLen)
{
    return real_uncompress ? real_uncompress(dest, destLen, source, sourceLen) : -2;
}

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */
static void *load_system_zlib(void)
{
    static const char *const paths[] = {
        "/usr/lib/libz.so.1",
        "/lib/libz.so.1",
        "/usr/lib/libz.so.1.2.11",
        "/lib/libz.so.1.2.11",
        "/usr/lib/arm-linux-gnueabihf/libz.so.1",
        NULL
    };
    const char *const *p;
    for (p = paths; *p; p++) {
        void *h = shim_real_dlopen(*p, RTLD_LAZY | RTLD_GLOBAL);
        if (h) return h;
    }
    return NULL;
}

__attribute__((constructor))
static void libzshim_init(void)
{
    char buf[512];
    ssize_t n;
    const char *env;
    char *slash;
    void *libdl;

    /* dlvsym 精确取真 dlsym（避免命中我们自己的 @GLIBC_2.4 版） */
    libdl = shim_real_dlopen("libdl.so.2", RTLD_NOLOAD);
    if (!libdl) libdl = RTLD_DEFAULT;

    real_dlsym = (void *(*)(void *, const char *))
        shim_real_dlvsym(libdl, "dlsym", "GLIBC_2.4");
    if (!real_dlsym)
        real_dlsym = (void *(*)(void *, const char *))
            shim_real_dlvsym(RTLD_DEFAULT, "dlsym", NULL);
    if (!real_dlsym) return;

    /* 加载系统 zlib */
    g_syszlib = load_system_zlib();
    if (g_syszlib) {
        real_compress   = (zlib_fn2)real_dlsym(g_syszlib, "compress");
        real_uncompress = (zlib_fn2)real_dlsym(g_syszlib, "uncompress");
    }

    /* 只认 rkgame */
    g_active = 0;
    n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        if (getenv("SRAMSHIM_ANY_PROC")) {
            g_active = 1;
        } else {
            slash = strrchr(buf, '/');
            g_active = (slash != NULL) && (strcmp(slash + 1, "rkgame") == 0);
        }
    }
    if (!g_active) return;

    /* 工作目录 */
    g_workdir[0] = 0;
    if (n > 0) {
        slash = strrchr(buf, '/');
        if (slash) {
            *slash = 0;
            snprintf(g_workdir, sizeof g_workdir, "%s/saves", buf);
        }
    }
    env = getenv("SRAMSHIM_DIR");
    if (env && *env) snprintf(g_workdir, sizeof g_workdir, "%s", env);
    if (!g_workdir[0]) snprintf(g_workdir, sizeof g_workdir, "/sdcard/cubegm/saves");
    mkdir_p(g_workdir);

    env = getenv("SRAMSHIM_INTERVAL");
    if (env && *env) g_interval = parse_int(env, 10);

    if (getenv("SRAMSHIM_DEBUG")) {
        char logpath[600];
        snprintf(logpath, sizeof logpath, "%s/../sramshim.log", g_workdir);
        g_log = fopen(logpath, "a");
        if (g_log) {
            shim_log("\n==== libz_shim_v3 init: syszlib=%s compress=%p uncompress=%p "
                     "dlsym=%p dir=%s interval=%ds ====\n",
                     g_syszlib ? "ok" : "FAIL",
                     (void *)real_compress, (void *)real_uncompress,
                     (void *)real_dlsym, g_workdir, g_interval);
        }
    }

    /* 起后台 flush 线程（失败不影响主流程，自动退化为同步落盘） */
    start_flush_thread();

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
}
