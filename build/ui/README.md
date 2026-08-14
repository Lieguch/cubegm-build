# UI 资源规范（CubeGM 开源替代系统）

> 配套工具：`tools/render_raw.py`（RGB565 1280×720 解码/编码/预览）

## 一、格式（实测，非推测）

| 项 | 值 |
|---|---|
| 像素格式 | **RGB565（小端，每像素 2 字节）** |
| 分辨率 | **1280 × 720** |
| 字节布局 | 字节 0 起 = 完整一帧（1,843,200 B = 1280×720×2），尾部为精灵/分层 |
| 反例 | `root.dat` 的条目是 **720×480**（仅启动 logo 区），**不适用于 ui_*.raw** |
| 设备双分辨率 | UI 1280×720 vs 原厂 720×480 并存 → 渲染目标固定 **1280×720** |

## 二、解码一个 UI 资源

```bash
python tools/render_raw.py decode cubegm/cubegm/ui_cn.zip  # 需先解压 zip
# 或直接对 .raw：
python tools/render_raw.py decode path/to/menu.raw menu.png
```

RGB565 → RGB888 公式（见 `render_raw.py`）：
```
R = ((v >> 11) & 0x1F) << 3
G = ((v >> 5)  & 0x3F) << 2
B = ( v        & 0x1F) << 3
```

## 三、生成 / 替换一个 UI 资源

```bash
python tools/render_raw.py encode my_menu.png menu.raw 1280 720
```

生成的 `menu.raw` 可直接放入 `cubegm/ui_xxx.zip` 对应槽位，picoarch 前端按原厂布局加载。

## 四、集成到构建

- UI 资源随 `make package` 一并打入 `cubegm_pkg/`，保持 `ui_*.raw` 命名与槽位。
- 任何 UI 资源改动后，用 `render_raw.py decode` 回读校验字节数 = 1,843,200（1280×720×2）。
- 新增分辨率/皮肤：复制 `ui_*.zip` 结构，保持 RGB565 1280×720；不要混入 720×480。

## 五、QA 要点（严过关）

- 字节级校验：每个 `ui_*.raw` 解出的帧尺寸必须严格 1280×720。
- 颜色保真：抽样像素 RGB565→RGB888 还原后无明显偏色。
- 槽位一致：资源名与前端加载索引对应（参考 `ui_cn.zip` 等原厂 zip 的条目清单）。
