# root.dat 文件分析报告

> 分析对象：`R:\aa\root.dat`（已随系统整体复制到 `C:\Users\Administrator\cubegm-work\cubegm\root.dat`）
> 方法：二进制 magic 解析 + 混淆 ZIP 局部头遍历 + raw DEFLATE 解压，全部基于文件实证。

---

## 一、基本事实

| 项 | 值 |
|----|----|
| 大小 | 725,382 字节（约 709 KB） |
| 位置 | SD 卡根目录（与 `cubegm/`、`Roms/` 并列，不在 `cubegm/` 内） |
| Magic（前 4 字节） | `57 51 57 03` = ASCII `WQW\x03` |
| 条目数 | 10 个 |

## 二、它是什么

**`root.dat` 不是一个普通数据文件，而是一个"签名混淆 + 文件名混淆"的 ZIP 容器。**

- 它的本地文件头签名是 `WQW\x03`，**不是**标准 ZIP 的 `PK\x03\x04`。这正是逆向笔记（g3gg0.de，针对 SF3000/DataFrog 固件）里描述的 `UI_Res.cpd` / `resource.cpd` 同款混淆方案。
- 标准压缩工具（Python `zipfile`、WinRAR 等）**无法直接打开**它，因为：
  1. 签名被改成 `WQW\x03`；
  2. 文件名被混淆为一串随机字节（见下）；
  3. **没有标准 central directory**（EOCD），普通工具只能识别 patch 签名后的第一条记录。

## 三、内部结构（解析结果）

| 条目 | 压缩方式 | 压缩大小 | 未压缩大小 | 文件名（混淆字节） |
|------|----------|----------|------------|--------------------|
| 0–8（共 9 个） | 8 (raw deflate) | 687 B | 691,200 B | 7 字节混淆（如 `\xd5\xd5\xd5\xcb\x97\x84\x92`） |
| 9（1 个） | 8 (raw deflate) | 718,267 B | 2,373,964 B | 12 字节混淆 |

- **`flags = 0x0000`**：条目本身**未加密**（g3gg0.de 提到的硬编码密码 `hichip123` 只在 libzip 报告条目被标记为加密时才使用；此文件所有条目 flag=0，是 raw DEFLATE）。
- **文件名混淆**：不是可读文件名，而是 7/12 字节的随机序列，无法直接得知每个条目原本叫什么。
- **解压后是纯二进制、无 ASCII 文本**：9 个 691,200 字节条目 = **720 × 480 × 2（RGB565 帧缓冲）**；第 9 个 2.37 MB 为更大的位图/混合资源。说明这些条目全是**界面/启动画面的位图资源**，不是配置或文本。

## 四、它的用途

`root.dat` 是 CubeGM 固件的**系统级 UI / 启动画面资源包**（区别于用户可自由替换的 `ui_*.zip` 语言界面包）：

- `ui_*.zip`（标准 ZIP，9 种语言）：菜单/游戏/设置/分类/搜索 5 张界面图，**用户可换主题**。
- `root.dat`（混淆 ZIP，WQW\x03）：开机 Logo、默认/系统级背景、硬件相关位图等，**厂商锁定、普通用户不碰**。

> 注：你这份固件里 `ui_*.zip` 是**标准 ZIP**（可用 Python `zipfile` 正常打开），而 `root.dat` 用的是混淆签名——说明二者打包规范不同，root.dat 属于更底层、厂商级的资源容器。

## 五、如何读取它（给后续开发用）

需要自定义解析器（不能靠标准 unzip）：

```python
import re, zlib, struct
d = open('root.dat','rb').read()
sig = b'WQW\x03'
for o in [m.start() for m in re.finditer(re.escape(sig), d)]:
    ver,flags,method,ct,cd,crc,cs,us,fnlen,exlen = struct.unpack_from('<HHHHHIIIHH', d, o+4)
    fn = d[o+30:o+30+fnlen]
    comp = d[o+30+fnlen+exlen : o+30+fnlen+exlen+cs]
    raw = zlib.decompress(comp, -15) if method == 8 else comp   # raw DEFLATE
    # raw 即 720x480 RGB565 帧缓冲（或其他位图），可直接送帧缓冲/转 PNG
```

要点：把 `WQW\x03` 当 `PK\x03\x04` 逐个遍历 local header，用 stored `compressed_size` 定位下一条，raw DEFLATE 解压即可得到位图像素。

## 六、风险提醒

- 这只是**资源容器**，不含可执行代码，修改它不会影响系统逻辑，但改错会导致开机画面异常。
- 文件名混淆 + 无 central directory，意味着**无法可靠地"增删条目"**，只能整体替换整包，且替换包须保持同样的 `WQW\x03` 混淆格式，否则固件读不出。
- 若要换开机画面，最稳妥的路径是：在 `ui_*.zip` 主题层改，而不是动 `root.dat`。
