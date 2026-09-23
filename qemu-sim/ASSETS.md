# 所需文件清单 / 来源 / 指纹 / 设备事实

## 1. assets/（11 MB，已随本套件提供）

| 文件 | 大小 | sha256 | 来源 |
|---|---|---|---|
| `kernel.zImage` | 4,308,440 | `c22dd63227da64e3ff4eeefee2834d3f4c07a8393c4ce0aef3acd6c831f06b5c` | **原厂 `org.bin` 的 GPT `boot` 分区**（Android boot image，`ANDROID!` v0、page=2048）里的 kernel；`[0x24]=0x016f2818` ⇒ 标准 ARM zImage |
| `rootfs.sqsh` | 2,752,512 | `6f8412c760147f062432fb2867de2e945ddf6f244ca2c35accb46686a4111f52` | **原厂 `org.bin` 的 GPT `rootfs` 分区**（squashfs，magic `hsqs`） |
| `rk3036.dtb` | 24,078 | `577e910f9e7aeaebe3a57eb5f04b663ab014939842f7ce055ccc0f813048d8a7` | 原厂设备树（143 节点，52 个带 `reg`） |
| `sdcard/icube` | 12,128 | `d26f900db39e278b9c4dc77c469670a8bd8d59be1d92b79b3dd2f54e47e39e41` | 原厂 SD 卡 |
| `sdcard/rkgame` | 3,921,108 | `8ff3b4b70c253ff700197a90b2ec31548922e6cfe5a3466bbc1cf6e5f0f88603` | 原厂 SD 卡（**原厂二进制，红线段：绝不修改**） |
| `sdcard/driver.so` | 39,844 | `16bbf93d1a4ed639d4512b4c525ecb9455686ce911dd5eac04f2efff74f81961` | 原厂 SD 卡（`NEEDED: libdrm.so.2 libkms.so.1 libasound.so.2`） |

**运行时另需（脚本自动取，不入库）**：
- Alpine `vmlinuz-lts` 8,028,672 B（通用内核）
- Alpine `modloop-lts` 48,832,512 B（同版本模块全集）

## 2. 从 org.bin 重建（`scripts/extract_assets.py`）

```sh
python3 scripts/extract_assets.py "F:/OTHER/D20游戏机/解包归档" ./assets
```
它做的是：
1. 解析 `org.bin` 的 GPT，取 `boot` 分区 ⇒ `ANDROID!` header ⇒ **kernel zImage + resource(second)**
2. 取 `rootfs` 分区 ⇒ `rootfs.sqsh`
3. 复制 `rk3036_原厂设备树.dtb`、`原厂SD卡/{icube,rkgame,driver.so}`
4. 打印每个文件的 sha256 供对账

## 3. ★ 设备事实（权威，来自原厂镜像）

### 3.1 SoC / 平台
```
SoC:        Rockchip RK3036(G)  双核 Cortex-A7 / Mali-400 / DDR3
内核:       原厂为 Linux 4.4.186（Buildroot）；本套件用通用 ARMv7 内核
glibc:      2.29        ← 证据：lib/libc-2.29.so、lib/ld-2.29.so，
                          版本串 "GNU C Library (Buildroot) stable release version 2.29."
```

### 3.2 内存映射（原厂 DTB 实测）
```
MEMORY          0x60000000  size 0x10000000 (256 MB)
```

### 3.3 外设地址表（从原厂 DTB 提取，52 个带 reg 节点）

| 设备 | 基址 | 大小 | compatible |
|---|---|---|---|
| CRU（时钟/复位） | `0x20000000` | 0x1000 | `rockchip,rk3036-cru` |
| GRF（syscon） | `0x20008000` | 0x1000 | `rockchip,rk3036-grf` |
| ACODEC（模拟音频） | `0x20030000` | 0x1000 | `rk3036-codec` |
| HDMI | `0x20034000` | 0x4000 | `rockchip,rk3036-hdmi` |
| timer | `0x20044000` | 0x20 | |
| watchdog | `0x2004c000` | 0x100 | |
| PWM ×4 | `0x20050000/10/20/30` | 0x10 | |
| I2C ×3 | `0x20056000 / 0x2005a000 / 0x20072000` | 0x1000 | |
| **UART0/1/2** | `0x20060000 / 0x20064000 / 0x20068000` | 0x100 | `rockchip,rk3036-uart` + `snps,dw-apb-uart` |
| SPI | `0x20074000` | 0x1000 | |
| DMA | `0x20078000` | 0x4000 | `arm,pl330` |
| **GPIO0/1/2** | `0x2007c000 / 0x20080000 / 0x20084000` | 0x100 | `rockchip,gpio` |
| efuse | `0x20090000` | 0x20 | |
| **VOP（显示）** | `0x10118000` | 0x19c | `rockchip,rk3036-vop` |
| VPU（视频解码） | `0x10108000` | 0x800 | |
| **GIC-400** | `0x10139000` | 0x1000 | `arm,gic-400` |
| USB ×2 | `0x10180000 / 0x101c0000` | 0x40000 | |
| EMAC | `0x10200000` | 0x4000 | |
| **SFC（串行闪存）** | `0x10208000` | 0x4000 | `rockchip,sfc` |
| **DWMMC ×3（SD/eMMC）** | `0x10214000 / 0x10218000 / 0x1021c000` | 0x4000 | `rockchip,rk3036-dw-mshc` |
| **I2S（音频）** | `0x10220000` | 0x4000 | `rockchip,rk3036-i2s` |
| NANDC | `0x10500000` | 0x4000 | |

其中 **5 个是通用 IP**（`arm,gic-400` / `arm,pl330` / `arm,cortex-a7` / `snps,dw-apb-uart` / `syscon`）
—— 这些在 qemu 里可能有可复用组件；**38 个是 Rockchip 专有**，要写模型才"真"。

### 3.4 原厂程序对设备的实际依赖（实测，决定"哪些必须真"）

```
rkgame   依赖：/dev/input/js0..js3（joystick）、/dev/mem（GRF_GPIO0A_IOMUX / GRF_GPIO2A_IOMUX）、/proc/meminfo
driver.so 依赖：/dev/dri（遍历目录找 DRM 设备）+ 标准 libdrm KMS API
              （drmModeGetResources/SetCrtc/SetPlane/AddFB2/PageFlip、DRM_IOCTL_MODE_CREATE_DUMB/MAP_DUMB/GEM_CLOSE）
              + 标准 ALSA（snd_pcm_open/hw_params/writei/...）
              NEEDED: libdrm.so.2 / libkms.so.1 / libasound.so.2
icube    依赖：libpthread.so.0 / libc.so.6（轻量，只负责拉起 rkgame）
启动链     /etc/inittab → /etc/init.d/rcS → S01logging … S80icube
          S80icube: DAEMON=/sdcard/cubegm/icube ; LD_LIBRARY_PATH=/sdcard/cubegm/lib:$LD_LIBRARY_PATH ; $DAEMON start
```
**⇒ 关键推论**：`driver.so` 用的是**标准 libdrm/libasound API**，且从 **`/dev/dri` 目录**发现设备
⇒ **"标准 API + 真实设备"就能让它跑，不必先写 RK VOP/I2S 模型**。
