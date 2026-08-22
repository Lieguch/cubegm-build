/* cubevol_bridge.c -- emulate the stock cubevol input daemon for FrogUI.
 *
 * FrogUI (frogui_libretro.c) reads input ONLY from the stock cubevol shared
 * memory at /tmp/joy_key (ftok 'a', 32-bit key mask; see CV_* defines):
 *     UP=4 DOWN=6 LEFT=7 RIGHT=5 A=13 B=14 X=12 Y=15 L=10 R=11
 *     L2=8 R2=9 SELECT=0 START=3
 * The stock cubevol daemon is embedded in rkgame (no standalone binary), so
 * after we hijack the boot there is NO input source -> FrogUI menus dead.
 * This bridge reads USB gamepads via evdev and writes that mask.
 *
 * PSX-style pad kernel keycodes (Twin USB Gamepad 0810:0001 rev 0110,
 * per stock profile 0810_0001_0110; usbhid maps to BTN_TRIGGER..BTN_BASE6):
 *     btn0=Y btn1=B btn2=A btn3=X btn4=L1 btn5=R1 btn6=L2 btn7=R2
 *     btn8=SELECT btn9=START   (keycodes 0x120..0x129)
 * d-pad arrives as ABS_HAT0X/HAT0Y (also ABS_X/ABS_Y stick, thresholded).
 *
 * Build: arm-linux-gnueabihf-gcc -O2 -Wall cubevol_bridge.c -o cubevol_bridge
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <linux/input.h>

/* cubevol key-mask bits (sf3000_keymap.txt / CV_* in frogui_libretro.c) */
#define K_UP    4
#define K_DOWN  6
#define K_LEFT  7
#define K_RIGHT 5
#define K_A    13
#define K_B    14
#define K_X    12
#define K_Y    15
#define K_L    10
#define K_R    11
#define K_L2    8
#define K_R2    9
#define K_SEL   0
#define K_START 3

static volatile uint32_t *g_mask = NULL;
static int g_down[KEY_CNT];
/* v8.7: per-fd ABS capability bitmap. The old code treated "axis value == 0"
 * as "unplugged port" (skip), which silently dropped the UP/LEFT directions:
 * a pad pushed to the TOP/LEFT reads ABS_Y/ABS_X == 0 (minimum) and was
 * skipped -> UP and LEFT were permanently dead while DOWN/RIGHT worked.
 * Capability is decided ONCE at open time via EVIOCGBIT(EV_ABS), not by the
 * live axis value. */
static unsigned long g_absbits[16][4];   /* [fd index][word] */
static int g_abs_ok[16];                 /* 1 if EVIOCGBIT(EV_ABS) succeeded */
#define ABS_HAS(fd_i, axis) \
    (g_abs_ok[fd_i] && ((g_absbits[fd_i][(axis) / (8 * sizeof(long))] >> ((axis) % (8 * sizeof(long)))) & 1))

/* v3: d-pad levels from BTN_DPAD_* key events + event-stream axis state */
#ifndef BTN_DPAD_UP
#define BTN_DPAD_UP    0x220
#define BTN_DPAD_DOWN  0x221
#define BTN_DPAD_LEFT  0x222
#define BTN_DPAD_RIGHT 0x223
#endif
static int g_dpad_up = 0, g_dpad_dn = 0, g_dpad_lt = 0, g_dpad_rt = 0;

static void upd(int code, int on) { if (code >= 0 && code < KEY_CNT) g_down[code] = on; }

/* Direction keys (d-pad hat + analog stick): debounced level-follow.
 * FrogUI's menu is edge-triggered (`up && !up_last`), so a held 1 produces
 * exactly ONE move per physical press. The old 40 ms pulse made short/fast
 * presses invisible to FrogUI's 60 Hz shm reads (observed: UP/LEFT dead while
 * DOWN/RIGHT worked -- pulses are sampled by 10 ms polls and can be missed).
 * Debounce (30 ms) kills mechanical hat bounce that previously moved 2-3 rows.
 * Games don't read this shm (they use picoarch's own evdev), so level-follow
 * can't break in-game hold. */
#define DIR_DEBOUNCE_MS 30
struct dir_state { int phys; int out; long change_ms; };
static struct dir_state g_dir[4];   /* 0=UP 1=DOWN 2=LEFT 3=RIGHT */
static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
/* feed the physical level of a direction, returns the debounced level */
static int dir_feed(int idx, int phys) {
    long now = now_ms();
    if (phys != g_dir[idx].phys) {        /* any change: record, suppress bounce */
        g_dir[idx].phys = phys;
        g_dir[idx].change_ms = now;
    }
    if (now - g_dir[idx].change_ms < DIR_DEBOUNCE_MS)
        return g_dir[idx].out;            /* still within debounce window */
    g_dir[idx].out = phys;                /* stable -> level-follow */
    return g_dir[idx].out;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    /* ftok() needs the file to exist */
    int f = open("/tmp/joy_key", O_CREAT | O_RDWR, 0666);
    if (f >= 0) close(f);
    key_t key = ftok("/tmp/joy_key", 'a');
    if (key == (key_t)-1) { fprintf(stderr, "cubevol_bridge: ftok failed: %s\n", strerror(errno)); return 1; }
    int shmid = shmget(key, 4, 0666 | IPC_CREAT);
    if (shmid < 0) { fprintf(stderr, "cubevol_bridge: shmget failed: %s\n", strerror(errno)); return 1; }
    uint32_t *m = shmat(shmid, NULL, 0);
    if (m == (void *)-1) { fprintf(stderr, "cubevol_bridge: shmat failed: %s\n", strerror(errno)); return 1; }
    *m = 0;
    g_mask = m;
    fprintf(stderr, "cubevol_bridge: shm OK key=%d id=%d\n", (int)key, shmid);

    /* open all evdev devices that report EV_KEY */
    int fds[16]; int nfd = 0;
    for (int i = 0; i < 16 && nfd < 16; i++) {
        char path[64];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned long evbits = 0;
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits) < 0) { close(fd); continue; }
        if (evbits & (1ul << EV_KEY)) {
            /* v8.7: record the device's ABS capability bitmap once, at open. */
            g_abs_ok[nfd] = 0;
            if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(g_absbits[nfd])), g_absbits[nfd]) >= 0)
                g_abs_ok[nfd] = 1;
            fds[nfd++] = fd;
            fprintf(stderr, "cubevol_bridge: watching %s\n", path);
        } else close(fd);
    }
    if (nfd == 0) { fprintf(stderr, "cubevol_bridge: no evdev devices\n"); return 1; }

    /* axis state tracked from the EVENT STREAM (some drivers only update the
     * abs state on EV_ABS events and report stale/zero via EVIOCGABS — the
     * observed "UP/LEFT dead in UI while in-game works" class). Updated on
     * every EV_ABS event; also polled via EVIOCGABS each cycle as a backup.
     * v8.7: g_axis_seen[] marks axes that produced an event, because value 0
     * is a legit "pushed to minimum" (UP/LEFT) state, not "unplugged". */
    static int g_axis[0x60];          /* ABS_MAX is 0x3f; 0x60 covers it */
    static int g_axis_seen[0x60];
    int log_ev = 1;                   /* dump the first raw events to zhijack.log */
    long ev_logged = 0;
    struct input_event ev;
    for (;;) {
        for (int i = 0; i < nfd; i++) {
            ssize_t rd;
            while ((rd = read(fds[i], &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
                if (log_ev && ev_logged < 200) {
                    fprintf(stderr, "bridge ev: type=%d code=%d val=%d\n",
                            ev.type, ev.code, ev.value);
                    ev_logged++;
                    if (ev_logged >= 200) log_ev = 0;
                }
                if (ev.type == EV_KEY) {
                    upd(ev.code, ev.value != 0);
                    /* d-pad reported as BTN_DPAD_* KEY events (some pads) */
                    if (ev.code == BTN_DPAD_UP)   g_dpad_up = ev.value != 0;
                    if (ev.code == BTN_DPAD_DOWN) g_dpad_dn = ev.value != 0;
                    if (ev.code == BTN_DPAD_LEFT) g_dpad_lt = ev.value != 0;
                    if (ev.code == BTN_DPAD_RIGHT)g_dpad_rt = ev.value != 0;
                } else if (ev.type == EV_ABS && ev.code >= 0 && ev.code < 0x60) {
                    g_axis[ev.code] = ev.value;   /* track axis from stream */
                    g_axis_seen[ev.code] = 1;     /* v8.7: value 0 is VALID (min) */
                }
            }
            if (rd < 0 && errno != EAGAIN) { close(fds[i]); fds[i] = fds[--nfd]; i--; }
        }
        uint32_t mask = 0;
        if (g_down[BTN_TRIGGER]) mask |= 1u << K_Y;   /* btn0 = Y */
        if (g_down[BTN_THUMB])   mask |= 1u << K_B;
        if (g_down[BTN_THUMB2])  mask |= 1u << K_A;
        if (g_down[BTN_TOP])     mask |= 1u << K_X;
        if (g_down[BTN_TOP2])    mask |= 1u << K_L;
        if (g_down[BTN_PINKIE])  mask |= 1u << K_R;
        if (g_down[BTN_BASE])    mask |= 1u << K_L2;
        if (g_down[BTN_BASE2])   mask |= 1u << K_R2;
        if (g_down[BTN_BASE3])   mask |= 1u << K_SEL;
        if (g_down[BTN_BASE4])   mask |= 1u << K_START;
        /* d-pad hat + analog stick (any device). Physical levels are
         * merged per-direction then fed through the pulse debouncer.
         * v3: axis state from BOTH the event stream (g_axis, updated above)
         * AND EVIOCGABS polling; all HATs (0..3) + stick axes X/Y/Z/RX/RY/RZ
         * with per-axis deadzone; BTN_DPAD_* key events (g_dpad_*) merge in. */
        int ph_up = g_dpad_up, ph_dn = g_dpad_dn, ph_lt = g_dpad_lt, ph_rt = g_dpad_rt;
        for (int i = 0; i < nfd; i++) {
            struct input_absinfo ai;
            /* HAT0..HAT3 (a 2-in-1 pad's P2 d-pad is often HAT1) */
            for (int hat = 0; hat < 4; hat++) {
                if (ioctl(fds[i], EVIOCGABS(ABS_HAT0X + hat * 2), &ai) == 0) {
                    if (ai.value < 0) ph_lt = 1;
                    if (ai.value > 0) ph_rt = 1;
                }
                if (ioctl(fds[i], EVIOCGABS(ABS_HAT0Y + hat * 2), &ai) == 0) {
                    if (ai.value < 0) ph_up = 1;
                    if (ai.value > 0) ph_dn = 1;
                }
            }
            /* event-stream axis state (some drivers don't expose hat via
             * EVIOCGABS but DO send EV_ABS events) */
            if (g_axis[ABS_HAT0X] < 0) ph_lt = 1;
            if (g_axis[ABS_HAT0X] > 0) ph_rt = 1;
            if (g_axis[ABS_HAT0Y] < 0) ph_up = 1;
            if (g_axis[ABS_HAT0Y] > 0) ph_dn = 1;
            if (g_axis[ABS_HAT1X] < 0) ph_lt = 1;
            if (g_axis[ABS_HAT1X] > 0) ph_rt = 1;
            if (g_axis[ABS_HAT1Y] < 0) ph_up = 1;
            if (g_axis[ABS_HAT1Y] > 0) ph_dn = 1;
            /* analog sticks: per-axis deadzone (25% of range).
             * v8.7 FIXES:
             *  - capability decided by the EVIOCGBIT(EV_ABS) bitmap recorded at
             *    open, NEVER by the live value. value==0 IS a valid "pushed to
             *    minimum" state; the old `ai.value==0 && g_axis==0 -> continue`
             *    misread UP (ABS_Y=0) and LEFT (ABS_X=0) as unplugged ports and
             *    skipped them -> UP/LEFT permanently dead, DOWN/RIGHT fine.
             *  - prefer the event-stream value once the axis has produced an
             *    event (g_axis_seen), EVIOCGABS poll only before the first one.
             *  - only X/Y/RX/RY are direction axes; Z/RZ are triggers on many
             *    pads (Twin USB Gamepad has ABS_Z/ABS_RZ) and must not move
             *    the cursor. */
            static const int dir_axes[4] = { ABS_X, ABS_Y, ABS_RX, ABS_RY };
            for (int a = 0; a < 4; a++) {
                int ax = dir_axes[a];
                if (!ABS_HAS(i, ax)) continue;                 /* device lacks it */
                if (ioctl(fds[i], EVIOCGABS(ax), &ai) != 0) continue;
                if (ai.minimum == ai.maximum) continue;        /* constant = absent */
                int lo = ai.minimum + (ai.maximum - ai.minimum) / 4;
                int hi = ai.maximum - (ai.maximum - ai.minimum) / 4;
                int v = g_axis_seen[ax] ? g_axis[ax] : ai.value;
                if (ax == ABS_X || ax == ABS_RX) {
                    if (v < lo) ph_lt = 1;
                    if (v > hi) ph_rt = 1;
                } else {
                    if (v < lo) ph_up = 1;
                    if (v > hi) ph_dn = 1;
                }
            }
        }
        if (dir_feed(0, ph_up)) mask |= 1u << K_UP;
        if (dir_feed(1, ph_dn)) mask |= 1u << K_DOWN;
        if (dir_feed(2, ph_lt)) mask |= 1u << K_LEFT;
        if (dir_feed(3, ph_rt)) mask |= 1u << K_RIGHT;
        *m = mask;
        /* periodic diagnostic (every ~2 s) so zhijack.log shows which physical
         * directions the bridge sees -- use to verify UP/LEFT reach evdev */
        static long last_diag = 0;
        static int last_up=-1, last_dn=-1, last_lt=-1, last_rt=-1;
        long now = now_ms();
        if (ph_up!=last_up || ph_dn!=last_dn || ph_lt!=last_lt || ph_rt!=last_rt) {
            fprintf(stderr, "bridge: dir CHANGE up=%d dn=%d lt=%d rt=%d mask=%08x\n",
                    ph_up, ph_dn, ph_lt, ph_rt, (unsigned)mask);
            last_up=ph_up; last_dn=ph_dn; last_lt=ph_lt; last_rt=ph_rt;
        }
        if (now - last_diag >= 2000) {
            last_diag = now;
            fprintf(stderr, "bridge: dir up=%d dn=%d lt=%d rt=%d mask=%08x\n",
                    ph_up, ph_dn, ph_lt, ph_rt, (unsigned)mask);
        }
        usleep(10000); /* 10 ms poll */
    }
    return 0;
}
