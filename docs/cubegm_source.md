# CubeGM 源码溯源报告（联网核实）

> 方法：对 `cubegm` 的二进制特征（软件名 `rkgame`/`icube`、定制 `retro_*` 接口、`libemu_*.so` 命名、`.raw`/`.cpd` 资源）做联网检索，定位到开源项目并 WebFetch 核实仓库、许可证与构建流程。
> 时间：2026-08-13

---

## 一、一句话结论

**能找到，而且基本是开源的。** `cubegm/` 是 **R36SX / DataFrog SF3000 / SF3500 / GB350** 等国产 ARM/MIPS 掌机固件的目录名。你手上的 `rkgame`/`icube` 是**厂商原厂闭源二进制**，但其前端与模拟器核心体系，对应 GitHub 上 **`tzubertowski` 组织**的 **TreeFrogUI / picoarch** 开源项目（`cubegm/cores/*.so` 就是它构建出的 libretro 核心，由 `picoarch` 驱动）。

---

## 二、编码特色 → 开源项目的对应

| 你系统里的特征 | 对应开源项目 / 来源 | 证据 |
|------|------|------|
| 目录 `cubegm/`，文件 `rkgame`(菜单引擎)、`icube`(启动器)、`driver.so`(硬件驱动) | R36SX / SF3000 / SF3500 / GB350 家族固件 | R36SX Wiki、SF3000-RE 逆向项目明确描述此布局 |
| `libemu_*.so` 核心命名（非标准 `libretro_*.so`） | TreeFrogUI `clone_cores.sh` 拉取 libretro 上游核心、构建为 `libemu_*.so`，`cp build/*.so …/cubegm/cores/` | treefrog-ui README 构建命令 |
| 定制 `retro_is_support` / `retro_save_state` / `retro_load_state` / `retro_set_unzip` / `retro_set_progress_callback` | picoarch / cubegm 私有 libretro ABI 扩展（面向低功耗设备的存档/解压/进度钩子） | 标准 RetroArch 用 `retro_serialize`，这些符号是此体系的私有契约 |
| `ui_*.zip` 内 5 张 `.raw` 位图 + `ui.cfg` | TreeFrogUI 界面资源（菜单/游戏/设置/分类/搜索） | treefrog-ui 文档描述 `UI_Res.cpd`/`resource.cpd` 为改名 ZIP 的 raw 资源 |
| `joystick.zip` 的 `ui.cfg` 按键映射矩阵 | 输入系统（cubevol 守护进程经 `/tmp/joy_key` 共享内存传给核心） | treefrog-ui 文档说明输入来自 cubevol |

> ⚠️ 架构注记：我对二进制实测为 **ARM32（ELF e_machine=0x28=EM_ARM）**；而 TreeFrogUI 文档称目标为 "MIPS-based Hichip"、使用 `sf3000toolchain`。说明该 `cubegm` 家族跨多型号（ARM/MIPS 都有），源码体系通用，但**交叉编译工具链需按你设备的实际架构选取**。

---

## 三、源码仓库清单（均开源，GitHub）

| 角色 | 仓库 | 分支 | 许可证 |
|------|------|------|--------|
| 总构建/补丁/文档 | `github.com/tzubertowski/treefrog-ui` | — | **CC BY-NC-SA 4.0（非商用）** |
| libretro 前端（驱动核心） | `github.com/tzubertowski/TreeFrogUI_picoarch` | `r36sx` | 含 `LICENSE` 文件；基于 libpicofe（GPL/LGPL/MAME 三选一）+ BSD 3-Clause |
| 启动器核心（菜单） | `github.com/tzubertowski/FrogUI` | `r36sx` | 随上游 |
| 独立 PS1 模拟器 | `github.com/tzubertowski/TreeFrogUI_pcsx4all` | — | 随上游 |
| 各模拟器核心上游 | 由 `clone_cores.sh` 克隆（FBNeo/fbalpha、mGBA、Snes9x、PCSX-ReARMed、Nestopia、Stella、TGB Dual、picodrive…） | — | 各核心保留上游许可（GPL/BSD/MIT/MAME） |
| 逆向参考 | `github.com/goph-R/SF3000-RE` | — | 研究用 |

> 社区工具（非源码，但有用）：`gb300-sf2000-tool`（网页主题编辑器，可改 UI 背景）、`sync_sd_card`（重建游戏列表 `allfiles.lst`/`filelist.csv`）、`download_covers`（抓封面 320×240 PNG）。

---

## 四、如何获取与构建

```bash
# 1) 工作区（所有源码仓库作为兄弟目录）
mkdir -p ~/sf3000-work && cd ~/sf3000-work

# 2) 克隆（前端、启动器、picoarch、PS1）
git clone git@github.com:tzubertowski/treefrog-ui.git sf3000_treefrogui
git clone -b r36sx git@github.com:tzubertowski/FrogUI.git FrogUI
git clone -b r36sx git@github.com:tzubertowski/TreeFrogUI_picoarch.git picoarch
git clone        git@github.com:tzubertowski/TreeFrogUI_pcsx4all.git pcsx4all
git -C picoarch submodule update --init libretro-common   # libpicofe 已 vendored

# 3) 拉取并构建所有模拟器核心
cd sf3000_treefrogui
./clone_cores.sh     # 克隆全部 libretro 核心上游到 cores/
./build_all.sh       # 应用 patches/，构建核心（需 cmake、交叉工具链）

# 4) 产物落盘：编译出的 .so 在 build/，复制到设备的 cubegm/cores/
cp build/*.so /mnt/sdcard/cubegm/cores/

# 仅构建启动器核心：
cd frogui && make -f Makefile.sf3000 frogui_libretro.so
cp frogui_libretro.so /mnt/sdcard/cubegm/cores/
```

**前置条件**：对应架构的交叉工具链（文档用 `sf3000toolchain`，放 `~/sf3000-work/sf3000toolchain/`）、`git/make/nproc/cmake`，以及 `libSDL 1.2 / libpng / libasound`（picoarch 本身构建依赖）。

---

## 五、必须注意的三件事

1. **你手上的 `rkgame`/`icube` 是厂商原厂（stock）闭源二进制，且部分加密。**
   - 逆向笔记（g3gg0.de）确认 `UI_Res.cpd` 是改名 ZIP（签名 `WQW\x03` 而非 `PK\x03\x04`），文件名混淆，硬编码密码 **`hichip123`**；新版设备开机**校验 `icube` 和 `rkgame`**，直接替换会报 "sdcard is damaged"。
   - 因此：**不要试图反编译替换原厂 `rkgame`/`icube`**，用开源方案时走"劫持"路线。

2. **开源的 TreeFrogUI 不替换原厂文件，而是劫持 `autorun`：**
   ```
   [boot] stock rkgame（原厂，未动）
        └─► autorun → cubegm/zhijack.sh
                 └─► picoarch frogui_libretro.so   ← TreeFrogUI 自身也是一个 libretro core
                       └─► 用户选 ROM → fork() 子进程跑游戏核心 → 退出后回到菜单
   ```
   想在自己设备上启用开源前端，按此机制挂载 `zhijack.sh`，不动 `rkgame`/`icube`。

3. **许可证是 NC（非商用）的**：TreeFrogUI 整体为 **CC BY-NC-SA 4.0**；picoarch 基于 libpicofe（GPL/LGPL/MAME）+ BSD 3-Clause；各核心保留上游许可。若你打算**商业分发/改装售卖**，需另行评估合规，NC 条款禁止商用。

---

## 六、给下一步的建议

- 想**读懂/改界面与前端**：从 `TreeFrogUI_picoarch` + `FrogUI` 入手（C + libpicofe + SDL）。
- 想**加/换模拟器核心**：用 `clone_cores.sh` 拉上游 → 在 `patches/` 加适配补丁 → `build_all.sh` → 复制 `.so` 到 `cubegm/cores/`，并在 `cores/config.xml` 登记（呼应前两份报告的"待开发模块 #1"）。
- 想**改 UI 外观**：用 `gb300-sf2000-tool` 编辑 `UI_Res.cpd`（改名 ZIP 的 raw 背景图），无需碰源码。
- 想**确认设备真实架构**：用 `file`/ELF 头核对（你这份实测 ARM32）；选对交叉工具链再编译，避免 MIPS/ARM 错配。
- ⚠️ ROM/BIOS（neogeo/pgm）版权：仅用于你已合法拥有的硬件备份，勿公开分发。
