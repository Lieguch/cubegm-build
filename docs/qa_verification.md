# 验证方案（QA）— CubeGM 开源替代系统

> 角色：QA 严过关 ｜ 阶段门禁：每个 Stage 退出前必须全绿

## 一、ABI 门禁（最高优先级，防运行期崩溃）

脚本：`build/toolchain/verify_target_abi.sh <binary>`（python 解析 ELF，零依赖）。
对**每一个**产物二进制断言：

| 检查 | 期望值 | 失败后果 |
|---|---|---|
| ELF magic | `\x7fELF` | 不是 ELF |
| EI_CLASS | 1（32-bit） | 64 位不兼容 RK3036G |
| E_MACHINE | 40（EM_ARM） | 非 ARM |
| E_FLAGS | **0x5000400**（armhf + EABIv5） | soft-float/ABI 错配 |
| GLIBC 上限 | ≤ **2.17** | 运行期 `GLIBC_2.xx not found` |

实测基线：设备 20 个 `libemu_*.so` 全部满足（见 `tools/probe_device_abi.py`）。

## 二、分阶段测试矩阵

### Stage0 SDK
- [ ] `setup_toolchain.sh` 成功安装 `arm-none-linux-gnueabihf-gcc`。
- [ ] `sysroot_from_device.sh` 拉到设备 rootfs，含 `libc.so.6`。
- [ ] `make smoke` 产出 `hello`，`verify_target_abi.sh hello` == PASS。

### Stage1 MVP（DRM+ALSA+evdev 闭环）
- [ ] 设备经 `zhijack.sh` 启动进入 FrogUI（不报 sdcard is damaged）。
- [ ] 显示：DRM/KMS dumb buffer 渲染 1280×720，屏幕出画。
- [ ] 输入：`evdev_to_action()` 对 P1/P2 扫描码返回正确 action（用 `evdev_keymap.h` 矩阵回测）。
- [ ] 音频：ALSA `snd_pcm_*` 出声。
- [ ] 至少 3–5 个 libemu_* 核心可被 picoarch `fork()` 加载运行。

### Stage2 功能对等
- [ ] `cores/config.xml` 注册 20 核心（含补的 gpsp/prosystem）；启动后前端识别全部。
- [ ] 4 个 USB 手柄 profile 全匹配（`USB_PROFILES[]` 中 VID/PID 匹配真实设备）。
- [ ] 完整 UI 皮肤：每个 `ui_*.raw` 解出严格 1280×720（`render_raw.py` 字节校验）。
- [ ] 存档/读档：核心 `save_state`/`load_state` 定制 ABI 经 shim 可用。

### Stage3 超越原厂
- [ ] 57 核心（`treefrog-ui/clone_cores.sh`）。
- [ ] Quick Resume 快照恢复。
- [ ] 主题 / 缩略图 / 多语言切换无崩溃。

## 三、专项校验

### 输入映射覆盖
- 用 `decode_joystick.py` 重新导出并与 `evdev_keymap.h` 逐字段 diff，确保生成文件与原始 `joystick.zip` 100% 一致（无转录误差）。
- 4 个 profile：`0810_0001_0100` / `0810_0001_0110` / `20bc_5500` / `2563_0555` 均存在且 20 token。

### UI 字节级
- 每个 `ui_*.raw`：`len % 2 == 0` 且解帧尺寸 == 1280×720（1,843,200 B 像素区）。
- `render_raw.py decode` 回读无异常；抽样像素 RGB565→RGB888 还原正常。

### 启动安全
- `zhijack.sh` 不引用/不写 `root.dat` 或任何校验分区（代码评审 + grep 确认）。
- 真机启动一次，确认无「sdcard is damaged」。

## 四、回归原则
- 任何新 core / 新 UI / 新输入设备接入，必须先过 `verify_target_abi.sh` 再合并。
- ABI 基线（EM_ARM / 0x5000400 / ≤2.17）作为 CI 硬断言，不达标即红灯。
