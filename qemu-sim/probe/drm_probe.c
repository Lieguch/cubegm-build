/* drm_probe.c —— 取 DRM_IOCTL_MODE_CREATE_DUMB 的真实 errno，并枚举设备能力。
 * 静态链接的 ARM32 程序；放进 initramfs 后由 S00cgmmod 之后运行。 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

struct drm_mode_create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct drm_mode_map_dumb    { uint32_t handle, pad; uint64_t offset; };
struct drm_mode_destroy_dumb{ uint32_t handle; };
struct drm_version { int major, minor, patchlevel; unsigned long name_len, date_len, desc_len;
                     char *name, *date, *desc; };
struct drm_get_cap { uint64_t capability; uint64_t value; };

#define DRM_IOCTL_BASE 'd'
#define DRM_IOWR(nr,t)  _IOWR(DRM_IOCTL_BASE,nr,t)
#define DRM_IOR(nr,t)   _IOR(DRM_IOCTL_BASE,nr,t)
#define DRM_IOCTL_VERSION          DRM_IOWR(0x00, struct drm_version)
#define DRM_IOCTL_GET_CAP          DRM_IOWR(0x0c, struct drm_get_cap)
#define DRM_IOCTL_MODE_CREATE_DUMB DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB    DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB DRM_IOWR(0xB4, struct drm_mode_destroy_dumb)
#define DRM_CAP_DUMB_BUFFER 0x1
#define DRM_CAP_DUMB_PREFERRED_DEPTH 0x3

static void show(const char *tag, int r, struct drm_mode_create_dumb *d) {
    printf("  %-34s r=%d errno=%d(%s)", tag, r, errno, strerror(errno));
    if (d) printf(" handle=%u pitch=%u size=%llu", d->handle, d->pitch,
                  (unsigned long long)d->size);
    printf("\n");
}

int main(void) {
    printf("=== CGM DRM PROBE ===\n");
    DIR *d = opendir("/dev/dri");
    printf("/dev/dri: %s\n", d ? "存在" : "缺失");
    if (d) { struct dirent *e; while ((e = readdir(d))) if (e->d_name[0] != '.') printf("  entry: %s\n", e->d_name); closedir(d); }

    int fd = open("/dev/dri/card0", O_RDWR);
    printf("open(/dev/dri/card0, O_RDWR) = %d errno=%d(%s)\n", fd, errno, strerror(errno));
    if (fd < 0) { fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC); printf("  重试(CLOEXEC) = %d errno=%d(%s)\n", fd, errno, strerror(errno)); }
    if (fd < 0) { printf("=== 无法打开 DRM 节点，结束 ===\n"); return 1; }

    char nm[64] = {0}, dt[64] = {0}, ds[128] = {0};
    struct drm_version v; memset(&v, 0, sizeof v);
    v.name = nm; v.name_len = sizeof nm - 1;
    v.date = dt; v.date_len = sizeof dt - 1;
    v.desc = ds; v.desc_len = sizeof ds - 1;
    int r = ioctl(fd, DRM_IOCTL_VERSION, &v);
    printf("DRM_IOCTL_VERSION r=%d errno=%d(%s) drm=%d.%d.%d name='%s'\n", r, errno, strerror(errno),
           v.major, v.minor, v.patchlevel, nm);

    struct { uint64_t cap; const char *n; } caps[] = {
        {0x1, "DUMB_BUFFER"}, {0x2, "VBLANK_HIGH_CRTC"}, {0x3, "DUMB_PREFERRED_DEPTH"},
        {0x6, "PRIME"}, {0x7, "TIMESTAMP_MONOTONIC"}, {0x9, "ADDFB2_MODIFIERS"},
        {0x13, "ATOMIC"}, {0x12, "ASYNC_PAGE_FLIP"},
    };
    for (unsigned i = 0; i < sizeof(caps)/sizeof(caps[0]); i++) {
        struct drm_get_cap c; c.capability = caps[i].cap; c.value = 0xdeadbeef;
        r = ioctl(fd, DRM_IOCTL_GET_CAP, &c);
        printf("  CAP %-22s r=%d errno=%d(%s) value=%llu\n", caps[i].n, r, errno,
               strerror(errno), (unsigned long long)c.value);
    }

    printf("--- CREATE_DUMB 参数扫描 ---\n");
    struct { uint32_t w, h, bpp; } cs[] = {
        {640,480,32},{640,480,24},{640,480,16},{1280,720,32},{1280,720,16},{1920,1080,32},{320,240,32},
    };
    for (unsigned i = 0; i < sizeof(cs)/sizeof(cs[0]); i++) {
        struct drm_mode_create_dumb dd; memset(&dd, 0, sizeof dd);
        dd.width = cs[i].w; dd.height = cs[i].h; dd.bpp = cs[i].bpp;
        errno = 0;
        r = ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dd);
        char tag[64]; snprintf(tag, sizeof tag, "CREATE_DUMB %ux%u bpp=%u", cs[i].w, cs[i].h, cs[i].bpp);
        show(tag, r, &dd);
        if (r == 0) {
            struct drm_mode_map_dumb m; memset(&m, 0, sizeof m); m.handle = dd.handle; errno = 0;
            int r2 = ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m);
            printf("    MAP_DUMB r=%d errno=%d(%s) offset=%llu\n", r2, errno, strerror(errno),
                   (unsigned long long)m.offset);
            if (r2 == 0) {
                void *p = mmap(NULL, dd.size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, m.offset);
                printf("    mmap r=%s errno=%d(%s)\n", p == MAP_FAILED ? "FAIL" : "OK", errno, strerror(errno));
                if (p != MAP_FAILED) { memset(p, 0x5a, dd.size > 4096 ? 4096 : dd.size); munmap(p, dd.size); printf("    munmap+write OK\n"); }
            }
            struct drm_mode_destroy_dumb z; z.handle = dd.handle;
            ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &z);
        }
    }
    close(fd);
    printf("=== PROBE END ===\n");
    return 0;
}
