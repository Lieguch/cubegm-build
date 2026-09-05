# sysroot 策略：无设备 rootfs 如何获取 glibc-2.17 编译环境

> 背景：用户无法从设备拉取 rootfs（或设备不可拉取）。本文件回答「能否用现有资源解决」并给出可落地的等价方案。
> 配套：架构设计书 §二/§六 已据本文件更正；脚本 `build/toolchain/build_sysroot_ctng.sh`、`sysroot_from_device.sh`、`verify_target_abi.sh`。

---

## 一、直接回答

**能，用现有/可获取资源即可解决。** 真正的硬约束不是「必须用设备 rootfs」，而是：

> **链接目标必须是 glibc ≤ 2.17 的 sysroot。**

设备 rootfs 只是「获取一份 2.17 sysroot」的途径之一；自举一份**等价**的 2.17 sysroot（从公开上游源码构建）完全够用，运行时一致性不受影响。

---

## 二、为什么之前说「要 rootfs」（以及它到底意味着什么）

- 设备全部二进制经实测均为 glibc ≤ 2.17：
  - **天花板 = GLIBC_2.17**，由 `libemu_fbalpha.so`（还引用了 2.15/2.7/2.4）拉高；
  - 其余 19 个 core 仅需 **GLIBC_2.4 – 2.7**，很多甚至是 **GCC 3.x** 古董工具链编的；
  - `driver.so` / `icube` 只需 2.4，`rkgame` 只需 2.7。
  - → 设备运行的是 **glibc 2.17**，向下兼容所有 core（glibc 向后兼容）。
- ARM GNU 13.2 工具链自带 sysroot = **glibc 2.38**；若用它动态链接产物直接跑在设备上，运行期会报 `version GLIBC_2.xx not found`。
- 因此「链接」必须指向一份 **2.17 的 libc / libm / libpthread / libdl / libstdc++ / libgcc_s**（C/C++ 运行库）。

**设备运行时实际提供的库**（来自 NEEDED 扫描）：
- 硬件层：`libdrm.so.2` / `libkms.so.1` / `libasound.so.2`（设备自带，运行时由系统给，无需打包）
- 标准运行库：`libc/libm/libpthread/libdl/libstdc++/libgcc_s/libz`（由我们的 2.17 sysroot 提供，运行时设备 2.17 也有）

---

## 三、关键更正：全静态链接被否决（读源码实证的）

读 picoarch 源码（`TreeFrogUI_picoarch/core.c:720`）：

```c
current_core.handle = dlopen(corefile, RTLD_NOW | RTLD_GLOBAL);
```

→ core 通过 **`dlopen` 加载进 picoarch 同一进程**（标准 libretro 模型）。`main.c` 通过 `execl(picoarch_for_core(...))` 调对应 picoarch 变体，`plat_sdl.c` 则 `fork()` 劫持 core 以保留原厂 rkgame。

**推论（决定性）**：picoarch 与 `libemu_*.so` 必须**共用同一份 glibc**。
- 若 picoarch 全静态链接 glibc 2.38，再 `dlopen` 设备 2.17 core → 同一进程里出现**两份 glibc**（malloc/stdio 符号冲突）→ 直接崩溃。
- 故：**picoarch 必须动态链接到 glibc 2.17**，与设备 core 对齐。
- 早前架构设计书 §二「兜底：全静态链接」**是错误的，已更正**（见 architecture.md 对应修订）。

---

## 四、解决方案 A（推荐）：crosstool-NG 自举 glibc-2.17 sysroot

用**公开上游源码**（gcc 6.5 + glibc 2.17 + binutils + Linux 4.4 headers）构建一份 `armv7a-hardfloat` sysroot：

- **无需设备**，确定性可复现；
- 产物等价于「设备 rootfs 的 C 运行库部分」，链接用即可；
- 脚本：`build/toolchain/build_sysroot_ctng.sh`（在 x86_64 Linux 构建机执行，耗时约 20–60 分钟）；
- 产出：`$PREFIX/armv7a-hardfloat-linux-gnueabihf/sysroot/`，含 `lib/libc.so.6`、`usr/include/...`、`usr/lib/...`。

组合用法：ARM GNU 13.2 作 **gcc 前端**，链接指向该 2.17 sysroot：

```sh
export PATH="$CTNG_PREFIX/bin:$ARM13_PREFIX/bin:$PATH"
export CC=arm-linux-gnueabihf-gcc
export CFLAGS="--sysroot=$CTNG_SYSROOT -march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -O2"
```

---

## 五、解决方案 B：预编译旧工具链（仅当可达时）

需要 glibc **恰好 ≤ 2.17** 的预编译工具链。

- **陷阱**：Linaro 2017.05 = glibc **~2.23**（高于 2.17 天花板，会踩雷）；需更老（2013–2015 时代，如 Linaro 2014.04 = glibc 2.17）或 buildroot 固定版本。
- **验证方法**（下载后必须做）：
  ```sh
  strings <toolchain>/arm-linux-gnueabihf/libc.so.6 | grep -o 'GLIBC_2\.[0-9]*' | sort -V | tail -3
  # 必须看到 GLIBC_2.17 且没有 >2.17
  ```
- 本沙箱对 Linaro CDN 有 CA 信任缺口（curl exit 35），但用户 Linux 构建机通常可达；若不可达，直接走方案 A。

---

## 六、解决方案 C（金标准，若将来能拿到 rootfs）

`build/toolchain/sysroot_from_device.sh`：从设备 dump 出 `/lib /usr/lib /usr/include` 作 sysroot，最贴近真机。
- 获取途径（若设备可访问）：设备开 ADB/串口，或从固件分区镜像解包。
- **非必需**——A/B 方案已完全够用。

---

## 七、编译 / 链接 / include 实战组合

| 环节 | 用什么 | 说明 |
|---|---|---|
| C 编译器前端 | ARM GNU 13.2（gcc 新） | 版本可新，只负责编译 |
| 链接 sysroot | 2.17 sysroot（A/B/C 任一） | `--sysroot=` 指向它，规避 glibc 错配 |
| **ALSA 头缺口** | alsa-lib 源码仅 `include/` | 2.17 sysroot 多缺 `alsa/asound.h`（ARM 13.2 也缺）。下载 alsa-lib（如 1.2.x），把 `include/` 加到 `-I`；**纯 C 头、无 glibc 版本耦合**，可配新 gcc。链接时 `libasound.so.2` 由设备运行期提供，不随包发布 |
| DRM / evdev 头 | 随 2.17 sysroot 自带 | 标准 Linux 头，无需额外处理 |
| ABI 门禁 | `verify_target_abi.sh` | 每个产物断言 EM_ARM / 0x5000400 / ≤ GLIBC_2.17 |

---

## 八、运行时一致性结论（最终闭环）

```
设备 boot → cubegm/zhijack.sh → picoarch(链接 2.17)
                                   ├─ dlopen driver.so   (设备自带, glibc 2.4)
                                   └─ dlopen libemu_*.so (设备自带, ≤2.17)
   → 全部在设备 glibc 2.17 下共存；libdrm/libkms/libasound 由设备提供
```

**无需设备 rootfs**，只需一份自举的 2.17 sysroot，即可在真机上跑通 DRM 出画 + ALSA 出声 + evdev 响应输入的闭环。

---

*下接 Stage1 MVP：在 Linux 机构建 picoarch（基于 `plat_sf3000.c`）+ FrogUI + 3–5 core，用本文件任一方案的 2.17 sysroot 链接，`make smoke` 过 ABI 门禁后真机验证。*
