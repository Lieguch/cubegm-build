#!/usr/bin/env bash
# verify_target_abi.sh <binary> [<binary> ...]
#   Assert every binary matches the RK3036G device ABI, determined empirically
#   from the 20 device cores (tools/probe_device_abi.py):
#     32-bit ELF, EM_ARM, E_FLAGS=0x5000400 (armhf + EABIv5), max GLIBC_2.17.
#   Run this on EVERY built binary before shipping to the device.
#   Exits 0 only if ALL binaries pass; exits 1 if any fail; exits 2 on usage.
set -u
if [ "$#" -eq 0 ]; then echo "usage: $0 <binary> [<binary> ...]"; exit 2; fi

RC=0
for BIN in "$@"; do
    [ -f "$BIN" ] || { echo "ERROR: not a file: $BIN"; RC=1; continue; }
    python3 - "$BIN" <<'PY' || RC=1
import sys, struct, re
f = sys.argv[1]
d = open(f, 'rb').read()
ok = True
def fail(msg):
    global ok; ok = False; print("  FAIL:", msg)

if d[:4] != b'\x7fELF':
    fail("not an ELF file")
else:
    ei_class = d[4]
    e_machine = struct.unpack_from('<H', d, 18)[0]
    e_flags   = struct.unpack_from('<I', d, 36)[0]
    if ei_class != 1:
        fail("EI_CLASS=%d (need 1 = 32-bit)" % ei_class)
    else:
        print("  OK   32-bit ELF")
    if e_machine != 40:
        fail("E_MACHINE=%d (need 40 = EM_ARM)" % e_machine)
    else:
        print("  OK   EM_ARM")
    if e_flags != 0x5000400:
        fail("E_FLAGS=0x%X (need 0x5000400 armhf+EABIv5)" % e_flags)
    else:
        print("  OK   armhf + EABIv5 (0x5000400)")
    gl = set(re.findall(rb'GLIBC_2\.\d+', d))
    maxv = None
    for g in gl:
        v = tuple(int(x) for x in g.decode().split('_')[1].split('.'))
        if maxv is None or v > maxv:
            maxv = v
    if maxv and maxv > (2, 17):
        fail("requires GLIBC_2.%d.%d > device ceiling 2.17" % maxv)
    else:
        s = " (max GLIBC_2.%d.%d)" % maxv if maxv else " (no GLIBC version need)"
        print("  OK   GLIBC ceiling <= 2.17" + s)

if ok:
    print("RESULT: PASS  %s" % f)
else:
    print("RESULT: FAIL  %s" % f)
    sys.exit(1)
PY
done
exit $RC
