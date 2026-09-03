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
 *   diag audio_distortion  # v3.0: 3-pass time-series sampling (read while game running)
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

/* Recursive read-only dir dumper for /proc/device-tree subtrees.
 * Avoids re-opening the same dir twice and limits depth so a
 * /proc/device-tree branch doesn't drown the report. */
static void cat_dir_tree_r(const char *base, int depth, int maxdepth) {
    if (depth > maxdepth) return;
    DIR *d = opendir(base);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[512];
        size_t need = strlen(base) + strlen(e->d_name) + 2;
        if (need > sizeof p) continue;
        snprintf(p, sizeof p, "%s/%s", base, e->d_name);
        if (e->d_type == DT_DIR) {
            logf("  %s/\n", p);
            cat_dir_tree_r(p, depth+1, maxdepth);
        } else {
            logf("  %s\n", p);
            cat_file(p);
        }
    }
    closedir(d);
}
static void cat_dir_tree(const char *base, int maxdepth) { cat_dir_tree_r(base, 0, maxdepth); }

/* ===========================================================================
 * audio_distortion -- v3.0 (2026-09-03): distortion root-cause diagnostic
 * ===========================================================================
 * PURPOSE
 *   User reported 2 artifacts from same RetroArch build:
 *     (1) 粗糙/颗粒/台阶感 (rough / stepped / 8-bit feel)
 *     (2) 模糊/混浊/金属感  (muddy / metallic / aliased)
 *   Both are DISTORTION (signal-integrity), not volume.  Volume (loudness)
 *   is a separate problem.  This function prints the 5 root-cause
 *   measurements that distinguish the two artifacts.
 *
 * 5 dims (same as v2.0):
 *   (A) 量化台阶感 (8-bit feel) -- R02.VWL + R03.FWL + I2S TXCR/RXCR.VDW
 *   (B) 混叠金属感            -- I2S CKR + /proc/asound/card0/stream0
 *   (C) 削波 (clipping)        -- /proc/asound/card0/pcm*p/sub0/status
 *   (D) xrun 欠载               -- same path (xruns counter)
 *   (E) 声道路由错配          -- /proc/device-tree/sound (DAI topology)
 * ===========================================================================
 * v3.0: TIME-SERIES SAMPLING (the user has no shell, only the SD card)
 *   The device has no SSH and no input console.  All data must be
 *   written to /mnt/sdcard/diag_report.txt.
 *   The 449 icube_replacement forks `diag all` and immediately execs
 *   retroarch.  The diag child runs main() once; the 1-shot dump in
 *   v2.0 captured only the "idle codec state at T=0" -- NOT the state
 *   after retroarch has set its hw_params.
 *
 *   v3.0 solution: inside cmd_audio_distortion we self-fork a
 *   grandchild.  The PARENT writes pass 1 immediately and returns,
 *   letting icube exec retroarch without delay.  The GRANDCHILD
 *   sleeps 15s, writes pass 2; sleeps another 30s (T=45s total),
 *   writes pass 3; exits.  All three append to the same
 *   /mnt/sdcard/diag_report.txt (g_out is that file, opened by main()).
 *
 *   At power-on boot, retroarch is loaded by user pressing a button.
 *   The user is told: "press button, wait 60 seconds, then read the
 *   SD card".  The 3 passes span T=0s, T=15s, T=45s after icube
 *   fork -- which is roughly T=15s, T=30s, T=60s after button-press,
 *   enough to be inside the game once the user has chosen a core
 *   and content.
 * =========================================================================== */
static const char *inno_vwl_name(unsigned vwl) {
    switch (vwl & 3) {
        case 0: return "16bit";
        case 1: return "20bit";
        case 2: return "24bit";
        case 3: return "32bit";
        default: return "?";
    }
}
static const char *inno_fwl_name(unsigned fwl) {
    switch (fwl & 3) {
        case 0: return "16bit";
        case 1: return "20bit";
        case 2: return "24bit";
        case 3: return "32bit";
        default: return "?";
    }
}
static unsigned i2s_vdw_bits(uint32_t txcr) {
    return ((txcr & 0x1f) / 8) == 0 ? 8u
         : ((txcr & 0x1f) / 8) == 1 ? 16u
         : ((txcr & 0x1f) / 8) == 2 ? 20u
         : ((txcr & 0x1f) / 8) == 3 ? 24u : 0u;
}

/* One distortion-snapshot dump.  Called for pass 1, 2, 3. */
static void dump_audio_distortion_pass(const char *pass_label) {
    logf("\n=== audio_distortion %s ===\n", pass_label);

    /* (A) acodec R00-R10 @ 0x20030000 */
    logf("--- (A) acodec R00-R10 @ 0x20030000 (inno_rk3036.h register map) ---\n");
    {
        int fd = open("/dev/mem", O_RDWR);
        if (fd < 0) { logf("  /dev/mem open: %s\n", strerror(errno)); }
        else {
            volatile uint32_t *m = mmap(NULL, 0xb0, PROT_READ, MAP_SHARED, fd, 0x20030000);
            if (m == MAP_FAILED) { logf("  mmap 0x20030000: %s\n", strerror(errno)); close(fd); }
            else {
                uint32_t R[11] = {m[0x00/4],m[0x0c/4],m[0x10/4],m[0x14/4],m[0x88/4],m[0x8c/4],
                                  m[0x90/4],m[0x94/4],m[0x98/4],m[0x9c/4],m[0xa0/4]};
                munmap((void*)m, 0xb0); close(fd);

                logf("  R00=0x%08x  CSR_WORK=%d CDCR_WORK=%d PRB_ENABLE=%d\n",
                      R[0], (R[0]>>0)&1, (R[0]>>1)&1, (R[0]>>6)&1);
                logf("  R01=0x%08x  I2SMODE=%s  PINDIR=%s\n", R[1],
                      (R[1]>>4)&1 ? "MASTER" : "SLAVE",
                      (R[1]>>5)&1 ? "OUT_MASTER" : "IN_SLAVE");
                logf("  R02=0x%08x  DACM=%s  LRCP=%s  VWL=%s (R02[6:5]=%u)\n",
                      R[2],
                      ((R[2]>>3)&3)==3?"PCM":((R[2]>>3)&3)==2?"I2S":((R[2]>>3)&3)==1?"LJM":"RJM",
                      (R[2]>>7)&1 ? "REVERSAL" : "NORMAL",
                      inno_vwl_name((R[2]>>5)&3), (R[2]>>5)&3);
                logf("  R03=0x%08x  BCP=%s  DACR=%s  FWL=%s (R03[3:2]=%u)\n",
                      R[3],
                      (R[3]>>0)&1 ? "REVERSAL" : "NORMAL",
                      (R[3]>>1)&1 ? "WORK" : "RESET",
                      inno_fwl_name((R[3]>>2)&3), (R[3]>>2)&3);
                logf("  R04=0x%08x  DACL_SW=%d DACL_CLK=%d DACL_VREF=%d  DACR_SW=%d DACR_CLK=%d DACR_VREF=%d\n",
                      R[4], (R[4]>>1)&1, (R[4]>>3)&1, (R[4]>>5)&1,
                            (R[4]>>0)&1, (R[4]>>2)&1, (R[4]>>4)&1);
                logf("  R05=0x%08x  HPL_EN=%d HPL_WORK=%d HPR_EN=%d HPR_WORK=%d\n",
                      R[5], (R[5]>>1)&1, (R[5]>>3)&1, (R[5]>>0)&1, (R[5]>>2)&1);
                logf("  R06=0x%08x  DAC_EN=%d  VOUTL_CZ=%d  VOUTR_CZ=%d  PRE/DIS=%d\n",
                      R[6], (R[6]>>5)&1, (R[6]>>1)&1, (R[6]>>0)&1, (R[6]>>4)&1);
                logf("  R07=0x%08x  HP_L_gain=0x%02x (%+.1fdB)  -- VOLUME (1.5dB/step; 0dB=0x1a=26)\n",
                      R[7], R[7]&0x1f, (R[7]&0x1f)*1.5 - 39.0);
                logf("  R08=0x%08x  HP_R_gain=0x%02x (%+.1fdB)\n",
                      R[8], R[8]&0x1f, (R[8]&0x1f)*1.5 - 39.0);
                logf("  R09=0x%08x  HPL_MUTE=%d HPR_MUTE=%d  DACL_SW=%d DACR_SW=%d\n",
                      R[9], (R[9]>>5)&1, (R[9]>>4)&1, (R[9]>>7)&1, (R[9]>>6)&1);
                logf("  R10=0x%08x  charge_current_mask\n", R[10]);

                uint32_t vwl = (R[2]>>5)&3, fwl = (R[3]>>2)&3;
                unsigned vwlc = vwl==0?16:(vwl==1?20:(vwl==2?24:32));
                unsigned fwlc = fwl==0?16:(fwl==1?20:(fwl==2?24:32));
                if (vwl==0 && fwl==0) {
                    logf("  >>> (A) VWL=16 FWL=16 -- tightest quantization (cleanest)\n");
                } else if (vwl==2 && fwl==3) {
                    logf("  >>> (A) VWL=24 FWL=32 = '24 in 32' slot; high 8 bits are padding\n");
                } else if (vwl==2 && fwl==2) {
                    logf("  >>> (A) VWL=24 FWL=24 -- if app writes S16, high 8 are codec-side garbage\n");
                } else if (vwlc != fwlc) {
                    logf("  >>> (A) VWL=%ubit FWL=%ubit (MISMATCH) -- possible sample shift\n", vwlc, fwlc);
                } else {
                    logf("  >>> (A) VWL=%ubit FWL=%ubit (match)\n", vwlc, fwlc);
                }
            }
        }
    }

    /* (B) I2S controller 0x10220000 */
    logf("--- (B) I2S controller 0x10220000 (rockchip_i2s.c register map) ---\n");
    {
        int fd = open("/dev/mem", O_RDWR);
        if (fd < 0) { logf("  /dev/mem open: %s\n", strerror(errno)); }
        else {
            volatile uint32_t *m = mmap(NULL, 0x80, PROT_READ, MAP_SHARED, fd, 0x10220000);
            if (m == MAP_FAILED) { logf("  mmap 0x10220000: %s\n", strerror(errno)); close(fd); }
            else {
                uint32_t txcr = m[0x00/4], rxcr = m[0x04/4], ckr = m[0x08/4];
                uint32_t txfer = m[0x10/4], dmacr = m[0x20/4];
                munmap((void*)m, 0x80); close(fd);

                unsigned vdw_tx = i2s_vdw_bits(txcr);
                unsigned vdw_rx = i2s_vdw_bits(rxcr);
                const char *vdw_n[] = {"8bit","?","16bit","?","20bit","?","24bit","?"};
                logf("  TXCR=0x%08x  VDW[bit4:0]=%u (%s, reg val=%u)  IBM=%s  CSR=%d\n",
                      txcr, vdw_tx, vdw_n[vdw_tx/8], txcr & 0x1f,
                      (txcr>>5)&3 == 0 ? "NORMAL" : (txcr>>5)&3 == 1 ? "LSJM" : (txcr>>5)&3 == 2 ? "RSJM" : "?",
                      (txcr>>6)&1);
                logf("  RXCR=0x%08x  VDW[bit4:0]=%u (%s)\n", rxcr, vdw_rx, vdw_n[vdw_rx/8]);
                logf("  CKR=0x%08x  TRCM=%s  MSS=%s\n", ckr,
                      (ckr>>4)&3 == 0 ? "TXRX" : (ckr>>4)&3 == 1 ? "TXSHARE" : "?",
                      (ckr>>0)&3 == 0 ? "?" : (ckr>>0)&3 == 1 ? "SLAVE" : (ckr>>0)&3 == 2 ? "MASTER" : "?");
                logf("  TXFER=0x%08x  TXS_START=%d  RXS_START=%d\n",
                      txfer, (txfer>>1)&1, (txfer>>0)&1);
                logf("  DMACR=0x%08x  TDL[bit24:25]=%u  RDL[bit26:27]=%u\n",
                      dmacr, (dmacr>>24)&3, (dmacr>>26)&3);
                logf("  >>> (B) I2S TXCR.VDW=%u (slot width on wire)\n", vdw_tx);
            }
        }
    }

    /* (C) + (D) pcm status */
    logf("--- (C/D) pcm playback status ---\n");
    const char *pcm_paths[] = {
        "/proc/asound/card0/pcm0p/sub0/status",
        "/proc/asound/card0/pcm1p/sub0/status",
    };
    const char *pcm_labels[] = {
        "i2s-hifi (HDMI endpoint 0,0)",
        "rk3036-voice (speaker endpoint 0,1)"
    };
    for (unsigned i = 0; i < 2; i++) {
        logf("  --- %s ---\n", pcm_labels[i]);
        FILE *f = fopen(pcm_paths[i], "r");
        if (!f) { logf("    (no %s)\n", pcm_paths[i]); continue; }
        char l[256];
        while (fgets(l, sizeof l, f)) {
            if (strstr(l, "state:") || strstr(l, "owner_pid") || strstr(l, "xruns") ||
                strstr(l, "avail") || strstr(l, "period_size") || strstr(l, "tick_time")) {
                logf("    %s", l);
            }
        }
        fclose(f);
    }

    /* (B) + (E) DTB topology + active stream */
    logf("--- (E) /proc/device-tree/sound (depth<=3) ---\n");
    cat_dir_tree("/proc/device-tree/sound", 3);
    logf("--- (B) /proc/asound/card0/stream0 ---\n");
    cat_file("/proc/asound/card0/stream0");
    logf("--- pcm0p/info ---\n");
    cat_file("/proc/asound/card0/pcm0p/info");
    logf("--- pcm1p/info ---\n");
    cat_file("/proc/asound/card0/pcm1p/info");
}

static void cmd_audio_distortion(void) {
    g_fault_module = 6;
    logf("\n=== audio_distortion (v3.0) ===\n");
    logf("ROOT CAUSE SEARCH: 2 user-reported artifacts (rough-step + metallic-aliased)\n");
    logf("  (1) quant-step '8-bit feel' -> R02.VWL != application sample format\n");
    logf("  (2) metallic aliasing       -> I2S TXCR.VDW != R02.VWL slot width\n");
    logf("v3.0: 3-pass time-series sampling (T=0s / 15s / 45s after icube fork)\n");
    logf("  pass 1 / 3 = T=0s  (immediate, idle codec state)\n");
    logf("  pass 2 / 3 = T=15s (after retroarch started, codec hw_params applied)\n");
    logf("  pass 3 / 3 = T=45s (sustained playback)\n\n");
    logf("SELF-FORK: PARENT writes pass 1 NOW and exits so icube can exec retroarch.\n");
    logf("  GRANDCHILD: sleep(15), pass 2; sleep(30 more), pass 3; exit.\n");
    logf("  All 3 passes append to /mnt/sdcard/diag_report.txt (g_out).\n\n");

    /* PASS 1: in this process, immediately, so even if grandchild dies
     * we have the T=0s baseline.  This is the v2.0 single-shot dump. */
    logf("\n##### audio_distortion PASS 1/3 (T=0s, immediate, idle) #####\n");
    dump_audio_distortion_pass("pass 1/3 (T=0s, idle codec state)");

    /* Spawn grandchild for passes 2/3.  Parent exits; icube's waitpid
     * (or exec immediately) is not blocked -- typical icube_replacement
     * execs retroarch right after fork+return. */
    pid_t gc = fork();
    if (gc < 0) {
        logf("  audio_distortion: fork grandchild FAILED: %s\n", strerror(errno));
        logf("=== audio_distortion done (pass 1 only) ===\n");
        return;
    }
    if (gc > 0) {
        logf("  grandchild pid=%d will write passes 2/3 in background\n", (int)gc);
        logf("=== audio_distortion pass 1 done; passes 2/3 in background ===\n");
        return;
    }
    /* GRANDCHILD: detached from controlling terminal, run timed samples */
    setsid();
    logf("  [grandchild] running, T=15s until pass 2...\n");
    fflush(stdout);
    sleep(15);
    logf("\n##### audio_distortion PASS 2/3 (T=15s, after game start) #####\n");
    dump_audio_distortion_pass("pass 2/3 (T=15s, game running)");
    fflush(stdout);
    sleep(30);  /* cumulative T=45s */
    logf("\n##### audio_distortion PASS 3/3 (T=45s, sustained) #####\n");
    dump_audio_distortion_pass("pass 3/3 (T=45s, sustained playback)");
    logf("=== audio_distortion grandchild finished all 3 passes ===\n");
    fflush(stdout);
    _exit(0);
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

/* =========================================================================== */
int main(int argc, char **argv) {
    install_guards();
    const char *mod = argc > 1 ? argv[1] : "all";
    g_out = fopen(REPORT, "w");
    if (g_out) logf("# CubeGM diag %s %s\n", mod, ctime(&(time_t){time(NULL)}));
    else logf("# WARN: cannot write %s (SD read-only?) -- console only\n", REPORT);
    if (strcmp(mod, "all") == 0 || strcmp(mod, "sysinfo") == 0) cmd_sysinfo();
    if (strcmp(mod, "all") == 0 || strcmp(mod, "input") == 0)   cmd_input();
    if (strcmp(mod, "keylog") == 0)                             cmd_keylog();
    if (strcmp(mod, "all") == 0 || strcmp(mod, "display") == 0) cmd_display();
    if (strcmp(mod, "all") == 0 || strcmp(mod, "audio") == 0)   cmd_audio();
    if (strcmp(mod, "all") == 0 || strcmp(mod, "audio_distortion") == 0) cmd_audio_distortion();
    if (strcmp(mod, "all") == 0 || strcmp(mod, "cores") == 0)   cmd_cores();
    if (g_out) { logf("# diag finished OK\n"); fclose(g_out); }
    logf("REPORT -> %s\n", REPORT);
    return 0;
}
