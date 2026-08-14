#include <stdio.h>
/* Cross-compile smoke test for the RK3036G toolchain.
 * Build:  arm-none-linux-gnueabihf-gcc -march=armv7-a -mtune=cortex-a7 \
 *           -mfpu=neon-vfpv4 -mfloat-abi=hard -O2 --sysroot=<device_rootfs> \
 *           -o hello hello.c
 * Then:   ./toolchain/verify_target_abi.sh hello   (expect PASS)
 */
int main(void) {
    printf("cubegm smoke test: hello from RK3036G toolchain\n");
    return 0;
}
