#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""extract_bootchain_assets.py —— 从原厂归档重建 bootchain/assets/ 的全部材料。

背景：qemu 跑原厂启动链需要 4 件材料，它们都能从用户手里的归档**确定性重建**：
  1) org.bin 的 GPT `boot` 分区 = Android boot image（ANDROID! v0）⇒ kernel zImage + resource
  2) org.bin 的 GPT `rootfs` 分区 = squashfs（= 归档里的 rootfs.sqsh）
  3) org.bin解析/rk3036_原厂设备树.dtb
  4) 原厂SD卡/{icube, rkgame, driver.so}

用法:
    python tools/extract_bootchain_assets.py "<解包归档目录>" [输出目录]
例:
    python tools/extract_bootchain_assets.py "F:/OTHER/D20游戏机/解包归档"
"""
import hashlib
import io
import os
import shutil
import struct
import sys

SRC = sys.argv[1] if len(sys.argv) > 1 else r'F:\OTHER\D20游戏机\解包归档'
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '..', 'bootchain', 'assets')
LBA = 512


def gpt_part(buf, name):
    hdr = buf.find(b'EFI PART')
    if hdr < 0:
        raise SystemExit('org.bin 里找不到 GPT header')
    pte_lba, = struct.unpack_from('<Q', buf, hdr + 72)
    nent, esz = struct.unpack_from('<II', buf, hdr + 80)
    po = pte_lba * LBA
    for k in range(nent):
        e = po + k * esz
        if e + 128 > len(buf):
            break
        if buf[e:e + 16] == b'\x00' * 16:
            continue
        n = buf[e + 56:e + 128].decode('utf-16-le').rstrip('\x00')
        if n == name:
            fs, fe = struct.unpack_from('<QQ', buf, e + 32)
            return buf[fs * LBA:(fe + 1) * LBA]
    raise SystemExit('org.bin 里找不到分区 %r' % name)


def main():
    os.makedirs(os.path.join(OUT, 'sdcard'), exist_ok=True)
    org = os.path.join(SRC, 'org.bin解析', 'org.bin')
    print('从 %s 抽取' % org)
    b = open(org, 'rb').read()
    print('  org.bin = %d B (%.2f MiB)' % (len(b), len(b) / 1048576.0))

    boot = gpt_part(b, 'boot')
    assert boot[:8] == b'ANDROID!', 'boot 分区不是 ANDROID! 镜像'
    ks, ka, rs, ra, ss, sa, ta, pg = struct.unpack_from('<8I', boot, 8)
    print('  ANDROID!: kernel=%d ramdisk=%d second=%d page=%d' % (ks, rs, ss, pg))
    off = pg
    kern = boot[off:off + ks]
    off += (ks + pg - 1) // pg * pg
    rd = boot[off:off + rs] if rs else b''
    off += (rs + pg - 1) // pg * pg
    sec = boot[off:off + ss] if ss else b''
    assert len(kern) > 0x2c and struct.unpack_from('<I', kern, 0x24)[0] == 0x016f2818, \
        'kernel 不是 ARM zImage'
    print('  kernel zImage = %d B（[0x24]=0x016f2818 ✓ ARM zImage）' % len(kern))
    open(os.path.join(OUT, 'kernel.zImage'), 'wb').write(kern)
    if sec:
        open(os.path.join(OUT, 'second.bin'), 'wb').write(sec)

    root = gpt_part(b, 'rootfs')
    assert root[:4] == b'hsqs', 'rootfs 分区不是 squashfs'
    open(os.path.join(OUT, 'rootfs.sqsh'), 'wb').write(root)
    print('  rootfs.sqsh = %d B（hsqs ✓）' % len(root))

    shutil.copy2(os.path.join(SRC, 'org.bin解析', 'rk3036_原厂设备树.dtb'),
                 os.path.join(OUT, 'rk3036.dtb'))
    for f in ('icube', 'rkgame', 'driver.so'):
        p = os.path.join(SRC, '原厂SD卡', f)
        if os.path.exists(p):
            shutil.copy2(p, os.path.join(OUT, 'sdcard', f))

    print('\n产物（含 sha256 前 16 位，便于与仓库内 assets 对账）：')
    for dp, dn, fn in os.walk(OUT):
        for f in sorted(fn):
            p = os.path.join(dp, f)
            d = open(p, 'rb').read()
            print('  %-28s %9d B  %s' % (os.path.relpath(p, OUT), len(d),
                                         hashlib.sha256(d).hexdigest()[:16]))


if __name__ == '__main__':
    main()
