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

/* v0.3 (2026-08-30，回归修复): 清理残留 libasound —— 音频确定性根治。
 * 旧 payload (399/400/401) 把 crosstool 的 1.2.10 libasound.so.2 打进 cubegm/lib，
 * 其编译期 ALSA_CONFIG_DIR=/home/runner/...（CI 路径）在设备上不存在 → 配置树
 * 加载失败 → "Unknown PCM default" + 设备列表空（default 消失）。402 起 payload
 * 不再打包 libasound，但用户覆盖拷贝不删旧文件 → 残留 1.2.10 仍被 LD_LIBRARY_PATH
 * 优先加载。此处每次启动主动 unlink cubegm/lib 与 cubegm/usr/lib 下的 libasound
 * 残留，强制回落设备 rootfs 原厂 1.1.5（ALSA_CONFIG_DIR=/usr/share/alsa，rootfs
 * 有完整配置树 + @hooks 自动加载 ~/.asoundrc）。不设 ALSA_CONFIG_PATH、不碰其余 lib。
 *
 * v0.11 (2026-09-01, 442 音频回归修复): 同步清 asound.conf / .asoundrc 残留。
 * 旧 payload 的 asound.conf（v11.8 写死 pcm.!default = plug → hw:0,0，无 format
 * 锁）会与新 payload（v0.11 锁 S16）冲突 → 用户覆盖拷贝不删旧文件 → 旧 asound.conf
 * 被 ALSA_CONFIG_PATH=~/.asoundrc 加载（rootfs alsa.conf @hooks 自动 include），
 * 导致 S32 underrun 复发。同步 unlink 强制回落新 payload 的 asound.conf。 */
static void cleanup_stale_libasound(void) {
    static const char *paths[] = {
        /* libasound 残留（v0.3 根治） */
        WORK_DIR "/lib/libasound.so.2",
        WORK_DIR "/lib/libasound.so",
        WORK_DIR "/usr/lib/libasound.so.2",
        WORK_DIR "/usr/lib/libasound.so",
        /* asound 配置残留（v0.11 根治） */
        WORK_DIR "/.asoundrc",
        WORK_DIR "/asound.conf",
        /* retroarch 旧 cfg 备份（含旧 audio_format=s32 等脏值） */
        WORK_DIR "/configs/retroarch/retroarch.cfg.bak",
        NULL
    };
    int i;
    for (i = 0; paths[i]; i++) {
        if (unlink(paths[i]) == 0)
            hlog("icube: removed stale libasound/asound (fallback to rootfs/device original)\n");
    }
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
             * 缺它会启动后立即退出（崩溃重启循环）。
             * v11.11 DEBUG 日志：--verbose --log-file 让 RetroArch 把 [INFO]/
             * [udev]/[Autoconf] 全部写进独立文件。之前的坑：stdout 重定向到
             * 文件后是全缓冲（4 KB），RetroArch 不退出就不 flush，导致
             * retroarch.log 里只剩 stderr 的 ALSA 错误、[INFO] 全部丢失，
             * 手柄 udev 枚举/autoconfig 匹配是否发生完全看不见。
             * 注：本版不设 ALSA_CONFIG_PATH（397 基线音频机制，rootfs 官方链
             * + ~/.asoundrc），避免引入 399/400 疑似宕机变量。 */
            execl(RETROARCH, "retroarch", "-c", RETROARCH_CFG, "--menu",
                  "--verbose",
                  "--log-file=/mnt/sdcard/retroarch_ra.log", (char *)NULL);
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

    /* v0.3 (2026-08-30，回归修复): 启动即清理残留 libasound（旧 payload 的 1.2.10
       CI-路径版），强制回落设备 rootfs 原厂 1.1.5（正确 ALSA_CONFIG_DIR=/usr/share/alsa）。 */
    cleanup_stale_libasound();

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

    /* 1.5 开机即 Debug（v10.9）：后台派 diag all + diag keylog，不阻塞 retroarch */
    run_diag_bg("all");
    run_diag_bg("keylog");

    /* 2. supervisor：循环 exec retroarch（自带 RGUI 菜单 + libretro 核心加载） */
    run_supervisor();

    return 0;
}