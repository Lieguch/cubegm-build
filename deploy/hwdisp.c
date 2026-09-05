/* sf3000-hwdisp implementation. dlopen's driver.so, calls video_driver_*.
 *
 * Driver scales src dims → panel (854x480) via HCGE DMA with bilinear filter.
 * Two filter modes:
 *   - HW (default): pass src as-is, driver scales (bilinear).
 *   - Nearest: SW nearest-upscale src to driver native (1280x720) framing,
 *     so driver doesn't scale further → pixels stay sharp.
 *
 * Aspect-pad: when target aspect set, source is centered horizontally with
 * black pillar-bars so driver's stretch becomes uniform. */

#include "hwdisp.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <errno.h>
/* DRM/KMS UAPI (Linux 4.4, MIT-licensed; bundled in deploy/drm_headers/).
 * RK3036G HDMI output needs a DRM modeset: fb0 is NOT the HDMI source
 * (payload-238 device logs: HDMI-A-1 connected+enabled, fb0 name empty). */
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_fourcc.h>
extern void dbg_log(const char *fmt, ...);
#define DBG(...) dbg_log(__VA_ARGS__)

/* RK3036G observability: fsync'd file log to /mnt/sdcard/picoarch_init.log so
 * the black-screen case leaves evidence on the SD card even though stderr goes
 * nowhere visible on the device (no console). Same file/append style as the
 * SF3000 init log in plat_sdl.c. */
static void hwdisp_fslog(const char *fmt, ...) {
    int fd = open("/mnt/sdcard/picoarch_init.log", O_WRONLY|O_CREAT|O_APPEND, 0644);
    if (fd < 0) return;
    char buf[512]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (n > 0) (void)!write(fd, buf, (size_t)n);
    (void)!dprintf(fd, "\n");
    fsync(fd); close(fd);
}

#ifndef HW_NATIVE_W
#define HW_NATIVE_W 1280
#endif
#ifndef HW_NATIVE_H
#define HW_NATIVE_H  720
#endif
#define HW_W   HW_NATIVE_W
#define HW_H   HW_NATIVE_H
#define HW_PITCH (HW_W * 2)
#define HW_BUFSZ (HW_W * HW_H * 2)

static void   *g_handle = NULL;
static int     g_active = 0;

/* RK3036G (and any device without a working driver.so / /dev/dis): present
 * straight to /dev/fb0 via hwdisp_fb_present(), bypassing driver.so disp_frame. */
static int     g_fb_only = 0;

typedef int  (*fn_init_t)(void);
typedef void (*fn_deinit_t)(void);
typedef int  (*fn_disp_t)(void *src, int w, int h, int pitch);
typedef void (*fn_aspect_t)(int fullscreen);
typedef void (*fn_enhance_t)(int p0, int p1, int p2, int p3, int p4);

static fn_init_t   p_init   = NULL;
static fn_deinit_t p_deinit = NULL;
static fn_disp_t   p_disp   = NULL;
/* Driver's fullscreen toggle (exported fbdev_video_aspect_ratio): arg 1 =
 * fullscreen fill (the GE stretches src→panel ignoring aspect), arg 0 =
 * aspect-fit (letterbox). This is how stock does distort-to-fill in pure HW. */
static fn_aspect_t p_aspect = NULL;
static int         g_fs_state = -1;   /* last value pushed, avoid redundant calls */

/* Display-controller enhance/sharpness probe (exported fbdev_set_enhance):
 * 5 params, first four 0-100, fifth 0-10 (a mode-like selector). Sweep them
 * on-device by editing /mnt/sdcard/enhance.txt (5 ints) — read at init, so a
 * game relaunch applies new values with no rebuild. Testing whether any combo
 * sharpens the GE's bilinear scale toward nearest. */
static fn_enhance_t p_enhance = NULL;
static int          g_sharpen = -1;   /* last sharpness pushed (0-10); -1 = unset */

/* Edge-sharpen level 0-10 (p4 of fbdev_set_enhance; colours left neutral at 50).
 * Whole-panel DIS post-process — the closest the HW gets to "nearest" (edge
 * peaking, not true point sampling). Applied on change only (it's an ioctl). */
void hwdisp_set_sharpen(int level) {
    if (level < 0) level = 0;
    if (level > 10) level = 10;
    if (!p_enhance || level == g_sharpen) return;
    p_enhance(50, 50, 50, 50, level);
    g_sharpen = level;
}

/* Aspect-pad staging buffer (lazy alloc, resized on demand) */
static uint16_t *g_pad_buf  = NULL;
static int       g_pad_cap  = 0;
static int       g_pad_w    = 0;
static int       g_pad_h    = 0;

/* Nearest-upscale buffer (always 1280x720) */
static uint16_t *g_near_buf = NULL;

static int g_aspect_num = 0;
static int g_aspect_den = 0;
static int g_filter_nearest = 0;   /* SW-scale path (true nearest OR sharp) */
static int g_filter_sharp   = 0;   /* sharp variant: integer prescale + HW residual */

/* Direct-fb present: after video_drivers_init the driver reconfigures fb0 to its
 * native landscape geometry (R36SX: 1280x720 RGB565) and programs the display
 * controller to scale fb0 → physical panel (640x480), rotate:0. The panel scans
 * fb0 continuously, so we present by writing RGB565 straight into fb0 — no
 * video_driver_disp_frame (which hard-hangs on the engine sync on this driver). */
static int       g_fbfd    = -1;
static uint16_t *g_fbmem   = NULL;
static uint32_t *g_fbmem32 = NULL;   /* set when fb0 is 32-bit */
static int       g_fbw     = 0;   /* fb visible width  (px) */
static int       g_fbh     = 0;   /* fb visible height (px) */
static int       g_fbstride= 0;   /* fb row stride     (px) */
static int       g_fbbpp   = 0;   /* fb0 pixel depth (16=RGB565, 32=ARGB...) */
static long      g_fbsize  = 0;   /* mmap length (bytes) */

/* ---- RK3036G DRM/KMS path (HDMI modeset; fb0 is not the HDMI source) ---- */
static int       g_drm_mode = 0;  /* present via dumb-buffer plane instead of fb0 */
static int       g_drm_fd   = -1;
static uint32_t  g_drm_fb_id = 0, g_drm_crtc_id = 0, g_drm_conn_id = 0;
static int       g_drm_w = 0, g_drm_h = 0, g_drm_pitch = 0;
static uint8_t  *g_drm_map = NULL;
static size_t    g_drm_size = 0;
static int       g_drm_bpp = 0;   /* dumb-buffer pixel depth: 16=RGB565, 32=XRGB8888 */
static struct drm_mode_modeinfo g_drm_modeinfo;  /* kept for hwdisp_restore() */

/* RGB565→ARGB8888 lookup table (64KB). Built lazily; hwdisp_cvt565() falls
 * back to the arithmetic path before it is ready. Eliminates 10 shift/or ops
 * per converted pixel in the 32-bpp present path. */
static uint32_t g_cvt_lut[65536];
static int      g_cvt_lut_ready = 0;
static void hwdisp_cvt_lut_init(void) {
    if (g_cvt_lut_ready) return;
    for (int i = 0; i < 65536; i++)
        g_cvt_lut[i] = 0xFF000000u |
            (((i & 0xF800) << 8) | ((i & 0xE000) << 3)) |
            (((i & 0x07E0) << 5) | ((i & 0x0600) >> 1)) |
            (((i & 0x001F) << 3) | ((i & 0x001C) >> 2));
    g_cvt_lut_ready = 1;
}

/* hwdisp_fb_present() (below) calls hwdisp_drm_present() — declare it here so
 * the call is NOT an implicit (non-static) declaration. */
static void hwdisp_drm_present(const void *src, int w, int h, int pitch_bytes);

static void hwdisp_fb_open(void) {
    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    g_fbfd = open("/dev/fb0", O_RDWR | O_CLOEXEC);   /* v8.7: CLOEXEC (see hwdisp_drm_init) */
    hwdisp_fslog("hwdisp_fb_open: open /dev/fb0 -> fd=%d errno=%d", g_fbfd, errno);
    if (g_fbfd < 0) { DBG("DBG fbwrite: open fb0 failed\n"); return; }
    if (ioctl(g_fbfd, FBIOGET_VSCREENINFO, &vi) < 0 ||
        ioctl(g_fbfd, FBIOGET_FSCREENINFO, &fi) < 0) {
        hwdisp_fslog("hwdisp_fb_open: ioctl GET info FAILED errno=%d", errno);
        DBG("DBG fbwrite: ioctl GET info failed\n");
        close(g_fbfd); g_fbfd = -1; return;
    }
    hwdisp_fslog("hwdisp_fb_open: vinfo %dx%d(v%dx%d) bpp=%d rotate=%d | fixed line_length=%d smem_len=%ld",
                 vi.xres, vi.yres, vi.xres_virtual, vi.yres_virtual,
                 vi.bits_per_pixel, vi.rotate, fi.line_length, fi.smem_len);
    g_fbw      = vi.xres;
    g_fbh      = vi.yres;
    g_fbbpp    = vi.bits_per_pixel;
    int bpp_bytes = (g_fbbpp >= 8) ? (g_fbbpp / 8) : 2;
    g_fbstride = fi.line_length / bpp_bytes;   /* bytes → px */
    g_fbsize   = fi.smem_len;
    g_fbmem = (uint16_t *)mmap(NULL, g_fbsize, PROT_READ|PROT_WRITE, MAP_SHARED, g_fbfd, 0);
    if (g_fbmem == MAP_FAILED) {
        hwdisp_fslog("hwdisp_fb_open: mmap FAILED errno=%d size=%ld", errno, g_fbsize);
        DBG("DBG fbwrite: mmap failed\n");
        g_fbmem = NULL; close(g_fbfd); g_fbfd = -1; return;
    }
    g_fbmem32  = (g_fbbpp == 32) ? (uint32_t *)g_fbmem : NULL;
    /* RK3036G: we killed rkgame which normally drives blank/unblank — make
     * sure the panel is unblanked so our writes are visible. Harmless on
     * devices that don't need it. */
    ioctl(g_fbfd, FBIOBLANK, 1);
    ioctl(g_fbfd, FBIOBLANK, 0);
    /* DRM/KMS probe: if fb0 is only a legacy emulation and HDMI lives on
     * /dev/dri/card0, we may need a modeset there instead. Log what exists. */
    hwdisp_fslog("hwdisp_fb_open: /dev/dri/card0 access=%d | /dev/fb0 access=%d",
                 access("/dev/dri/card0", R_OK) == 0, access("/dev/fb0", R_OK) == 0);
    hwdisp_fslog("hwdisp_fb_open: OK fb0=%dx%d stride=%dpx bpp=%d size=%ld mmap=%p",
                 g_fbw, g_fbh, g_fbstride, g_fbbpp, g_fbsize, (void*)g_fbmem);
}

/* Write src(w×h RGB565) directly to fb0, nearest-scaled to fill it.
 *   R36SX: landscape fb, rotate:0 → straight scale.
 *   SF3000: panel is 480x854 portrait-mounted; the driver's disp_frame normally
 *           rotates 90°, but we bypass it, so rotate the frame 90° CW here.
 * Returns 1 if drawn. */
/* RGB565 → ARGB8888 (for fb0 configured as 32-bit). Alpha fixed opaque.
 * Uses the 64KB LUT when ready (fast path); falls back to arithmetic. */
static inline uint32_t hwdisp_cvt565(uint16_t c) {
    if (g_cvt_lut_ready) return g_cvt_lut[c];
    return 0xFF000000u |
           (((c & 0xF800) << 8) | ((c & 0xE000) << 3)) |
           (((c & 0x07E0) << 5) | ((c & 0x0600) >> 1)) |
           (((c & 0x001F) << 3) | ((c & 0x001C) >> 2));
}

/* Write one RGB565 pixel into fb0 at (dx,dy), honoring the fb0 pixel depth. */
static inline void hwdisp_fb_put(int dx, int dy, uint16_t px) {
    size_t off = (size_t)dy * g_fbstride + dx;
    if (g_fbbpp == 32 && g_fbmem32) g_fbmem32[off] = hwdisp_cvt565(px);
    else                            g_fbmem[off]   = px;
}

static int hwdisp_fb_present(const void *src, int w, int h, int pitch_bytes) {
    static int s_n = 0; int lg = (s_n < 3); s_n++;
    if (lg) hwdisp_fslog("hwdisp_fb_present#%d: src=%dx%d pitch=%d -> fb0=%dx%d stride=%d bpp=%d drm=%d",
                         s_n, w, h, pitch_bytes, g_fbw, g_fbh, g_fbstride, g_fbbpp, g_drm_mode);
    if (g_drm_mode) { hwdisp_drm_present(src, w, h, pitch_bytes); return 1; }
    if (!g_fbmem || g_fbw <= 0 || g_fbh <= 0 || w <= 0 || h <= 0) return 0;
    extern int sf3000_is_r36sx(void);
    extern int sf3000_is_rk3036(void);
    const int sp = pitch_bytes / 2;
    const uint16_t *s = (const uint16_t *)src;
    int draw_w = g_fbw < 2048 ? g_fbw : 2048;

    if (!(sf3000_is_r36sx() || sf3000_is_rk3036())) {
        /* SF3000: 90° CW rotate. fb row → src x; fb col(reversed) → src y. */
        static int symap[2048];
        static int last_h = -1, last_fbw = -1;
        if (h != last_h || g_fbw != last_fbw) {
            for (int dx = 0; dx < draw_w; dx++) symap[dx] = (h - 1) - (dx * h / g_fbw);
            last_h = h; last_fbw = g_fbw;
        }
        for (int dy = 0; dy < g_fbh; dy++) {
            int sx = dy * w / g_fbh;
            for (int dx = 0; dx < draw_w; dx++)
                hwdisp_fb_put(dx, dy, s[(size_t)symap[dx] * sp + sx]);
        }
        return 1;
    }

    /* R36SX / RK3036G: landscape fb, rotate:0 → straight scale. */
    static int xmap[2048];
    static int last_w = -1, last_fbw = -1;
    if (w != last_w || g_fbw != last_fbw) {
        for (int dx = 0; dx < draw_w; dx++) xmap[dx] = dx * w / g_fbw;
        last_w = w; last_fbw = g_fbw;
    }
    for (int dy = 0; dy < g_fbh; dy++) {
        int sy = dy * h / g_fbh;
        const uint16_t *srow = s + sy * sp;
        for (int dx = 0; dx < draw_w; dx++)
            hwdisp_fb_put(dx, dy, srow[xmap[dx]]);
    }
    return 1;
}

/* ---- RK3036G DRM/KMS modeset (standard DRM UAPI via ioctl, no libdrm) ----
 * Sets the HDMI-A connector to a mode, creates a 1280x720 XRGB8888 dumb
 * buffer, mmaps it, ADDFB2 + SETCRTC -> HDMI shows the dumb buffer. */
/* Linux-4.4 UAPI lacks the DRM_MODE_CONNECTED enum constant (added later). */
#define DRM_MODE_CONNECTED 1
static int hwdisp_drm_init(void) {
    uint32_t *fbs = NULL, *crtcs = NULL, *encs = NULL, *conns = NULL;
    hwdisp_cvt_lut_init();   /* lazy 64KB RGB565→ARGB LUT (32bpp fallback path) */
    /* v8.7: O_CLOEXEC. The FrogUI launcher forks+execs the game child; without
     * CLOEXEC the child inherits this card0 fd and closes it on exit, which
     * invalidates the parent's fd — the parent's hwdisp_restore() then fails
     * and the screen stays garbled ("half white" after ~10 min of game
     * enter/exit cycles). The child re-opens card0 itself. */
    g_drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (g_drm_fd < 0) {
        hwdisp_fslog("hwdisp_drm: open /dev/dri/card0 FAILED errno=%d", errno);
        return -1;
    }
    struct drm_mode_card_res res;
    memset(&res, 0, sizeof res);
    if (ioctl(g_drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        hwdisp_fslog("hwdisp_drm: GETRESOURCES(1) failed errno=%d", errno); goto fail;
    }
    /* Official two-pass protocol (kernel drm_mode_getresources): pass 1 returns
     * counts; pass 2 copies IDs into the arrays. ALL FOUR pointers must be
     * valid — the kernel put_user()s into every one whose count is >= actual
     * (a NULL fb_id_ptr/encoder_id_ptr with non-zero count_fbs/count_encoders
     * is EFAULT, seen as GETRESOURCES(2) failed errno=14 on the device). */
    fbs   = calloc(res.count_fbs ? res.count_fbs : 1, sizeof(uint32_t));
    crtcs = calloc(res.count_crtcs ? res.count_crtcs : 1, sizeof(uint32_t));
    encs  = calloc(res.count_encoders ? res.count_encoders : 1, sizeof(uint32_t));
    conns = calloc(res.count_connectors ? res.count_connectors : 1, sizeof(uint32_t));
    if (!fbs || !crtcs || !encs || !conns) { hwdisp_fslog("hwdisp_drm: calloc failed"); goto fail; }
    res.fb_id_ptr = (uintptr_t)fbs;
    res.crtc_id_ptr = (uintptr_t)crtcs;
    res.encoder_id_ptr = (uintptr_t)encs;
    res.connector_id_ptr = (uintptr_t)conns;
    /* counts stay as returned by pass 1 (kernel copies when count >= actual) */
    if (ioctl(g_drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        hwdisp_fslog("hwdisp_drm: GETRESOURCES(2) failed errno=%d", errno); goto fail;
    }
    hwdisp_fslog("hwdisp_drm: card0 conn=%u crtc=%u enc=%u fb=%u",
                 res.count_connectors, res.count_crtcs, res.count_encoders, res.count_fbs);

    struct drm_mode_modeinfo chosen;
    memset(&chosen, 0, sizeof chosen);
    int chosen_found = 0;
    for (uint32_t i = 0; i < res.count_connectors; i++) {
        struct drm_mode_get_connector conn;
        memset(&conn, 0, sizeof conn);
        conn.connector_id = conns[i];
        if (ioctl(g_drm_fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) continue;
        int nmodes = (int)conn.count_modes, nprops = (int)conn.count_props, nencs = (int)conn.count_encoders;
        if (nmodes == 0) continue;
        /* pass 2 (libdrm protocol): fresh struct, restore counts, set ALL FOUR
         * pointers — the kernel copies props/encoders too (NULL ptr -> EFAULT). */
        struct drm_mode_modeinfo *mode_buf  = calloc((size_t)nmodes, sizeof(struct drm_mode_modeinfo));
        uint32_t *prop_ids   = calloc((size_t)(nprops ? nprops : 1), sizeof(uint32_t));
        uint64_t *prop_vals  = calloc((size_t)(nprops ? nprops : 1), sizeof(uint64_t));
        uint32_t *enc_ids    = calloc((size_t)(nencs ? nencs : 1), sizeof(uint32_t));
        if (!mode_buf || !prop_ids || !prop_vals || !enc_ids) {
            free(mode_buf); free(prop_ids); free(prop_vals); free(enc_ids);
            continue;
        }
        memset(&conn, 0, sizeof conn);
        conn.connector_id = conns[i];
        conn.count_modes = nmodes;
        conn.count_props = nprops;
        conn.count_encoders = nencs;
        conn.modes_ptr = (uintptr_t)mode_buf;
        conn.props_ptr = (uintptr_t)prop_ids;
        conn.prop_values_ptr = (uintptr_t)prop_vals;
        conn.encoders_ptr = (uintptr_t)enc_ids;
        if (ioctl(g_drm_fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
            free(mode_buf); free(prop_ids); free(prop_vals); free(enc_ids);
            continue;
        }
        hwdisp_fslog("hwdisp_drm: conn %u type=%d connected=%d modes=%u enc=%u",
                     conns[i], conn.connector_type, conn.connection,
                     conn.count_modes, conn.encoder_id);
        if (conn.connection == DRM_MODE_CONNECTED && conn.count_modes > 0) {
            int sel = 0;
            for (int m = 0; m < (int)conn.count_modes; m++) {
                struct drm_mode_modeinfo *mm = &mode_buf[m];
                if (mm->hdisplay == 1280 && mm->vdisplay == 720 && mm->vrefresh == 60) { sel = m; break; }
            }
            chosen = mode_buf[sel];
            chosen_found = 1;
            g_drm_conn_id = conns[i];
            hwdisp_fslog("hwdisp_drm: SELECT mode %dx%d@%d name='%s'",
                         chosen.hdisplay, chosen.vdisplay, chosen.vrefresh, chosen.name);
            free(mode_buf); free(prop_ids); free(prop_vals); free(enc_ids);
            break;
        }
        free(mode_buf); free(prop_ids); free(prop_vals); free(enc_ids);
    }
    if (!chosen_found) { hwdisp_fslog("hwdisp_drm: NO connected HDMI connector/mode"); goto fail; }

    /* CRTC: via connector's encoder, else first CRTC. */
    uint32_t crtc_id = 0;
    struct drm_mode_get_connector cq;
    memset(&cq, 0, sizeof cq);
    cq.connector_id = g_drm_conn_id;
    if (ioctl(g_drm_fd, DRM_IOCTL_MODE_GETCONNECTOR, &cq) == 0 && cq.encoder_id) {
        struct drm_mode_get_encoder enc;
        memset(&enc, 0, sizeof enc);
        enc.encoder_id = cq.encoder_id;
        if (ioctl(g_drm_fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0)
            crtc_id = enc.crtc_id;
    }
    if (!crtc_id && res.count_crtcs > 0) crtc_id = crtcs[0];
    if (!crtc_id) { hwdisp_fslog("hwdisp_drm: no CRTC found"); goto fail; }
    g_drm_crtc_id = crtc_id;
    hwdisp_fslog("hwdisp_drm: crtc=%u", crtc_id);

    struct drm_mode_create_dumb cd;
    memset(&cd, 0, sizeof cd);
    /* v8.5: try RGB565 first (halves the per-frame write volume vs XRGB8888 —
     * the present path is the dominant per-frame cost on this SoC and the
     * HDMI writes to the dumb buffer look uncached). Fall back to 32bpp if
     * the VOP rejects RGB565. */
    cd.width = 1280; cd.height = 720; cd.bpp = 16;
    if (ioctl(g_drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
        hwdisp_fslog("hwdisp_drm: CREATE_DUMB(16bpp) failed errno=%d, retry 32bpp", errno);
        memset(&cd, 0, sizeof cd);
        cd.width = 1280; cd.height = 720; cd.bpp = 32;
        if (ioctl(g_drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
            hwdisp_fslog("hwdisp_drm: CREATE_DUMB(32bpp) failed errno=%d", errno); goto fail;
        }
    }
    g_drm_bpp = (int)cd.bpp;
    g_drm_w = 1280; g_drm_h = 720; g_drm_pitch = (int)cd.pitch; g_drm_size = cd.size;
    hwdisp_fslog("hwdisp_drm: dumb %ux%u bpp=%u pitch=%u size=%llu",
                 cd.width, cd.height, cd.bpp, cd.pitch, (unsigned long long)cd.size);

    struct drm_mode_map_dumb md;
    memset(&md, 0, sizeof md);
    md.handle = cd.handle;
    if (ioctl(g_drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
        hwdisp_fslog("hwdisp_drm: MAP_DUMB failed errno=%d", errno); goto fail;
    }
    g_drm_map = mmap(NULL, g_drm_size, PROT_READ|PROT_WRITE, MAP_SHARED, g_drm_fd, md.offset);
    if (g_drm_map == MAP_FAILED) {
        g_drm_map = NULL;
        hwdisp_fslog("hwdisp_drm: mmap failed errno=%d", errno); goto fail;
    }

    struct drm_mode_fb_cmd2 fb;
    memset(&fb, 0, sizeof fb);
    fb.width = 1280; fb.height = 720;
    fb.pixel_format = (g_drm_bpp == 16) ? DRM_FORMAT_RGB565 : DRM_FORMAT_XRGB8888;
    fb.handles[0] = cd.handle;
    fb.pitches[0] = cd.pitch;
    if (ioctl(g_drm_fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0) {
        hwdisp_fslog("hwdisp_drm: ADDFB2 failed errno=%d", errno); goto fail;
    }
    g_drm_fb_id = fb.fb_id;
    g_drm_modeinfo = chosen;

    struct drm_mode_crtc cc;
    memset(&cc, 0, sizeof cc);
    cc.crtc_id = g_drm_crtc_id;
    cc.fb_id = g_drm_fb_id;
    cc.x = 0; cc.y = 0;
    cc.mode_valid = 1;
    cc.mode = chosen;
    cc.set_connectors_ptr = (uintptr_t)&g_drm_conn_id;  /* Linux-4.4 field name */
    cc.count_connectors = 1;
    if (ioctl(g_drm_fd, DRM_IOCTL_MODE_SETCRTC, &cc) < 0) {
        hwdisp_fslog("hwdisp_drm: SETCRTC failed errno=%d", errno); goto fail;
    }
    /* White test frame: if HDMI lights up white, the modeset path is live. */
    memset(g_drm_map, 0xFF, g_drm_size);
    hwdisp_fslog("hwdisp_drm: SETCRTC ok fb_id=%u -> HDMI %dx%d (white frame written)",
                 g_drm_fb_id, 1280, 720);
    free(fbs); free(crtcs); free(encs); free(conns);
    return 0;
fail:
    hwdisp_fslog("hwdisp_drm: INIT FAILED");
    /* v8.7: destroy the framebuffer object if it was created (leak guard —
     * every re-init used to ADD a new fb and the kernel fb pool is small). */
    if (g_drm_fb_id != 0 && g_drm_fd >= 0) {
        struct drm_mode_fb_cmd rm;
        memset(&rm, 0, sizeof rm);
        rm.fb_id = g_drm_fb_id;
        ioctl(g_drm_fd, DRM_IOCTL_MODE_RMFB, &rm);
        g_drm_fb_id = 0;
    }
    if (g_drm_map) { munmap(g_drm_map, g_drm_size); g_drm_map = NULL; }
    if (g_drm_fd >= 0) { close(g_drm_fd); g_drm_fd = -1; }
    if (fbs) free(fbs);
    if (crtcs) free(crtcs);
    if (encs) free(encs);
    if (conns) free(conns);
    return -1;
}

/* RGB565 src (w x h, pitch_bytes) -> dumb buffer (RGB565 or XRGB8888),
 * nearest scale, aspect-fit (black bars), 90° rotation for portrait sources.
 * v8.5: 16bpp dumb buffer halves the per-frame write volume (the HDMI write
 * to the dumb buffer is the dominant cost on this SoC); game frames are
 * aspect-fitted instead of stretched; rotation row-pointer bug fixed. */
static void hwdisp_drm_present(const void *src, int w, int h, int pitch_bytes) {
    static long t_last = 0, t_acc = 0; static int t_cnt = 0;
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    if (!g_drm_map || g_drm_w <= 0 || g_drm_h <= 0 || !src) return;
    if (w > 2048) w = 2048;      /* rowbuf/xmap bound (no core frame exceeds this) */
    const uint16_t *s = (const uint16_t *)src;
    const int sp = pitch_bytes / 2;
    const int DW = g_drm_w, DH = g_drm_h, DP = g_drm_pitch;
    uint8_t *base = g_drm_map;

    /* 1:1 fast path (FrogUI menu frames): straight copy, no scale lookups. */
    if (w == DW && h == DH) {
        if (g_drm_bpp == 16) {
            for (int y = 0; y < DH; y++) {
                uint16_t *drow = (uint16_t *)(base + (size_t)y * DP);
                const uint16_t *srow = s + (size_t)y * sp;
                memcpy(drow, srow, (size_t)DW * 2);
            }
        } else {
            for (int y = 0; y < DH; y++) {
                uint32_t *drow = (uint32_t *)(base + (size_t)y * DP);
                const uint16_t *srow = s + (size_t)y * sp;
                for (int x = 0; x < DW; x++)
                    drow[x] = hwdisp_cvt565(srow[x]);
            }
        }
        goto done;
    }

    /* Game frames: aspect-fit (no stretching) into the panel, black bars
     * baked. Geometry changes only when the src size flips, so the clear
     * (full-buffer memset) runs once per geometry, not per frame. */
    static int g_last_w = -1, g_last_h = -1, g_dw = 0, g_dh = 0, g_ox = 0, g_oy = 0;
    if (w != g_last_w || h != g_last_h) {
        g_last_w = w; g_last_h = h;
        double A = (double)w / (double)h;          /* src aspect */
        if (A > (double)DW / (double)DH) {          /* wider than panel */
            g_dw = DW; g_dh = (int)((double)DW / A + 0.5);
        } else {
            g_dh = DH; g_dw = (int)((double)DH * A + 0.5);
        }
        if (g_dw < 1) g_dw = 1; if (g_dh < 1) g_dh = 1;
        g_ox = (DW - g_dw) / 2; g_oy = (DH - g_dh) / 2;
        if (g_drm_bpp == 16) memset(base, 0, (size_t)DH * DP);
        else                memset(base, 0, (size_t)DH * DP);
        hwdisp_fslog("hwdisp_drm: aspect-fit %dx%d -> rect %dx%d+%d+%d (bpp=%d)",
                     w, h, g_dw, g_dh, g_ox, g_oy, g_drm_bpp);
    }

    if (w < h) {
        /* Portrait source (vertical arcade): rotate 90° CW into the fit rect.
         * dst(x,y) <- src(col = (g_dw-1-x)*w/g_dw, row = y*h/g_dh).
         * v8.7: precompute BOTH maps when the geometry changes — the old code
         * did two 64-bit mul+div per PIXEL per FRAME (1280x720 ≈ 0.9M px),
         * which pinned portrait shooters (1941: 224x384) at ~25 FPS. Table
         * lookup turns it into a straight gather. */
        static int v_xcol[1280]; static int v_xcol_w = -1, v_xcol_dw = -1;
        static int v_yrow[720];  static int v_yrow_h = -1, v_yrow_dh = -1;
        if (v_xcol_w != w || v_xcol_dw != g_dw) {
            for (int x = 0; x < g_dw; x++) {
                int sy = (g_dw > 1) ? (int)(((long long)(g_dw - 1 - x) * w) / g_dw) : 0;
                if (sy >= h) sy = h - 1;
                v_xcol[x] = sy;
            }
            v_xcol_w = w; v_xcol_dw = g_dw;
        }
        if (v_yrow_h != h || v_yrow_dh != g_dh) {
            for (int y = 0; y < g_dh; y++) {
                int sx = (h > 1) ? (int)(((long long)y * h) / g_dh) : 0;
                if (sx >= w) sx = w - 1;
                v_yrow[y] = sx;
            }
            v_yrow_h = h; v_yrow_dh = g_dh;
        }
        if (g_drm_bpp == 16) {
            for (int y = 0; y < g_dh; y++) {
                const uint16_t *scol = s + (size_t)v_yrow[y] * sp;
                uint16_t *drow = (uint16_t *)(base + (size_t)(g_oy + y) * DP) + g_ox;
                for (int x = 0; x < g_dw; x++)
                    drow[x] = scol[v_xcol[x]];
            }
        } else {
            for (int y = 0; y < g_dh; y++) {
                const uint16_t *scol = s + (size_t)v_yrow[y] * sp;
                uint32_t *drow = (uint32_t *)(base + (size_t)(g_oy + y) * DP) + g_ox;
                for (int x = 0; x < g_dw; x++)
                    drow[x] = hwdisp_cvt565(scol[v_xcol[x]]);
            }
        }
        goto done;
    }

    /* Landscape source: nearest scale into the fit rect. x/y maps rebuilt
     * only when the src size changes — removes all per-frame divisions. */
    static int xmap[2048]; static int xmap_w = -1;
    static int ymap[2048]; static int ymap_h = -1;
    if (xmap_w != w || ymap_h != h) {
        for (int x = 0; x < g_dw; x++) xmap[x] = (w > 1) ? (int)(((long long)x * w) / g_dw) : 0;
        for (int y = 0; y < g_dh; y++) ymap[y] = (h > 1) ? (int)(((long long)y * h) / g_dh) : 0;
        xmap_w = w; ymap_h = h;
    }
    if (g_drm_bpp == 16) {
        for (int y = 0; y < g_dh; y++) {
            const uint16_t *srow = s + (size_t)ymap[y] * sp;
            uint16_t *drow = (uint16_t *)(base + (size_t)(g_oy + y) * DP) + g_ox;
            for (int x = 0; x < g_dw; x++)
                drow[x] = srow[xmap[x]];
        }
    } else {
        static uint32_t rowbuf[2048];
        for (int y = 0; y < g_dh; y++) {
            const uint16_t *srow = s + (size_t)ymap[y] * sp;
            for (int sx = 0; sx < w; sx++)
                rowbuf[sx] = hwdisp_cvt565(srow[sx]);
            uint32_t *drow = (uint32_t *)(base + (size_t)(g_oy + y) * DP) + g_ox;
            for (int x = 0; x < g_dw; x++)
                drow[x] = rowbuf[xmap[x]];
        }
    }
done:
    /* per-frame present cost (diagnostic): cumulative avg every ~2s */
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    long us = (ts1.tv_sec - ts0.tv_sec) * 1000000L + (ts1.tv_nsec - ts0.tv_nsec) / 1000L;
    t_acc += us; t_cnt++;
    long now = ts1.tv_sec * 1000L + ts1.tv_nsec / 1000000L;
    if (t_last == 0) t_last = now;
    if (now - t_last >= 2000) {
        hwdisp_fslog("hwdisp_drm: present %d frames in %ld ms (avg %.2f ms, last %.2f ms)",
                     t_cnt, now - t_last, (double)t_acc / t_cnt / 1000.0, (double)us / 1000.0);
        t_acc = 0; t_cnt = 0; t_last = now;
    }
}

/* Re-assert our CRTC after a forked game child died (its exit released DRM
 * master and the kernel blanked the connector — "no signal" on HDMI). Called
 * by the front-end via dlsym after waitpid(). Exported (non-static) so
 * -rdynamic makes it visible to dlsym(RTLD_DEFAULT). */
int hwdisp_restore(void) {
    if (g_drm_fd < 0 || g_drm_fb_id == 0) return -1;
    struct drm_mode_crtc cc;
    memset(&cc, 0, sizeof cc);
    cc.crtc_id = g_drm_crtc_id;
    cc.fb_id = g_drm_fb_id;
    cc.x = 0; cc.y = 0;
    cc.mode_valid = 1;
    cc.mode = g_drm_modeinfo;
    cc.set_connectors_ptr = (uintptr_t)&g_drm_conn_id;
    cc.count_connectors = 1;
    int rc = ioctl(g_drm_fd, DRM_IOCTL_MODE_SETCRTC, &cc);
    hwdisp_fslog("hwdisp_restore: SETCRTC fb=%u rc=%d errno=%d", g_drm_fb_id, rc, errno);
    return rc == 0 ? 0 : -1;
}

int hwdisp_init(void) {
    extern void sf3000_dump_fb_state(const char *);
    if (g_active) return 0;

    /* RK3036G: payload-305 device logs REVERSED the payload-285 conclusion.
     * payload-285 saw "fb0 white flash visible" (a FIXED log string, not a real
     * measurement) and picoarch's DRM dumb-buffer present at 50 FPS but black
     * screen -> concluded "fb0 is the only visible path". WRONG.
     *
     * payload-305 diag proves the opposite: HDMI output lives on DRM/KMS
     * (card0-HDMI-A-1 status=connected enabled=enabled, 6 modes incl.
     * 1280x720p60). /dev/fb0 is a legacy emulation that does NOT drive the HDMI
     * scanout (writing it leaves the screen black). The earlier "fb0 white
     * flash" was rc=0 of the shell write (head|tr>/dev/fb0), NOT a visual
     * confirmation.
     *
     * So: DRM dumb-buffer + SETCRTC FIRST (this is the real HDMI path), fb0
     * direct-write only as a last-resort fallback. */
    extern int sf3000_is_rk3036(void);
    if (sf3000_is_rk3036()) {
        hwdisp_fslog("hwdisp_init: RK3036G branch, DRM modeset FIRST (HDMI is DRM/KMS)");
        if (hwdisp_drm_init() == 0) {
            g_active = 1;
            g_fb_only = 1;
            g_drm_mode = 1;
            hwdisp_fslog("hwdisp_init: DRM modeset ACTIVE %dx%d (dumb-buffer present)",
                         g_drm_w, g_drm_h);
            fprintf(stderr, "hwdisp: RK3036G DRM modeset active %dx%d\n", g_drm_w, g_drm_h);
            return 0;
        }
        hwdisp_fslog("hwdisp_init: DRM modeset failed, falling back to fb0-direct");
        hwdisp_fb_open();
        if (g_fbmem && g_fbw > 0 && g_fbh > 0) {
            g_active = 1;
            g_fb_only = 1;
            g_drm_mode = 0;
            hwdisp_fslog("hwdisp_init: g_fb_only=1 fb0-direct ACTIVE %dx%d bpp=%d stride=%d",
                         g_fbw, g_fbh, g_fbbpp, g_fbstride);
            fprintf(stderr, "hwdisp: RK3036G fb0-direct active %dx%d bpp=%d\n",
                    g_fbw, g_fbh, g_fbbpp);
            return 0;
        }
        hwdisp_fslog("hwdisp_init: RK3036G both DRM and fb0 FAILED (fbmem=%p fbw=%d fbh=%d) errno=%d",
                     (void*)g_fbmem, g_fbw, g_fbh, errno);
        fprintf(stderr, "hwdisp: RK3036G both DRM and fb0 failed\n");
        return -1;
    }

    sf3000_dump_fb_state("hwdisp_init/pre");

    /* Device-specific driver: each device ships its own driver.so build (panel
     * init + render behavior differ). Single source of truth in plat_sdl.c;
     * fall back to a generic driver.so if the per-device file is absent. */
    extern const char *sf3000_driver_path(void);
    const char *drv = sf3000_driver_path();
    g_handle = dlopen(drv, RTLD_NOW | RTLD_GLOBAL);
    if (!g_handle) {
        DBG("DBG hwdisp: dlopen %s failed (%s), trying driver.so\n", drv, dlerror());
        g_handle = dlopen("/mnt/sdcard/cubegm/driver.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!g_handle) {
        fprintf(stderr, "hwdisp: dlopen failed: %s\n", dlerror());
        return -1;
    }
    DBG("DBG hwdisp: loaded %s\n", drv);

    p_init   = (fn_init_t)  dlsym(g_handle, "video_drivers_init");
    p_deinit = (fn_deinit_t)dlsym(g_handle, "video_driver_deinit");
    p_disp   = (fn_disp_t)  dlsym(g_handle, "video_driver_disp_frame");
    p_aspect = (fn_aspect_t)dlsym(g_handle, "fbdev_video_aspect_ratio");
    p_enhance = (fn_enhance_t)dlsym(g_handle, "fbdev_set_enhance");
    g_fs_state = -1;

    if (!p_init || !p_deinit || !p_disp) {
        fprintf(stderr, "hwdisp: dlsym failed (init=%p deinit=%p disp=%p)\n",
                p_init, p_deinit, p_disp);
        dlclose(g_handle); g_handle = NULL;
        return -1;
    }

    int rv = p_init();
    if (rv <= 0) {
        fprintf(stderr, "hwdisp: video_drivers_init returned %d\n", rv);
        dlclose(g_handle); g_handle = NULL;
        return -1;
    }

    g_active = 1;
    fprintf(stderr, "hwdisp: HW path active (init rv=%d)\n", rv);

    /* R36SX: enable the HCGE engine via video_driver_setmode so disp_frame's
     * engine-sync doesn't hang (rkgame configures before presenting). disp_frame
     * then HW-scales src→panel (no CPU upscale). Try a few mode args; logged
     * fsync'd so a hang still tells us how far it got. */
    extern int sf3000_is_r36sx(void);
    if (sf3000_is_r36sx()) {
        typedef int (*fn_setmode_t)(int, int);
        fn_setmode_t p_setmode = (fn_setmode_t)dlsym(g_handle, "video_driver_setmode");
        DBG("DBG hwdisp: setmode=%p calling setmode(0,0)\n", (void*)p_setmode);
        if (p_setmode) { int sr = p_setmode(0, 0); DBG("DBG hwdisp: setmode(0,0) ret=%d\n", sr); }
    }
    g_sharpen = -1;   /* force re-apply on the next hwdisp_set_sharpen() */
    sf3000_dump_fb_state("hwdisp_init/post");
    /* fb0 for direct presents is mmap'd lazily by hwdisp_present_direct(). */
    return 0;
}

int hwdisp_active(void) { return g_active; }

void hwdisp_set_target_aspect(int num, int den) {
    g_aspect_num = num;
    g_aspect_den = den;
}

/* filter: scale_filter enum — 0 nearest, 1 bilinear, 2 sharp (integer prescale
 * + HW residual). Nearest and Sharp both use the SW path; Sharp additionally
 * sets g_filter_sharp so present_direct prescales instead of full-stretching. */
void hwdisp_set_filter(int filter) {
    g_filter_nearest = (filter != 1);   /* nearest(0) or sharp(2) */
    g_filter_sharp   = (filter == 2);
    /* If switching to a SW-scale filter, ensure native buffer exists. */
    if (g_filter_nearest && !g_near_buf) {
        g_near_buf = (uint16_t*)malloc(HW_BUFSZ);
        if (g_near_buf) memset(g_near_buf, 0, HW_BUFSZ);
    }
}

/* Pad horizontally: src(w×h) → g_pad_buf(pad_w×h), src centered, sides black. */
static void pad_horizontal(const void *src, int w, int h, int pitch_bytes, int pad_w) {
    int need = pad_w * h;
    if (need > g_pad_cap) {
        free(g_pad_buf);
        g_pad_cap = need + 4096;
        g_pad_buf = (uint16_t*)malloc(g_pad_cap * sizeof(uint16_t));
        g_pad_h   = 0;
        g_pad_w   = 0;
    }
    if (!g_pad_buf) return;

    int off_x = (pad_w - w) / 2;
    if (off_x < 0) off_x = 0;

    if (pad_w != g_pad_w || h != g_pad_h) {
        memset(g_pad_buf, 0, (size_t)pad_w * h * sizeof(uint16_t));
        g_pad_w = pad_w;
        g_pad_h = h;
    }

    for (int y = 0; y < h; y++) {
        const uint16_t *srow = (const uint16_t *)((const char *)src + y * pitch_bytes);
        uint16_t *drow = g_pad_buf + (size_t)y * pad_w + off_x;
        memcpy(drow, srow, (size_t)w * sizeof(uint16_t));
    }
}

/* Nearest-upscale src into g_near_buf (1280×720). Pads with black to fit
 * target aspect if set. Otherwise full-stretch upscales to 1280×720.
 *
 * Fast paths:
 *   - Integer scale (dst_w = w*n, dst_h = h*m): unrolled replication +
 *     vertical row memcpy. Avoids per-pixel lookup tables.
 *   - Generic: xmap lookup. */
static void upscale_nearest(const void *src, int w, int h, int pitch_bytes) {
    if (!g_near_buf) return;

    int dst_w, dst_h;
    if (g_aspect_num > 0 && g_aspect_den > 0) {
        /* Integer scale preferring largest factor that still fits */
        int my = HW_H / h;
        if (my < 1) my = 1;
        int dw = w * my;
        if (dw > HW_W) {
            /* Width-limited: pick scale by width instead */
            my = HW_W / w; if (my < 1) my = 1;
            dw = w * my;
        }
        dst_h = h * my;
        dst_w = dw;
    } else {
        /* Full stretch: integer-snap to 1280×720 if possible */
        int mx = HW_W / w; if (mx < 1) mx = 1;
        int my = HW_H / h; if (my < 1) my = 1;
        dst_w = w * mx; dst_h = h * my;
    }
    int off_x = (HW_W - dst_w) / 2;
    int off_y = (HW_H - dst_h) / 2;
    if (off_x < 0) off_x = 0;
    if (off_y < 0) off_y = 0;

    /* Clear borders only when geometry changes */
    static int last_dst_w = -1, last_dst_h = -1;
    if (dst_w != last_dst_w || dst_h != last_dst_h) {
        memset(g_near_buf, 0, HW_BUFSZ);
        last_dst_w = dst_w; last_dst_h = dst_h;
    }

    const int sp = pitch_bytes / 2;
    const uint16_t *s = (const uint16_t *)src;
    const int nx = dst_w / w;    /* H replication factor (integer) */
    const int ny = dst_h / h;    /* V replication factor (integer) */

    /* Integer-scale fast path: expand one row, copy ny times.
     * Use uint32_t writes (2 px/word) where alignment permits. */
    if (nx >= 1 && ny >= 1 && nx * w == dst_w && ny * h == dst_h) {
        const int row_bytes = dst_w * 2;
        for (int sy = 0; sy < h; sy++) {
            const uint16_t *srow = s + sy * sp;
            uint16_t *drow = g_near_buf + (size_t)(sy * ny + off_y) * HW_W + off_x;

            switch (nx) {
            case 1:
                memcpy(drow, srow, (size_t)w * 2);
                break;
            case 2: {
                /* 1 src px → 1 uint32_t write (p|p<<16) */
                uint32_t *d32 = (uint32_t *)drow;
                for (int sx = 0; sx < w; sx++) {
                    uint32_t p = srow[sx];
                    d32[sx] = p | (p << 16);
                }
                break;
            }
            case 3:
                for (int sx = 0; sx < w; sx++) {
                    uint16_t p = srow[sx];
                    uint16_t *dp = drow + sx * 3;
                    dp[0] = p; dp[1] = p; dp[2] = p;
                }
                break;
            case 4: {
                /* 1 src px → 2 uint32_t writes */
                uint32_t *d32 = (uint32_t *)drow;
                for (int sx = 0; sx < w; sx++) {
                    uint32_t p = srow[sx];
                    uint32_t pp = p | (p << 16);
                    d32[sx*2  ] = pp;
                    d32[sx*2+1] = pp;
                }
                break;
            }
            case 5:
                for (int sx = 0; sx < w; sx++) {
                    uint16_t p = srow[sx];
                    uint16_t *dp = drow + sx * 5;
                    dp[0] = p; dp[1] = p; dp[2] = p; dp[3] = p; dp[4] = p;
                }
                break;
            case 6: {
                uint32_t *d32 = (uint32_t *)drow;
                for (int sx = 0; sx < w; sx++) {
                    uint32_t p = srow[sx];
                    uint32_t pp = p | (p << 16);
                    d32[sx*3  ] = pp;
                    d32[sx*3+1] = pp;
                    d32[sx*3+2] = pp;
                }
                break;
            }
            case 8: {
                uint32_t *d32 = (uint32_t *)drow;
                for (int sx = 0; sx < w; sx++) {
                    uint32_t p = srow[sx];
                    uint32_t pp = p | (p << 16);
                    d32[sx*4  ] = pp;
                    d32[sx*4+1] = pp;
                    d32[sx*4+2] = pp;
                    d32[sx*4+3] = pp;
                }
                break;
            }
            default:
                for (int sx = 0; sx < w; sx++) {
                    uint16_t p = srow[sx];
                    uint16_t *dp = drow + sx * nx;
                    for (int k = 0; k < nx; k++) dp[k] = p;
                }
                break;
            }

            /* Vertical replication: copy this row (ny-1) more times */
            for (int v = 1; v < ny; v++)
                memcpy(drow + (size_t)v * HW_W, drow, row_bytes);
        }
        return;
    }

    /* Generic fallback: xmap lookup */
    static int xmap[HW_W];
    static int last_w_map = -1, last_dst_w_map = -1;
    if (w != last_w_map || dst_w != last_dst_w_map) {
        for (int dx = 0; dx < dst_w; dx++) xmap[dx] = dx * w / dst_w;
        last_w_map = w; last_dst_w_map = dst_w;
    }
    for (int dy = 0; dy < dst_h; dy++) {
        int sy = dy * h / dst_h;
        const uint16_t *srow = s + sy * sp;
        uint16_t *drow = g_near_buf + (size_t)(dy + off_y) * HW_W + off_x;
        for (int dx = 0; dx < dst_w; dx++)
            drow[dx] = srow[xmap[dx]];
    }
}

/* Direct present: write the frame straight into fb0; the display controller
 * scales fb0 → panel. Bypasses video_driver_disp_frame, which HANGS on R36SX
 * and ABORTS on SF3000 panel-size input. Used for FrogUI (both devices) and all
 * R36SX frames. Returns 1 if presented. */
/* Panel scale mode for R36SX disp_frame present: 0=integer(NONE), 1=aspect, 2=full.
 * disp_frame itself aspect-fits src→panel, so:
 *   aspect → pass src straight (driver aspect-fits, bars).
 *   full   → SW-stretch src into a 640x480 panel buffer → disp_frame 1:1 (fills).
 *   integer→ SW integer-replicate src centered in 640x480 buffer → disp_frame 1:1. */
#define PANEL_PW 640
#define PANEL_PH 480
static int g_panel_scale = 2;       /* default full */
void hwdisp_set_panel_scale(int m) { g_panel_scale = m; }

/* Ping-pong output buffers: disp_frame DMA-reads the previous frame async, so we
 * write the other buffer — the result is safe to hand straight to disp_frame,
 * no extra staging copy (present_direct skips its copy for panel_build output).
 * Only the source rows that differ are scaled; duplicated rows are memcpy'd, and
 * the full-buffer memset happens only when the output geometry changes. */
/* mode: 0=integer replicate (exact NxN, centered), 1=aspect-fit nearest stretch
 * (centered, bars), 2=full nearest stretch (fills panel). All nearest → sharp. */
static uint16_t *panel_build(const void *src, int w, int h, int pitch_bytes, int mode) {
    static uint16_t *pb[2]; static unsigned pbgeo[2]; static int pbi;
    pbi ^= 1;
    if (!pb[pbi]) { pb[pbi] = (uint16_t*)malloc(PANEL_PW*PANEL_PH*2); if (!pb[pbi]) return NULL; pbgeo[pbi] = 0; }
    uint16_t *d = pb[pbi];
    const int sp = pitch_bytes/2; const uint16_t *s = (const uint16_t*)src;
    if (mode == 0) {
        int n = PANEL_PW/w; int ny = PANEL_PH/h; if (ny<n) n=ny; if (n<1) n=1;
        int dw=w*n, dh=h*n, ox=(PANEL_PW-dw)/2, oy=(PANEL_PH-dh)/2;
        unsigned geo = ((unsigned)dw<<16)|(unsigned)dh;
        if (pbgeo[pbi] != geo) { memset(d, 0, PANEL_PW*PANEL_PH*2); pbgeo[pbi] = geo; }
        for (int y=0; y<h; y++){ const uint16_t *sr=s+(size_t)y*sp;
            uint16_t *dr=d+(size_t)(oy+y*n)*PANEL_PW+ox;
            if (n == 2) {
                /* Fast 2x: one 32-bit store per src pixel (two dup pixels at
                 * once) instead of two 16-bit stores. dr is 32-bit aligned
                 * (PANEL_PW, y*n, and ox=(PANEL_PW-w*2)/2 are all even). */
                uint32_t *d32 = (uint32_t *)dr;
                for (int x=0;x<w;x++){ uint32_t px=sr[x]; d32[x]=(px<<16)|px; }
            } else {
                for (int x=0;x<w;x++){ uint16_t px=sr[x]; uint16_t *dp=dr+x*n; for(int rx=0;rx<n;rx++) dp[rx]=px; }
            }
            for (int ry=1; ry<n; ry++) memcpy(dr+(size_t)ry*PANEL_PW, dr, (size_t)dw*2); }
    } else { /* nearest stretch: full fills the panel, aspect fits centered w/ bars */
        int dw = PANEL_PW, dh = PANEL_PH, ox = 0, oy = 0;
        if (mode == 1) {                       /* aspect-fit */
            if (w * PANEL_PH >= h * PANEL_PW) dh = h * PANEL_PW / w;
            else                              dw = w * PANEL_PH / h;
            ox = (PANEL_PW - dw) / 2; oy = (PANEL_PH - dh) / 2;
        }
        unsigned geo = ((unsigned)dw<<16)|(unsigned)dh;
        if (pbgeo[pbi] != geo) { memset(d, 0, PANEL_PW*PANEL_PH*2); pbgeo[pbi] = geo; }
        /* Src-driven run lengths: dst column x maps to src x*w/dw (nearest,
         * monotonic), so each src pixel covers a run of rl[sx] dst pixels. One
         * sequential read per src pixel (no gather), and runs are written with
         * 32-bit stores when aligned. */
        static int rl[PANEL_PW]; static int lw=-1, ldw=-1;
        if (w!=lw || dw!=ldw){ int prev=0; for(int sx=0;sx<w;sx++){ int e=(sx+1)*dw/w; rl[sx]=e-prev; prev=e; } lw=w; ldw=dw; }
        int lsy = -1;
        for (int y=0;y<dh;y++){
            int sy = y*h/dh;
            uint16_t *dr=d+(size_t)(oy+y)*PANEL_PW+ox;
            if (sy == lsy) { memcpy(dr, dr-PANEL_PW, (size_t)dw*2); continue; }
            lsy = sy;
            const uint16_t *sr=s+(size_t)sy*sp;
            uint16_t *dp=dr;
            for (int sx=0; sx<w; sx++){
                uint16_t px=sr[sx]; int r=rl[sx];
                uint32_t px2=((uint32_t)px<<16)|px;
                while (r>=2 && !((uintptr_t)dp&3)){ *(uint32_t*)dp=px2; dp+=2; r-=2; }
                while (r-->0) *dp++=px;
            }
        }
    }
    return d;
}

/* Sharp-bilinear prescale: integer-replicate src by n (fast word-store path)
 * into a tight n*w × n*h buffer. The GE then does only the small leftover
 * fractional stretch to the panel (fill/fit) in HW — far cheaper than a full SW
 * stretch to 640x480, and near-nearest sharp (pixels already n×-doubled). */
static uint16_t *prescale_int(const void *src, int w, int h, int pitch_bytes,
                              int n, int *out_w, int *out_h) {
    static uint16_t *pb[2]; static int pbi;
    pbi ^= 1;
    if (!pb[pbi]) { pb[pbi] = (uint16_t*)malloc(PANEL_PW*PANEL_PH*2); if (!pb[pbi]) return NULL; }
    uint16_t *d = pb[pbi];
    const int sp = pitch_bytes/2; const uint16_t *s = (const uint16_t*)src;
    int dw = w * n;
    for (int y=0; y<h; y++){
        const uint16_t *sr = s + (size_t)y*sp;
        uint16_t *dr = d + (size_t)(y*n)*dw;
        if (n == 2) {
            uint32_t *d32 = (uint32_t *)dr;
            int x = 0;
            /* Read 2 src pixels per iter (one 32-bit load vs two 16-bit) when
             * the src row is 32-bit aligned; emit two doubled dst words. */
            if (!(((uintptr_t)sr) & 3)) {
                const uint32_t *s32 = (const uint32_t *)sr;
                for (; x + 1 < w; x += 2) {
                    uint32_t two = s32[x>>1];
                    uint32_t a = two & 0xffff, b = two >> 16;
                    d32[x]   = (a<<16)|a;
                    d32[x+1] = (b<<16)|b;
                }
            }
            for (; x < w; x++){ uint32_t px=sr[x]; d32[x]=(px<<16)|px; }
        } else {
            for (int x=0;x<w;x++){ uint16_t px=sr[x]; uint16_t *dp=dr+x*n; for(int k=0;k<n;k++) dp[k]=px; }
        }
        for (int ry=1; ry<n; ry++) memcpy(dr+(size_t)ry*dw, dr, (size_t)dw*2);
    }
    *out_w = dw; *out_h = h*n;
    return d;
}

int hwdisp_present_direct(const void *src, int w, int h, int pitch_bytes) {
    static int s_n = 0;
    int lg = (s_n < 8);
    s_n++;
    if (!g_active || !src) return 0;
    /* RK3036G: no driver.so — present straight to fb0. */
    if (g_fb_only) { hwdisp_fb_present(src, w, h, pitch_bytes); return 1; }
    if (!p_disp) return 0;
    /* disp_frame HW-scales src→panel; the driver's fullscreen flag
     * (fbdev_video_aspect_ratio, like stock) picks fill vs aspect-fit. So:
     *   full   → flag=fill, pass src straight → GE distort-fills in HW (fast).
     *   aspect → flag=fit,  pass src straight → GE letterboxes in HW.
     *   integer→ SW integer-replicate centered in 640x480 (exact NxN, sharp) —
     *            the GE can't do nearest. panel-size (FrogUI) passes straight.
     * Only integer still pays SW cost. */
    /* Filter route (game frames only; FrogUI/menu panel-size frames pass straight):
     *   Bilinear → hand src straight to the GE, fullscreen flag picks fill/fit.
     *              Fast (HW scale), but soft.
     *   Nearest  → SW-scale into a 640x480 panel buffer (integer replicate /
     *              aspect-fit / full-fill), present 1:1. True sharp, costs CPU. */
    int game = !(w == PANEL_PW && h == PANEL_PH);
    int sw_nearest = game && g_filter_nearest;
    int sw_integer = game && !g_filter_nearest && g_panel_scale == 0;

    const void *psrc = src; int pw = w, ph = h, ppitch = pitch_bytes;
    int staged = 0;
    int hw_scale = !sw_nearest;   /* whether the GE still scales (needs fill/fit flag) */

    if (sw_nearest && g_filter_sharp && g_panel_scale != 0 /*full/aspect*/) {
        /* Sharp: prescale by the largest N (>=2) that fits the panel, then let
         * the GE do the small residual stretch (fill for full, fit for aspect).
         * Halves the SW cost vs a full 640x480 stretch and reuses the fast
         * integer path. N=1 (source already big) → fall back to true stretch. */
        int n = PANEL_PW / w; int ny = PANEL_PH / h; if (ny < n) n = ny;
        if (n >= 2) {
            int ow, oh;
            uint16_t *b = prescale_int(src, w, h, pitch_bytes, n, &ow, &oh);
            if (b) { psrc = b; pw = ow; ph = oh; ppitch = ow*2; staged = 1; hw_scale = 1; }
        }
        if (!staged) {   /* N<2 or alloc fail: true SW stretch to panel */
            uint16_t *b = panel_build(src, w, h, pitch_bytes, g_panel_scale);
            if (b) { psrc = b; pw = PANEL_PW; ph = PANEL_PH; ppitch = PANEL_PW*2; staged = 1; }
        }
    } else if (sw_nearest || sw_integer) {
        /* True nearest (full/aspect SW stretch) or exact NxN integer — present
         * the 640x480 panel buffer 1:1, no HW scaling. */
        uint16_t *b = panel_build(src, w, h, pitch_bytes, g_panel_scale);
        if (b) { psrc = b; pw = PANEL_PW; ph = PANEL_PH; ppitch = PANEL_PW*2; staged = 1; }
    }

    if (p_aspect && hw_scale) {
        /* Driver arg is inverted on this build: 0 = fill, 1 = aspect-fit. */
        int want = (g_panel_scale == 2) ? 0 : 1;   /* full = fill, else fit */
        if (want != g_fs_state) { p_aspect(want); g_fs_state = want; }
    }
    /* disp_frame DMA-reads src asynchronously; handing it the caller's live
     * buffer races the next frame's rendering (font/pixel shimmer during menu
     * scrolling on R36SX). Stage into ping-pong buffers so the engine always
     * scans a stable copy. panel_build output is already ping-ponged — skip. */
    static uint16_t *g_dpp[2];
    static int g_dppi;
    if (!g_dpp[0]) { g_dpp[0] = (uint16_t*)malloc(PANEL_PW*PANEL_PH*2); g_dpp[1] = (uint16_t*)malloc(PANEL_PW*PANEL_PH*2); }
    if (!staged && g_dpp[0] && g_dpp[1] && pw <= PANEL_PW && ph <= PANEL_PH) {
        uint16_t *dst = g_dpp[g_dppi]; g_dppi ^= 1;
        for (int y = 0; y < ph; y++)
            memcpy(dst + (size_t)y*pw, (const uint8_t*)psrc + (size_t)y*ppitch, (size_t)pw*2);
        psrc = dst; ppitch = pw*2;
    }
    if (lg) DBG("DBG present_direct#%d: pre disp_frame %dx%d scale=%d\n", s_n, pw, ph, g_panel_scale);
    int rv = p_disp((void *)psrc, pw, ph, ppitch);
    if (lg) DBG("DBG present_direct#%d: post rv=%d\n", s_n, rv);
    return 1;
}

void hwdisp_present(const void *src, int w, int h, int pitch_bytes) {
    static int s_n = 0;
    int lg = (s_n < 8);
    s_n++;
    if (lg) DBG("DBG present#%d: src=%p w=%d h=%d pitch=%d active=%d p_disp=%p filt=%d asp=%d/%d HW=%dx%d\n",
                s_n, src, w, h, pitch_bytes, g_active, (void*)p_disp,
                g_filter_nearest, g_aspect_num, g_aspect_den, HW_W, HW_H);
    if (!g_active || !src) { if (lg) DBG("DBG present#%d: EARLY-RET\n", s_n); return; }
    if (g_fb_only) { hwdisp_fb_present(src, w, h, pitch_bytes); return; }
    if (!p_disp) { if (lg) DBG("DBG present#%d: no p_disp\n", s_n); return; }
    int rv;
    /* Nearest filter: SW upscale to 1280×720, driver does no further scale. */
    if (g_filter_nearest) {
        if (!g_near_buf) {
            g_near_buf = (uint16_t*)malloc(HW_BUFSZ);
            if (g_near_buf) memset(g_near_buf, 0, HW_BUFSZ);
        }
        if (g_near_buf) {
            upscale_nearest(src, w, h, pitch_bytes);
            if (lg) DBG("DBG present#%d: nearest pre p_disp(%p,%d,%d,%d)\n", s_n, (void*)g_near_buf, HW_W, HW_H, HW_PITCH);
            rv = p_disp(g_near_buf, HW_W, HW_H, HW_PITCH);
            if (lg) DBG("DBG present#%d: nearest post p_disp rv=%d\n", s_n, rv);
            return;
        }
        /* Fallthrough to HW path if alloc failed */
    }

    /* Unpadded presents must NOT hand the core's live framebuffer to
     * disp_frame — its HCGE DMA reads the source asynchronously while the core
     * renders the next frame into it (bus contention + engine re-sync = the
     * "Full-screen lags" report; Aspect was accidentally immune because its
     * pad step copies to staging). Ping-pong stage, like present_direct. */
    static uint16_t *fs[2];
    static int fsi;
    const void *psrc = src;
    int ppitch = pitch_bytes;
    if (!fs[0]) { fs[0] = (uint16_t*)malloc(640*480*2); fs[1] = (uint16_t*)malloc(640*480*2); }
    if (fs[0] && fs[1] && w <= 640 && h <= 480) {
        uint16_t *dst = fs[fsi]; fsi ^= 1;
        for (int y = 0; y < h; y++)
            memcpy(dst + (size_t)y*w, (const uint8_t*)src + (size_t)y*pitch_bytes, (size_t)w*2);
        psrc = dst; ppitch = w*2;
    }

    /* HW (bilinear) path: pass through, optional aspect pad. */
    if (g_aspect_num <= 0 || g_aspect_den <= 0) {
        if (lg) DBG("DBG present#%d: passthru pre p_disp(%p,%d,%d,%d)\n", s_n, psrc, w, h, ppitch);
        rv = p_disp((void *)psrc, w, h, ppitch);
        if (lg) DBG("DBG present#%d: passthru post p_disp rv=%d\n", s_n, rv);
        return;
    }

    int pad_w = h * g_aspect_num / g_aspect_den;
    pad_w &= ~1;   /* odd width wedges disp_frame/HCGE to black on R36SX (e.g.
                    * 853 for 480-tall, 455 for 256-tall). Round to even. */
    if (pad_w <= w) {
        if (lg) DBG("DBG present#%d: nopad pre p_disp\n", s_n);
        rv = p_disp((void *)psrc, w, h, ppitch);
        if (lg) DBG("DBG present#%d: nopad post p_disp rv=%d\n", s_n, rv);
        return;
    }

    pad_horizontal(psrc, w, h, ppitch, pad_w);
    if (!g_pad_buf) {
        rv = p_disp((void *)psrc, w, h, ppitch);
        if (lg) DBG("DBG present#%d: padfail post rv=%d\n", s_n, rv);
        return;
    }
    if (lg) DBG("DBG present#%d: pad pre p_disp(pad_w=%d)\n", s_n, pad_w);
    rv = p_disp(g_pad_buf, pad_w, h, pad_w * 2);
    if (lg) DBG("DBG present#%d: pad post p_disp rv=%d\n", s_n, rv);
}

/* Panel-integer present: SW nearest-upscale src by largest integer N where
 * N*w<=854 && N*h<=480, center result in 854x480 black panel buffer, send to
 * driver with filter=0 (pass-through). True integer pixel ratio on panel. */
#ifndef PANEL_W
#define PANEL_W 854
#endif
#ifndef PANEL_H
#define PANEL_H 480
#endif
#define PANEL_PITCH (PANEL_W * 2)

static uint16_t *g_panel_buf = NULL;

void hwdisp_present_integer(const void *src, int w, int h, int pitch_bytes) {
    if (!g_active || !p_disp || !src) return;
    if (w <= 0 || h <= 0) return;

    if (!g_panel_buf) {
        g_panel_buf = (uint16_t *)malloc(PANEL_W * PANEL_H * sizeof(uint16_t));
        if (!g_panel_buf) return;
        memset(g_panel_buf, 0, PANEL_W * PANEL_H * sizeof(uint16_t));
    }

    int sx = PANEL_W / w;
    int sy = PANEL_H / h;
    int n = sx < sy ? sx : sy;
    if (n < 1) n = 1;
    int dw = w * n, dh = h * n;
    if (dw > PANEL_W) dw = PANEL_W;
    if (dh > PANEL_H) dh = PANEL_H;
    int ox = (PANEL_W - dw) / 2;
    int oy = (PANEL_H - dh) / 2;

    /* Clear borders only when geometry changes */
    static int last_dw = -1, last_dh = -1;
    if (dw != last_dw || dh != last_dh) {
        memset(g_panel_buf, 0, PANEL_W * PANEL_H * sizeof(uint16_t));
        last_dw = dw; last_dh = dh;
    }

    const int sp = pitch_bytes / 2;
    const uint16_t *s = (const uint16_t *)src;

    /* Row expand (one src row → n dst rows), per-pixel replicate. */
    for (int srow_i = 0; srow_i < h; srow_i++) {
        const uint16_t *srow = s + srow_i * sp;
        uint16_t *drow = g_panel_buf + (size_t)(oy + srow_i * n) * PANEL_W + ox;

        switch (n) {
        case 1:
            memcpy(drow, srow, (size_t)w * 2);
            break;
        case 2: {
            uint32_t *d32 = (uint32_t *)drow;
            for (int x = 0; x < w; x++) {
                uint32_t p = srow[x];
                d32[x] = p | (p << 16);
            }
            break;
        }
        case 3:
            for (int x = 0; x < w; x++) {
                uint16_t p = srow[x];
                drow[x*3] = drow[x*3+1] = drow[x*3+2] = p;
            }
            break;
        case 4: {
            uint32_t *d32 = (uint32_t *)drow;
            for (int x = 0; x < w; x++) {
                uint32_t p = srow[x];
                uint32_t pp = p | (p << 16);
                d32[x*2] = pp; d32[x*2+1] = pp;
            }
            break;
        }
        default:
            for (int x = 0; x < w; x++) {
                uint16_t p = srow[x];
                uint16_t *dp = drow + x * n;
                for (int k = 0; k < n; k++) dp[k] = p;
            }
            break;
        }

        /* Vertical replication: copy this row n-1 more times */
        for (int v = 1; v < n; v++)
            memcpy(drow + (size_t)v * PANEL_W, drow, (size_t)dw * 2);
    }

    p_disp(g_panel_buf, PANEL_W, PANEL_H, PANEL_PITCH);
}

void hwdisp_deinit(void) {
    extern void sf3000_dump_fb_state(const char *);
    if (!g_active) { DBG("DBG hwdisp_deinit: not active\n"); return; }
    sf3000_dump_fb_state("hwdisp_deinit/pre");
    if (p_deinit) p_deinit();
    sf3000_dump_fb_state("hwdisp_deinit/post-p_deinit");
    if (g_fbmem) { munmap(g_fbmem, g_fbsize); g_fbmem = NULL; }
    if (g_fbfd >= 0) { close(g_fbfd); g_fbfd = -1; }
    if (g_drm_map) { munmap(g_drm_map, g_drm_size); g_drm_map = NULL; g_drm_mode = 0; }
    if (g_drm_fd >= 0) { close(g_drm_fd); g_drm_fd = -1; }
    if (g_pad_buf) { free(g_pad_buf); g_pad_buf = NULL; g_pad_cap = 0; g_pad_w = 0; g_pad_h = 0; }
    if (g_near_buf) { free(g_near_buf); g_near_buf = NULL; }
    if (g_panel_buf) { free(g_panel_buf); g_panel_buf = NULL; }
    if (g_handle) { dlclose(g_handle); g_handle = NULL; }
    p_init = NULL; p_deinit = NULL; p_disp = NULL;
    g_active = 0;
    sf3000_dump_fb_state("hwdisp_deinit/post-dlclose");
}
