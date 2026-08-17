/* RK3036G MI (MStar) stub -- no real MI hardware on this SoC.
 *
 * The upstream picoarch 'unix' platform builds plat_sdl.c, whose miyoo
 * HW-scaling block (#ifndef PLATFORM_SF3000) #includes <mi_sys.h> and calls
 * MI_SYS_* APIs that only exist on MStar-based Miyoo/Trimui devices. RK3036G
 * is a standard Rockchip buildroot Linux handheld with no MI subsystem, so we
 * provide malloc-backed shims so the block COMPILES and LINKS.
 *
 * At runtime the MI blit path (GFX_BlitSurfaceExec) is gated on BOTH the src
 * and dst SDL surfaces carrying a MI physical address (pixelsPa); the generic
 * SDL 'screen' surface never sets pixelsPa, so it falls back to
 * SDL_BlitSurface and the NEON software scaler (scale1x..6x_n16) still drives
 * the display. This keeps the generic 'unix' platform (no -DPLATFORM_SF3000)
 * intact and avoids the ~16 SF3000-specific code paths.
 */
#ifndef RK3036G_MI_STUB
#define RK3036G_MI_STUB

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef int       MI_BOOL;
typedef uint16_t  MI_U16;
typedef uintptr_t MI_PHY;

static inline int MI_SYS_MMA_Alloc(void *owner, size_t size, MI_PHY *pa) {
    (void)owner;
    void *p = malloc(size);
    if (!p) return -1;
    *pa = (MI_PHY)(uintptr_t)p;
    return 0;
}
static inline void MI_SYS_MemsetPa(MI_PHY pa, int c, size_t size) {
    memset((void *)(uintptr_t)pa, c, size);
}
static inline int MI_SYS_Mmap(MI_PHY pa, size_t size, void **va, MI_BOOL cachable) {
    (void)size; (void)cachable;
    *va = (void *)(uintptr_t)pa;
    return 0;
}
static inline int MI_SYS_Munmap(void *va, size_t size) {
    (void)va; (void)size;
    return 0;
}
static inline int MI_SYS_MMA_Free(MI_PHY pa) {
    free((void *)(uintptr_t)pa);
    return 0;
}
static inline void MI_SYS_FlushInvCache(void *virAddr, size_t size) {
    (void)virAddr; (void)size;
}

#endif /* RK3036G_MI_STUB */
