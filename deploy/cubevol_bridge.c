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

static void upd(int code, int on) { if (code >= 0 && code < KEY_CNT) g_down[code] = on; }

/* Direction keys (d-pad hat + analog stick) go through a 40 ms pulse on the
 * press edge instead of a level. FrogUI's menu is edge-triggered
 * (`up && !up_last`), and the 10 ms poll here can sample mechanical hat
 * bounce into multiple level transitions across frame boundaries -> one press
 * moves 2-3 rows (observed on device). A short pulse makes every physical
 * press produce exactly one rising edge. Games don't read this shm (they use
 * picoarch's own evdev input), so pulses can't break in-game hold. */
#define DIR_PULSE_MS 40
struct dir_state { int phys; int pulse; long deadline_ms; };
static struct dir_state g_dir[4];   /* 0=UP 1=DOWN 2=LEFT 3=RIGHT */
static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
/* feed the physical level of a direction, returns the pulse level to OR in */
static int dir_feed(int idx, int phys) {
    long now = now_ms();
    if (!phys) {
        g_dir[idx].phys = 0;
        g_dir[idx].pulse = 0;
        return 0;
    }
    if (!g_dir[idx].phys) {           /* rising edge -> start pulse */
        g_dir[idx].pulse = 1;
        g_dir[idx].deadline_ms = now + DIR_PULSE_MS;
    }
    g_dir[idx].phys = 1;
    if (g_dir[idx].pulse && now >= g_dir[idx].deadline_ms)
        g_dir[idx].pulse = 0;          /* pulse expired while held */
    return g_dir[idx].pulse;
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
            fds[nfd++] = fd;
            fprintf(stderr, "cubevol_bridge: watching %s\n", path);
        } else close(fd);
    }
    if (nfd == 0) { fprintf(stderr, "cubevol_bridge: no evdev devices\n"); return 1; }

    struct input_event ev;
    for (;;) {
        for (int i = 0; i < nfd; i++) {
            ssize_t rd;
            while ((rd = read(fds[i], &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
                if (ev.type == EV_KEY)
                    upd(ev.code, ev.value != 0);
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
         * merged per-direction then fed through the pulse debouncer. */
        int ph_up = 0, ph_dn = 0, ph_lt = 0, ph_rt = 0;
        for (int i = 0; i < nfd; i++) {
            struct input_absinfo ai;
            if (ioctl(fds[i], EVIOCGABS(ABS_HAT0X), &ai) == 0) {
                if (ai.value < 0) ph_lt = 1;
                if (ai.value > 0) ph_rt = 1;
            }
            if (ioctl(fds[i], EVIOCGABS(ABS_HAT0Y), &ai) == 0) {
                if (ai.value < 0) ph_up = 1;
                if (ai.value > 0) ph_dn = 1;
            }
            /* Analog stick: require a non-zero reading — an unplugged port of
             * a 2-in-1 pad reports ABS all-zero, and 0 < min+lz would stick
             * LEFT/UP forever otherwise. */
            if (ioctl(fds[i], EVIOCGABS(ABS_X), &ai) == 0 && ai.value != 0) {
                int lz = (ai.maximum - ai.minimum) / 4;
                if (ai.value < ai.minimum + lz) ph_lt = 1;
                if (ai.value > ai.maximum - lz) ph_rt = 1;
            }
            if (ioctl(fds[i], EVIOCGABS(ABS_Y), &ai) == 0 && ai.value != 0) {
                int lz = (ai.maximum - ai.minimum) / 4;
                if (ai.value < ai.minimum + lz) ph_up = 1;
                if (ai.value > ai.maximum - lz) ph_dn = 1;
            }
        }
        if (dir_feed(0, ph_up)) mask |= 1u << K_UP;
        if (dir_feed(1, ph_dn)) mask |= 1u << K_DOWN;
        if (dir_feed(2, ph_lt)) mask |= 1u << K_LEFT;
        if (dir_feed(3, ph_rt)) mask |= 1u << K_RIGHT;
        *m = mask;
        usleep(10000); /* 10 ms poll */
    }
    return 0;
}
