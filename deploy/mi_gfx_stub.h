/* RK3036G MI GFX stub -- no real MI GFX hardware on this SoC.
 *
 * Companion to mi_sys.h. Provides the MI_GFX_* types/functions referenced by
 * the upstream miyoo HW-scaling block in plat_sdl.c so it compiles on RK3036G.
 * MI_GFX_BitBlit is a no-op; GFX_BlitSurfaceExec falls back to SDL_BlitSurface
 * at runtime (see mi_sys.h note).
 */
#ifndef RK3036G_MI_GFX_STUB
#define RK3036G_MI_GFX_STUB

#include <stdint.h>

#ifndef MI_U16
typedef uint16_t MI_U16;
#endif
#ifndef MI_PHY
typedef uintptr_t MI_PHY;
#endif
#ifndef MI_BOOL
typedef int MI_BOOL;
#endif

typedef int MI_GFX_Rotate_e;
typedef int MI_GFX_Mirror_e;
typedef int MI_GFX_DFB_BLD_One;

#ifndef FALSE
#define FALSE 0
#endif
#define E_MI_GFX_DFB_BLD_ONE 0

typedef enum {
    E_MI_GFX_FMT_RGB565 = 0,
    E_MI_GFX_FMT_ARGB1555,
    E_MI_GFX_FMT_ARGB4444,
    E_MI_GFX_FMT_ABGR4444,
    E_MI_GFX_FMT_RGBA4444,
    E_MI_GFX_FMT_BGRA4444,
    E_MI_GFX_FMT_ARGB8888,
    E_MI_GFX_FMT_RGBA8888,
    E_MI_GFX_FMT_BGRA8888,
    E_MI_GFX_FMT_ABGR8888
} MI_GFX_ColorFmt_e;

typedef struct {
    MI_PHY             phyAddr;
    uint32_t           u32Width;
    uint32_t           u32Height;
    uint32_t           u32Stride;
    MI_GFX_ColorFmt_e  eColorFmt;
} MI_GFX_Surface_t;

typedef struct {
    int32_t  s32Xpos;
    int32_t  s32Ypos;
    uint32_t u32Width;
    uint32_t u32Height;
} MI_GFX_Rect_t;

typedef struct {
    MI_GFX_DFB_BLD_One eSrcDfbBldOp;
    MI_GFX_Rotate_e    eRotate;
    MI_GFX_Mirror_e    eMirror;
} MI_GFX_Opt_t;

static inline void MI_GFX_BitBlit(MI_GFX_Surface_t *src, MI_GFX_Rect_t *sr,
                                  MI_GFX_Surface_t *dst, MI_GFX_Rect_t *dr,
                                  MI_GFX_Opt_t *opt, MI_U16 *fence) {
    (void)src; (void)sr; (void)dst; (void)dr; (void)opt; (void)fence;
}
static inline void MI_GFX_WaitAllDone(MI_BOOL wait, MI_U16 fence) {
    (void)wait; (void)fence;
}

#endif /* RK3036G_MI_GFX_STUB */
