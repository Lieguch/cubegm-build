#!/usr/bin/env python3
"""
gen_rgui_assets.py — CubeGM 原厂 Thumbnails 移植到 RetroArch RGUI 的构建期生成器
================================================================================
生成：2026-09-14 ｜ 基线：478（c82e9c5）｜ RA：826219d（RGUI 缩略图机制同 282a12d）

输入：原厂 SD 卡结构 /path/to/<SD根>/000-008/*.dat（WQW\\x03 混淆 ZIP 容器）
输出：<dst>/configs/retroarch/playlists/<Playlist>.lpl          （JSON 1.0）
      <dst>/configs/retroarch/thumbnails/<Playlist>/Named_Boxarts/<rom>.png
                                                    （160×107 调色板 PNG）

完全遵循 libretro 官方文档（docs.libretro.com/guides/roms-playlists-thumbnails）：
  * thumbnails/<Playlist 名>/Named_Boxarts/<label>.png
  * 文件名 = playlist 条目 label，官方 1.17+ 三种匹配方式第 1 种：ROM 文件名 ↔ png 文件名
  * 非法字符 &*/:\\<>?| 替换为 _
  * playlist JSON 格式 version 1.0（path/label/core_path/core_name/crc32/db_name）

原厂 dat 解码（反编译 + 逐字节实证）：
  * 条目 = [WQW\\x03][ZIP 头 22B][fnlen][fname=XOR 0xE5 加密 "<rom>_NNN.raw"][exlen][zlib data]
  * 封面条目 usize=307200 = 480×320×2 RGB565 LE（raw DEFLATE method=8）
  * 文本清单条目：GBK 编码 "rom.zip;英文名;中文名"，CRLF 行
"""
import re, zlib, struct, os, sys, json, argparse

MAGIC3 = b'WQW\x03'
XOR_KEY = 0xE5
COVER_US = 307200          # 480*320*2
COVER_W, COVER_H = 480, 320
THUMB_W, THUMB_H = 160, 107

# 分类 → (Playlist 名, default core, RDB 官方数据库名, 默认核心 .so, 默认核心显示名)。
# 方案甲（RDB 官网标准）：db_name=官方数据库名（驱动内容扫描精确识别 + 缩略图目录），
#   default_core_path 预设首选核心（加载直接用；可切）。
RDB_BASE = '/mnt/sdcard/cubegm/cores/'
PLATFORM = {
    0: ('000-Arcade',       'DETECT', 'FBNeo - Arcade Games',                        'fbneo_libretro.so',            'fbneo'),
    1: ('001-NES',          'DETECT', 'Nintendo - Nintendo Entertainment System',    'fceumm_libretro.so',           'fceumm'),
    2: ('002-SNES',         'DETECT', 'Nintendo - Super Nintendo Entertainment System', 'snes9x2005_libretro.so',   'snes9x2005'),
    3: ('003-MegaDrive',    'DETECT', 'Sega - Mega Drive - Genesis',                 'genesis_plus_gx_libretro.so',  'genesis_plus_gx'),
    4: ('004-GBA',          'DETECT', 'Nintendo - Game Boy Advance',                 'mgba_libretro.so',             'mgba'),
    5: ('005-GB',           'DETECT', 'Nintendo - Game Boy',                         'gambatte_libretro.so',         'gambatte'),
    6: ('006-GBC',          'DETECT', 'Nintendo - Game Boy Color',                   'gambatte_libretro.so',         'gambatte'),
    7: ('007-PlayStation',  'DETECT', 'Sony - PlayStation',                          'pcsx_rearmed_libretro.so',     'pcsx_rearmed'),
    8: ('008-Atari2600',    'DETECT', 'Atari - 2600',                                'stella2014_libretro.so',       'stella2014'),
}

# 原厂 libemu_*.so → 478 已部署 libretro core 名（filelist.xml 例外核心转换表）
CORE_ALIAS = {
    'libemu_fbalpha2012.so': 'fbalpha2012_libretro.so',
    'libemu_fba.so':         'fbalpha2012_libretro.so',
    'libemu_fbafast.so':     'fbneo_libretro.so',
    'libemu_extend.so':      'fbneo_libretro.so',
    'libemu_cps2.so':        'fbalpha2012_cps2_libretro.so',
    'libemu_mame2000.so':    'mame2000_libretro.so',
    'libemu_snes9x.so':      'snes9x2010_libretro.so',  # 478 DEFAULT_CORES 无 snes9x，用 snes9x2010
    'libemu_snes9x2010.so':  'snes9x2010_libretro.so',
    'libemu_mgba.so':        'mgba_libretro.so',
    'libemu_nes.so':         'fceumm_libretro.so',
    'libemu_nestopia.so':    'nestopia_libretro.so',
    'libemu_pcsx.so':        'pcsx_rearmed_libretro.so',
    'libemu_gpsp.so':        'gpsp_libretro.so',
}


def iter_entries(data, sig=MAGIC3):
    """
    顺序偏移定位法遍历 WQW\\x03 混淆 ZIP 条目。
    真实条目紧密排列：下一条 offset = 当前 hdr + csize。
    （不用全盘 re.finditer —— 压缩数据内部可能偶然出现 WQW\\x03 假阳性，
     002.dat 实测第 2376 条就是假阳性，method/cs 全垃圾。）
    产出 (offset, method, csize, usize, fn_len, fn_raw)。
    """
    o = data.find(sig)
    while o >= 0 and o + 30 <= len(data):
        try:
            (_, _, method, _, _, crc, cs, us, fnlen, exlen) = \
                struct.unpack_from('<HHHHHIIIHH', data, o + 4)
        except struct.error:
            break
        if fnlen > 4096 or o + 30 + fnlen + exlen > len(data):
            # 头字段非法 = 假阳性，向后找下一个魔数
            o = data.find(sig, o + 1)
            continue
        hdr = o + 30 + fnlen + exlen
        if hdr + cs > len(data):
            break
        fn = data[o + 30:o + 30 + fnlen]
        yield o, method, cs, us, exlen, fn
        o = hdr + cs           # 顺序定位下一条


def xor_name(fn):
    return bytes(b ^ XOR_KEY for b in fn)


def gbk_to_utf8(raw):
    for enc in ('gbk', 'gb18030'):
        try:
            return raw.decode(enc)
        except Exception:
            continue
    return raw.decode('utf-8', errors='replace')


def sanitize_label(s):
    """官方规则：与 RA 源码 gfx_thumbnail_fill_content_img 的 strpbrk("&*/:`\\"<>?\\|") 完全一致。
    这些字符在缩略图文件名里换成 _（兼容 No-Intro 文件名标准）。"""
    for ch in '&*/:`"<>?\\|':
        s = s.replace(ch, '_')
    return s


def load_filelist(sdcard_root):
    """解析原厂 cores/filelist.xml → {分类号: {rom名: libretro core 名}}。"""
    fl = os.path.join(sdcard_root, 'cubegm', 'cores', 'filelist.xml')
    mapping = {}
    if not os.path.exists(fl):
        return mapping
    for m in re.finditer(
            r'<file\s+name="(\d{3})/([^"]+)"\s+core="([^"]+)"', open(fl, encoding='utf-8').read()):
        cat, rom, core = m.group(1), m.group(2), m.group(3)
        mapping.setdefault(int(cat), {})[rom] = CORE_ALIAS.get(core, core)
    return mapping


def parse_dat(dat_path, filelist_map=None):
    """
    解 dat → (covers, games)
      covers: [(rom_base, png_bytes)]   封面，rom_base = XOR 解密的条目名去 _NNN.raw
      games:  [(rom, en, cn)]           文本清单行（rom 可能带扩展名）
    filelist_map: {rom名: core名} 逐游戏 core 例外（000 街机用）
    """
    data = open(dat_path, 'rb').read()
    covers = []
    games = []
    row_id = 0
    for (o, method, cs, us, exlen, fn) in iter_entries(data):
        hdr = o + 30 + len(fn) + exlen
        comp = data[hdr:hdr + cs]
        try:
            raw = zlib.decompress(comp, -15) if method == 8 else comp
        except Exception:
            continue
        if us == COVER_US:
            dec = xor_name(fn)
            try:
                fn_utf8 = dec.decode('utf-8', errors='replace')
            except Exception:
                fn_utf8 = dec.decode('gbk', errors='replace')
            base_full = fn_utf8.strip()
            mframe = re.search(r'_(\d{3})\.raw$', base_full)
            frame = int(mframe.group(1)) if mframe else 0
            base = re.sub(r'_\d{3}\.raw$', '', base_full)
            if base:
                covers.append((base, frame, raw))
        else:
            txt = gbk_to_utf8(raw)
            for line in txt.splitlines():
                line = line.strip()
                if not line:
                    continue
                parts = line.split(';')
                if len(parts) >= 3:
                    rom = parts[0].strip()
                    en = parts[1].strip()
                    cn = ';'.join(parts[2:]).strip()
                elif len(parts) == 2:
                    rom, en = parts[0].strip(), parts[1].strip()
                    cn = ''
                else:
                    continue
                # rom 名去扩展名（官方匹配用 label）
                base = rom.rsplit('.', 1)[0] if '.' in rom else rom
                core = filelist_map.get(rom) if filelist_map else None
                games.append({'idx': row_id, 'rom': rom, 'base': base, 'en': en,
                              'cn': cn, 'core': core})
                row_id += 1
    return covers, games


def make_palette_png(raw, dw=THUMB_W, dh=THUMB_H):
    """480×320 RGB565 LE → 160×107 调色板 PNG（3-3-2 量化，官方支持的基础尺寸）。"""
    sw, sh = COVER_W, COVER_H
    idx = bytearray(dw * dh)
    for y in range(dh):
        sy = y * sh // dh
        for x in range(dw):
            sx = x * sw // dw
            v = struct.unpack_from('<H', raw, (sy * sw + sx) * 2)[0]
            r = (v >> 11) & 0x1f
            g = (v >> 5) & 0x3f
            b = v & 0x1f
            idx[y * dw + x] = ((r >> 2) << 5) | ((g >> 3) << 2) | (b >> 2)
    pal = bytearray(256 * 3)
    for i in range(256):
        pal[i * 3] = ((i >> 5) & 7) << 5
        pal[i * 3 + 1] = ((i >> 2) & 7) << 5
        pal[i * 3 + 2] = (i & 3) << 6
    raw_lines = b''
    for y in range(dh):
        raw_lines += b'\x00' + bytes(idx[y * dw:(y + 1) * dw])
    comp = zlib.compress(raw_lines, 9)

    def chunk(tag, payload):
        c = struct.pack('>I', len(payload)) + tag + payload
        return c + struct.pack('>I', zlib.crc32(tag + payload) & 0xffffffff)

    ihdr = struct.pack('>IIBBBBB', dw, dh, 8, 3, 0, 0, 0)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) +
            chunk(b'PLTE', bytes(pal)) + chunk(b'IDAT', comp) + chunk(b'IEND', b''))


def build_playlist(playlist_name, games, idx2core, rdb_name, def_core_so, def_core_name):
    """官方 JSON 1.0 playlist（方案甲 RDB 官网标准）。
    path 指向原厂 000-008 目录；core_path=DETECT（官方自适应可选核心）；
    db_name=官方 RDB 数据库名（内容扫描精确识别 + 缩略图目录 system_name）；
    default_core_path/name 预设首选核心（加载即用，可切）。"""
    items = []
    for g in games:
        label = g['cn'] or g['en'] or g['base']
        cat = playlist_name.split('-')[0]  # '000'
        items.append({
            'path': '/mnt/sdcard/%s/%s' % (cat, g['rom']),
            'label': label,
            'core_path': 'DETECT',
            'core_name': 'DETECT',
            'crc32': '',
            'db_name': rdb_name,          # 官方数据库名（如 "Sega - Mega Drive - Genesis"）
        })
    return {'version': '1.0',
            'default_core_path': RDB_BASE + def_core_so,   # /mnt/sdcard/cubegm/cores/xxx.so
            'default_core_name': def_core_name,
            'label_display_mode': 0, 'items': items}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('sdcard_root', help='原厂 SD 卡根目录（含 000-008/ 与 cubegm/）')
    ap.add_argument('dst', help='输出根目录（会写入 configs/retroarch/）')
    ap.add_argument('--cats', default='all', help='逗号分隔分类号，如 0,2,8 （默认 all)')
    ap.add_argument('--no-png', action='store_true', help='不生成缩略图 PNG（只 lpl + 统计）')
    args = ap.parse_args()

    cats = [int(c) for c in args.cats.split(',')] if args.cats != 'all' else list(range(9))
    flmap = load_filelist(args.sdcard_root)
    arts = os.path.join(args.dst, 'configs', 'retroarch')
    play_dir = os.path.join(arts, 'playlists')
    thumb_root = os.path.join(arts, 'thumbnails')
    os.makedirs(play_dir, exist_ok=True)
    os.makedirs(thumb_root, exist_ok=True)

    total_covers = total_png = total_games = 0
    for cat in cats:
        datp = os.path.join(args.sdcard_root, '%03d' % cat, '%03d.dat' % cat)
        if not os.path.exists(datp):
            print(f'[{cat:03d}] dat 缺失，跳过')
            continue
        pname, def_core, rdb_name, def_so, def_dn = PLATFORM[cat]
        covers, games = parse_dat(datp, flmap.get(cat))
        # 封面 rom 名集合（label 用于缩略图文件名）
        # 封面去重：同一 rom 多帧时保留最小帧号（_000 首帧）
        seen = {}
        for base, frame, raw in covers:
            if base.lower() not in seen or frame < seen[base.lower()][1]:
                seen[base.lower()] = (base, frame, raw)
        covers = [v for _, v in sorted(seen.items())]
        cover_roms = {c[0].lower() for c in covers}
        # 全部 DETECT（官方自适应，idx2core 值恒为 DETECT，仅保留结构兼容）
        idx2core = {g['idx']: def_core for g in games}

        # 1) playlist JSON（方案甲：RDB db_name + default_core）
        pl = build_playlist(pname, games, idx2core, rdb_name, def_so, def_dn)
        pl_path = os.path.join(play_dir, pname + '.lpl')
        with open(pl_path, 'w', encoding='utf-8') as f:
            json.dump(pl, f, ensure_ascii=False, indent=2)

        # 2) 缩略图 PNG（目录用官方 RDB 数据库名 = RA 的 system_name）
        thumb_cat = os.path.join(thumb_root, rdb_name, 'Named_Boxarts')
        os.makedirs(thumb_cat, exist_ok=True)
        n_png = 0
        for base, _frame, raw in covers:
            fn = sanitize_label(base) + '.png'
            png = make_palette_png(raw)
            with open(os.path.join(thumb_cat, fn), 'wb') as f:
                f.write(png)
            n_png += 1
        total_png += n_png

        join = sum(1 for g in games if g['base'].lower() in cover_roms)
        print(f'[{cat:03d}] {pname}: 封面={len(covers)} PNG={n_png} '
              f'清单={len(games)} join={join} ({join * 100 // max(len(games), 1)}%) '
              f'lpl={os.path.basename(pl_path)}')
        total_covers += len(covers)
        total_games += len(games)
    print(f'TOTAL: 封面 {total_covers} / PNG {total_png} / 清单 {total_games}')


if __name__ == '__main__':
    sys.exit(main())