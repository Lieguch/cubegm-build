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
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>

#define LOG_PATH      "/mnt/sdcard/icube.log"
#define WORK_DIR      "/mnt/sdcard/cubegm"
#define RETROARCH     "/mnt/sdcard/cubegm/retroarch"
#define RETROARCH_CFG "/mnt/sdcard/cubegm/retroarch.cfg"
#define RETROARCH_LOG   "/mnt/sdcard/retroarch.log"
/* icube-owned video-debug override (rewritten every boot; NOT the user cfg).
 * Loaded via RetroArch --appendconfig (configuration.c L6603, same
 * check_verbosity_settings path as the main cfg). Its keys win over
 * retroarch.cfg because appendconfig is loaded after and is additive. */
#define RETROARCH_DEBUG_CFG "/mnt/sdcard/cubegm/retroarch_debug.cfg"
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

/* GPU 降频（RK3036 Mali-400 devfreq 根治，2026-09-20 v2）：
 *   论坛成功案例：原厂 Mali devfreq 只有两档 200MHz/400MHz，启动默认上 400MHz
 *   高档，在该频点 GPU 不稳（LibreELEC RK3036 实测 lockup，CubeGM 实测 eglGetDisplay
 *   NULL）。降到 200MHz 低档即可稳定启用 GLES。
 *   v2 修复（2026-09-20 统一日志分析）：
 *   - v1 的 write() 返回值被 (void) 吞掉，hlog 报"locked"但实际写失败
 *   - v1 先写 min_freq 再写 max_freq；内核要求 min ≤ max，但 min_freq sysfs
 *     在 simple_ondemand governor 下可能拒绝写（diag 实测 min_freq=400MHz 未变）
 *   - v2 正确顺序：①切 governor=performance（锁定最高频）②写 max_freq=200MHz
 *     （先降上限）③写 min_freq=200MHz（再降下限，此时 min=max=200MHz）
 *   - 每步检查 write() 返回值，hlog 报真实结果 */
static void downclock_gpu(void) {
    const char *bus = "/sys/class/devfreq";
    DIR *d = opendir(bus);
    if (!d) { hlog("icube: GPU devfreq: /sys/class/devfreq not accessible\n"); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        char p[320];
        if (e->d_name[0] == '.') continue;
        if (!strstr(e->d_name, "gpu") && !strstr(e->d_name, "mali")) continue;

        char msg[256];
        int ok_gov = 0, ok_max = 0, ok_min = 0;

        /* ① 切 governor=performance（锁定最高频，使手动 min/max 写入生效） */
        snprintf(p, sizeof p, "%s/%s/governor", bus, e->d_name);
        int fd = open(p, O_WRONLY);
        if (fd >= 0) {
            ssize_t w = write(fd, "performance\n", 12);
            ok_gov = (w > 0);
            close(fd);
        }
        /* ② 写 max_freq=200MHz（先降上限，保证 min ≤ max 不变量成立） */
        snprintf(p, sizeof p, "%s/%s/max_freq", bus, e->d_name);
        fd = open(p, O_WRONLY);
        if (fd >= 0) {
            ssize_t w = write(fd, "200000000\n", 10);
            ok_max = (w > 0);
            close(fd);
        }
        /* ③ 写 min_freq=200MHz（再降下限，此时 min=max=200MHz） */
        snprintf(p, sizeof p, "%s/%s/min_freq", bus, e->d_name);
        fd = open(p, O_WRONLY);
        if (fd >= 0) {
            ssize_t w = write(fd, "200000000\n", 10);
            ok_min = (w > 0);
            close(fd);
        }

        snprintf(msg, sizeof msg,
            "icube: GPU devfreq %s downclock: governor=%s max_freq=%s min_freq=%s\n",
            e->d_name,
            ok_gov ? "performance OK" : "FAIL",
            ok_max ? "200MHz OK" : "FAIL",
            ok_min ? "200MHz OK" : "FAIL");
        hlog(msg);
    }
    closedir(d);
}

/* v12.0 GPU 硬件加速根治（2026-09-20 gbm/drm 变体）：
 *   设备显示栈 = DRM(/dev/dri/card0 + renderD128, fbs=0 无 fbdev 双缓冲)。
 *   旧 libmali fbdev 变体走 /dev/fb0 framebuffer panning → eglGetDisplay EGL_NO_DISPLAY。
 *   换 gbm(drm-dma_buf) 变体后, RetroArch 需用 kms context (drm_ctx.c, ident="kms")。
 *   首次启动写默认 cfg(video_context_driver="kms"), 用户手改的 retroarch.cfg 永不被覆盖。 */
static void write_default_cfg(void) {
    FILE *f = fopen(RETROARCH_CFG, "r");
    if (f) { fclose(f); return; }   /* user cfg exists -> never touch */
    f = fopen(RETROARCH_CFG, "w");
    if (!f) { hlog("icube: write retroarch.cfg FAILED\n"); return; }
    fprintf(f,
        "# CubeGM default cfg (generated; edit freely, never overwritten once present)\n"
        "video_driver = \"gl\"\n"
        "video_context_driver = \"kms\"\n");
    fclose(f);
}

/* v12.1 视频 Debug 日志全开（2026-09-23 gpu-probe 516 刷机验证后）：
 * 开 RA 官方"视频全 debug"的两级门控：
 *   ① CLI --verbose        -> verbosity_enable()        (retroarch.c L7855)
 *   ② cfg  frontend_log_level=0 -> verbosity_set_log_level(0)
 *                             (configuration.c L6418; RARCH_DBG 门控在
 *                              verbosity.c L527: verbosity ON 且 level<=0 才放行)
 * RA 默认 frontend_log_level=1 把 RARCH_DBG 全滤掉；drm_ctx.c/egl_common.c 里
 * 所有 [KMS]/[EGL] 调试行都是 RARCH_DBG 级别 -> 必须置 0 才全出。
 *
 * 走 --appendconfig 官方通道（configuration.c L6603，与主 cfg 同一条
 * check_verbosity_settings；appendconfig 后加载，键级覆盖主 cfg）。
 * 机器 owned，每 boot 重写（O_WRONLY 整文件），retroarch.cfg 用户红线不碰。
 * 用户若手改 retroarch.cfg 的 video_context_driver 仍可生效（这里不写它）。 */
static void write_debug_cfg(void) {
    FILE *f = fopen(RETROARCH_DEBUG_CFG, "w");
    if (!f) { hlog("icube: write retroarch_debug.cfg FAILED\n"); return; }
    fprintf(f,
        "# CubeGM video-debug override (icube-owned, rewritten every boot)\n"
        "# NOT the user config: RetroArch loads it via --appendconfig and its\n"
        "# keys override retroarch.cfg (additive, loaded last).\n"
        "frontend_log_level = 0\n"
        "log_to_file = true\n");
    fclose(f);
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

/* 前台串行跑 diag（等待完成）：用于 RA 启动前一次性取证 live EGL/GBM/GL 链。
 * diag 内部按 mod 分流输出到 diag_video_report.txt，与后台 diag all 的
 * diag_report.txt 不冲突；60s 看门狗在 diag 内部，这里不多加限时。 */
static void run_diag_fg(const char *arg) {
    pid_t pid = fork();
    if (pid < 0) { hlog("icube: fork diag fg failed\n"); return; }
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        execl(DIAG_BIN, "diag", arg, (char *)NULL);
        _exit(127);
    }
    char buf[128];
    snprintf(buf, sizeof buf, "icube: diag %s (fg, blocking) started\n", arg);
    hlog(buf);
    int st = 0;
    waitpid(pid, &st, 0);
    snprintf(buf, sizeof buf, "icube: diag %s (fg) exited status=%d\n", arg, st);
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
                  /* v12.1 视频 Debug 日志全开：官方 appendconfig 通道
                   * （configuration.c L6603）加载 icube-owned 的
                   * retroarch_debug.cfg，键级覆盖用户 retroarch.cfg。
                   * frontend_log_level=0 + --verbose => RARCH_DBG 全放行。 */
                  "--appendconfig=" RETROARCH_DEBUG_CFG,
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
    downclock_gpu();   /* 启动 retroarch 前把 Mali GPU 锁到 200MHz（降频根治） */
    if (chdir(WORK_DIR) != 0) hlog("icube: chdir WORK_DIR failed (continuing)\n");
    write_default_cfg();   /* v12.0: 首次写 video_context_driver="kms" 默认 cfg */
    write_debug_cfg();     /* v12.1: 视频 Debug 全开 override (appendconfig, 每 boot 重写) */

    /* 1.5 开机即 Debug（v10.9）：后台派 diag all + diag keylog，不阻塞 retroarch */
    run_diag_bg("all");
    run_diag_bg("keylog");

    /* 1.6 一次性取证：RA 启动前串行跑 live EGL/GBM/GL 链（diag video），
     * 链尾 DROP_MASTER 后干净退出，再启动 RA，不与 RA 抢 DRM master；
     * 输出到 diag_video_report.txt，不与后台 diag all 的 diag_report.txt 冲突。 */
    run_diag_fg("video");

    /* 2. supervisor：循环 exec retroarch（自带 RGUI 菜单 + libretro 核心加载） */
    run_supervisor();

    return 0;
}