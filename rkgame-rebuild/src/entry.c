/* ============================================================
 * rkgame-rebuild — 自定义入口点（绕过 __libc_start_main@GLIBC_2.34）
 * ============================================================
 *
 * 问题：crt1.o 的 _start 调用 __libc_start_main，而 Ubuntu 22.04 的
 * cross-compiler (glibc 2.35 sysroot) 让 crt1.o 记录了
 * __libc_start_main@GLIBC_2.34 的引用。
 * 设备 glibc 2.29 只导出 __libc_start_main@GLIBC_2.0，
 * ld.so 在运行时找不到 @GLIBC_2.34 → "version GLIBC_2.34 not found" 崩溃。
 *
 * 解决：用 -nostartfiles 禁用默认 CRT 入口，自定义 _rkgame_start 直接调用 main。
 *
 * 注意事项：
 *   - 不做静态构造函数初始化（rkgame 无 static init）
 *   - 不用 TLS 初始化（rkgame 只用 pthread 互斥锁，不用 thread-local）
 *   - _exit() 是 GLIBC_2.0 符号，设备可用
 *   - main 返回 0 表示正常退出
 * ============================================================ */

#define _GNU_SOURCE
#include <unistd.h>

extern int main(int argc, char **argv);

/* 入口点：由 ld.so 通过 -e _rkgame_start 调用。
 * ld.so 已设置好栈、TLS、环境变量等基础环境。
 */
void _rkgame_start(void)
{
    extern int main(int, char **);
    /* rkgame 不读取 argv[1]/argv[2]，config.xml 已提供路径 */
    int ret = main(0, 0);
    _exit(ret);
}
