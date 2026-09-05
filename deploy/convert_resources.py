#!/usr/bin/env python3
"""
convert_resources.py — 把解码的原厂 UI 资源转成 FrogUI 可加载的 RGB565 raw 格式

用途：
1. 原厂 .raw（精灵表）→ 提取整屏背景 → PNG（阶段1已完成）
2. PNG → RGB565 raw（本脚本，供 FrogUI load_background() 加载）

RGB565 格式：每像素 2 字节，little-endian，R(5bit) G(6bit) B(5bit)
与 FrogUI render.c 的 load_raw_rgb565() 完全一致。

输出到 new_cubegm/资源提取/res/ 目录，部署到设备的 /mnt/sdcard/cubegm/res/
"""

import struct
import os
from PIL import Image

SRC_DIR = '/workspace/new_cubegm/资源提取'
OUT_DIR = '/workspace/new_cubegm/资源提取/res'

def rgb888_to_rgb565(r, g, b):
    """RGB888 (0-255) → RGB565 (16-bit)"""
    r5 = (r >> 3) & 0x1F
    g6 = (g >> 2) & 0x3F
    b5 = (b >> 3) & 0x1F
    return (r5 << 11) | (g6 << 5) | b5

def png_to_rgb565(png_path, out_path):
    """PNG → RGB565 raw"""
    img = Image.open(png_path).convert('RGB')
    w, h = img.size
    pixels = list(img.getdata())
    
    data = bytearray()
    for r, g, b in pixels:
        val = rgb888_to_rgb565(r, g, b)
        data += struct.pack('<H', val)
    
    with open(out_path, 'wb') as f:
        f.write(data)
    return w, h

def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    
    # 5 个界面背景
    mappings = {
        'menu_背景.png': 'menu.rgb565',
        'search_背景.png': 'search.rgb565',
        'setting_背景.png': 'setting.rgb565',
        'type_背景.png': 'type.rgb565',
        'game_背景.png': 'game.rgb565',
    }
    
    print("=== 转换原厂背景 → RGB565 raw ===")
    for png_name, out_name in mappings.items():
        png_path = os.path.join(SRC_DIR, png_name)
        out_path = os.path.join(OUT_DIR, out_name)
        if not os.path.exists(png_path):
            print(f"  ✗ {png_name} 不存在，跳过")
            continue
        w, h = png_to_rgb565(png_path, out_path)
        size = os.path.getsize(out_path)
        print(f"  ✓ {png_name} → {out_name} ({w}x{h}, {size} bytes)")
        assert size == w * h * 2, f"大小错误: {size} != {w*h*2}"

    print(f"\n完成！RGB565 raw 输出到 {OUT_DIR}")
    print("部署：复制到设备的 /mnt/sdcard/cubegm/res/")

if __name__ == '__main__':
    main()
