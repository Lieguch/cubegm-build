# 000.dat – 008.dat 平台映射文档

> 依据：HANDOFF §9（2026-08-16 锁定） + `cores/config.xml` 核心注册 + 原设备目录结构（用户提供的 11.png 待核）。
> 状态：**映射已锁定**；000.dat–008.dat 原始文件沿用原设备（用户规定，不额外构建记录）。
> 注意：本文档此前缺失（HANDOFF 引用但未入库），2026-08-17 依据 HANDOFF 记录重建；待用户上传 9 个 dat 文件后做逐文件字节核验。

---

## 一、9 分类平台映射（已锁定）

| 目录/文件 | 平台 | 对应模拟器核心 | 扩展名 | 备注 |
|---|---|---|---|---|
| 000.dat | **Arcade（街机）** | libemu_fbalpha / fbalpha2012 / mame2000 / pgm / fba / cps2 / extend | ZIP,7Z | 街机核心群，需 neogeo.zip/pgm.zip BIOS |
| 001.dat | **NES（红白机）** | libemu_nes.so（FCEUmm）/ libemu_nestopia.so | NES,FDS,UNIF,UNF | FCEUmm 主；nestopia 备用 |
| 002.dat | **SFC（超任）** | libemu_snes9x.so / libemu_sfc.so / libemu_snes9x2010.so | SMC,SFC,SWC,FIG | snes9x 系列 |
| 003.dat | **MD（世嘉五代）** | libemu_md.so（picodrive） | BIN,GEN,SMD,MD | |
| 004.dat | **GBA（掌机）** | libemu_mgba.so / libemu_vbam.so / libemu_gpsp.so | GBA,GB,GBC | gpsp 已在 config.xml 补充注册 |
| 005.dat | **NES 变体/FC（待核）** | libemu_nes.so（FCEUmm）或 nestopia | NES,FDS | HANDOFF 记为 001/005=NES，待 dat 文件核验是否独立分类 |
| 006.dat | **GB/GBC（掌机）** | libemu_tgbdual.so / libemu_mgba.so | GB,GBC,SGB | TGB Dual 双屏 |
| 007.dat | **PS1（PSX）** | libemu_pcsx.so（PCSX ReARMed） | BIN,IMG,MDF,PBP,ISO,TOC | 需 BIOS（pcsx_bios） |
| 008.dat | **Atari 2600** | libemu_stella.so | A26,BIN | |

> 设备还有 `libemu_prosystem.so`（Atari 7800）已注册但**不在 000–008 分类**内，属 9 分类之外的通用 Roms 目录（用户规定：9 个文件夹外的核心工作目录指向 `Roms`）。

## 二、目录与 Roms 约定（用户规定）

- **9 个 Dat 文件（000.dat–008.dat）沿用原设备**，交付 1.0 版本直接复用，不额外构建记录。
- 9 个文件夹之外的模拟器核心工作目录统一指向 **`Roms`**（`/mnt/SDCARD/Roms`，见 `deploy/build_sf3000_armhf.sh` 的 `-DCONTENT_DIR`）。
- picoarch 侧 `CONTENT_DIR=/mnt/SDCARD/Roms`；FrogUI 侧 `CUBEGM_CORES_DIR=/mnt/sda1/cubegm/cores`（运行时路径约定，构建验证不校验）。

## 三、待办（待用户上传 000.dat–008.dat 后执行）

1. 逐文件读取 9 个 dat 的字节内容，核验本映射表（尤其是 005.dat 是否独立分类、001 与 005 的差异）。
2. 确认每个 dat 内的核心指向与 `cores/config.xml` 注册名一致（`libemu_*.so` 文件名校验）。
3. 校验 9 个 dat 之外，`Roms/` 目录与 `cores/filelist.xml` 的 ROM 引用路径对应。

## 四、铁律

- 在拿到真实 dat 文件前，**不做任何臆测性结论**；本文档标注"待核"的条目在核验前不作为交付依据。
- dat 文件属用户交付资产（沿用原设备），不得修改/重建，仅做解析映射。
