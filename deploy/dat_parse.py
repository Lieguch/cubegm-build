#!/usr/bin/env python3
"""dat_parse.py -- extract the game-list text entry from CubeGM category .dat
files (000.dat..008.dat). The files are WQW\\x03 (also \\x01/\\x02 variants)
obfuscated ZIP containers: most entries are 480x320 RGB565 screenshots
(usize=307200), the last entry (or a WQW\\x02 section in 002.dat) is the
game list in '<rom.zip>;<english>;<chinese>' lines, GBK encoded.

Usage:
    python3 dat_parse.py /path/to/000.dat /path/to/002.dat ...   (extract lists)
    python3 dat_parse.py --all                                    (scan a dir tree)

Only text entries are decompressed; bitmap entries are skipped (keeps memory
low even for the 240MB 000.dat). Output: _dat_texts_<date>.txt
"""
import re, zlib, struct, os, sys, datetime

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
        yield us, raw, ok, (o, cs, method)

def extract_list(path):
    data = open(path, "rb").read()
    out = []
    # primary signature WQW\x03
    for us, raw, ok, _ in iter_entries(data):
        if us == 307200 or not ok:
            continue
        try:
            txt = raw.decode("gbk", errors="replace")
            lines = [l for l in txt.splitlines() if l.strip()]
            if lines:
                out.append((len(lines), lines))
        except Exception:
            pass
    # 002.dat variant: WQW\x02 section may hold the list
    for us, raw, ok, _ in iter_entries(data, sig=b"WQW\x02"):
        if not ok or us == 307200:
            continue
        try:
            txt = raw.decode("gbk", errors="replace")
            lines = [l for l in txt.splitlines() if l.strip()]
            if lines:
                out.append((len(lines), lines))
        except Exception:
            pass
    return out

def main():
    files = sys.argv[1:]
    if not files:
        print(__doc__)
        return 1
    stamp = datetime.date.today().isoformat()
    outfile = f"_dat_texts_{stamp}.txt"
    with open(outfile, "w", encoding="utf-8") as fo:
        for f in files:
            name = os.path.basename(f)
            fo.write(f"### {name} ({os.path.getsize(f)} B)\n")
            try:
                lists = extract_list(f)
                if not lists:
                    fo.write("--- no text list found ---\n")
                for n, lines in lists:
                    fo.write(f"--- list: {n} games ---\n")
                    fo.write("\n".join(lines[:50]))
                    fo.write(f"\n--- ... total {n} ---\n")
            except Exception as e:
                fo.write(f"--- ERROR: {e} ---\n")
    print("wrote", outfile)
    return 0

if __name__ == "__main__":
    sys.exit(main())
