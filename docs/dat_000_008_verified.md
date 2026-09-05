# 000.dat–008.dat 实证解析报告（B4/M2 完成 · 2026-08-22）

> 分析对象：`D:/output/原厂SD卡根目录结构/000/000.dat` … `008/008.dat`（原设备 SD 副本）
> 方法：`WQW\x03` 混淆 ZIP 容器遍历 + raw DEFLATE 解压 + GBK 解码（与 root.dat / 007 / 008 既有分析同法）
> 结论：**9 个 dat 均为"分类游戏清单 + 每游戏截图位图"的加密容器，印证 item 14"沿用原设备、不可重建"的约束。**

## 一、容器事实

| 文件 | 大小 | 位图条目数 | 清单条目 | 清单大小 | 游戏数 | 平台（确认） |
|---|---|---|---|---|---|---|
| 000.dat | 240,935,839 B | 3905 | 1 | 528,753 B | **8514** | **街机 Arcade**（MAME/FBA ROM 名：kof97/mslug/ffight…） |
| 001.dat | 105,182,879 B | 2911 | 1 | 354,721 B | **4768** | **NES/FC**（超级玛丽/魂斗罗/冒险岛…） |
| 002.dat | 213,233,573 B | 2376 | WQW\x02 段 | — | 2376（截图像） | **SFC/SNES**（清单用 WQW\x02 变体签名存放） |
| 003.dat | 30,374,283 B | ~ | 1 | 16,585 B | 数百 | **MD/Genesis**（冒险岛/魂斗罗铁血兵团….zip） |
| 004.dat | 101,894,922 B | 1073 | 1 | 78,146 B | **1072** | **GBA**（恶魔城月轮/忍者神龟/高级战争…） |
| 005.dat | 8,994,626 B | ~ | 1 | 10,368 B | 数百 | **NES 变体**（洛克人/银河战士/恶魔城/萨尔达…） |
| 006.dat | 12,427,788 B | 150 | 1 | 9,103 B | 150 | **GB/GBC** |
| 007.dat | 3,171,039 B | 28 | 1 | 1,599 B | 28 | **PS1**（已有文档 007_dat_analysis.md） |
| 008.dat | 127,222 B | 9 | 1 | 379 B | 9 | **Atari 2600**（已有文档 008_dat_analysis.md） |

## 二、清单文本格式（统一）

```
<ROM 文件名(.zip)>;<英文名>;<中文名>
kof97.zip;KOF 97;拳皇97
Super Mario Bros 3.zip;Super Mario Bros 3;超级玛丽3
Island adventure.zip;Island adventure;冒险岛
```
- ROM 以 **.zip** 打包（分类目录内实际存放 .zip）；FrogUI/核心按清单内文件名加载。
- 中文名可作 UI 显示；英文名/文件名作 ROM 定位。

## 三、对系统实现的意义

1. **核心映射确认**（与 `docs/00x_dat_platform_mapping.md` 一致）：000→街机(fbalpha/mame2000/cps2/pgm)、001→NES(fceumm/nestopia)、002→SFC(snes9x_plus)、003→MD(picodrive)、004→GBA(mgba/vbam/gpsp)、005→NES 变体(nestopia/fceumm)、006→GB/GBC(tgbdual/mgba)、007→PS1(pcsx_rearmed)、008→Atari2600(stella)。
2. **item 14 落实**：9 个 dat **沿用原设备**（加密容器，无法/不应重建）；FrogUI 的 `get_core_for_folder` 已按 000–008 映射核心，ROM 从各 `NNN/` 目录取。
3. **FrogUI 菜单可直接消费清单**：若需原生列出游戏名，可解析清单文本（工具：`deploy/dat_parse.py`）或直接沿用 FrogUI 现有目录扫描。
4. 002.dat 的清单在 **WQW\x02** 段（其他为 \x03）——解析器需同时支持 \x01/\x02/\x03 签名。

## 四、解析工具

`deploy/dat_parse.py`（本报告同批交付）：遍历 WQW 容器，提取各 dat 的清单文本到 `_dat_texts_<date>.txt`，供 UI/测试用。
