# CubeGM 开源前端 · 部署与验证指南（RK3036G）

> 配套构建：`Lieguch/cubegm-build` 已绿（run #145，tag `v0.1`）。
> 本指南描述如何把构建产物部署到实体机 SD 卡，并通过 `autorun` 劫持启动开源前端（picoarch + FrogUI），**不替换任何原厂二进制**。

---

## 一、SD 卡目录布局（提取 `cubegm-deploy.tar.gz` 到 SD 根）

```
<SD 根>/
└─ cubegm/
   ├─ picoarch              # 前端二进制（armhf, glibc≤2.17, 已通过 ABI 门禁）
   ├─ zhijack.sh            # autorun 入口：拉起 picoarch + FrogUI
   ├─ cores/
   │  ├─ frogui_libretro.so # 启动器（菜单，本质也是一个 libretro core）
   │  ├─ fceumm_libretro.so # NES
   │  ├─ mgba_libretro.so   # GBA / GB / GBC
   │  ├─ nestopia_libretro.so
   │  ├─ picodrive_libretro.so # MegaDrive / Genesis / SMS
   │  └─ snes9x_libretro.so # SNES
   ├─ Roms/                # 放用户 ROM（空目录占位 .keep）
   ├─ setting.xml          # 模板：仅含 <autorun>，需合并进设备原 setting.xml
   └─ DEPLOY.md            # 本文件
```

> 原厂已有的 `rkgame` / `icube` / `driver.so` / `*.dat` **全部保留、不修改**。

---

## 二、部署步骤

1. **打包**（在构建机上，构建完成后）：
   ```sh
   cd deploy && ./package.sh ../cubegm-deploy.tar.gz
   ```
2. **传到 SD 卡**：把 `cubegm-deploy.tar.gz` 解压到 SD 卡根目录，得到 `cubegm/`。
3. **启用 autorun**（关键、且安全）：编辑设备已有的 `cubegm/setting.xml`，把
   `<autorun file="" driver="" />` 改为
   `<autorun file="cubegm/zhijack.sh" driver="" />`。
   - 只改这一行；其余设置原样保留。
   - 也可直接用本包里的 `setting.xml` 作参考，但**不要整文件覆盖**设备原 setting.xml。
4. **弹出 SD 卡，开机**。

---

## 三、启动链路（已核实，非猜测）

```
原厂 boot → rkgame → 读 setting.xml 的 autorun → cubegm/zhijack.sh
   → picoarch ./cores/frogui_libretro.so   （进入 FrogUI 菜单）
   → 选 ROM → fork() 跑对应 libretro 核心
   → 游戏退出 → 回到菜单；菜单退出 → 回到原厂 rkgame（安全兜底）
```

- 显示：标准 DRM/KMS（`/dev/dri`，dumb buffer mmap），由 picoarch `plat_sf3000.c` 驱动。
- 音频：标准 ALSA（`snd_pcm_*`）。
- 输入：标准 evdev（`/dev/input/event*`）。
- 设备 LCD 实际 1280×720；UI 渲染固定 1280×720 RGB565。

---

## 四、上线验证清单（在实体机执行）

- [ ] 开机进入 FrogUI 菜单（而非原厂菜单，或与原厂并存）。
- [ ] 放一个 NES ROM 到 `cubegm/Roms/`，菜单内能看到并启动，出画（DRM）。
- [ ] 游戏有声音（ALSA）。
- [ ] 手柄/按键可用（evdev；原厂 joystick.zip 的 26 动作词表可后续作映射参考）。
- [ ] 退出游戏回到菜单；退出菜单回到原厂（或重启前端，取决于 zhijack 设计）。
- [ ] 原厂菜单仍可用（证明未破坏校验，未触发 "sdcard is damaged"）。

---

## 五、已知边界 / 下一步（Stage2，需先查源码，勿猜）

1. **核心注册（扩展名→核心映射）**：本包的 `cores/*.so` 已就位，但 FrogUI/picoarch
   选择核心所依据的映射表（非原厂 `cores/config.xml`，那是给原厂 rkgame 用的）**尚未
   对照 treefrog 源码核实**。上线后若某 ROM 不被识别，下一步是读 `TreeFrogUI_picoarch`
   / `FrogUI` 源码确认其扩展名→核心选择逻辑，再补对应配置。此步**不在本包内臆造**。
2. **gpsp / prosystem 启用**：原厂 `libemu_gpsp.so`(GBA) / `libemu_prosystem.so`(Atari 7800)
   存在但未注册；我们这 5 个核心之外要扩，需按上面第 1 点确认映射后加入。
3. **输入映射全覆盖**：joystick.zip 的 26 动作词表 + ui.cfg 扫描码矩阵（2×27）+ 4 个
   USB 手柄 profile（0810_0001_0100 / 0110、20bc_5500、2563_0555）可作 evdev 映射参考，
   需在设备上实测校准。
4. **UI 适配**：FrogUI 默认分辨率可能不是 1280×720，需在实体机确认拉伸/黑边并按
   `ui_*.raw`(RGB565 1280×720) 资源管线适配。
5. **许可证**：TreeFrogUI / FrogUI = CC BY-NC-SA 4.0（非商用），仅限个人自用。

---

## 六、回滚

删除/改名 `cubegm/zhijack.sh`，或把 `setting.xml` 的 `<autorun file="">` 改回空，
重启即完全回到原厂行为。本方案对原厂分区零写入，回滚无风险。
