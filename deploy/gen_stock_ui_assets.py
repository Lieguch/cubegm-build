#!/usr/bin/env python3
"""
gen_stock_ui_assets.py -- build-time generator for stock CubeGM UI assets.
Reads 原厂SD卡根目彁结构/000-008/*.dat (WQW\x03 obfuscated zip) and emits:

  deploy/cubegm/covers/00.idx..08.idx   binary cover index (StockCoverIdx @16B)
  deploy/cubegm/gamelist/00.txt..08.txt UTF-8 game lists ("rom|en|cn" per line)
  deploy/cubegm/layout.txt              decoded sprite layout table (debug/doc)

Usage: python3 gen_stock_ui_assets.py <原厂SD卡根目录结构> <deploy/cubegm>
"""
import re, zlib, struct, os, sys, codecs

MAGIC = b"WQW\x03"

def iter_entries(data, sig=b"WQW\x03"):
    for m in re.finditer(re.escape(sig), data):
        o = m.start()
        try:
            ver, flags, method, ctime, cdate, crc, cs, us, fnlen, exlen = \
                struct.unpack_from("<HHHHHIIIHH", data, o + 4)
        except struct.error:
            break
        hdr = o + 30 + fnlen + exlen
        if hdr + cs > len(data):
            break
        comp = data[hdr:hdr + cs]
        try:
            raw = zlib.decompress(comp, -15) if method == 8 else comp
            ok = True
        except Exception:
            raw = b""
            ok = False
        yield o, cs, us, fnlen, raw, ok

def iter_entries_any(data):
    """主同时支持 WQW\x03 与 WQW\x02 变体（002.dat 用 \x02）"""
    seen = set()
    for sig in (b"WQW\x03", b"WQW\x02"):
        for (o, cs, us, fnlen, raw, ok) in iter_entries(data, sig):
            key = (o, cs, us)
            if key in seen:
                continue
            seen.add(key)
            yield o, cs, us, fnlen, raw, ok

def gbk_to_utf8(b):
    if isinstance(b, str):
        return b
    for enc in ("gbk", "gb18030"):
        try:
            return b.decode(enc)
        except Exception:
            continue
    return b.decode("utf-8", "replace")

def build_idx(srcroot, dstroot):
    os.makedirs(os.path.join(dstroot, "covers"), exist_ok=True)
    os.makedirs(os.path.join(dstroot, "gamelist"), exist_ok=True)
    total_covers = 0
    for cat in range(9):
        catdir = os.path.join(srcroot, "%03d" % cat)
        datp = os.path.join(catdir, "%03d.dat" % cat)
        if not os.path.exists(datp):
            print(f"[{cat:03d}] DAT missing, skip")
            continue
        data = open(datp, "rb").read()
        entries = list(iter_entries_any(data))
        rows = []
        idxrows = []
        texts = []
        fnlist = []
        for (o, cs, us, fnlen, raw, ok) in entries:
            if not ok:
                continue
            # zip 头内文件名：data[o+30 : o+30+fnlen]（GBK）
            fnameraw = data[o+30 : o+30+fnlen]
            try:
                fname = fnameraw.decode("gbk", errors="replace").strip()
            except Exception:
                fname = ""
            fnlist.append(fname)
            if us == 307200:
                idxrows.append((o, cs, us, 0))
            else:
                try:
                    t = gbk_to_utf8(raw)
                    lines = [l for l in t.splitlines() if l.strip()]
                    if lines and len(t) > 100:
                        texts.append(lines)
                except Exception:
                    pass
        # game list text = largest text block
        gamelines = None
        if texts:
            gamelines = max(texts, key=len)
        if gamelines:
            for l in gamelines:
                parts = l.split(";")
                rom = parts[0].strip() if len(parts) > 0 else ""
                en = parts[1].strip() if len(parts) > 1 else ""
                cn = ";".join(parts[2:]).strip() if len(parts) > 2 else ""
                rows.append(f"{rom}|{en}|{cn}")
        elif fnlist:
            # 容器无文本列表（如 002.dat）：用 zip 头内 GBK 文件名做 rom 名
            for (o, cs, us, fnlen, raw, ok) in entries:
                if not ok or us != 307200:
                    continue
                nm = data[o+30 : o+30+fnlen].decode("gbk", errors="replace").strip()
                nm = nm.replace("/", "_")
                rows.append(f"{nm}|{nm}|{nm}")
        with open(os.path.join(dstroot, "covers", f"{cat:02d}.idx"), "wb") as f:
            for (o, cs, us, r) in idxrows:
                f.write(struct.pack("<IIII", o, cs, us, r))
        with open(os.path.join(dstroot, "gamelist", f"{cat:02d}.txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(rows) + ("\n" if rows else ""))
        total_covers += len(idxrows)
        print(f"[{cat:03d}] covers={len(idxrows)} list={len(rows)} (dat {len(data)/1e6:.1f}MB)")
    print(f"TOTAL covers: {total_covers}")

if __name__ == "__main__":
    src = sys.argv[1] if len(sys.argv) > 1 else "/workspace/原厂SD卡根目录结构"
    dst = sys.argv[2] if len(sys.argv) > 2 else "/workspace/cubegm/cubegm-latest/deploy/cubegm"
    build_idx(src, dst)