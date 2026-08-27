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
#define DIAG_BIN      "/mnt/sdcard/cubegm/diag"

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

/* v10.8 ALSA 官方机制（源码级，alsa-lib src/conf.c:4560-4568 + pcm.c:2720-2722）：
 *   ALSA_CONFIG_PATH 必须指向【已存在的配置文件】（默认 /usr/share/alsa/alsa.conf）。
 *   snd_config_update_r 用 fopen() 读它；文件不存在/是目录都会 fopen 失败、只报
 *   "cannot access file" 不中断 -> 配置树为空 -> snd_pcm_open("default") 报 Unknown PCM。
 *   设备 rootfs 无任何 ALSA 配置（squashfs 只读，/etc 不可写），payload 在
 *   /mnt/sdcard/cubegm/asound.conf 直接提供 pcm.default -> hw:0,0（build.sh
 *   STAGE 9 已打包）。因此不必复制到 /etc 或 /tmp —— 直接把 ALSA_CONFIG_PATH
 *   指向 TF 卡上该文件即可，最简且消除只读问题。本函数仅校验存在性。 */
static void ensure_asound_conf(void) {
    FILE *f = fopen("/mnt/sdcard/cubegm/asound.conf", "r");
    if (!f) {
        hlog("icube: cubegm/asound.conf MISSING (audio may be silent)\n");
        return;
    }
    fclose(f);
    hlog("icube: asound.conf OK at /mnt/sdcard/cubegm/asound.conf\n");
}

/* v10.9 开机即 Debug（用户硬性指令 2026-08-27）：
 * 主路径「替换 icube」开机后立即在后台派生 diag：
 *   - diag all    -> /mnt/sdcard/diag_report.txt（sysinfo/input/display/audio/cores）
 *   - diag keylog -> /mnt/sdcard/keylog.txt（持续键位/轴事件日志，守护进程）
 * 父进程不 wait（避免阻塞 retroarch 启动）。fallback 路径 zhijack.sh 已有同款。
 * 若 diag 缺失（payload 异常）则不阻塞启动，仅记录。 */
static void run_diag_bg(const char *arg) {
    pid_t pid = fork();
    if (pid < 0) { hlog("icube: fork diag failed\n"); return; }
    if (pid == 0) {
        /* 子进程：diag 输出重定向到 /dev/null（diag 自身写 report/keylog 文件） */
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        execl(DIAG_BIN, "diag", arg, (char *)NULL);
        _exit(127);
    }
    /* 父进程不 wait —— diag 后台运行，retroarch 立即启动 */
    char buf[128];
    snprintf(buf, sizeof buf, "icube: diag %s forked (bg)\n", arg);
    hlog(buf);
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
    setenv("HOME", WORK_DIR, 1);     /* v11.0: 修 "//.config" 双斜杠 —— RetroArch
                                        getenv("HOME") 拼用户路径，未设则解析为空 */
    setenv("XDG_CONFIG_HOME", WORK_DIR "/configs", 1);
    setenv("LD_LIBRARY_PATH",
           "/mnt/sdcard/cubegm/lib:/mnt/sdcard/cubegm/usr/lib", 1);
    /* ALSA v10.8 官方机制：ALSA_CONFIG_PATH 直接指向 TF 卡上已存在的
       /mnt/sdcard/cubegm/asound.conf（build.sh STAGE 9 已打包部署），
       定义 pcm.default->hw:0,0。无需写入 /etc 或 /tmp（rootfs squashfs 只读）。 */
    ensure_asound_conf();
    setenv("ALSA_CONFIG_PATH", "/mnt/sdcard/cubegm/asound.conf", 1);
    set_cpu_performance();
    if (chdir(WORK_DIR) != 0) hlog("icube: chdir WORK_DIR failed (continuing)\n");

    /* 1.5 开机即 Debug（v10.9）：后台派 diag all + diag keylog，不阻塞 retroarch */
    run_diag_bg("all");
    run_diag_bg("keylog");

    /* 2. supervisor：循环 exec retroarch（自带 RGUI 菜单 + libretro 核心加载） */
    run_supervisor();

    return 0;
}