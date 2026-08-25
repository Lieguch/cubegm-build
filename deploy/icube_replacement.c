/* icube_replacement.c — 替换原厂 icube 的启动器（S80icube 环节事前接管）
 *
 * 逆向确认原厂 icube 职责（ShareMemCreat + fork rkgame + waitpid 监控重启）。
 * 本启动器替代它：直接启动 picoarch + FrogUI，原厂 rkgame/driver.so 永远不会启动，
 * 因此显示/音频由 picoarch 自己初始化（DRM dumb buffer + ALSA），彻底脱离 driver.so。
 *
 * 关键事实（逆向 + 源码确认）：
 *   - picoarch 靠 sf3000_is_rk3036() 读 /proc/device-tree/compatible 硬件自检
 *     → rk3036 走 DRM/ALSA/evdev，不依赖 launcher 环境，但 tfdevice.env 双保险。
 *   - FrogUI 菜单输入源 = cubevol_bridge 写 /tmp/joy_key 共享内存（evdev → shm）。
 *   - 共享内存 key = ftok("/tmp/joy_key", 'a')；/tmp/joy_key 文件必须存在。
 *   - 游戏启动：FrogUI 内部 fork+execl picoarch <game_core> <rom>（launcher 不用管）。
 *
 * 与原厂 icube 的区别：
 *   - 原厂 fork/execl rkgame（闭源，dlopen driver.so 显示/音频）
 *   - 本启动器 exec picoarch（开源，自己 DRM/ALSA）
 *
 * 安全性（已验证）：
 *   - root.dat 不引用 icube，无校验 → 替换不触发 "sdcard is damaged"
 *   - 无看门狗（icube/rkgame 均无 watchdog 字符串）
 *
 * 编译：arm-linux-gnueabihf-gcc -O2 -Wall icube_replacement.c -o icube
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>

#define LOG_PATH   "/mnt/sdcard/icube.log"
#define WORK_DIR   "/mnt/sdcard/cubegm"

static void hlog(const char *msg) {
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    fprintf(f, "%s", msg);
    fclose(f);
}

/* 写 /tmp/tfdevice.env（对齐 zhijack.sh；picoarch/FrogUI 会读 TF_* 作为面板几何双保险，
 * 虽然 hardware 自检已能判定 rk3036 + 1280x720，但保留 env 便于诊断与人工覆盖）。 */
static void write_tfdevice_env(void) {
    FILE *f = fopen("/tmp/tfdevice.env", "w");
    if (!f) { hlog("icube: write /tmp/tfdevice.env FAILED\n"); return; }
    fprintf(f,
        "TF_DEVICE=rk3036g\n"
        "TF_PANEL_W=1280\n"
        "TF_PANEL_H=720\n"
        "TF_UI_SCALE=150\n"
        "TF_ASPECT_NUM=16\n"
        "TF_ASPECT_DEN=9\n"
        "TF_ROTATE=0\n"
        "TF_PRESENT=1\n"
        "TF_DRIVER=\n");
    fclose(f);
}

/* CPU 性能调度（帮助模拟器；对齐 zhijack.sh）。硬件不支持则静默跳过。 */
static void set_cpu_performance(void) {
    FILE *f;
    char path[160];
    int i;
    for (i = 0; i < 4; i++) {
        snprintf(path, sizeof path,
                 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", i);
        f = fopen(path, "w");
        if (f) { fprintf(f, "performance\n"); fclose(f); }
    }
}

/* 确保 /tmp/joy_key 文件存在（ftok 需要文件存在才能算 key） */
static void ensure_joy_key_file(void) {
    int fd = open("/tmp/joy_key", O_WRONLY | O_CREAT, 0666);
    if (fd >= 0) close(fd);
}

/* 启动后台进程（cubevol_bridge） */
static pid_t spawn_background(const char *path) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(path, path, (char *)NULL);
        hlog("icube: exec cubevol_bridge FAILED\n");
        _exit(1);
    }
    return pid;
}

/* supervisor：循环 exec picoarch+FrogUI，崩溃后重启（复刻原厂 icube 的 waitpid 监控）。
 * FrogUI 内部 fork+execl 游戏，游戏退出后回到菜单，无需本循环处理游戏。 */
static void run_supervisor(const char *picoarch, const char *core) {
    int restart_count = 0;
    for (;;) {
        pid_t pid = fork();
        if (pid == 0) {
            /* picoarch <core> <content>；FrogUI 菜单按 zhijack.sh 惯例传 core 两次。 */
            /* v9.0: 若有 stockui 则优先用独立原厂 UI（不再依赖 frogui 文件夹浏览器） */
            if (access("/mnt/sdcard/cubegm/stockui", X_OK) == 0) {
                execl("/mnt/sdcard/cubegm/stockui", "stockui", (char *)NULL);
            } else {
                execl(picoarch, picoarch, core, core, (char *)NULL);
            }
            hlog("icube: exec picoarch FAILED\n");
            _exit(1);
        }
        if (pid < 0) {
            hlog("icube: fork failed\n");
            sleep(2);
            continue;
        }
        int status;
        waitpid(pid, &status, 0);
        char buf[160];
        snprintf(buf, sizeof buf, "icube: picoarch exited (rc=%d), restart #%d\n",
                 WIFEXITED(status) ? WEXITSTATUS(status) : -1, ++restart_count);
        hlog(buf);
        sleep(1);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    hlog("icube (replacement) v1.1 starting\n");

    /* 1. 设备环境：tfdevice.env + TF_* 导出 + 库路径 + 黑屏修复 + CPU 调度 */
    write_tfdevice_env();
    setenv("TF_DEVICE", "rk3036g", 1);
    setenv("TF_PANEL_W", "1280", 1);
    setenv("TF_PANEL_H", "720", 1);
    setenv("TF_UI_SCALE", "150", 1);
    setenv("SDL_NOMOUSE", "1", 1);
    setenv("LD_LIBRARY_PATH",
           "/mnt/sdcard/cubegm/lib:/mnt/sdcard/cubegm/usr/lib", 1);
    set_cpu_performance();
    if (chdir(WORK_DIR) != 0) hlog("icube: chdir WORK_DIR failed (continuing)\n");

    /* 2. 确保 /tmp/joy_key 存在（供 cubevol_bridge ftok） */
    ensure_joy_key_file();

    /* 3. 启动输入桥接（evdev → /tmp/joy_key shm，FrogUI 菜单输入源） */
    pid_t bridge = spawn_background("/mnt/sdcard/cubegm/cubevol_bridge");
    (void)bridge;
    hlog("icube: cubevol_bridge spawned\n");

    /* 3.5 等 bridge 创建共享内存（避免 frogui cv_init 竞态） */
    usleep(200000);

    /* 4. supervisor：循环 exec picoarch + FrogUI */
    run_supervisor("/mnt/sdcard/cubegm/picoarch",
                   "/mnt/sdcard/cubegm/cores/frogui_libretro.so");

    return 0;
}