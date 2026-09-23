# probe/ —— 设备能力探针

`drm_probe.c`：ARM32 探针，用来把「上层程序只打印 `ret=-1`」变成**真实 errno**。

**为什么需要**（实测教训，见 `../PITFALLS.md` #10）：目标程序常常只打印 `ret`，不打印 `errno`，
于是"设备能力不匹配"会被误判成"设备坏"。跨层失败必须自己到 API 层把 errno 取回来。

它做的事：
1. 列 `/dev/dri` 目录项
2. `open("/dev/dri/card0", O_RDWR)`
3. `DRM_IOCTL_VERSION`（注意：本探针的结构体定义有个已知小瑕疵，返回值仅供参考）
4. `DRM_IOCTL_GET_CAP` 枚举 `DUMB_BUFFER / PRIME / ADDFB2_MODIFIERS / ATOMIC / ASYNC_PAGE_FLIP …`
5. **`CREATE_DUMB` 参数扫描**（多组 `w/h/bpp`）+ 成功时继续 `MAP_DUMB` → `mmap` → 写入 → `munmap` → `DESTROY_DUMB`

**编译**（本机 Windows + zig，实测可用）：
```sh
zig cc -target arm-linux-gnueabihf.2.29 -mfloat-abi=hard -mfpu=neon -O2 -o drm_probe drm_probe.c
```
★ 注意：`-target arm-linux-gnueabihf.*` **不支持 `-static`**（报 "libc of the specified target requires
dynamic linking"）。要静态就用 `-target arm-linux-musleabihf`。本探针用**动态链接**，正好匹配原厂
rootfs 的 `ld-2.29.so`（`interp=/lib/ld-linux-armhf.so.3`）。

**跑法**：把 `drm_probe` 放进 rootfs 的 `/usr/bin/`，并在 `/etc/init.d/S00cgmmod` 里
**`exit 0` 之前**调用（★ 不能 append 到文件末尾 —— 那会在 `exit 0` 之后，永不执行）。

**实测结论**（本设备环境）：
```
CREATE_DUMB bpp=32 → 成功（含 MAP_DUMB+mmap）
CREATE_DUMB bpp=24 → EINVAL(22)
CREATE_DUMB bpp=16 → EINVAL(22)
```
