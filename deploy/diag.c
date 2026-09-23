/* diag.c -- RK3036G device-side diagnostics (CubeGM direction B)
 * =============================================================================
 * PURPOSE
 *   Replace "guess from logs" with measured facts. Runs ON the device, writes a
 *   structured report to /mnt/sdcard/diag_report.txt. One SD-card test round
 *   then answers every question that previously took 10+ patch-test cycles:
 *     - input : the REAL keycode/axis/HAT each physical button produces
 *     - display: does the DRM modeset light HDMI? (test patterns)
 *     - audio : does ALSA "default" open and play?
 *     - cores : which cubegm/cores/*.so dlopen + retro_init cleanly?
 *
 * BUILD (cross, static-ish -- libc + dl only, NO SDL/libpng):
 *   arm-linux-gnueabihf-gcc -O2 -march=armv7-a -mtune=cortex-a7 \
 *     -mfpu=neon-vfpv4 -mfloat-abi=hard --sysroot=$SYSROOT \
 *     -Idrm_headers diag.c -o diag -ldl -static-libgcc
 *   # (drm headers from deploy/drm_headers/; the target board is standard
 *   #  buildroot Linux with libasound.so.2 dlopen'd at runtime)
 *
 * USAGE on device:
 *   diag all          # run every module (default when no arg)
 *   diag sysinfo      # uname / cpu / mem / sound cards / DRM resources
 *   diag input        # dump evdev capabilities + capture keys (interactive)
 *   diag display      # DRM modeset 1280x720 + color/pattern test
 *   diag audio        # ALSA 1 kHz tone, 2 s
 *   diag cores        # dlopen every core in cubegm/cores/
 *
 * SAFETY
 *   Read-only on the SD card. Never touches root.dat / boot / cubegm binaries.
 *   Crashes (SIGSEGV etc.) are caught and logged with the failing module, so a
 *   broken core or a broken DRM call cannot hang the report.
 * ============================================================================= */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/input.h>
/* User-space DRM usage: the kernel UAPI headers annotate pointers with
 * __user (address-space). It is not defined for userspace builds, so define
 * it away BEFORE including <drm/drm.h> (run 281: drm.h:133 "expected ':'
 * before '*' token" — char __user *name). */
#ifndef __user
#define __user
#endif
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>   /* DRM_FORMAT_RGB565 (run 281: undeclared) */
#include <sys/statvfs.h>
#include <sys/stat.h>      /* stat() for GPU probe (cmd_gpu) */

#define REPORT "/mnt/sdcard/diag_report.txt"
static FILE *g_out = NULL;

static void logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    if (g_out) { va_start(ap, fmt); vfprintf(g_out, fmt, ap); va_end(ap); fflush(g_out); }
}

/* ---- crash guard: one bad module must not kill the whole report ---------- */
static volatile sig_atomic_t g_fault_module = 0;
/* Live EGL/GBM/GL probing (dlopen of the Mali blob + DRM master probing) is
 * only safe when NOT running next to a live RetroArch. Boot 'diag all' is
 * forked in background by icube while RetroArch starts -> it must stay GPU
 * read-only. 'diag video' (manual, explicit) is the only entry that runs the
 * live chain. */
static volatile sig_atomic_t g_video_live = 0;
/* Watchdog for the live chain: even a manual 'diag video' must not hang if a
 * blob call deadlocks the GPU. SIGALRM handler stays minimal (async-signal
 * safe): logf already fflush()s g_out after every call, so a plain _exit(3)
 * loses nothing already printed. */
static void video_watchdog(int sig) {
    (void)sig;
    _exit(3);
}
static void crash_handler(int sig) {
    const char *nm = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS"
                   : sig == SIGABRT ? "SIGABRT" : sig == SIGILL ? "SIGILL"
                   : sig == SIGFPE  ? "SIGFPE"  : "SIG?";
    logf("\n[FAULT] %s in module %d (see above; continuing)\n", nm, (int)g_fault_module);
    signal(sig, SIG_DFL);
    /* cannot raise() in handler that may recur -- just exit so report flushes */
    if (g_out) fflush(g_out);
    _exit(1);
}
static void install_guards(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = crash_handler;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
}

/* ===========================================================================
 * sysinfo
 * ========================================================================== */
static void cat_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { logf("  [%s] open failed: %s\n", path, strerror(errno)); return; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        logf("  %s\n", line);
    }
    fclose(f);
}

/* Dump HW register block via /dev/mem mmap */
static void dump_mem(const char *path, uint32_t phys, size_t len) {
    int fd = open(path, O_RDWR);
    if (fd < 0) { logf("  open %s FAILED: %s\n", path, strerror(errno)); return; }
    uint32_t *map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, phys);
    if (map == MAP_FAILED) { logf("  mmap 0x%x FAILED: %s\n", phys, strerror(errno)); close(fd); return; }
    for (size_t i = 0; i < len / 4; i++) {
        if (i % 8 == 0) logf("  0x%05x: ", phys + i * 4);
        logf("%08x ", map[i]);
        if (i % 8 == 7) logf("\n");
    }
    if ((len / 4) % 8 != 0) logf("\n");
    munmap(map, len);
    close(fd);
}

static void cmd_sysinfo(void) {
    g_fault_module = 1;
    logf("=== sysinfo ===\n");
    logf("uname: ");
    fflush(stdout);
    system("uname -a");
    logf("--- /proc/cpuinfo (cpu part) ---\n");
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) { char l[256]; while (fgets(l, sizeof l, f)) {
        if (strstr(l, "Hardware") || strstr(l, "processor") || strstr(l, "CPU part"))
            logf("  %s", l);
    } fclose(f); }
    logf("--- meminfo (first 6) ---\n");
    f = fopen("/proc/meminfo", "r");
    if (f) { char l[256]; int n = 0; while (fgets(l, sizeof l, f) && n < 6) { logf("  %s", l); n++; } fclose(f); }
    logf("--- /proc/asound/cards ---\n");
    cat_file("/proc/asound/cards");
    logf("--- /proc/asound/pcm ---\n");
    cat_file("/proc/asound/pcm");
    logf("--- /proc/asound/card0 PCM devices ---\n");
    DIR *asnd = opendir("/proc/asound/card0");
    if (asnd) { struct dirent *e; while ((e = readdir(asnd))) {
        if (strstr(e->d_name, "pcm")) {
            char p[256];
            snprintf(p, sizeof p, "/proc/asound/card0/%s/info", e->d_name);
            logf("  %s:\n", e->d_name);
            cat_file(p);
        }
    } closedir(asnd); }
    logf("--- acodec-ana registers @ 0x20030000 ---\n");
    dump_mem("/dev/mem", 0x20030000, 0x100);
    logf("--- i2s registers @ 0x10220000 ---\n");
    dump_mem("/dev/mem", 0x10220000, 0x80);
    logf("--- hdmi registers @ 0x20034000 ---\n");
    dump_mem("/dev/mem", 0x20034000, 0x80);
    logf("--- /proc/modules (snd) ---\n");
    f = fopen("/proc/modules", "r");
    if (f) { char l[256]; while (fgets(l, sizeof l, f)) {
        if (strstr(l, "snd") || strstr(l, "dma")) logf("  %s", l);
    } fclose(f); }
    logf("--- /proc/asound/version ---\n");
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { logf("  open card0 failed: %s\n", strerror(errno)); }
    else {
        struct drm_mode_card_res res; memset(&res, 0, sizeof res);
        if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0) {
            logf("  connectors=%u crtcs=%u encoders=%u fbs=%u\n",
                 res.count_connectors, res.count_crtcs, res.count_encoders, res.count_fbs);
        } else logf("  GETRESOURCES failed: %s\n", strerror(errno));
        close(fd);
    }
    logf("--- /sys/class/drm ---\n");
    DIR *d = opendir("/sys/class/drm");
    if (d) { struct dirent *e; while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[128]; snprintf(p, sizeof p, "/sys/class/drm/%s", e->d_name);
        logf("  %s\n", p);
    } closedir(d); }
    logf("=== sysinfo done ===\n");
}

/* ===========================================================================
 * input -- the KEY FACT SOURCE: real keycodes + axes + HATs
 * ========================================================================== */
static void dump_abs_bits(int fd, int idx) {
    unsigned long absbits[4] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits) < 0) return;
    for (int a = 0; a < 0x40; a++) {
        if (!((absbits[a / (8*sizeof(long))] >> (a % (8*sizeof(long)))) & 1)) continue;
        struct input_absinfo ai;
        if (ioctl(fd, EVIOCGABS(a), &ai) != 0) continue;
        logf("      ABS %2d (min=%d max=%d flat=%d fuzz=%d) val=%d\n",
             a, ai.minimum, ai.maximum, ai.flat, ai.fuzz, ai.value);
    }
}

static void cmd_input(void) {
    g_fault_module = 2;
    logf("=== input ===\n");
    logf("Interactive capture: press EVERY button/direction you own, one at a"
         " time, for ~25 seconds. The report records every keycode/axis seen.\n");
    int fds[16]; int nfd = 0; char names[16][128];
    for (int i = 0; i < 16; i++) {
        char path[64]; snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long evbits = 0;
        if (ioctl(fd, EVIOCGBIT(0, sizeof evbits), &evbits) < 0) { close(fd); continue; }
        names[nfd][0] = 0;
        ioctl(fd, EVIOCGNAME(sizeof names[nfd]-1), names[nfd]);
        logf("  device %d: %s (EV bits=%08lx)\n", i, names[nfd], evbits);
        if (evbits & (1ul << EV_KEY)) {
            logf("    KEY capabilities:\n");
            /* v11.5: kb was `unsigned long kb[4]` = 128 bits, but the scan
             * loop below goes to 0x300 = 768 bits -> out-of-bounds read,
             * so any button above code 127 (BTN_TRIGGER=288 and up, used
             * by classic gamepads like Twin USB 0810:0001) never showed
             * up in diag_report.  Sizing the array for the full scan
             * range fixes the diagnosis path. */
            unsigned long kb[24] = {0};
            /* EVIOCGBIT returns bytes copied (>=0) on success; ==0 was a bug
             * that made KEY capabilities NEVER print (same trap as keylog). */
            if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof kb), kb) >= 0)
                for (int k = 0; k < 0x300; k++)
                    if ((kb[k/(8*sizeof(long))] >> (k%(8*sizeof(long)))) & 1)
                        logf("      KEY/BTN %3d (0x%03x)\n", k, k);
        }
        if (evbits & (1ul << EV_ABS)) dump_abs_bits(fd, nfd);
        fds[nfd++] = fd;
    }
    if (nfd == 0) { logf("  NO evdev devices found!\n"); logf("=== input done ===\n"); return; }

    /* 25 s interactive capture: unique keycodes per device + axis ranges */
    logf("--- capture (25 s) ---\n");
    struct input_event ev;
    time_t t0 = time(NULL);
    while (time(NULL) - t0 < 25) {
        for (int i = 0; i < nfd; i++) {
            ssize_t rd;
            while ((rd = read(fds[i], &ev, sizeof ev)) == (ssize_t)sizeof ev) {
                if (ev.type == EV_KEY && ev.value == 1)
                    logf("  [%s] KEY code=%d (0x%03x)\n", names[i], ev.code, ev.code);
                else if (ev.type == EV_ABS)
                    logf("  [%s] ABS code=%d val=%d\n", names[i], ev.code, ev.value);
            }
        }
        usleep(5000);
    }
    for (int i = 0; i < nfd; i++) close(fds[i]);
    logf("=== input done ===\n");
}

/* ===========================================================================
 * keylog -- continuous boot-time key/axis event logger (v10.9, user hard
 * requirement 2026-08-27: 开机即 Debug，含键位触发等所有日志).
 * Daemonized by icube_replacement at boot. Appends every EV_KEY/EV_ABS/EV_REL
 * with a timestamp + device name to /mnt/sdcard/keylog.txt.
 * This answers the ROOT question: does the kernel actually deliver the button
 * events? (If keylog.txt has events but RetroArch ignores them -> problem is
 * RetroArch-side driver/autoconfig. If keylog.txt is empty -> device never
 * reports. No more guessing.)
 * ========================================================================== */
#define KEYLOG "/mnt/sdcard/keylog.txt"
static void cmd_keylog(void) {
    int fds[16]; int nfd = 0; char names[16][128]; int devidx[16];
    FILE *kl = fopen(KEYLOG, "ab");
    if (!kl) { fprintf(stderr, "keylog: cannot open %s\n", KEYLOG); return; }
    fprintf(kl, "# CubeGM keylog started %s", ctime(&(time_t){time(NULL)}));
    for (int i = 0; i < 16; i++) {
        char path[64]; snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long evbits = 0;
        if (ioctl(fd, EVIOCGBIT(0, sizeof evbits), &evbits) < 0) { close(fd); continue; }
        names[nfd][0] = 0;
        ioctl(fd, EVIOCGNAME(sizeof names[nfd]-1), names[nfd]);
        devidx[nfd] = i;
        fprintf(kl, "# device %d: %s (EV bits=%08lx)\n", nfd, names[nfd], evbits);
        /* v11.5: dump full KEY + ABS capability bitsets so the log itself
         * proves whether the kernel exposes buttons/axes at all.  Before
         * this, diag/keylog only showed the EV type mask, so a gamepad
         * whose buttons live above code 127 looked identical to a dead
         * device in the logs. */
        unsigned long kbdump[24] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof kbdump), kbdump) >= 0) {
            fprintf(kl, "#   KEY bits:");
            int any = 0;
            for (int k = 0; k < 0x300; k++)
                if ((kbdump[k/(8*sizeof(long))] >> (k%(8*sizeof(long)))) & 1) {
                    if (any++ % 16 == 0) fprintf(kl, "\n#     ");
                    fprintf(kl, "%d(0x%03x) ", k, k);
                }
            if (!any) fprintf(kl, " (none)");
            fprintf(kl, "\n");
        }
        unsigned long absdump[4] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absdump), absdump) >= 0) {
            fprintf(kl, "#   ABS bits:");
            int any = 0;
            for (int a = 0; a < 0x40; a++)
                if ((absdump[a/(8*sizeof(long))] >> (a%(8*sizeof(long)))) & 1) {
                    if (any++ % 16 == 0) fprintf(kl, "\n#     ");
                    fprintf(kl, "%d(0x%02x) ", a, a);
                }
            if (!any) fprintf(kl, " (none)");
            fprintf(kl, "\n");
        }
        fds[nfd++] = fd;
    }
    if (nfd == 0) { fprintf(kl, "# NO evdev devices found\n"); fflush(kl); fclose(kl); return; }
    fflush(kl);
    /* continuous loop: never exits (daemon). Log every input event. */
    long long last_rescan = 0, last_grabprobe = 0;
    for (;;) {
        long long ms = (long long)time(NULL) * 1000;
        /* ---- DEBUG A：设备出现时间线。每 3 s 重扫 /dev/input/eventN，新出现的
         * 设备（如 USB 手柄枚举较慢）此前永远不被 keylog 看到 → 误判"手柄没插"。
         * 现在记录 NEW 设备 + 能力位，回答"手柄何时被内核枚举、早于/晚于
         * RetroArch 启动"。 ---- */
        if (ms - last_rescan > 3000) {
            last_rescan = ms;
            for (int i = 0; i < 16; i++) {
                int dup = 0;
                for (int j = 0; j < nfd; j++) if (devidx[j] == i) { dup = 1; break; }
                if (dup) continue;
                char path[64]; snprintf(path, sizeof path, "/dev/input/event%d", i);
                int nf = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                if (nf < 0) continue;
                char nm[128]; nm[0] = 0;
                ioctl(nf, EVIOCGNAME(sizeof nm - 1), nm);
                if (!nm[0]) { close(nf); continue; }
                if (nfd < 16) {
                    unsigned long evbits = 0;
                    ioctl(nf, EVIOCGBIT(0, sizeof evbits), &evbits);
                    fds[nfd] = nf; strncpy(names[nfd], nm, 127); names[nfd][127] = 0; devidx[nfd] = i;
                    time_t now = time(NULL);
                    struct tm tm; localtime_r(&now, &tm);
                    fprintf(kl, "# [DEBUG:%02d:%02d:%02d] NEW device %d: %s (EV bits=%08lx)\n",
                            tm.tm_hour, tm.tm_min, tm.tm_sec, nfd, nm, evbits);
                    fflush(kl);
                    nfd++;
                } else close(nf);
            }
        }
        /* ---- DEBUG B：EVIOCGRAB 主动探测。每 5 s 对一个设备尝试 grab 并立即
         * 释放：成功=未被任何进程独占(FREE)；EBUSY=被某进程独占(BUSY)。
         * 内核语义：grab 后非 grabber 的 read 返回 EAGAIN（与"无事件"不可分），
         * 所以只有主动 EVIOCGRAB 能判定"设备被独占"vs"内核无事件"。
         * 注：RetroArch 官方 udev 驱动并不 grab 设备（已验证 udev_joypad.c
         * 无 EVIOCGRAB），BUSY 若出现说明是其他进程独占。 ---- */
        if (nfd > 0 && ms - last_grabprobe > 5000) {
            last_grabprobe = ms;
            static int frac = 0; int idx = (frac++) % nfd;
            char path[64]; snprintf(path, sizeof path, "/dev/input/event%d", devidx[idx]);
            int gp = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (gp >= 0) {
                time_t now = time(NULL);
                struct tm tm; localtime_r(&now, &tm);
                if (ioctl(gp, EVIOCGRAB, (void*)1) == 0) {
                    ioctl(gp, EVIOCGRAB, (void*)0);   /* free immediately, no steal */
                    fprintf(kl, "# [DEBUG:%02d:%02d:%02d] GRABPROBE event%d(%s): FREE\n",
                            tm.tm_hour, tm.tm_min, tm.tm_sec, devidx[idx], names[idx]);
                } else if (errno == EBUSY) {
                    fprintf(kl, "# [DEBUG:%02d:%02d:%02d] GRABPROBE event%d(%s): BUSY (grabbed by another process)\n",
                            tm.tm_hour, tm.tm_min, tm.tm_sec, devidx[idx], names[idx]);
                } else {
                    fprintf(kl, "# [DEBUG:%02d:%02d:%02d] GRABPROBE event%d(%s): errno=%d\n",
                            tm.tm_hour, tm.tm_min, tm.tm_sec, devidx[idx], names[idx], errno);
                }
                fflush(kl);
                close(gp);
            }
        }
        for (int i = 0; i < nfd; i++) {
            struct input_event ev;
            ssize_t rd;
            while ((rd = read(fds[i], &ev, sizeof ev)) == (ssize_t)sizeof ev) {
                /* only record interesting event types (key press/release, abs, rel) */
                if (ev.type == EV_KEY || ev.type == EV_ABS || ev.type == EV_REL) {
                    time_t now = time(NULL);
                    struct tm tm; localtime_r(&now, &tm);
                    fprintf(kl, "[%02d:%02d:%02d] [%s] type=%u code=%d (0x%03x) val=%d\n",
                            tm.tm_hour, tm.tm_min, tm.tm_sec, names[i],
                            ev.type, ev.code, ev.code, ev.value);
                    fflush(kl);
                }
            }
            /* only ENODEV (device removed) is meaningful here: grab leaves read()
             * returning EAGAIN, so "device vanished" is the one diagnosable error. */
            if (rd == -1 && errno == ENODEV) {
                static int reported_once[16];
                if (!reported_once[i]) {
                    time_t now = time(NULL);
                    struct tm tm; localtime_r(&now, &tm);
                    fprintf(kl, "[%02d:%02d:%02d] [%s] READ-ERR ENODEV (device removed)\n",
                            tm.tm_hour, tm.tm_min, tm.tm_sec, names[i]);
                    fflush(kl);
                    reported_once[i] = 1;
                }
            }
        }
        usleep(10000);   /* 10 ms poll interval */
    }
}

/* ===========================================================================
 * display -- DRM modeset + test patterns (proves the HDMI link + buffer)
 * ========================================================================== */
static void cmd_display(void) {
    g_fault_module = 3;
    logf("=== display ===\n");
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { logf("  open card0 FAILED: %s\n", strerror(errno)); return; }
    struct drm_mode_card_res res; memset(&res, 0, sizeof res);
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        logf("  GETRESOURCES FAILED: %s\n", strerror(errno)); close(fd); return; }
    /* Two-pass protocol: ALL FOUR id pointers must be valid — the kernel
     * put_user()s into every one whose count >= actual (NULL fb_id_ptr /
     * encoder_id_ptr with non-zero count = EFAULT "Bad address"). */
    uint32_t *conns = calloc(res.count_connectors ? res.count_connectors : 1, 4);
    uint32_t *crtcs = calloc(res.count_crtcs ? res.count_crtcs : 1, 4);
    uint32_t *encs  = calloc(res.count_encoders ? res.count_encoders : 1, 4);
    uint32_t *fbs   = calloc(res.count_fbs ? res.count_fbs : 1, 4);
    res.connector_id_ptr = (uintptr_t)conns;
    res.crtc_id_ptr      = (uintptr_t)crtcs;
    res.encoder_id_ptr   = (uintptr_t)encs;
    res.fb_id_ptr        = (uintptr_t)fbs;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        logf("  GETRESOURCES(2) FAILED: %s\n", strerror(errno)); close(fd); return; }
    int conn_id = -1, crtc_id = -1;
    struct drm_mode_modeinfo chosen; memset(&chosen, 0, sizeof chosen);
    for (uint32_t i = 0; i < res.count_connectors && conn_id < 0; i++) {
        struct drm_mode_get_connector gc; memset(&gc, 0, sizeof gc);
        gc.connector_id = conns[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) < 0) continue;
        struct drm_mode_modeinfo *modes = calloc(gc.count_modes ? gc.count_modes : 1, sizeof *modes);
        struct drm_mode_get_encoder *encs = NULL;
        gc.modes_ptr = (uintptr_t)modes;
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) == 0 && gc.count_modes > 0) {
            logf("  connector %u: connected=%d modes=%d\n", conns[i], gc.connection, gc.count_modes);
            for (uint32_t m = 0; m < gc.count_modes; m++)
                logf("    mode: %dx%d@%d '%s'\n", modes[m].hdisplay, modes[m].vdisplay,
                     modes[m].vrefresh, modes[m].name);
            /* prefer 1280x720 */
            for (uint32_t m = 0; m < gc.count_modes; m++)
                if (modes[m].hdisplay == 1280 && modes[m].vdisplay == 720) { chosen = modes[m]; break; }
            if (!chosen.hdisplay && gc.count_modes) chosen = modes[0];
            conn_id = conns[i];
        }
        free(modes); free(encs);
        if (conn_id >= 0 && gc.encoder_id) {
            /* find crtc for the encoder */
            for (uint32_t c = 0; c < res.count_crtcs; c++) crtc_id = crtcs[c];
        }
        /* simpler: crtc = first available */
        if (res.count_crtcs) crtc_id = crtcs[0];
    }
    if (conn_id < 0 || !chosen.hdisplay) { logf("  no usable connector/mode\n"); close(fd); return; }

    /* dumb buffer 1280x720 RGB565 */
    struct drm_mode_create_dumb cd; memset(&cd, 0, sizeof cd);
    cd.width = chosen.hdisplay; cd.height = chosen.vdisplay; cd.bpp = 16;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
        logf("  CREATE_DUMB FAILED: %s\n", strerror(errno)); close(fd); return; }
    struct drm_mode_map_dumb md; memset(&md, 0, sizeof md);
    md.handle = cd.handle;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
        logf("  MAP_DUMB FAILED: %s\n", strerror(errno)); close(fd); return; }
    uint16_t *map = mmap(NULL, cd.size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, md.offset);
    if (map == MAP_FAILED) { logf("  mmap FAILED: %s\n", strerror(errno)); close(fd); return; }
    struct drm_mode_fb_cmd2 fb; memset(&fb, 0, sizeof fb);
    fb.width = cd.width; fb.height = cd.height;
    fb.pixel_format = DRM_FORMAT_RGB565;
    fb.handles[0] = cd.handle; fb.pitches[0] = cd.pitch;
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0) {
        logf("  ADDFB2 FAILED: %s\n", strerror(errno)); munmap(map, cd.size); close(fd); return; }
    struct drm_mode_crtc cc; memset(&cc, 0, sizeof cc);
    cc.crtc_id = crtc_id; cc.fb_id = fb.fb_id; cc.x = 0; cc.y = 0;
    cc.mode_valid = 1; cc.mode = chosen;
    cc.set_connectors_ptr = (uintptr_t)&conn_id; cc.count_connectors = 1;
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &cc) < 0) {
        logf("  SETCRTC FAILED: %s\n", strerror(errno));
    } else {
        logf("  SETCRTC OK fb=%u %dx%d -> expect test patterns on HDMI now\n",
             fb.fb_id, cd.width, cd.height);
        const uint16_t colors[5] = { 0xF800 /*red*/, 0x07E0 /*green*/, 0x001F /*blue*/,
                                     0xFFFF /*white*/, 0x0000 /*black*/ };
        const char *cname[5] = { "RED", "GREEN", "BLUE", "WHITE", "BLACK" };
        for (int c = 0; c < 5; c++) {
            for (uint32_t p = 0; p < cd.width * cd.height; p++) map[p] = colors[c];
            logf("  pattern %s (%d/5) ...\n", cname[c], c + 1);
            usleep(900000);
        }
        /* vertical gradient bar grid: proves scaling + no row offset */
        for (uint32_t y = 0; y < cd.height; y++)
            for (uint32_t x = 0; x < cd.width; x++) {
                uint16_t v = (x / (cd.width / 16)) * 4096;
                map[y * (cd.pitch/2) + x] = v;
            }
        logf("  gradient bars shown (16 columns)\n");
        usleep(2000000);
        memset(map, 0, cd.size);
    }
    struct drm_mode_fb_cmd rm; memset(&rm, 0, sizeof rm); rm.fb_id = fb.fb_id;
    ioctl(fd, DRM_IOCTL_MODE_RMFB, &rm);
    munmap(map, cd.size); close(fd);
    logf("=== display done ===\n");
}

/* ===========================================================================
 * audio -- ALSA "default" 1 kHz via dlopen'd libasound
 * ========================================================================== */
static void cmd_audio(void) {
    g_fault_module = 4;
    logf("=== audio ===\n");
    void *h = dlopen("libasound.so.2", RTLD_LAZY);
    if (!h) { logf("  dlopen libasound.so.2 FAILED: %s\n", dlerror()); return; }
    int (*p_open)(void **, const char *, int, int) = dlsym(h, "snd_pcm_open");
    int (*p_sp)(void *, unsigned int, int, int, int, int, unsigned int) = dlsym(h, "snd_pcm_set_params");
    long (*p_wr)(void *, const void *, unsigned long) = dlsym(h, "snd_pcm_writei");
    int (*p_cl)(void *) = dlsym(h, "snd_pcm_close");
    int (*p_cfgfree)(void) = dlsym(h, "snd_config_update_free_global");
    if (!p_open || !p_sp || !p_wr || !p_cl) {
        logf("  missing ALSA symbols: open=%p sp=%p wr=%p cl=%p\n",
             (void*)p_open, (void*)p_sp, (void*)p_wr, (void*)p_cl);
        dlclose(h); return;
    }
    if (p_cfgfree) p_cfgfree();
    /* 1 kHz sine, 0.1 s buffer @48k (matches stock dmix rate) */
    int16_t buf[4800];
    for (int i = 0; i < 4800; i++) {
        double t = (double)i / 48000.0;
        int16_t v = (int16_t)(12000.0 * (t * 1000.0 < 0.5 ? 1.0 : -1.0));
        buf[i] = v;
    }
    /* v11.2: probe EVERY device + default. No shell on device, so this
     * per-device open/play result IS the audio truth: it tells us which
     * PCM drives the built-in speaker (user confirmed stock plays BOTH
     * HDMI + speaker simultaneously). 48k, S16_LE, stereo. */
    const char *devs[] = { "hw:0,0", "hw:0,1", "default" };
    for (unsigned di = 0; di < sizeof(devs)/sizeof(devs[0]); di++) {
        void *pcm = NULL;
        int rc = p_open(&pcm, devs[di], 0 /*PLAYBACK*/, 0);
        logf("  [%s] open rc=%d pcm=%p\n", devs[di], rc, pcm);
        if (rc < 0 || !pcm) continue;
        rc = p_sp(pcm, 2 /*S16_LE*/, 3 /*INTERLEAVED*/, 2, 48000, 1, 50000);
        logf("  [%s] set_params(48k stereo) rc=%d\n", devs[di], rc);
        if (rc < 0) { p_cl(pcm); continue; }
        long w = 0;
        for (int rep = 0; rep < 10; rep++) { /* 10 x 0.1 s = 1 s */
            w = p_wr(pcm, buf, 4800);
            if (w < 0) { logf("  [%s] writei rc=%ld (xrun?)\n", devs[di], w); break; }
        }
        logf("  [%s] played 1 kHz 1 s rc=%ld -- HEARD? (speaker test)\n", devs[di], w);
        p_cl(pcm);
    }
    logf("=== audio done ===\n");
    dlclose(h);
}

/* ===========================================================================
 * cores -- dlopen + symbol check + retro_api_version for every core
 * ========================================================================== */
typedef unsigned (*api_fn)(void);
typedef void (*init_fn)(void);
static void cmd_cores(void) {
    g_fault_module = 5;
    logf("=== cores ===\n");
    DIR *d = opendir("/mnt/sdcard/cubegm/cores");
    if (!d) { logf("  opendir /mnt/sdcard/cubegm/cores failed: %s\n", strerror(errno)); return; }
    struct dirent *e;
    int n = 0, ok = 0;
    while ((e = readdir(d))) {
        if (strstr(e->d_name, "_libretro.so") == NULL && strcmp(e->d_name, "frogui_libretro.so") != 0) continue;
        if (strncmp(e->d_name, "libemu", 6) == 0) continue; /* stock cores: skip (different ABI) */
        char path[256]; snprintf(path, sizeof path, "/mnt/sdcard/cubegm/cores/%s", e->d_name);
        g_fault_module = 10 + (n % 90);
        void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        n++;
        if (!h) { logf("  [%s] dlopen FAILED: %s\n", e->d_name, dlerror()); continue; }
        api_fn api = (api_fn)dlsym(h, "retro_api_version");
        init_fn ri = (init_fn)dlsym(h, "retro_init");
        init_fn rg = (init_fn)dlsym(h, "retro_load_game");
        if (!api || !ri) { logf("  [%s] missing retro_api_version/retro_init\n", e->d_name); dlclose(h); continue; }
        logf("  [%s] retro_api_version=%u retro_init=%p retro_load_game=%p\n",
             e->d_name, api(), (void*)ri, (void*)rg);
        /* full retro_init: exercises core's setup (geometry alloc etc.); a
         * crashing core is caught by the fault guard and reported. */
        if (ri && getenv("DIAG_CORE_INIT")) { ri(); logf("  [%s] retro_init OK\n", e->d_name); }
        dlclose(h);
        ok++;
    }
    closedir(d);
    logf("  cores scanned=%d ok=%d\n", n, ok);
    logf("=== cores done ===\n");
}
/* ==== P1+P2: full-system debug modules ===== */
/* ===========================================================================
 * P1 -- helpers + cmd_sysdeep (full-system deep debug snapshot)
 * Inserted before main() in the enhanced diag.  Depends only on helpers that
 * helpers already in the 401 base (logf / cat_file).
 * ========================================================================== */

/* ---- safe single-value reader ---- */
static long sysfs_read_int(const char *path, long dflt) {
    FILE *f = fopen(path, "r");
    if (!f) return dflt;
    char buf[128]; long v = dflt;
    if (fgets(buf, sizeof buf, f)) {
        char *end = NULL; v = strtol(buf, &end, 0);
        if (end == buf) v = dflt;
    }
    fclose(f); return v;
}
static void sysfs_print_raw(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { logf("    [%s] n/a (%s)\n", path, strerror(errno)); return; }
    char line[512];
    if (fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; logf("    %s\n", line); }
    fclose(f);
}

/* ---- list a /sys class dir ---- */
static void sys_class_list(const char *base) {
    DIR *d = opendir(base);
    if (!d) { logf("    [%s] n/a (%s)\n", base, strerror(errno)); return; }
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[256]; snprintf(p, sizeof p, "%s/%s", base, e->d_name);
        logf("    %s\n", p);
        n++;
    }
    closedir(d);
    if (n == 0) logf("    (empty)\n");
}

/* ---- process table ---- */
static void proc_ps(void) {
    logf("--- processes /proc/[pid] (pid ppid st threads rss cmdline) ---\n");
    DIR *d = opendir("/proc");
    if (!d) { logf("    opendir /proc failed: %s\n", strerror(errno)); return; }
    struct dirent *e;
    int total = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char statp[64], cmdp[64];
        snprintf(statp, sizeof statp, "/proc/%s/stat", e->d_name);
        snprintf(cmdp,  sizeof cmdp,  "/proc/%s/cmdline", e->d_name);
        FILE *f = fopen(statp, "r");
        if (!f) continue;
        char stbuf[512];
        if (!fgets(stbuf, sizeof stbuf, f)) { fclose(f); continue; }
        fclose(f);
        char *lp = strchr(stbuf, ')');
        if (!lp) continue;
        char *p = lp + 2;
        char state[3]; unsigned ppid = 0;
        sscanf(p, "%2s %u", state, &ppid);
        char statusp[64]; snprintf(statusp, sizeof statusp, "/proc/%s/status", e->d_name);
        long threads = -1, rss = -1;
        FILE *fs = fopen(statusp, "r");
        if (fs) { char l[128]; while (fgets(l, sizeof l, fs)) {
            if (strncmp(l, "Threads:", 8) == 0) threads = strtol(l + 8, NULL, 10);
            else if (strncmp(l, "VmRSS:", 6) == 0) rss = strtol(l + 6, NULL, 10);
        } fclose(fs); }
        char cmd[140] = "?"; size_t cl = 0;
        FILE *fc = fopen(cmdp, "r");
        if (fc) { cl = fread(cmd, 1, sizeof cmd - 1, fc); cmd[cl] = 0;
                  for (size_t i = 0; i < cl; i++) if (cmd[i] == 0) cmd[i] = ' '; fclose(fc); }
        if (!cl) snprintf(cmd, sizeof cmd, "[%s]", e->d_name);
        logf("    %6s %2s %6u th=%3ld rss=%6ldkB %s\n", e->d_name, state, ppid, threads, rss, cmd);
        total++;
    }
    closedir(d);
    logf("    total processes: %d\n", total);
}

/* ---- CPU util sample (needs two calls) ---- */
static void cpu_usage_sample(void) {
    static long prev_total = -1, prev_idle = -1;
    long total = 0, idle = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return;
    char l[256];
    if (fgets(l, sizeof l, f)) {
        unsigned long a,b,c,d,e,g,h,i,j,k;
        if (sscanf(l, "cpu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
                   &a,&b,&c,&d,&e,&g,&h,&i,&j,&k) == 10) {
            total = a+b+c+d+e+g+h+i+j+k; idle = d;
        }
    }
    fclose(f);
    if (prev_total > 0) {
        long dt = total - prev_total;
        long di = idle - prev_idle;
        long pct = (dt > 0) ? 100 * (dt - di) / dt : 0;
        if (pct < 0) pct = 0; if (pct > 100) pct = 100;
        logf("    cpu util(last sample) = %ld%% (idle %ld%%)\n", pct, 100 - pct);
    }
    prev_total = total; prev_idle = idle;
}

/* ---- memory deep dump ---- */
static void mem_deep(void) {
    logf("--- /proc/meminfo (full) ---\n");
    cat_file("/proc/meminfo");
    logf("--- /proc/vmstat (paging) ---\n");
    FILE *f = fopen("/proc/vmstat", "r");
    if (f) { char l[128]; while (fgets(l, sizeof l, f)) {
        if (strstr(l, "pgpgin") || strstr(l, "pgpgout") || strstr(l, "pswpin") ||
            strstr(l, "pswpout") || strstr(l, "pgsteal") || strstr(l, "oom_kill"))
            logf("    %s", l);
    } fclose(f); }
    logf("--- /proc/loadavg ---\n");
    sysfs_print_raw("/proc/loadavg");
}

/* ---- storage / mounts / SD space ---- */
static void storage_deep(void) {
    logf("--- /proc/mounts ---\n");
    cat_file("/proc/mounts");
    logf("--- /proc/partitions ---\n");
    FILE *f = fopen("/proc/partitions", "r");
    if (f) { char l[256]; while (fgets(l, sizeof l, f)) logf("    %s", l); fclose(f); }
    logf("--- filesystem space (statvfs) ---\n");
    const char *mp[] = {"/mnt/sdcard", "/", "/mnt"};
    for (unsigned i = 0; i < sizeof mp / sizeof mp[0]; i++) {
        struct statvfs sv;
        if (statvfs(mp[i], &sv) == 0 && sv.f_blocks > 0) {
            double gb = 1024.0 * 1024.0 * 1024.0;
            logf("    %-14s total=%.2fGB free=%.2fGB used=%llu%% inodes=%llu free_ino=%llu\n",
                 mp[i], (double)sv.f_blocks * sv.f_frsize / gb,
                 (double)sv.f_bfree * sv.f_frsize / gb,
                 (unsigned long long)(100 - 100ULL * sv.f_bfree / sv.f_blocks),
                 (unsigned long long)sv.f_files, (unsigned long long)sv.f_ffree);
        } else logf("    %-14s statvfs failed: %s\n", mp[i], strerror(errno));
    }
    logf("--- /proc/sys/fs/file-nr ---\n");
    sysfs_print_raw("/proc/sys/fs/file-nr");
}

/* ---- network ---- */
static void net_deep(void) {
    logf("--- /proc/net/dev ---\n");
    cat_file("/proc/net/dev");
    logf("--- /proc/net/sockstat ---\n");
    sysfs_print_raw("/proc/net/sockstat");
    logf("--- /proc/net/route (first 8) ---\n");
    FILE *f = fopen("/proc/net/route", "r");
    if (f) { char l[256]; int n = 0; while (fgets(l, sizeof l, f) && n < 8) { logf("    %s", l); n++; } fclose(f); }
}

/* ---- thermal ---- */
static void thermal_deep(void) {
    logf("--- thermal zones ---\n");
    sys_class_list("/sys/class/thermal");
    /* print temps */
    DIR *d = opendir("/sys/class/thermal");
    if (d) { struct dirent *e; while ((e = readdir(d))) {
        if (strncmp(e->d_name, "thermal_zone", 12) == 0) {
            char p[160]; snprintf(p, sizeof p, "/sys/class/thermal/%s/temp", e->d_name);
            FILE *f = fopen(p, "r");
            if (f) { char l[32]; if (fgets(l, sizeof l, f)) {
                long mv = strtol(l, NULL, 10);
                logf("    %s = %ld.%02ld°C\n", e->d_name, mv / 1000, labs(mv % 1000));
            } fclose(f); }
        }
    } closedir(d); }
}

/* ---- IRQs of interest ---- */
static void irq_deep(void) {
    logf("--- /proc/interrupts (i2s/dma/eth) ---\n");
    FILE *f = fopen("/proc/interrupts", "r");
    if (f) { char l[256]; while (fgets(l, sizeof l, f)) {
        if (strstr(l, "i2s") || strstr(l, "dma") || strstr(l, "eth") || strstr(l, "CPU0"))
            logf("    %s", l);
    } fclose(f); }
}

/* ---- kernel ring buffer (non-destructive) ---- */
static void kmsg_capture(int max_bytes) {
    logf("--- kernel log ring (/dev/kmsg, non-destructive, <=%dKB) ---\n", max_bytes / 1024);
    FILE *f = fopen("/dev/kmsg", "r");
    if (!f) { logf("    /dev/kmsg open failed: %s (need root)\n", strerror(errno)); return; }
    int fd = fileno(f); int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    char line[512]; int got = 0;
    while (fgets(line, sizeof line, f)) {
        if (max_bytes > 0 && got >= max_bytes) break;
        char *msg = strchr(line, ';');
        logf("    %s", (msg ? msg + 1 : line));
        got += (int)strlen(msg ? msg + 1 : line);
    }
    if (got == 0) logf("    (ring empty or no permission)\n");
    fclose(f);
}

/* ---- the snapshot ---- */
static void cmd_sysdeep(void) {
    g_fault_module = 20;
    logf("=== sysdeep (full-system deep debug snapshot) ===\n");

    logf("--- kernel / boot ---\n");
    sysfs_print_raw("/proc/version");
    sysfs_print_raw("/proc/cmdline");
    sysfs_print_raw("/proc/uptime");

    logf("--- CPU ---\n");
    cat_file("/proc/cpuinfo");
    cpu_usage_sample();
    cpu_usage_sample();
    logf("--- cpufreq cur (per core) ---\n");
    sys_class_list("/sys/devices/system/cpu");
    DIR *cf = opendir("/sys/devices/system/cpu");
    if (cf) { struct dirent *e; while ((e = readdir(cf))) {
        char p[160];
        if (strncmp(e->d_name, "cpu", 3) == 0 && e->d_name[3] >= '0' && e->d_name[3] <= '9') {
            snprintf(p, sizeof p, "/sys/devices/system/cpu/%s/cpufreq/scaling_cur_freq", e->d_name);
            long cur = sysfs_read_int(p, -1);
            if (cur > 0) {
                snprintf(p, sizeof p, "/sys/devices/system/cpu/%s/cpufreq/scaling_governor", e->d_name);
                FILE *fg = fopen(p, "r"); char gv[32] = "?";
                if (fg) { if (fgets(gv, sizeof gv, fg)) gv[strcspn(gv,"\r\n")]=0; fclose(fg); }
                logf("    %s cur=%ldkHz gov=%s\n", e->d_name, cur, gv);
            }
        }
    } closedir(cf); }

    mem_deep();
    storage_deep();
    net_deep();
    thermal_deep();
    irq_deep();
    proc_ps();
    kmsg_capture(16384);

    logf("=== sysdeep done ===\n");
}

/* ===========================================================================
 * GPU probe -- Mali-400 @ 0x10090000 feasibility check (no side effects)
 *
 * Reads sysfs/platform/debugfs to determine whether the Mali kernel driver
 * is loaded and bound, and whether the GPU is clocked.  All paths are
 * read-only; nothing is opened for write.  Output answers:
 *   1. Does /sys/bus/platform/devices/10091000.gpu exist?  (DT node)
 *   2. Is a driver bound?  (driver symlink → driver name)
 *   3. Is devfreq registered?  (cur_freq / available_frequencies)
 *   4. Are Mali debugfs entries present?  (/sys/kernel/debug/mali/*)
 *   5. Are Mali character devices present?  (/dev/mali* / /dev/dri/renderD*)
 *   6. GPU register block identity read via /dev/mem mmap (Mali_ID register)
 *   7. Any Mali-related kernel log lines in dmesg ring buffer?
 *
 * VERDICT line at the end:
 *   GPU_ENABLED      → kernel driver loaded + bound + clocked
 *   GPU_DT_ONLY     → DT node present but no driver (kernel not compiled)
 *   GPU_ABSENT      → no DT node at all
 * ========================================================================== */
static void cmd_gpu(void) {
    g_fault_module = 30;
    logf("=== gpu probe ===\n");

    /* 1. Platform device existence */
    logf("--- platform device ---\n");
    struct stat st;
    if (stat("/sys/bus/platform/devices/10091000.gpu", &st) == 0) {
        logf("  /sys/bus/platform/devices/10091000.gpu exists\n");
    } else {
        logf("  /sys/bus/platform/devices/10091000.gpu ABSENT (%s)\n", strerror(errno));
    }

    /* 2. Driver binding (symlink "driver" → /sys/bus/platform/drivers/xxx) */
    logf("--- driver binding ---\n");
    char drvlink[256];
    ssize_t n = readlink("/sys/bus/platform/devices/10091000.gpu/driver",
                         drvlink, sizeof(drvlink) - 1);
    if (n > 0) {
        drvlink[n] = '\0';
        const char *base = strrchr(drvlink, '/');
        logf("  driver bound: %s\n", base ? base + 1 : drvlink);
    } else {
        logf("  driver NOT bound (no driver symlink: %s)\n", strerror(errno));
    }

    /* 3. Devfreq (GPU frequency management) — 先枚举全部节点（GPU 节点名随内核变），再定点读取 */
    logf("--- devfreq ---\n");
    sys_class_list("/sys/class/devfreq");
    cat_file("/sys/class/devfreq/10091000.gpu/cur_freq");
    cat_file("/sys/class/devfreq/10091000.gpu/available_frequencies");
    cat_file("/sys/class/devfreq/10091000.gpu/governor");
    cat_file("/sys/class/devfreq/10091000.gpu/min_freq");
    cat_file("/sys/class/devfreq/10091000.gpu/max_freq");
    cat_file("/sys/class/devfreq/10091000.gpu/trans_stat");

    /* 4. Mali debugfs */
    logf("--- /sys/kernel/debug/mali ---\n");
    sys_class_list("/sys/kernel/debug/mali");
    /* Also try debugfs under /sys/kernel/debug/ directly (some kernels) */
    DIR *dbg = opendir("/sys/kernel/debug/mali");
    if (dbg) {
        struct dirent *e;
        while ((e = readdir(dbg))) {
            if (e->d_name[0] == '.') continue;
            char p[256];
            snprintf(p, sizeof p, "/sys/kernel/debug/mali/%s", e->d_name);
            logf("  %s: ", e->d_name);
            cat_file(p);
        }
        closedir(dbg);
    } else {
        logf("  /sys/kernel/debug/mali not accessible (%s)\n", strerror(errno));
    }

    /* 4.5 Mali kernel driver version — blob 版本匹配判定关键（2026-09-18）：
     *   老 utgard kbase 驱动在 /proc/mali/version 暴露版本号；若内核以模块加载，
     *   /sys/module/mali/version 也有。此前只 dump debugfs \"version\"（GPU 型号），
     *   拿不到「Inserting Mali vXXX / Driver revision」级别的驱动版本，导致 r7p0 blob
     *   与内核驱动 API 是否匹配无法判定。 */
    logf("--- Mali kernel driver version ---\n");
    cat_file("/proc/mali/version");
    cat_file("/sys/module/mali/version");
    cat_file("/sys/module/mali/uevent");

    /* 4.6 Mali UK probe（2026-09-21 一次拿全诊断字段）：
     *   内核 UK API 通过 ioctl 暴露诊断信息，一锤定音判定 r7p0 blob
     *   与内核驱动的匹配关系。此前两轮教训：
     *   ① ioctl 号曾错成 0xc0046d01（type/nr 全错）→ errno=25 ENOTTY；
     *   ② 参数曾错成 4 字节 u32，而内核 handler 按 12 字节结构体
     *      _mali_uk_get_api_version_s 读写，version 写 offset 4 溢出丢失。
     *
     *   ioctl 号铁证（mripard/sunxi-mali r6p0~r9p0 + paolosabatino r7p0
     *   源码逐文件核对，全部版本一致）：
     *     MALI_IOC_BASE=0x82；CORE_SUBSYSTEM=0→CORE_BASE=0x82；
     *     PP_SUBSYSTEM=2→PP_BASE=0x84；GP_SUBSYSTEM=3→GP_BASE=0x85。
     *   CORE 枚举: OPEN=0,CLOSE=1,WAIT=2,GET_API_VERSION=3,POST=4,
     *              GET_USER_SETTING=5,GET_USER_SETTINGS=6
     *   PP 枚举: ...,GET_PP_NUMBER_OF_CORES=4,GET_PP_CORE_VERSION=5
     *   GP 枚举: ...,GET_GP_NUMBER_OF_CORES=9,GET_GP_CORE_VERSION=10
     *   r7p0 内核 UK API 常量 = _MAKE_VERSION_ID(900) = 0x03840384 */
    logf("--- Mali UK probe ---\n");
    {
        int mfd = open("/dev/mali", O_RDWR);
        if (mfd < 0) {
            logf("  open /dev/mali failed: %s\n", strerror(errno));
        } else {
            logf("  /dev/mali opened\n");

            /* [1] GET_API_VERSION V1 —— 内核 UK API 版本 + 兼容判定（核心） */
            {
                struct { unsigned int ctx; unsigned int version; int compatible; } a;
                memset(&a, 0, sizeof a);
                a.ctx = 1;
                int r = ioctl(mfd, 0xC0048203, &a);
                if (r < 0) {
                    logf("  GET_API_VERSION(V1) ioctl failed: %s (errno=%d)\n",
                         strerror(errno), errno);
                } else {
                    logf("  UK API version(V1): 0x%08x (dec %u)  compatible=%d  [r7p0期望0x03840384=900]\n",
                         a.version, a.version, a.compatible);
                }
            }

            /* [2] GET_API_VERSION_V2 —— r7p0 现代规范（u64 ctx） */
            {
                struct { unsigned long long ctx; unsigned int version; int compatible; } a;
                memset(&a, 0, sizeof a);
                a.ctx = 1;
                int r = ioctl(mfd, 0xC0108203, &a);
                if (r < 0) {
                    logf("  GET_API_VERSION(V2) ioctl failed: %s (errno=%d)\n",
                         strerror(errno), errno);
                } else {
                    logf("  UK API version(V2): 0x%08x (dec %u)  compatible=%d  [r7p0期望0x03840384=900]\n",
                         a.version, a.version, a.compatible);
                }
            }

            /* [3] PP 核数（Fragment Processor） */
            {
                struct { unsigned long long ctx; unsigned int total; unsigned int enabled; } a;
                memset(&a, 0, sizeof a);
                a.ctx = 1;
                int r = ioctl(mfd, 0x80108404, &a);
                if (r < 0) {
                    logf("  PP_NUMBER_OF_CORES ioctl failed: %s (errno=%d)\n",
                         strerror(errno), errno);
                } else {
                    logf("  PP cores: total=%u enabled=%u\n", a.total, a.enabled);
                }
            }

            /* [4] PP 核硬件版本 */
            {
                struct { unsigned long long ctx; unsigned int version; unsigned int padding; } a;
                memset(&a, 0, sizeof a);
                a.ctx = 1;
                int r = ioctl(mfd, 0x80108405, &a);
                if (r < 0) {
                    logf("  PP_CORE_VERSION ioctl failed: %s (errno=%d)\n",
                         strerror(errno), errno);
                } else {
                    logf("  PP core version: 0x%08x\n", a.version);
                }
            }

            /* [5] GP 核数（Vertex Processor） */
            {
                struct { unsigned long long ctx; unsigned int cores; unsigned int padding; } a;
                memset(&a, 0, sizeof a);
                a.ctx = 1;
                int r = ioctl(mfd, 0x80108509, &a);
                if (r < 0) {
                    logf("  GP_NUMBER_OF_CORES ioctl failed: %s (errno=%d)\n",
                         strerror(errno), errno);
                } else {
                    logf("  GP cores: %u\n", a.cores);
                }
            }

            /* [6] GP 核硬件版本 */
            {
                struct { unsigned long long ctx; unsigned int version; unsigned int padding; } a;
                memset(&a, 0, sizeof a);
                a.ctx = 1;
                int r = ioctl(mfd, 0x8010850A, &a);
                if (r < 0) {
                    logf("  GP_CORE_VERSION ioctl failed: %s (errno=%d)\n",
                         strerror(errno), errno);
                } else {
                    logf("  GP core version: 0x%08x\n", a.version);
                }
            }

            close(mfd);
        }
    }

    /* 5. Character devices */
    logf("--- char devices ---\n");
    const char *devs[] = {
        "/dev/mali", "/dev/mali0", "/dev/dri/card0",
        "/dev/dri/renderD128", "/dev/fb0"
    };
    for (unsigned i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
        if (stat(devs[i], &st) == 0) {
            logf("  %s exists (mode %o, rdev %lx)\n",
                 devs[i], st.st_mode & 0777, (unsigned long)st.st_rdev);
        } else {
            logf("  %s absent (%s)\n", devs[i], strerror(errno));
        }
    }

    /* 6. GPU register identity read — SKIPPED
     * dump_mem("/dev/mem", 0x10090000, 0x80) 读取 GPU 物理寄存器空间,
     * 在内核无 Mali driver 时该地址未映射, mmap/读取触发内核 panic
     * (500 实测 60s 后宕机, 501 无法启动). 改用 sysfs 判定, 不碰 /dev/mem. */
    logf("--- GPU registers @ 0x10090000 (skipped: /dev/mem on unmapped GPU space may cause kernel panic) ---\n");

    /* 7. Kernel log scan for Mali-related lines */
    logf("--- dmesg Mali lines (non-destructive) ---\n");
    FILE *kmsg = fopen("/proc/kmsg", "r");
    if (kmsg) {
        /* /proc/kmsg blocks until new messages; read what's buffered then stop.
         * We use non-blocking by setting O_NONBLOCK on the fd. */
        int fd = fileno(kmsg);
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        char line[512];
        int found = 0;
        /* Read recent ring buffer entries (kernel keeps them buffered) */
        while (fgets(line, sizeof line, kmsg)) {
            if (strcasestr(line, "mali") || strcasestr(line, "gpu") ||
                strcasestr(line, " Mali") || strcasestr(line, "PP0") ||
                strcasestr(line, "devfreq")) {
                logf("  %s", line);
                found++;
            }
        }
        if (!found) logf("  (no Mali/GPU lines in kmsg ring buffer)\n");
        fclose(kmsg);
    } else {
        logf("  /proc/kmsg open failed: %s\n", strerror(errno));
    }

    /* VERDICT */
    int dt_ok = (stat("/sys/bus/platform/devices/10091000.gpu", &st) == 0);
    int drv_bound = (access("/sys/bus/platform/devices/10091000.gpu/driver", F_OK) == 0);
    int devfreq = (stat("/sys/class/devfreq/10091000.gpu", &st) == 0);

    logf("--- VERDICT ---\n");
    if (dt_ok && drv_bound && devfreq) {
        logf("  GPU_ENABLED: Mali kernel driver loaded + bound + devfreq active\n");
    } else if (dt_ok && !drv_bound) {
        logf("  GPU_DT_ONLY: DT node present, kernel driver NOT compiled/loaded\n");
    } else if (!dt_ok) {
        logf("  GPU_ABSENT: no 10091000.gpu platform device\n");
    } else {
        logf("  GPU_PARTIAL: dt=%d driver=%d devfreq=%d\n", dt_ok, drv_bound, devfreq);
    }
    logf("=== gpu probe done ===\n");
}

/* ===========================================================================
 * P2 -- cmd_monitor: resident periodic full-system debug logger.
 *
 *   diag monitor [interval_sec] [max_minutes]
 *   (default interval 30s; max_minutes 0 = run forever)
 *
 * Writes a separate /mnt/sdcard/diag_monitor.log (never touches the one-shot
 * diag_report.txt).  Each tick appends a timestamped snapshot:
 *   - pcm0p/pcm1p state + avail + xruns (audio liveliness; catches silent-drop
 *     moments that a 45s probe cannot reach)
 *   - key-process alive checks (retroarch / icube / diag / zhijack)
 *   - cpu util delta, memory, loadavg, thermal, cpufreq
 *   - kernel ring increments (since last tick) filtered to snd/i2s/dma/xrun
 * Read-only on /proc//sys//dev.  Uses private FILE* m_out so main()'s g_out
 * stays exactly as the one-shot report handle.
 * ========================================================================== */

#define MONITOR_LOG "/mnt/sdcard/diag_monitor.log"
#define MONITOR_MAX_BYTES (2 * 1024 * 1024)

static FILE *m_out = NULL;
static void mlogf(const char *fmt, ...) {
    va_list ap;
    if (m_out) { va_start(ap, fmt); vfprintf(m_out, fmt, ap); va_end(ap); fflush(m_out); }
    va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap);
}

/* --- pcm /proc status: exact field-name parse (state is a string!) ------ */
static void pcm_status_line(const char *sub0, const char *label) {
    FILE *f = fopen(sub0, "r");
    if (!f) { mlogf("  [%s] n/a\n", label); return; }
    char l[256];
    char state[16] = "?";
    long owner = -1, xruns = -1, avail = -1, avail_max = -1, hw = -1, app = -1;
    while (fgets(l, sizeof l, f)) {
        char *colon = strchr(l, ':');
        if (!colon) continue;
        /* field name = leading token, trim trailing spaces */
        char name[32];
        size_t n = (size_t)(colon - l);
        if (n > sizeof name - 2) n = sizeof name - 2;
        memcpy(name, l, n);
        name[n] = 0;
        while (n && name[n-1] == ' ') name[--n] = 0;
        /* value */
        char *v = colon + 1; while (*v == ' ') v++;
        if (strcmp(name, "state") == 0) {
            size_t vn = 0; while (v[vn] && v[vn] >= 0x20 && v[vn] != '\n' && vn < sizeof state - 2) vn++;
            memcpy(state, v, vn); state[vn] = 0;
        } else {
            char *end = NULL;
            long val = strtol(v, &end, 10);
            if (end != v) {
                if      (strcmp(name, "owner_pid") == 0) owner = val;
                else if (strcmp(name, "xruns")     == 0) xruns = val;
                else if (strcmp(name, "avail")     == 0) avail = val;
                else if (strcmp(name, "avail_max") == 0) avail_max = val;
                else if (strcmp(name, "hw_ptr")    == 0) hw = val;
                else if (strcmp(name, "app_ptr")   == 0) app = val;
            }
        }
    }
    fclose(f);
    const char *alert = "";
    if (strcmp(state, "RUNNING") != 0) alert = "  <<< NOT RUNNING!";
    else if (avail > 4000) alert = "  <<< avail LARGE (buffer draining)";
    mlogf("  [%s] state=%s owner=%ld xruns=%ld avail=%ld avail_max=%ld hw=%ld app=%ld%s\n",
          label, state, owner, xruns, avail, avail_max, hw, app, alert);
}

static pid_t pid_of(const char *comm_name) {
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *e; pid_t found = -1;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char cmd[64]; snprintf(cmd, sizeof cmd, "/proc/%s/comm", e->d_name);
        FILE *f = fopen(cmd, "r");
        if (!f) continue;
        char c[64]; if (fgets(c, sizeof c, f)) {
            c[strcspn(c, "\r\n")] = 0;
            if (strcmp(c, comm_name) == 0) { found = (pid_t)strtol(e->d_name, NULL, 10); fclose(f); break; }
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

static void monitor_tick(long *prev_total, long *prev_idle, FILE *kmsg) {
    time_t now = time(NULL);
    char ts[64];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    mlogf("\n===== tick @ %s uptime=", ts);
    FILE *ut = fopen("/proc/uptime", "r");
    if (ut) { char l[64]; if (fgets(l, sizeof l, ut)) mlogf("%s", l); fclose(ut); } else mlogf("?\n");
    mlogf("=====\n");

    mlogf("-- audio pcm status --\n");
    pcm_status_line("/proc/asound/card0/pcm0p/sub0/status", "i2s-hifi 0,0");
    pcm_status_line("/proc/asound/card0/pcm1p/sub0/status", "rk3036-voice 0,1");

    mlogf("-- key processes --\n");
    const char *watch[] = { "retroarch", "icube_replacement", "diag", "zhijack", NULL };
    for (int i = 0; watch[i]; i++) {
        pid_t p = pid_of(watch[i]);
        mlogf("  %-16s %s\n", watch[i], p > 0 ? "ALIVE" : "not-running");
    }

    long total = 0, idle = 0;
    FILE *st = fopen("/proc/stat", "r");
    if (st) { char l[256];
        if (fgets(l, sizeof l, st)) {
            unsigned long a,b,c,d,e,g,h,i,j,k;
            if (sscanf(l, "cpu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu", &a,&b,&c,&d,&e,&g,&h,&i,&j,&k) == 10) {
                total = a+b+c+d+e+g+h+i+j+k; idle = d;
            }
        } fclose(st); }
    if (*prev_total > 0) {
        long dt = total - *prev_total, di = idle - *prev_idle;
        long pct = (dt > 0) ? 100 * (dt - di) / dt : 0;
        mlogf("-- cpu util since last tick: %ld%% --\n", pct < 0 ? 0 : pct);
    }
    *prev_total = total; *prev_idle = idle;

    FILE *mi = fopen("/proc/meminfo", "r");
    if (mi) { char l[128]; mlogf("-- memory --\n"); while (fgets(l, sizeof l, mi))
        if (strstr(l,"MemFree")||strstr(l,"MemAvailable")||strstr(l,"Buffers")||strstr(l,"Cached")||strstr(l,"SwapTotal")||strstr(l,"SwapFree"))
            mlogf("    %s", l);
        fclose(mi); }
    FILE *la = fopen("/proc/loadavg", "r");
    if (la) { char l[64]; if (fgets(l, sizeof l, la)) mlogf("  loadavg: %s", l); fclose(la); }

    mlogf("-- thermal / freq --\n");
    DIR *td = opendir("/sys/class/thermal");
    if (td) { struct dirent *e; while ((e = readdir(td))) {
        if (strncmp(e->d_name, "thermal_zone", 12) == 0) {
            char p[160]; snprintf(p, sizeof p, "/sys/class/thermal/%s/temp", e->d_name);
            long mv = sysfs_read_int(p, -1);
            if (mv >= 0) { long a = mv / 1000, b = mv % 1000; if (b < 0) b = -b; mlogf("    %s = %ld.%02ld C\n", e->d_name, a, b); }
        }
    } closedir(td); }
    long cf = sysfs_read_int("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", -1);
    if (cf > 0) mlogf("    cpu0 cur=%ldkHz\n", cf);

    if (kmsg) {
        mlogf("-- kernel ring delta --\n");
        char line[512]; int got = 0;
        int fl = fcntl(fileno(kmsg), F_GETFL, 0);
        if (fl >= 0) fcntl(fileno(kmsg), F_SETFL, fl | O_NONBLOCK);
        while (fgets(line, sizeof line, kmsg)) {
            if (strstr(line, "snd") || strstr(line, "i2s") || strstr(line, "dma") ||
                strstr(line, "xrun") || strstr(line, "underrun") || strstr(line, "frozen")) {
                char *m = strchr(line, ';'); mlogf("    kw:%s", m ? m + 1 : line);
                got++;
            }
        }
        if (got == 0) mlogf("    (no snd/i2s/dma/xrun lines since last tick)\n");
    }
}

static void cmd_monitor(int argc, char **argv) {
    g_fault_module = 21;
    int interval = 30, max_min = 0;
    if (argc > 2) { int v = atoi(argv[2]); if (v > 0 && v <= 3600) interval = v; }
    if (argc > 3) { int v = atoi(argv[3]); if (v > 0) max_min = v; }

    m_out = fopen(MONITOR_LOG, "a");
    time_t now = time(NULL);
    mlogf("\n# CubeGM diag MONITOR started %s  interval=%ds max_min=%d\n", ctime(&now), interval, max_min);
    if (!m_out) mlogf("# WARN: cannot open %s -- stdout only\n", MONITOR_LOG);

    long prev_total = -1, prev_idle = -1;
    FILE *kmsg = fopen("/dev/kmsg", "r");

    long ticks = 0;
    time_t start = time(NULL);
    for (;;) {
        monitor_tick(&prev_total, &prev_idle, kmsg);
        if (m_out && ftell(m_out) > MONITOR_MAX_BYTES) {
            fclose(m_out);
            m_out = fopen(MONITOR_LOG, "w");
            if (m_out) mlogf("# (log rolled >%dKB)\n", MONITOR_MAX_BYTES / 1024);
        }
        ticks++;
        if (max_min > 0 && (time(NULL) - start) / 60 >= max_min) {
            mlogf("# monitor finished after %ld min (%ld ticks)\n", (time(NULL)-start)/60, ticks);
            break;
        }
        sleep(interval);
    }
    if (kmsg) fclose(kmsg);
    if (m_out) { fclose(m_out); m_out = NULL; }
}


/* ===========================================================================
 * video -- KMS/GBM/EGL/GL full-stack probe (one-shot, run 516+ video fix line).
 *
 * Replicates the EXACT RetroArch @3c3561f call order (gfx/video_driver.c L1440
 * calls ctx->bind_api BEFORE egl_init_context; gfx/drivers_context/drm_ctx.c
 * L1155 bind_api = eglBindAPI(EGL_OPENGL_ES_API); L310 egl_init_context on
 * EGL_PLATFORM_GBM_KHR(0x31D7); egl_common.c L617 egl_init_context:
 * get_egl_display -> eglInitialize -> egl_init_context_common (chooseConfig)).
 * Every step logs rc + errno + eglGetError() so the failing ring is visible
 * without guessing. All EGL/GBM/GL entry points come from dlopen of the
 * payload blob (same libmali r1p1-gbm blob RetroArch uses at runtime via
 * LD_LIBRARY_PATH=/mnt/sdcard/cubegm/lib) -- diag adds NO new link deps.
 * ============================================================================ */

#include <elf.h>
#include <sys/param.h>

/* EGL constants -- real values, sourced (device sysroot headers, i.e. what
 * RetroArch was built against in run 516: /tmp/verify_sysroot MESA headers,
 * identical to upstream KHR egl.h/eglext.h):
 *   EGL/egl.h:    NONE 0x3038, VENDOR 0x3053, VERSION 0x3054,
 *                 EXTENSIONS 0x3055, CLIENT_APIS 0x308D, SURFACE_TYPE 0x3033,
 *                 WINDOW_BIT 0x0004, RENDERABLE_TYPE 0x3040, RED 0x3024,
 *                 GREEN 0x3023, BLUE 0x3022, ALPHA 0x3021, DEPTH 0x3025,
 *                 RENDER_BUFFER 0x3086, BACK_BUFFER 0x3084,
 *                 OPENGL_ES2_BIT 0x0004, CONTEXT_CLIENT_VERSION 0x3098,
 *                 OPENGL_ES_API 0x30A0
 *   EGL/eglext.h: EGL_PLATFORM_GBM_KHR 0x31D7 (RA drm_ctx.c L68 fallback
 *                 defines the same value)
 *   egl errors:   SUCCESS 0x3000, NOT_INIT 0x3001, BAD_ATTRIBUTE 0x3004,
 *                 BAD_CONFIG 0x3005, BAD_CONTEXT 0x3006, BAD_DISPLAY 0x3008,
 *                 BAD_NATIVE_PIXMAP 0x300A, BAD_SURFACE 0x300D,
 *                 CONTEXT_LOST 0x300E
 * All entry points still resolved via dlsym (no header link needed). */
#define D_EGL_NONE                  0x3038
#define D_EGL_VENDOR                0x3053
#define D_EGL_VERSION               0x3054
#define D_EGL_EXTENSIONS            0x3055
#define D_EGL_CLIENT_APIS           0x308D
#define D_EGL_SURFACE_TYPE          0x3033
#define D_EGL_WINDOW_BIT            0x0004
#define D_EGL_RENDERABLE_TYPE       0x3040
#define D_EGL_RED_SIZE              0x3024
#define D_EGL_GREEN_SIZE            0x3023
#define D_EGL_BLUE_SIZE             0x3022
#define D_EGL_ALPHA_SIZE            0x3021
#define D_EGL_DEPTH_SIZE            0x3025
#define D_EGL_RENDER_BUFFER         0x3086
#define D_EGL_BACK_BUFFER           0x3084
#define D_EGL_OPENGL_ES2_BIT        0x0004
#define D_EGL_CONTEXT_CLIENT_VERSION 0x3098
#define D_EGL_OPENGL_ES_API         0x30A0
#define D_EGL_PLATFORM_GBM_KHR      0x31D7   /* MESA eglext.h L302; RA drm_ctx.c L68 */

/* GBM constants -- Mali official gbm.h (/tmp/mali_hdr/gbm.h L180-240):
 *   enum gbm_bo_flags: SCANOUT = 1<<0, CURSOR = 1<<1, RENDERING = 1<<2.
 *   gbm_surface_create(dev,w,h,format,flags) takes a 5th flags arg (upstream
 *   mesa gbm API; Mali blob implements it).
 *   XRGB8888 fourcc: GBM_FORMAT_XRGB8888 (mali gbm.h L122) ==
 *   DRM_FORMAT_XRGB8888 (device UAPI drm_fourcc.h L77) = fourcc 'X','R','2','4'. */
#define D_GBM_BO_USE_SCANOUT        (1u << 0)
#define D_GBM_BO_USE_RENDERING      (1u << 2)
#define D_GBM_FORMAT_XRGB8888       ((uint32_t)'X' | ((uint32_t)'R' << 8) | \
                                     ((uint32_t)'2' << 16) | ((uint32_t)'4' << 24))
/* GL (GLES2) string tokens (standard) */
#define D_GL_VENDOR                 0x1F00
#define D_GL_RENDERER               0x1F01
#define D_GL_VERSION                0x1F02
#define D_GL_SHADING_LANGUAGE_VER   0x8B8E

typedef void *DPFN_display;

struct d_video {
    /* egl */
    void *h;
    DPFN_display (*eglGetError)(void);
    DPFN_display (*eglGetDisplay)(unsigned long);
    DPFN_display (*eglGetPlatformDisplay)(long, void *, const long *);
    DPFN_display (*eglGetPlatformDisplayEXT)(long, void *, const long *);
    int          (*eglInitialize)(DPFN_display, unsigned int *, unsigned int *);
    const char *(*eglQueryString)(DPFN_display, unsigned int);
    int          (*eglChooseConfig)(DPFN_display, const long *, void **, unsigned int *);
    void *       (*eglCreateContext)(DPFN_display, void *, void *, const long *);
    void *       (*eglCreateWindowSurface)(DPFN_display, void *, void *, const long *);
    int          (*eglMakeCurrent)(DPFN_display, void *, void *, void *);
    int          (*eglDestroySurface)(DPFN_display, void *);
    int          (*eglDestroyContext)(DPFN_display, void *);
    int          (*eglTerminate)(DPFN_display);
    int          (*eglBindAPI)(unsigned int);
    void *       (*eglGetCurrentContext)(void);
    DPFN_display (*eglGetConfigAttrib)(DPFN_display, void *, unsigned int, unsigned int *);
    /* gbm (Mali official gbm.h: surface_create takes a 5th flags arg;
     * there is NO gbm_surface_create_with_bo in the Mali blob API) */
    void *h_g;
    void *(*gbm_create_device)(int);
    void *(*gbm_bo_create)(void *, unsigned int, unsigned int, unsigned int, unsigned int);
    void *(*gbm_surface_create)(void *, unsigned int, unsigned int, unsigned int, unsigned int);
    unsigned int (*gbm_bo_get_width)(void *);
    unsigned int (*gbm_bo_get_height)(void *);
    unsigned int (*gbm_bo_get_format)(void *);
    int  (*gbm_bo_destroy)(void *);
    int  (*gbm_surface_destroy)(void *);
    /* gles2 */
    void *h_s;
    const char *(*glGetString)(unsigned int);
};

static const char *d_egl_err_str(unsigned e) {
    /* MESA EGL/egl.h error values (device sysroot, run 516 build) */
    switch (e) {
    case 0x3000: return "EGL_SUCCESS";
    case 0x3001: return "EGL_NOT_INITIALIZED";
    case 0x3004: return "EGL_BAD_ATTRIBUTE";
    case 0x3005: return "EGL_BAD_CONFIG";
    case 0x3006: return "EGL_BAD_CONTEXT";
    case 0x3007: return "EGL_BAD_CURRENT_SURFACE";
    case 0x3008: return "EGL_BAD_DISPLAY";
    case 0x3009: return "EGL_BAD_MATCH";
    case 0x300A: return "EGL_BAD_NATIVE_PIXMAP";
    case 0x300B: return "EGL_BAD_NATIVE_WINDOW";
    case 0x300C: return "EGL_BAD_PARAMETER";
    case 0x300D: return "EGL_BAD_SURFACE";
    case 0x300E: return "EGL_CONTEXT_LOST";
    default: return "(other)";
    }
}

/* dlopen the payload blob lib by candidate names (RA runtime path first). */
static void *d_video_open(const char *names[], int n) {
    for (int i = 0; i < n; i++) {
        void *h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (h) { logf("  dlopen %s OK -> %p\n", names[i], h); return h; }
        logf("  dlopen %s failed: %s\n", names[i], dlerror());
    }
    return NULL;
}

/* ELF32 DT_NEEDED / DT_SONAME (manual parse: no libelf). */
static void d_parse_elf(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { logf("  [%s] open failed: %s\n", path, strerror(errno)); return; }
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0 || sz > (off_t)256 * 1024 * 1024) {
        logf("  [%s] bad size %lld\n", path, (long long)sz);
        if (fd >= 0) close(fd);
        return;
    }
    char *b = (char *)malloc((size_t)sz);
    if (!b) { close(fd); return; }
    lseek(fd, 0, SEEK_SET);
    if (read(fd, b, (size_t)sz) != sz) { logf("  [%s] short read\n", path); free(b); close(fd); return; }
    close(fd);
    if ((unsigned char)b[0] != 0x7f || memcmp(b + 1, "ELF", 3) != 0) {
        logf("  [%s] not an ELF\n", path); free(b); return;
    }
    if (b[4] != 1) { logf("  [%s] 64-bit ELF, expected 32-bit ARM\n", path); free(b); return; }
    const Elf32_Ehdr *e = (const Elf32_Ehdr *)b;
    const Elf32_Shdr *sh = (const Elf32_Shdr *)(b + e->e_shoff);
    const char *shstr = b + sh[e->e_shstrndx].sh_offset;
    const char *dynstr = NULL; const char *dynamic = NULL;
    unsigned dynstr_sz = 0;
    for (int i = 0; i < e->e_shnum; i++) {
        const char *nm = shstr + sh[i].sh_name;
        if (sh[i].sh_size == 0) continue;
        if (strcmp(nm, ".dynstr") == 0) { dynstr = b + sh[i].sh_offset; dynstr_sz = sh[i].sh_size; }
        else if (strcmp(nm, ".dynamic") == 0) dynamic = b + sh[i].sh_offset;
    }
    if (!dynamic || !dynstr) { logf("  [%s] no .dynamic/.dynstr (fully stripped?)\n", path); free(b); return; }
    const Elf32_Dyn *dy = (const Elf32_Dyn *)dynamic;
    logf("  [%s] DT_NEEDED: ", path);
    int first = 1;
    for (int i = 0; dy[i].d_tag != DT_NULL; i++) {
        if (dy[i].d_tag == DT_NEEDED) {
            logf("%s%s", first ? "" : ", ", dynstr + dy[i].d_un.d_val);
            first = 0;
        }
    }
    logf("\n");
    for (int i = 0; dy[i].d_tag != DT_NULL; i++) {
        if (dy[i].d_tag == DT_SONAME) {
            logf("  [%s] DT_SONAME = %.*s\n", path, (int)dynstr_sz, dynstr + dy[i].d_un.d_val);
            break;
        }
    }
    free(b);
}

/* hex dump a sysfs byte file (EDID blocks are raw bytes) */
static void d_hex_dump(const char *path, int max_bytes) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { logf("    [%s] open failed: %s\n", path, strerror(errno)); return; }
    unsigned char buf[512];
    ssize_t n = read(fd, buf, sizeof buf);
    close(fd);
    if (n <= 0) { logf("    [%s] empty (%s)\n", path, strerror(errno)); return; }
    int lim = (int)n < max_bytes ? (int)n : max_bytes;
    int all_ff = 1;
    for (int i = 0; i < lim; i++) if (buf[i] != 0xFF) all_ff = 0;
    logf("    [%s] %d bytes%s\n", path, (int)n, all_ff ? " -- ALL 0xFF (empty/missing EDID)" : "");
    for (int off = 0; off < lim; off += 16) {
        logf("      %04x: ", off);
        int cnt = lim - off; if (cnt > 16) cnt = 16;
        char line[16 * 3 + 20]; int pos = 0;
        for (int i = 0; i < cnt; i++) pos += snprintf(line + pos, sizeof(line) - pos, "%02x ", buf[off + i]);
        line[pos] = 0;
        logf("%s\n", line);
    }
    /* PnP ID: EDID 0..5 = mfr(2) + product(2) + serial(4) */
    if (n >= 8 && !all_ff)
        logf("      EDID header mfr=%02x%02x product=%02x%02x serial=%02x%02x%02x%02x\n",
             buf[4], buf[5], buf[6], buf[7], buf[0], buf[1], buf[2], buf[3]);
}

static void cmd_video(void) {
    g_fault_module = 40;
    logf("\n=== video (KMS/GBM/EGL/GL full-stack; replicates RA@3c3561f call order) ===\n");

    /* --- 0. whoami / env --- */
    logf("  uid=%d euid=%d\n", getuid(), geteuid());
    {
        const char *ldl = getenv("LD_LIBRARY_PATH");
        logf("  LD_LIBRARY_PATH=%s\n", ldl ? ldl : "(unset)");
        const char *home = getenv("HOME");
        logf("  HOME=%s\n", home ? home : "(unset)");
    }
    cat_file("/proc/dri/card0");

    /* --- 1. device nodes --- */
    logf("  --- device nodes ---\n");
    {
        DIR *d = opendir("/dev/dri");
        logf("    /dev/dri:\n");
        if (d) {
            struct dirent *e;
            int any = 0;
            while ((e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                struct stat st;
                char p[128];
                snprintf(p, sizeof p, "/dev/dri/%s", e->d_name);
                if (stat(p, &st) == 0) {
                    logf("      %s  perm=%o dev=%d:%d\n", e->d_name, (unsigned)st.st_mode,
                         (int)(st.st_rdev >> 8), (int)(st.st_rdev & 0xff));
                    any = 1;
                }
            }
            if (!any) logf("      (empty)\n");
            closedir(d);
        } else logf("      opendir failed: %s\n", strerror(errno));
        struct stat st;
        logf("    /dev/mali: %s\n", stat("/dev/mali", &st) == 0
               ? "present" : "ABSENT");
        DIR *dfb = opendir("/dev");
        logf("    /dev/fb*:\n");
        if (dfb) {
            struct dirent *e;
            int nfb = 0;
            while ((e = readdir(dfb))) {
                if (strncmp(e->d_name, "fb", 2) != 0 || (e->d_name[2] == 0 || e->d_name[2] == '.')) continue;
                logf("      /dev/%s\n", e->d_name);
                nfb++;
            }
            if (!nfb) logf("      (none -- fbs=0, pure DRM display stack)\n");
            closedir(dfb);
        } else logf("      opendir failed: %s\n", strerror(errno));
    }

    /* --- 2. DRM resources (independent copy of the two-pass protocol) --- */
    logf("  --- DRM resources (card0) ---\n");
    int cfd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (cfd < 0) { logf("    open card0 O_RDWR FAILED: %s\n", strerror(errno)); cfd = -1; }
    if (cfd < 0) {
        cfd = open("/dev/dri/card0", O_RDONLY | O_CLOEXEC);
        if (cfd < 0) logf("    open card0 O_RDONLY also failed: %s\n", strerror(errno));
    }
    if (cfd >= 0) {
        struct drm_mode_card_res res; memset(&res, 0, sizeof res);
        if (ioctl(cfd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0) {
            logf("    count_connectors=%u count_encoders=%u count_crtcs=%u count_fbs=%u\n",
                 res.count_connectors, res.count_encoders, res.count_crtcs, res.count_fbs);
            uint32_t *conns = calloc(res.count_connectors ? res.count_connectors : 1, 4);
            uint32_t *crtcs = calloc(res.count_crtcs ? res.count_crtcs : 1, 4);
            uint32_t *encs  = calloc(res.count_encoders ? res.count_encoders : 1, 4);
            uint32_t *fbs   = calloc(res.count_fbs ? res.count_fbs : 1, 4);
            res.connector_id_ptr = (uintptr_t)conns;
            res.crtc_id_ptr      = (uintptr_t)crtcs;
            res.encoder_id_ptr   = (uintptr_t)encs;
            res.fb_id_ptr        = (uintptr_t)fbs;
            if (ioctl(cfd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0) {
                for (uint32_t i = 0; i < res.count_connectors; i++) {
                    struct drm_mode_get_connector gc; memset(&gc, 0, sizeof gc);
                    gc.connector_id = conns[i];
                    if (ioctl(cfd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) != 0) {
                        logf("    conn%u(%u): GETCONNECTOR failed: %s\n", i, conns[i], strerror(errno));
                        continue;
                    }
                    static const char *tnames[] = {
                        "Unknown", "VGA", "DVI", "Composite", "S-Video", "LVDS", "Component",
                        "Dp", "HDMIA", "HDMIB", "TBT", "eDP", "Virtual1", "Virtual2", "Virtual3"
                    };
                    const char *tn = gc.connector_type < 15 ? tnames[gc.connector_type] : "ext";
                    static const char *connames[] = { "Disconn", "Connected", "Unknown" };
                    logf("    conn%u id=%u name? type=%s(%u) connection=%s encoders_current=%u\n",
                         i, gc.connector_id, tn, gc.connector_type,
                         gc.connection < 3 ? connames[gc.connection] : "?", gc.encoder_id);
                    logf("      mm=%ux%u  count_modes=%u\n", gc.mm_width, gc.mm_height, gc.count_modes);
                    if (gc.count_modes) {
                        struct drm_mode_modeinfo *modes = calloc(gc.count_modes, sizeof *modes);
                        gc.modes_ptr = (uintptr_t)modes;
                        if (ioctl(cfd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) == 0) {
                            for (uint32_t m = 0; m < gc.count_modes; m++)
                                logf("      mode%u: %ux%u @%u.%02f%s\n", m,
                                     modes[m].hdisplay, modes[m].vdisplay,
                                     modes[m].clock / 1000, modes[m].clock % 1000,
                                     (m == 0 && gc.connector_id ? " *" : ""));
                        }
                        free(modes);
                    } else {
                        logf("      count_modes=0  <-- NO MODES on this connector (matches 'no usable connector/mode')\n");
                    }
                    logf("      encoders=%u props=%u\n", gc.count_encoders, gc.count_props);
                }
            } else logf("    GETRESOURCES(2) failed: %s\n", strerror(errno));
            free(conns); free(crtcs); free(encs); free(fbs);
        } else logf("    GETRESOURCES failed: %s\n", strerror(errno));

        /* --- 4. DRM master --- */
        logf("  --- DRM master ---\n");
        {
            /* Try to (re)acquire master on this fd. If RA already holds it,
             * the kernel denies (EACCES) -- that itself is evidence. Do NOT
             * hold it afterwards (diag should not steal from RA). */
            int r = ioctl(cfd, DRM_IOCTL_SET_MASTER);
            logf("    SET_MASTER rc=%d errno=%d (%s)\n", r,
                 r < 0 ? errno : 0, r < 0 ? strerror(errno) : "ok");
            if (r == 0) {
                /* drop it again so RA/driver state is untouched */
                int d = ioctl(cfd, DRM_IOCTL_DROP_MASTER);
                logf("    DROP_MASTER rc=%d errno=%d (%s)\n", d,
                     d < 0 ? errno : 0, d < 0 ? strerror(errno) : "ok");
            }
            cat_file("/proc/dri/card0");
        }
    }

    /* --- 3. EDID via sysfs (raw bytes; all-0xFF = innohdmi has no EDID -> no modes) --- */
    logf("  --- EDID / connector sysfs ---\n");
    {
        DIR *sd = opendir("/sys/class/drm");
        if (sd) {
            struct dirent *e;
            while ((e = readdir(sd))) {
                if (strncmp(e->d_name, "card0", 5) != 0) continue;
                char base[256];
                snprintf(base, sizeof base, "/sys/class/drm/%s", e->d_name);
                char statusp[300], edidp[300];
                snprintf(statusp, sizeof statusp, "%s/status", base);
                if (access(statusp, R_OK) == 0) {
                    FILE *f = fopen(statusp, "r");
                    char line[64] = "";
                    if (f) { fgets(line, sizeof line, f); fclose(f); }
                    logf("    %s\n      status=%s", e->d_name, line);
                }
                snprintf(edidp, sizeof edidp, "%s/edid", base);
                d_hex_dump(edidp, 18);
            }
            closedir(sd);
        } else logf("    /sys/class/drm opendir failed: %s\n", strerror(errno));
    }

    /* --- 5. payload lib layout (blob + symlinks) --- */
    logf("  --- payload libs (/mnt/sdcard/cubegm/lib) ---\n");
    {
        DIR *d = opendir("/mnt/sdcard/cubegm/lib");
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (e->d_name[0] == '.' ) {
                    if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
                        logf("      %s\n", e->d_name);
                    continue;
                }
                char p[200];
                snprintf(p, sizeof p, "/mnt/sdcard/cubegm/lib/%s", e->d_name);
                struct stat st;
                char target[256] = "";
                if (lstat(p, &st) == 0)
                    snprintf(target, sizeof target, " [symlink -> %s]",
                             readlink(p, target, sizeof target - 1) > 0 ? target : "(unreadable)");
                logf("      %s  %d bytes%s\n", e->d_name, (int)st.st_size, target);
            }
            closedir(d);
        } else logf("    opendir failed: %s\n", strerror(errno));
    }
    logf("  --- ELF deps ---\n");
    d_parse_elf("/mnt/sdcard/cubegm/retroarch");
    d_parse_elf("/mnt/sdcard/cubegm/lib/libmali.so.1");

    /* --- 6. current user cfg values (informational; icube never overwrites) --- */
    logf("  --- retroarch.cfg (user-owned; values in effect) ---\n");
    {
        FILE *f = fopen("/mnt/sdcard/cubegm/retroarch.cfg", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof line, f)) {
                if (strstr(line, "video_") || strstr(line, "log_") ||
                    strstr(line, "frontend_log_level") || strstr(line, "verbosity"))
                    logf("    %s", line);
            }
            fclose(f);
        } else logf("    (no retroarch.cfg at /mnt/sdcard/cubegm/retroarch.cfg)\n");
    }

    /* --- 7+8. live RA call chain: gbm -> EGL -> GL ---
     * GPU-live: dlopen of the Mali blob + DRM master probing. Gated to the
     * manual 'diag video' only; boot 'diag all' (forked bg next to a running
     * RetroArch) keeps the GPU read-only. 60s SIGALRM watchdog guards the
     * live path: a blob deadlock must not hang the report/boot. */
    if (!g_video_live) {
        logf("  -- live EGL/GBM/GL chain SKIPPED (boot diag all keeps GPU read-only)\n");
        logf("     (run 'diag video' manually for live gbm/egl/gl probing)\n");
        logf("=== video done ===\n");
        return;
    }
    {
        struct sigaction wsa; memset(&wsa, 0, sizeof wsa);
        wsa.sa_handler = video_watchdog;
        sigaction(SIGALRM, &wsa, NULL);
        alarm(60);
    }
    logf("  --- EGL/GBM/GL chain (dlopen payload blob; RA original call order first) ---\n");
    struct d_video d; memset(&d, 0, sizeof d);

    {
        static const char *egl_names[] = {
            "/mnt/sdcard/cubegm/lib/libEGL.so",
            "/mnt/sdcard/cubegm/lib/libEGL.so.1",
            "/mnt/sdcard/cubegm/lib/libmali.so.1",
            "libEGL.so", "libEGL.so.1", "libmali.so.1"
        };
        d.h = d_video_open(egl_names, 6);
    }
    if (d.h) {
        d.eglGetError            = (DPFN_display)dlsym(d.h, "eglGetError");
        d.eglGetDisplay         = (DPFN_display)dlsym(d.h, "eglGetDisplay");
        d.eglGetPlatformDisplay = (DPFN_display)dlsym(d.h, "eglGetPlatformDisplay");
        d.eglGetPlatformDisplayEXT = (DPFN_display)dlsym(d.h, "eglGetPlatformDisplayEXT");
        d.eglInitialize         = (int (*)(DPFN_display, unsigned int *, unsigned int *))dlsym(d.h, "eglInitialize");
        d.eglQueryString        = (const char *(*)(DPFN_display, unsigned int))dlsym(d.h, "eglQueryString");
        d.eglChooseConfig       = (int (*)(DPFN_display, const long *, void **, unsigned int *))dlsym(d.h, "eglChooseConfig");
        d.eglCreateContext      = (void *(*)(DPFN_display, void *, void *, const long *))dlsym(d.h, "eglCreateContext");
        d.eglCreateWindowSurface= (void *(*)(DPFN_display, void *, void *, const long *))dlsym(d.h, "eglCreateWindowSurface");
        d.eglMakeCurrent        = (int (*)(DPFN_display, void *, void *, void *))dlsym(d.h, "eglMakeCurrent");
        d.eglDestroySurface     = (int (*)(DPFN_display, void *))dlsym(d.h, "eglDestroySurface");
        d.eglDestroyContext     = (int (*)(DPFN_display, void *))dlsym(d.h, "eglDestroyContext");
        d.eglTerminate          = (int (*)(DPFN_display))dlsym(d.h, "eglTerminate");
        d.eglBindAPI            = (int (*)(unsigned int))dlsym(d.h, "eglBindAPI");
        d.eglGetCurrentContext  = (void *(*)(void))dlsym(d.h, "eglGetCurrentContext");
        logf("    EGL symbols: GetError=%p GetDisplay=%p GetPlatformDisplay=%p Initialize=%p QueryString=%p ChooseConfig=%p\n",
             (void*)d.eglGetError, (void*)d.eglGetDisplay, (void*)d.eglGetPlatformDisplay,
             (void*)d.eglInitialize, (void*)d.eglQueryString, (void*)d.eglChooseConfig);
    }
    logf("    GBM symbols (same blob): ");
    {
        static const char *gbm_names[] = {
            "/mnt/sdcard/cubegm/lib/libgbm.so",
            "/mnt/sdcard/cubegm/lib/libgbm.so.1",
            "/mnt/sdcard/cubegm/lib/libmali.so.1",
            "libgbm.so", "libgbm.so.1"
        };
        d.h_g = d_video_open(gbm_names, 5);
    }
    if (d.h_g) {
        d.gbm_create_device = (void *(*)(int))dlsym(d.h_g, "gbm_create_device");
        d.gbm_bo_create    = (void *(*)(void *, unsigned int, unsigned int, unsigned int, unsigned int))dlsym(d.h_g, "gbm_bo_create");
        d.gbm_surface_create = (void *(*)(void *, unsigned int, unsigned int, unsigned int, unsigned int))dlsym(d.h_g, "gbm_surface_create");
        d.gbm_bo_get_width = (unsigned int (*)(void *))dlsym(d.h_g, "gbm_bo_get_width");
        d.gbm_bo_get_height= (unsigned int (*)(void *))dlsym(d.h_g, "gbm_bo_get_height");
        d.gbm_bo_get_format= (unsigned int (*)(void *))dlsym(d.h_g, "gbm_bo_get_format");
        d.gbm_bo_destroy   = (int (*)(void *))dlsym(d.h_g, "gbm_bo_destroy");
        d.gbm_surface_destroy = (int (*)(void *))dlsym(d.h_g, "gbm_surface_destroy");
        logf("create_device=%p bo_create=%p surface_create=%p\n",
             (void*)d.gbm_create_device, (void*)d.gbm_bo_create, (void*)d.gbm_surface_create);
    }
    {
        static const char *gles_names[] = {
            "/mnt/sdcard/cubegm/lib/libGLESv2.so",
            "/mnt/sdcard/cubegm/lib/libGLESv2.so.2",
            "/mnt/sdcard/cubegm/lib/libmali.so.1",
            "libGLESv2.so"
        };
        d.h_s = d_video_open(gles_names, 4);
    }
    if (d.h_s) {
        d.glGetString = (const char *(*)(unsigned int))dlsym(d.h_s, "glGetString");
        logf("    glGetString=%p\n", (void*)d.glGetString);
    }

    if (cfd < 0 || !d.h) {
        logf("  (skipping EGL chain: %s %s)\n",
             cfd < 0 ? "card0 not open" : "card0 open",
             !d.h ? "-- no EGL handle" : "");
    } else {
        /* GBM device on card0 (RA: gbm_create_device(fd) after drmSetMaster) */
        void *gbm_dev = NULL;
        if (d.gbm_create_device) {
            gbm_dev = d.gbm_create_device(cfd);
            logf("    gbm_create_device(card0) -> %p %s\n", gbm_dev, gbm_dev ? "" : "(NULL)");
        } else logf("    gbm_create_device symbol MISSING in blob\n");

        /* === Stage A: RA original order (bind_api BEFORE any EGL init) === */
        logf("  -- Stage A: RA original order (video_driver.c L1440 bind_api first) --\n");
        if (d.eglBindAPI) {
            int rc = d.eglBindAPI(D_EGL_OPENGL_ES_API);
            logf("    A1 eglBindAPI(OPENGL_ES_API=0x30A0) rc=%d eglErr=0x%04x (%s)\n",
                 rc, (unsigned)(d.eglGetError ? d.eglGetError() : 0),
                 d_egl_err_str((unsigned)(d.eglGetError ? d.eglGetError() : 0)));
        } else logf("    A1 eglBindAPI symbol MISSING\n");

        DPFN_display dpy = NULL;
        int dpy_path = 0;
        if (dpy == NULL && d.eglGetPlatformDisplay && gbm_dev) {
            dpy = d.eglGetPlatformDisplay((long)D_EGL_PLATFORM_GBM_KHR, gbm_dev, NULL);
            dpy_path = 1;
        }
        if (dpy == NULL && d.eglGetPlatformDisplayEXT && gbm_dev) {
            dpy = d.eglGetPlatformDisplayEXT((long)D_EGL_PLATFORM_GBM_KHR, gbm_dev, NULL);
            dpy_path = 2;
        }
        if (dpy == NULL && d.eglGetDisplay) {
            dpy = d.eglGetDisplay(0);
            dpy_path = 3;
        }
        logf("    A2 egl display: path=%d (%s) dpy=%p eglErr=0x%04x (%s)\n",
             dpy_path,
             dpy_path == 1 ? "eglGetPlatformDisplay(GBM)" :
             dpy_path == 2 ? "eglGetPlatformDisplayEXT(GBM)" :
             dpy_path == 3 ? "eglGetDisplay(0) fbdev-fallback" : "NONE",
             dpy,
             (unsigned)(d.eglGetError ? d.eglGetError() : 0),
             d_egl_err_str((unsigned)(d.eglGetError ? d.eglGetError() : 0)));

        unsigned int maj = 0, mino = 0;
        if (dpy && d.eglInitialize) {
            int rc = d.eglInitialize(dpy, &maj, &mino);
            logf("    A3 eglInitialize rc=%d ver=%u.%u eglErr=0x%04x (%s)\n",
                 rc, maj, mino,
                 (unsigned)(d.eglGetError ? d.eglGetError() : 0),
                 d_egl_err_str((unsigned)(d.eglGetError ? d.eglGetError() : 0)));
        } else logf("    A3 eglInitialize SKIPPED (no display or symbol)\n");

        if (dpy && d.eglQueryString) {
            const char *vend = d.eglQueryString(dpy, D_EGL_VENDOR);
            const char *ver  = d.eglQueryString(dpy, D_EGL_VERSION);
            const char *apis = d.eglQueryString(dpy, D_EGL_CLIENT_APIS);
            const char *exts = d.eglQueryString(dpy, D_EGL_EXTENSIONS);
            logf("    A4 VENDOR=%s\n", vend ? vend : "(NULL)");
            logf("       VERSION=%s\n", ver ? ver : "(NULL)");
            logf("       CLIENT_APIS=%s\n", apis ? apis : "(NULL)");
            logf("       EXTENSIONS=%s\n", exts ? exts : "(NULL)");
        }

        /* RA exact attribs (drm_ctx.c L239 DRM_EGL_ATTRIBS_BASE + GLES2 bit) */
        long attribs[] = {
            D_EGL_SURFACE_TYPE, D_EGL_WINDOW_BIT,
            D_EGL_RED_SIZE, 1, D_EGL_GREEN_SIZE, 1, D_EGL_BLUE_SIZE, 1,
            D_EGL_ALPHA_SIZE, 0, D_EGL_DEPTH_SIZE, 0,
            D_EGL_RENDERABLE_TYPE, D_EGL_OPENGL_ES2_BIT,
            D_EGL_NONE
        };
        void *cfg = NULL;
        unsigned int n = 0;
        if (dpy && d.eglChooseConfig) {
            void *cfgs = (void *)calloc(1, sizeof(void *));
            int rc = d.eglChooseConfig(dpy, attribs, &cfgs, &n);
            logf("    A5 eglChooseConfig(RA attrs) rc=%d n=%u eglErr=0x%04x (%s)\n",
                 rc, n,
                 (unsigned)(d.eglGetError ? d.eglGetError() : 0),
                 d_egl_err_str((unsigned)(d.eglGetError ? d.eglGetError() : 0)));
            if (rc && n > 0) cfg = cfgs;
            free(cfgs);
        }

        void *ctx = NULL;
        if (cfg && d.eglCreateContext) {
            long cattrs[] = { D_EGL_CONTEXT_CLIENT_VERSION, 2, D_EGL_NONE };
            ctx = d.eglCreateContext(dpy, cfg, NULL, cattrs);
            logf("    A6 eglCreateContext(ES2) -> %p eglErr=0x%04x (%s)\n",
                 ctx,
                 (unsigned)(d.eglGetError ? d.eglGetError() : 0),
                 d_egl_err_str((unsigned)(d.eglGetError ? d.eglGetError() : 0)));
        } else logf("    A6 eglCreateContext SKIPPED (no config/ctx symbol)\n");

        /* GBM buffer + surface (RA official: gbm_surface_create(dev,w,h,fmt,flags)
         * with GBM_FORMAT_XRGB8888 + SCANOUT|RENDERING — drm_ctx.c L1027/L672) */
        void *bo = NULL, *gsurf = NULL;
        if (gbm_dev && d.gbm_bo_create && d.gbm_surface_create) {
            bo = d.gbm_bo_create(gbm_dev, 1280, 720, D_GBM_FORMAT_XRGB8888,
                                 D_GBM_BO_USE_SCANOUT | D_GBM_BO_USE_RENDERING);
            logf("    A7a gbm_bo_create(1280x720 XRGB8888) -> %p\n", bo);
            if (bo && d.gbm_bo_get_width)
                logf("        bo w=%u h=%u fmt=0x%08x\n",
                     d.gbm_bo_get_width(bo), d.gbm_bo_get_height(bo), d.gbm_bo_get_format(bo));
            gsurf = d.gbm_surface_create(gbm_dev, 1280, 720, D_GBM_FORMAT_XRGB8888,
                                         D_GBM_BO_USE_SCANOUT | D_GBM_BO_USE_RENDERING);
            logf("    A7b gbm_surface_create(1280x720 XRGB8888, flags=%u) -> %p\n",
                 D_GBM_BO_USE_SCANOUT | D_GBM_BO_USE_RENDERING, gsurf);
        } else logf("    A7 gbm bo/surface SKIPPED (no gbm_dev/symbols)\n");

        void *wsurf = NULL;
        if (dpy && cfg && ctx && gsurf && d.eglCreateWindowSurface) {
            long wattrs[] = { D_EGL_RENDER_BUFFER, D_EGL_BACK_BUFFER, D_EGL_NONE };
            wsurf = d.eglCreateWindowSurface(dpy, cfg, gsurf, wattrs);
            logf("    A8 eglCreateWindowSurface -> %p eglErr=0x%04x (%s)\n",
                 wsurf,
                 (unsigned)(d.eglGetError ? d.eglGetError() : 0),
                 d_egl_err_str((unsigned)(d.eglGetError ? d.eglGetError() : 0)));
        } else logf("    A8 eglCreateWindowSurface SKIPPED (prereq missing)\n");

        if (dpy && ctx && d.eglMakeCurrent) {
            int rc;
            if (wsurf)
                rc = d.eglMakeCurrent(dpy, wsurf, wsurf, ctx);
            else
                rc = d.eglMakeCurrent(dpy, NULL, NULL, ctx); /* EGL 1.4/1.5 core context w/o surface */
            logf("    A9 eglMakeCurrent(%s) rc=%d eglErr=0x%04x (%s)\n",
                 wsurf ? "wsurf" : "no-surface", rc,
                 (unsigned)(d.eglGetError ? d.eglGetError() : 0),
                 d_egl_err_str((unsigned)(d.eglGetError ? d.eglGetError() : 0)));
        }

        /* Stage B: bind AFTER proper init+makeCurrent (the "correct" order) */
        logf("  -- Stage B: correct-order bind (after Initialize+MakeCurrent) --\n");
        if (d.eglBindAPI) {
            int rc = d.eglBindAPI(D_EGL_OPENGL_ES_API);
            logf("    B1 eglBindAPI(OPENGL_ES_API) rc=%d eglErr=0x%04x (%s)\n",
                 rc,
                 (unsigned)(d.eglGetError ? d.eglGetError() : 0),
                 d_egl_err_str((unsigned)(d.eglGetError ? d.eglGetError() : 0)));
            if (d.eglGetCurrentContext)
                logf("    B2 eglGetCurrentContext -> %p\n", d.eglGetCurrentContext());
        }

        if (d.glGetString && d.eglGetCurrentContext && d.eglGetCurrentContext() != NULL) {
            const char *v = d.glGetString(D_GL_VENDOR);
            const char *r = d.glGetString(D_GL_RENDERER);
            const char *g = d.glGetString(D_GL_VERSION);
            const char *s = d.glGetString(D_GL_SHADING_LANGUAGE_VER);
            logf("    B3 GL_VENDOR=%s\n", v ? v : "(NULL)");
            logf("       GL_RENDERER=%s\n", r ? r : "(NULL)");
            logf("       GL_VERSION=%s\n", g ? g : "(NULL)");
            logf("       GL_SHADING_LANGUAGE_VERSION=%s\n", s ? s : "(NULL)");
        } else {
            logf("    B3 GL strings SKIPPED (no GL handle/current ctx)\n");
        }

        /* cleanup (best-effort, guarded) */
        if (wsurf && d.eglDestroySurface)  d.eglDestroySurface(dpy, wsurf);
        if (gsurf && d.gbm_surface_destroy) d.gbm_surface_destroy(gsurf);
        if (bo && d.gbm_bo_destroy)        d.gbm_bo_destroy(bo);
        if (ctx && d.eglDestroyContext)     d.eglDestroyContext(dpy, ctx);
        if (dpy && d.eglTerminate)         d.eglTerminate(dpy);
        logf("    cleanup done\n");
    }
    alarm(0);   /* live chain finished in time -> cancel watchdog */
    logf("=== video done ===\n");
}

/* =========================================================================== */
int main(int argc, char **argv) {
    install_guards();
    const char *mod = argc > 1 ? argv[1] : "all";
    /* Live EGL/GBM/GL chain runs ONLY on explicit 'diag video' (manual).
     * Boot 'diag all' (forked bg next to live RetroArch) keeps the GPU
     * read-only. */
    g_video_live = (strcmp(mod, "video") == 0);
    /* keylog 只写 keylog.txt，不碰 diag_report.txt —— 否则并发覆盖 diag all 的 gpu 段 */
    if (strcmp(mod, "keylog") != 0) g_out = fopen(REPORT, "w");
    if (g_out) logf("# CubeGM diag %s %s\n", mod, ctime(&(time_t){time(NULL)}));
    else logf("# WARN: cannot write %s (SD read-only?) -- console only\n", REPORT);
    /* gpu 排最前：diag all 最先执行 GPU 探针，避免进程被外部杀掉时 gpu 段来不及落盘 */
    if (strcmp(mod, "all") == 0 || strcmp(mod, "gpu") == 0)            cmd_gpu();
    if (strcmp(mod, "all") == 0 || strcmp(mod, "sysinfo") == 0) cmd_sysinfo();
    if (strcmp(mod, "all") == 0 || strcmp(mod, "input") == 0)   cmd_input();
    if (strcmp(mod, "keylog") == 0)                             cmd_keylog();
    if (strcmp(mod, "all") == 0 || strcmp(mod, "display") == 0) cmd_display();
    if (strcmp(mod, "all") == 0 || strcmp(mod, "video") == 0)  cmd_video();
    if (strcmp(mod, "all") == 0 || strcmp(mod, "audio") == 0)   cmd_audio();
    if (strcmp(mod, "all") == 0 || strcmp(mod, "cores") == 0)   cmd_cores();
    if (strcmp(mod, "all") == 0 || strcmp(mod, "sysdeep") == 0)         cmd_sysdeep();
    if (strcmp(mod, "monitor") == 0)                                    cmd_monitor(argc, argv);
    if (g_out) { logf("# diag finished OK\n"); fclose(g_out); }
    logf("REPORT -> %s\n", REPORT);
    return 0;
}
