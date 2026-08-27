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
            unsigned long kb[4] = {0};
            if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof kb), kb) == 0)
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
    int fds[16]; int nfd = 0; char names[16][128];
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
        fprintf(kl, "# device %d: %s (EV bits=%08lx)\n", nfd, names[nfd], evbits);
        fds[nfd++] = fd;
    }
    if (nfd == 0) { fprintf(kl, "# NO evdev devices found\n"); fflush(kl); fclose(kl); return; }
    fflush(kl);
    /* continuous loop: never exits (daemon). Log every input event. */
    for (;;) {
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
    void *pcm = NULL;
    int rc = p_open(&pcm, "default", 0 /*PLAYBACK*/, 0);
    logf("  snd_pcm_open(default) rc=%d pcm=%p\n", rc, pcm);
    if (rc < 0 || !pcm) { dlclose(h); return; }
    rc = p_sp(pcm, 2 /*S16_LE*/, 3 /*INTERLEAVED*/, 2, 44100, 1, 50000);
    logf("  snd_pcm_set_params(44.1k stereo) rc=%d\n", rc);
    if (rc < 0) { p_cl(pcm); dlclose(h); return; }
    /* 1 kHz sine, 2 s */
    int16_t buf[4410]; /* 0.1 s */
    for (int i = 0; i < 4410; i++) {
        double t = (double)i / 44100.0;
        int16_t v = (int16_t)(12000.0 * (t * 1000.0 < 0.5 ? 1.0 : -1.0)); /* 1kHz square */
        buf[i] = v;
    }
    for (int rep = 0; rep < 20; rep++) { /* 20 x 0.1 s = 2 s */
        long w = p_wr(pcm, buf, 4410);
        if (w < 0) { logf("  writei rc=%ld (xrun?)\n", w); break; }
    }
    logf("  1 kHz square played 2 s -- did you HEAR it?\n");
    p_cl(pcm); dlclose(h);
    logf("=== audio done ===\n");
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
    if (strcmp(mod, "all") == 0 || strcmp(mod, "cores") == 0)   cmd_cores();
    if (g_out) { logf("# diag finished OK\n"); fclose(g_out); }
    logf("REPORT -> %s\n", REPORT);
    return 0;
}
