# 补丁说明：全手柄支持 + 策略游戏存档

> 范围：在已有开源替代源码 `cubegm-work/source/TreeFrogUI_picoarch/` 上**原地打补丁**。
> 这两个需求无法在闭源原厂二进制上实现（见末尾"闭源系统的边界"），只能走开源替代。

## 一、两个需求的真实结论（源码实证，非印象）

### 1. 策略游戏存档（电池 SRAM）
- **开源 picoarch 已经实现了电池存档持久化**：`content.c`/`core.c` 中 `sram_write` / `sram_read` /
  `sram_autosave` 带变更检测，且 `main.c` 每帧调用 `sram_autosave()`（节流 ~10s、仅变更时落盘 `.srm`），
  暂停菜单也写 `sram_write()`。
- 也就是说，策略游戏依赖的"电池 RAM"（SRAM/EEPROM/Flash）在开源替代里**本就支持**——游戏里存的档退出不丢。
- **唯一缺口是 RTC（真实时钟）**：原 `core.c` 的 environment 回调 switch 里没有
  `RETRO_ENVIRONMENT_GET_SYSTEM_TIME`。带真实时间的策略/模拟游戏（游戏内日历、昼夜循环）会因为没有
  时钟而漂移。→ **本补丁补上 RTC**。
- 原厂闭源系统只有"断点保存"（整份内存快照），**完全无法加 SRAM**（行为写死在二进制里）。

### 2. 全手柄支持
- 原厂 `joystick.zip` 只有 **4 个写死的 VID_PID 档案**，加载器只认已知 VID_PID → 做不到"全部手柄"。
- 开源 picoarch 的 `libpicofe/linux/in_evdev.c` 是**通用 evdev 后端**：扫 `/dev/input/event*`，
  读标准 `BTN_*` / `ABS_*` 码。Linux 内核的 `usbhid` 把**任意标准 HID 手柄**映射到同一套
  `BTN_A/B/X/Y/TL/TR/SELECT/START` 等码 → 通用 evdev **天然全支持，无需逐设备档案**。
- 而实际激活的输入后端是 SDL（`plat_sdl.c` → `in_sdl_init`），其 `handle_joy_event` 把
  `SDL_JOYBUTTONDOWN` 映射成 `SDLK_WORLD_0 + N`，但 `defbinds`/`joy_map` 里**没有 WORLD_N→动作绑定**
  （`joy_map` 只覆盖 4 个菜单键）→ 外接手柄按键落不到 joypad 动作。这正是"手柄支持不全"的根因。
- **本补丁**：激活通用 evdev 后端（与 SDL 并存，SDL 管机身面板、evdev 管外接 USB 手柄），并把标准
  `BTN_*` 码映射到 `RETRO_DEVICE_ID_JOYPAD_*`。任何标准 HID 手柄插上即用。

## 二、改动文件清单（已原地写入源码）

### `core.c`（RTC）
- 新增 `#include <time.h>`。
- 守卫式定义 `RETRO_ENVIRONMENT_GET_SYSTEM_TIME 56`（本树精简版 `libretro.h` 缺此枚举；
  值 56 与规范及本树编号一致：55=SET_CORE_OPTIONS_DISPLAY、57=GET_DISK_CONTROL_INTERFACE_VERSION；
  若真实 `libretro.h` 子模块已定义则自动跳过）。
- 在 environment 回调 switch 中新增 `case RETRO_ENVIRONMENT_GET_SYSTEM_TIME:`，
  填充 `*t = time(NULL)`。

### `plat_sf3000.c`（evdev 手柄平台数据）
- 新增 `#include "libpicofe/linux/in_evdev.h"`。
- 本地守卫式定义 `BTN_*` 常量（稳定 Linux ABI，避免把 `<linux/input.h>` 拉进 SDL 翻译单元）。
- 新增 `in_evdev_defbinds[]`：`BTN_DPAD*`/`BTN_A..Y`/`BTN_TL/TR/TL2/TR2`/`THUMBL/R`/`SELECT`/`START`
  → `RETRO_DEVICE_ID_JOYPAD_*`，`BTN_MODE`→`EACTION_MENU`（开暂停菜单）。
- 新增 `in_evdev_key_map[]`：`BTN_*` → `PBTN_*`（菜单导航）。
- 新增 `in_evdev_platform_data`（与 `in_sdl_platform_data` 同结构）。

### `plat_sdl.c`（激活 evdev 后端，两处）
- 在 `in_sdl_init(...)` 之后、`in_probe()` 之前，用 `#ifdef PLATFORM_SF3000` 守护调用
  `in_evdev_init(&in_evdev_platform_data)`（SF3000 构建里 `plat_sdl.c` 被 `#include` 进
  `plat_sf3000.c` 同一翻译单元，故可直接引用上述数据）。
- 两处 init 调用点（含/不含 SF3000 前缀的 PA_ERROR 分支）均已加。

## 三、为什么不会"双报"输入
SF3000 机身面板通过**独立硬件寄存器** `sf3000_keys_ptr` 读取（`pa_input_poll` 中处理），**不走 evdev**；
evdev 只探测到插上的 USB 手柄 → 面板与手柄各走各路，无重复。

## 四、构建与验证（沿用已搭好的工具链，无需另外搭环境）
- 用现有 `build_sf3000.sh`（它**已经把 `libpicofe/linux/in_evdev.o` 编进去**），但把里面的
  MIPS 工具链/SDL 替换为我们锁定的 **armhf + glibc 2.17 sysroot**（见 `deploy/build.sh` 与
  `build/toolchain/build_sysroot_ctng.sh`；`in_evdev.c` 需要的 `<linux/input.h>` 由设备 sysroot 提供）。
- 需拉取 **`libretro-common` 子模块**（提供 `RETRO_DEVICE_ID_JOYPAD_*` 等常量）：
  `git submodule update --init libretro-common`（或构建时 `-I` 指向它）。
- 产物跑 `build/toolchain/verify_target_abi.sh` 门禁（EM_ARM / e_flags=0x5000400 / ≤GLIBC_2.17）。
- 真机验证：
  1. 插任意标准 USB 手柄 → 菜单/游戏内按键即生效（无需改 `joystick.zip`）。
  2. 加载带 RTC 的核心（如部分 RPG/Sim），游戏内时间正确推进、退出重进不漂移。
  3. 策略游戏：游戏里存一次档 → 退出 → 重进，档还在（`.srm` 已落盘）。

## 五、闭源原厂系统的边界（重要，避免误解）
- **策略 SRAM：加不了**。原厂 `rkgame`/`icube`/`driver.so`/`libemu_*.so` 是编译好的二进制，存档行为
  写死；只能数据文件级改 `joystick.zip` / `config.xml`，无法改二进制逻辑。
- **全手柄：加不了**。原厂加载器只认 `joystick.zip` 里写死的 VID_PID 档案。可以**一个一个加特定手柄**
  （向 zip 追加 `VID_PID` 档案，20-token 格式见 `tools/add_gamepad_profile.py`），但永远无法"全部自动支持"。
- 因此"两个需求一次性满足"的**唯一路径是构建开源替代**（本补丁已就位）。

## 六、已知风险 / 后续
- 极少数**非标准 HID** 手柄（自定义按键码、不上报标准 `BTN_*`）通用 evdev 仍覆盖不到 →
  用 `joystick.zip` VID_PID 覆盖档案兜底（未来扩展：把现有 4 档案 + SDL gamecontrollerdb 做成覆盖库）。
- `GET_SYSTEM_TIME=56` 依赖本树枚举编号一致；若构建用的真实 `libretro.h` 编号不同需核对（守卫定义已防重复）。
- 模拟器核心的 RTC 是否真用 `GET_SYSTEM_TIME` 取决于核心本身；picoarch 只是把系统时钟透传给核心。
