# CubeGM 开源替代系统 — 工作区总览

> 目标硬件：**Rockchip RK3036G**（双核 Cortex-A7 / Mali-400 / DDR3）
> 用途：**个人非商用自用** ｜ 目标：**功能对等并超越原厂**（原厂已停更）
> 交付模式：SoftwareCompany SOP（PM 许清楚 → 架构 高见远 → 工程 寇豆码 → QA 严过关）

## 一、目录布局

```
cubegm-work/
├── cubegm/cubegm/        # 原厂系统文件（逆向对象）：cores/*.so, config.xml,
│                         #   joystick.zip, ui_*.zip, root.dat, rkgame/icube/driver.so
├── source/               # 开源源码（GitHub tzubertowski）：TreeFrogUI_picoarch,
│                         #   FrogUI, treefrog-ui, TreeFrogUI_pcsx4all（clone 中）
├── docs/                 # 分析报告 + 团队交付物
│   ├── prd.md            #   PM 许清楚：产品需求
│   ├── architecture.md   #   架构 高见远：系统设计 + ABI/工具链规格
│   ├── qa_verification.md#   QA 严过关：验证方案
│   ├── cubegm_input_and_ui.md, cubegm_zip_contents.md, cubegm_replacement_feasibility.md
├── build/                # 工程 寇豆码：可构建脚手架
│   ├── Makefile
│   ├── toolchain/        #   setup_toolchain.sh / verify_target_abi.sh / sysroot_from_device.sh
│   ├── cores/config.xml  #   全 20 核心注册（补 gpsp+prosystem）
│   ├── input/evdev_keymap.h  # 由 joystick.zip 生成（26 动作 + 2×27 矩阵 + 4 USB profile）
│   ├── autorun/zhijack.sh    # 启动劫持（规避 sdcard is damaged）
│   ├── ui/README.md      #   1280×720 RGB565 资源规范
│   └── cross_compile_smoke/hello.c
└── tools/                # 解析/校验脚本
    ├── probe_device_abi.py   # 实测 20 core ABI（EM_ARM/0x5000400/GLIBC_2.17）
    ├── decode_joystick.py    # 解码 joystick.zip
    ├── gen_evdev_keymap.py   # 生成 evdev_keymap.h
    └── render_raw.py         # RGB565 1280×720 解码/编码/预览
```

## 二、已验证的关键事实

| 事实 | 方法 | 结论 |
|---|---|---|
| 设备 ABI | `tools/probe_device_abi.py` 解析 20 个 libemu_*.so | 32-bit ARM / armhf(0x5000400) / **GLIBC 上限 2.17** |
| 输入映射 | `tools/decode_joystick.py` + `gen_evdev_keymap.h` | 26 动作(13×P1/P2) + 2×27 扫描码矩阵 + 4 USB 手柄 profile |
| UI 格式 | `tools/render_raw.py` 字节级解码 | RGB565 **1280×720**（字节 0 起为整帧） |
| 核心注册 | 读 `cores/config.xml` | 18/20 注册；`gpsp`/`prosystem` 存在未注册 → 已补 |
| 驱动层 | `driver.so` 字符串证据 | 标准 Linux **DRM/ALSA/evdev**，picoarch `plat_linux.c` 直接复用 |
| 工具链 | WebSearch + 实测下载 | ARM GNU 13.2（Linux-hosted）可下载；链接须用设备 rootfs(sysroot) 规避 glibc 错配 |

## 三、四阶段路线

| 阶段 | 目标 | 退出标准 |
|---|---|---|
| Stage0 SDK | 工具链+sysroot+ABI 门禁 | `verify_target_abi.sh` 通过 |
| Stage1 MVP | picoarch+FrogUI+3–5 core 证明 DRM+ALSA+evdev 闭环 | 设备进入 FrogUI，选游戏出画+出声+响应输入 |
| Stage2 对等 | 20+ core、config.xml 全注册、1280×720 UI、输入全映射 | 原厂能跑的都能跑；gpsp/prosystem 启用 |
| Stage3 超越 | 57 core、Quick Resume、主题、缩略图、多语言 | 超出原厂可维护/可扩展性 |

## 四、在 Linux 构建机上的用法

```bash
# 1) 安装交叉编译器
./build/toolchain/setup_toolchain.sh
# 2) 从设备 dump rootfs 作为 sysroot（glibc 2.17，关键！）
DEVICE=root@<设备IP> ./build/toolchain/sysroot_from_device.sh
# 3) 构建 + 打包 + ABI 校验
make SYSROOT=$HOME/cubegm-sysroot
# 4) 冒烟测试（验证工具链+sysroot 链接产出合法 armhf 二进制）
make SYSROOT=$HOME/cubegm-sysroot smoke
```

## 五、合规红线

- 仅**个人非商用**；对外分发须保留 CC BY-NC-SA 4.0 署名 + 非商用条款，禁止闭源再发布。
- 启动劫持**绝不改写 root.dat/校验分区**，否则设备报「sdcard is damaged」。
- 每次构建强制 `verify_target_abi.sh`，杜绝 glibc 错配上线。
