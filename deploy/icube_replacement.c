/* icube_replacement.c — 替换原厂 icube 的启动器
 *
 * 复刻原厂 icube 的职责（逆向确认）：
 *   1. 原厂 icube：ShareMemCreat（创建共享内存）+ fork rkgame + waitpid 监控
 *   2. 本启动器：启动 cubevol_bridge（它自己创建共享内存 + 读 evdev）+ exec picoarch
 *
 * 关键事实（逆向 + 源码确认）：
 *   - 共享内存 key = ftok("/tmp/joy_key", 'a')（不是 /tmp）
 *   - cubevol_bridge 自己 shmget(IPC_CREAT) 创建共享内存
 *   - frogui 只 shmget(0666) 读共享内存（不创建）
 *   - 所以启动器只需：确保 /tmp/joy_key 文件存在 → 启动 bridge → exec picoarch
 *
 * 与原厂 icube 的区别：
 *   - 原厂 fork/execl rkgame（闭源，靠 dlopen driver.so 显示/音频）
 *   - 本启动器 exec picoarch（开源，自己 DRM/ALSA），彻底绕开 driver.so
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

#define LOG_PATH "/mnt/sdcard/icube.log"

static void hlog(const char *msg) {
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    fprintf(f, "%s", msg);
    fclose(f);
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
        /* 子进程：exec bridge */
        execl(path, path, (char *)NULL);
        hlog("icube: exec cubevol_bridge FAILED\n");
        _exit(1);
    }
    return pid;
}

/* supervisor：循环 exec picoarch，退出后重启（复刻 icube 的 waitpid 监控） */
static void run_supervisor(const char *picoarch, const char *core) {
    int restart_count = 0;
    for (;;) {
        pid_t pid = fork();
        if (pid == 0) {
            execl(picoarch, picoarch, core, (char *)NULL);
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
        char buf[128];
        snprintf(buf, sizeof buf, "icube: picoarch exited (rc=%d), restart #%d\n",
                 WIFEXITED(status) ? WEXITSTATUS(status) : -1, ++restart_count);
        hlog(buf);
        sleep(1);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    hlog("icube (replacement) v1.0 starting\n");

    /* 1. 确保 /tmp/joy_key 存在（供 ftok） */
    ensure_joy_key_file();

    /* 2. 环境变量（复用黑屏修复 + 库路径） */
    setenv("SDL_NOMOUSE", "1", 1);
    setenv("LD_LIBRARY_PATH", "/sdcard/cubegm/lib:/sdcard/cubegm", 1);

    /* 3. 启动输入桥接（后台，自己创建共享内存 + 读 evdev） */
    pid_t bridge = spawn_background("/mnt/sdcard/cubegm/cubevol_bridge");
    (void)bridge;
    hlog("icube: cubevol_bridge spawned\n");

    /* 3.5 等 bridge 创建共享内存（避免 frogui cv_init 竞态） */
    usleep(200000);  /* 200ms，足够 bridge exec + shmget IPC_CREAT */

    /* 4. supervisor：循环 exec picoarch + FrogUI */
    run_supervisor("/mnt/sdcard/cubegm/picoarch",
                   "/mnt/sdcard/cubegm/cores/frogui_libretro.so");

    return 0;
}
