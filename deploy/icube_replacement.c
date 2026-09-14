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
 * release-1.0 (2026-09-14, 基于 478=c82e9c5)：去掉全部日志生成——
 *   - hlog 静默（icube.log 不再写）；retroarch 子进程 stdout/stderr 重定向 /dev/null；
 *   - 不再注入 --verbose / --log-file（retroarch_ra.log 不再生成）；
 *   - 不再后台 fork diag（diag_report.txt / keylog.txt 不再生成）；
 *   - diag 二进制仍随 payload 部署，仅作手动诊断（sh diag <module>）。
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

#define LOG_PATH      "/dev/null"            /* release-1.0: 不写 icube.log */
#define WORK_DIR      "/mnt/sdcard/cubegm"
#define RETROARCH     "/mnt/sdcard/cubegm/retroarch"
#define RETROARCH_CFG "/mnt/sdcard/cubegm/retroarch.cfg"
/* release-1.0: RETROARCH_LOG / DIAG_BIN 已删（日志静默；diag 仅手动运行） */

static void hlog(const char *msg) {
    /* release-1.0: 启动器完全静默（LOG_PATH=/dev/null），hlog 保留仅为代码可读性 */
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

/* v11.6 音频根治（2026-08-28，rootfs 官方机制 + ~/.asoundrc，不打补丁）：
 *   设备 rootfs 自带完整 /usr/share/alsa/alsa.conf（官方 pcm.default = empty->plug->hw card0，
 *   及 @hooks 自动加载 /etc/asound.conf 与 ~/.asoundrc）。此前用 ALSA_CONFIG_PATH 覆盖整棵
 *   配置树反而断链（hw:0,0/hw:0,1/default 全 Unknown/ENOENT，设备 diag 实证）。
 *   v11.6 不再设置 ALSA_CONFIG_PATH：HOME=/mnt/sdcard/cubegm 已由下方 setenv 设定，
 *   官方 alsa.conf 的 @hooks 会自动 include /mnt/sdcard/cubegm/.asoundrc（payload 已部署），
 *   其中把 pcm.!default 定义为 plug->route->multi(hw:0,0 HDMI + hw:0,1 内置扬声器) 双输出
 *   （内联 type hw，禁字符串 "hw:0,0"）。本机 ALSA 模拟已验证合并与解析正确。 */

/* release-1.0：run_diag_bg 已删（不再开机 fork diag；diag 仅手动运行） */

/* supervisor：循环 exec retroarch，崩溃后重启（复刻原厂 icube 的 waitpid 监控）。 */
static void run_supervisor(void) {
    int restart_count = 0;
    for (;;) {
        pid_t pid = fork();
        if (pid == 0) {
            /* release-1.0: retroarch 子进程 stdout/stderr 静默（不生成 retroarch.log） */
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
            /* RetroArch 自带 RGUI 菜单。--menu 显式声明「无 content 也要驻留菜单」，
             * 缺它会启动后立即退出（崩溃重启循环）。
             * release-1.0：不再注入 --verbose / --log-file（retroarch_ra.log 不再生成）。
             * 注：本版不设 ALSA_CONFIG_PATH（397 基线音频机制，rootfs 官方链
             * + ~/.asoundrc），避免引入 399/400 疑似宕机变量。 */
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
    /* v11.6：不再覆盖 ALSA_CONFIG_PATH。rootfs 官方 alsa.conf 的 @hooks 会根据
       HOME=/mnt/sdcard/cubegm 自动加载 ~/.asoundrc（双输出定义，payload 已部署）。 */
    set_cpu_performance();
    if (chdir(WORK_DIR) != 0) hlog("icube: chdir WORK_DIR failed (continuing)\n");

    /* release-1.0：不再后台派 diag（diag_report.txt / keylog.txt 不再生成）。
     * diag 二进制仍随 payload 部署，手动运行 sh diag <module> 仍可用。 */

    /* 2. supervisor：循环 exec retroarch（自带 RGUI 菜单 + libretro 核心加载） */
    run_supervisor();

    return 0;
}