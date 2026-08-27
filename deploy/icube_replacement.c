/* icube_replacement.c — 替换原厂 icube 的启动器（S80icube 环节事前接管）
 *
 * 逆向确认原厂 icube 职责（ShareMemCreat + fork rkgame + waitpid 监控重启）。
 * 本启动器替代它：直接启动 RetroArch（自带 RGUI 菜单 + libretro 核心加载），
 * 原厂 rkgame/driver.so 永远不会启动，因此显示/音频由 RetroArch 自己初始化
 * （SDL 1.2 fbcon 视频 + ALSA 音频），彻底脱离 driver.so。
 *
 * v10.0 (2026-08-26)：方向切换 picoarch+FrogUI → RetroArch。
 *   - exec: retroarch -c /mnt/sdcard/cubegm/retroarch.cfg --menu
 *   - 不再 spawn cubevol_bridge（RetroArch udev/linuxraw 直接读 input 设备）
 *   - 不再写 /tmp/joy_key（FrogUI 专属 shm，已废弃）
 * v10.4 (2026-08-27)：输入切 udev 驱动（libudev-zero），删除 linuxraw 时代 player1 硬编码。
 *   - --menu 必须：官方文档确认「不加载 content 时必须显式 --menu，否则
 *     RetroArch 启动后立即退出」→ 缺它会变成 crash 重启循环。
 *     zhijack.sh 同路径已同步补上。
 *
 * 与原厂 icube 的区别：
 *   - 原厂 fork/execl rkgame（闭源，dlopen driver.so 显示/音频）
 *   - 本启动器 exec retroarch（开源，自己 SDL/ALSA）
 *
 * 安全性（已验证）：
 *   - root.dat 不引用 icube，无校验 → 替换不触发 "sdcard is damaged"
 *   - 无看门狗（icube/rkgame 均无 watchdog 字符串）
 *
 * 编译：arm-linux-gnueabihf-gcc -O2 -Wall icube_replacement.c -o icube_replacement
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>

#define LOG_PATH      "/mnt/sdcard/icube.log"
#define WORK_DIR      "/mnt/sdcard/cubegm"
#define RETROARCH     "/mnt/sdcard/cubegm/retroarch"
#define RETROARCH_CFG "/mnt/sdcard/cubegm/retroarch.cfg"
#define RETROARCH_LOG "/mnt/sdcard/retroarch.log"

static void hlog(const char *msg) {
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    fprintf(f, "%s", msg);
    fclose(f);
}

/* 写 /tmp/tfdevice.env（对齐 zhijack.sh，保留设备几何约定便于诊断与人工覆盖）。 */
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

/* v10.5 ALSA 官方机制（源码级，alsa-lib src/conf.c:4546-4693）：
 *   ALSA_CONFIG_PATH 必须指向【配置文件】（默认 /usr/share/alsa/alsa.conf），
 *   不是目录。snd_config_update_r 用 fopen() 读它；指向目录会 fopen 失败、
 *   只报"cannot access file"不中断 -> 配置树为空 -> snd_pcm_open("default")
 *   报 Unknown PCM（src/pcm/pcm.c:2720-2722）。
 *   asound.conf 是附加配置，靠 alsa.conf 的 @hooks func load 引入；设备 rootfs
 *   无任何 ALSA 配置 -> 直接把 asound.conf 当主配置：定义 pcm.default -> hw:0,0。
 *   本函数确保 /etc/asound.conf 存在（root 可写 /etc）。 */
static void ensure_asound_conf(void) {
    FILE *f = fopen("/etc/asound.conf", "r");
    if (f) { fclose(f); return; }
    FILE *src = fopen("/mnt/sdcard/cubegm/asound.conf", "r");
    if (!src) { hlog("icube: cubegm/asound.conf missing (audio may be silent)\n"); return; }
    FILE *dst = fopen("/etc/asound.conf", "w");
    if (!dst) {
        /* squashfs rootfs 的 /etc 是只读的，fallback 到 /tmp（可写） */
        hlog("icube: /etc is read-only, using /tmp/asound.conf\n");
        dst = fopen("/tmp/asound.conf", "w");
        if (!dst) { fclose(src); return; }
        setenv("ALSA_CONFIG_PATH", "/tmp/asound.conf", 1);
    } else {
        setenv("ALSA_CONFIG_PATH", "/etc/asound.conf", 1);
    }
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, src)) > 0)
        fwrite(buf, 1, n, dst);
    fclose(src);
    fclose(dst);
    hlog("icube: wrote asound.conf (default PCM -> hw:0,0)\n");
}

/* supervisor：循环 exec retroarch，崩溃后重启（复刻原厂 icube 的 waitpid 监控）。 */
static void run_supervisor(void) {
    int restart_count = 0;
    for (;;) {
        pid_t pid = fork();
        if (pid == 0) {
            /* 子进程：把 retroarch 的 stdout/stderr 重定向到日志，便于诊断 */
            int fd = open(RETROARCH_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
            /* RetroArch 自带 RGUI 菜单。--menu 显式声明「无 content 也要驻留菜单」，
             * 缺它会启动后立即退出（崩溃重启循环）。 */
            execl(RETROARCH, "retroarch", "-c", RETROARCH_CFG, "--menu", (char *)NULL);
            hlog("icube: exec retroarch FAILED\n");
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
        snprintf(buf, sizeof buf, "icube: retroarch exited (rc=%d), restart #%d\n",
                 WIFEXITED(status) ? WEXITSTATUS(status) : -1, ++restart_count);
        hlog(buf);
        sleep(1);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    hlog("icube (replacement) v10.0 starting (RetroArch launcher)\n");

    /* 1. 设备环境：tfdevice.env + TF_* 导出 + 库路径 + 黑屏修复 + CPU 调度 */
    write_tfdevice_env();
    setenv("TF_DEVICE", "rk3036g", 1);
    setenv("TF_PANEL_W", "1280", 1);
    setenv("TF_PANEL_H", "720", 1);
    setenv("TF_UI_SCALE", "150", 1);
    setenv("SDL_NOMOUSE", "1", 1);   /* SDL fbcon 黑屏根因修复 */
    setenv("LD_LIBRARY_PATH",
           "/mnt/sdcard/cubegm/lib:/mnt/sdcard/cubegm/usr/lib", 1);
    /* ALSA v10.5 官方机制：ALSA_CONFIG_PATH 指向【配置文件】/etc/asound.conf
       （默认 /usr/share/alsa/alsa.conf；但设备 rootfs 无 ALSA 配置，用 asound.conf
       直接定义 pcm.default->hw:0,0）。ensure_asound_conf 保证文件存在。 */
    ensure_asound_conf();
    setenv("ALSA_CONFIG_PATH", "/etc/asound.conf", 1);
    set_cpu_performance();
    if (chdir(WORK_DIR) != 0) hlog("icube: chdir WORK_DIR failed (continuing)\n");

    /* 2. supervisor：循环 exec retroarch（自带 RGUI 菜单 + libretro 核心加载） */
    run_supervisor();

    return 0;
}