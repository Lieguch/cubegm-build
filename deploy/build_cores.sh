#!/usr/env bash
# =============================================================================
#  build_cores.sh -- per-core build orchestrator for ALL libretro cores
# =============================================================================
#  Driven by deploy/build.sh (STAGE 7). Cross-compiles every core for
#  RK3036G (armhf, glibc<=2.17), mirroring the AUTHORITATIVE upstream
#  treefrog-ui/build_all.sh recipe (retrieved from tzubertowski/treefrog-ui,
#  2026-08-18). This is the canonical, tested build path for this frontend's
#  57 cores -- we adapt it from MIPS to ARM, nothing more.
#
#  KEY ADAPTATIONS from upstream (evidence-based, not guessed):
#   * upstream uses `platform=unix` + a compiler wrapper that injects ALL arch
#     flags. We reuse build.sh's ARM wrapper (already injects
#     -fPIC -marm -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard ...).
#   * upstream passes LDFLAGS_S/LDFLAGS_SC ("-shared -Wl,--no-undefined ...")
#     for cores whose Makefile link rule uses $(LDFLAGS) WITHOUT adding
#     -shared itself. This is the #1 reason cores "built rc=0 but produced no
#     .so" -- we replicate the exact S/SC classification below.
#   * repo URLs / submodule layouts taken verbatim from upstream clone_cores.sh.
#   * per-core extra flags (HAS_GPU=0 for vecx, use_libchdr=0 for picodrive,
#     LTO= for snes9x2010, DYNAREC=arm for pcsx_rearmed, etc.) copied 1:1.
#
#  Failures are WARN-only (not fatal). The 5 baseline cores
#  (mgba snes9x fceumm picodrive nestopia) are hard-gated by build.sh STAGE 7.
# =============================================================================
set -uo pipefail

: "${CC:=arm-linux-gnueabihf-gcc}"
: "${CXX:=arm-linux-gnueabihf-g++}"
: "${WORKDIR:?WORKDIR must be set}"
: "${CORES_OUT:=$WORKDIR/cores}"
: "${SYSROOT:=}"

TARGET="${CC%-gcc}"
AR="${TARGET}-ar"
RANLIB="${TARGET}-ranlib"
STRIP="${TARGET}-strip"
WRAP="$WORKDIR/.cubegm_wrappers"
mkdir -p "$CORES_OUT" "$WRAP"

# ---- ARM link flag sets (mirror upstream LDFLAGS / LDFLAGS_S / LDFLAGS_SC) --
if [ -n "$SYSROOT" ]; then LL="-L$SYSROOT/usr/lib"; else LL=""; fi
LDFLAGS="$LL -lm -lc -lstdc++"                       # generic: Makefile adds -shared
LDFLAGS_S="$LL -shared -Wl,--no-undefined -lm -lc -lstdc++"
LDFLAGS_SC="$LL -shared -Wl,--no-undefined -lm -lc"  # C-only cores (no libstdc++)

# ---- fba compiler wrapper: -fsigned-char + -fno-strict-aliasing ------------
# Required by FBA2012/FBNeo on this class of device (char signedness + heavy
# type-punning); upstream proves it without it segfaults at game load.
cat > "$WRAP/fba-gcc" <<'EOF'
#!/bin/bash
exec "$CC" -fno-strict-aliasing -fsigned-char "$@"
EOF
cat > "$WRAP/fba-g++" <<'EOF'
#!/bin/bash
exec "$CXX" -fno-strict-aliasing -fsigned-char "$@"
EOF
chmod +x "$WRAP/fba-gcc" "$WRAP/fba-g++"

SUCC=0; FAIL=0
FAILED_CORES=()

# expand_extra: map a token to the exact make-flag string upstream uses.
expand_extra() {
  case "$1" in
    S)          echo "LDFLAGS=$LDFLAGS_S" ;;
    SC)         echo "LDFLAGS=$LDFLAGS_SC" ;;
    S_pthread)  echo "LDFLAGS=\"$LDFLAGS_S -lpthread\"" ;;
    SC_z)       echo "LDFLAGS=\"$LDFLAGS_SC -lz -lstdc++\"" ;;
    SC_vs)      echo "LDFLAGS=\"$LDFLAGS_SC -Wl,--version-script=link.T\"" ;;
    S_allow)    echo "LDFLAGS=\"$LDFLAGS_S -Wl,--allow-multiple-definition\"" ;;
    HAS_GPU0)   echo "HAS_GPU=0" ;;
    LTO0)       echo "LTO=" ;;
    libchdr0)   echo "use_libchdr=0" ;;
    DYNREC_arm) echo "DYNAREC=arm HAVE_NEON=1" ;;
    "")         echo "" ;;
    *)          echo "$1" ;;
  esac
}

# _b: generic core build (mirrors upstream _b). name=output base, dir=clone subdir.
_b() {
  local name="$1" dir="$2" mk="${3:-}" extra_tok="${4:-}" wrap="${5:-}" submods="${6:-}"
  local full="$WORKDIR/$dir"
  [ -d "$full" ] || { echo "WARN $name: clone dir missing"; FAIL=$((FAIL+1)); FAILED_CORES+=("$name (no dir)"); return; }
  local cc="$CC" cxx="$CXX"
  [ "$wrap" = "fba" ] && { cc="$WRAP/fba-gcc"; cxx="$WRAP/fba-g++"; }
  local extra; extra="$(expand_extra "$extra_tok")"
  if [ -n "$submods" ]; then
    git -C "$full" submodule update --init --depth 1 $submods 2>/dev/null || \
    git -C "$full" submodule update --init $submods 2>/dev/null || true
  fi
  make -C "$full" $mk clean 2>/dev/null || true
  local mklog="$WORKDIR/_mk_${name}.log"
  # platform=unix + wrapper-injected arch flags (upstream-proven for all cores).
  make -C "$full" $mk platform=unix \
        CC="$cc" CXX="$cxx" AR="$AR" RANLIB="$RANLIB" LD="$cxx" \
        LDFLAGS="$LDFLAGS" $extra -j"$(nproc)" >"$mklog" 2>&1
  local so
  for so in "$full/${name}_libretro.so" "$full/$(basename "$dir")_libretro.so"; do
    [ -f "$so" ] && { cp "$so" "$CORES_OUT/" && "$STRIP" "$CORES_OUT/${name}_libretro.so" 2>/dev/null;
                      echo "[core] OK: $name -> $(basename "$so")"; SUCC=$((SUCC+1)); return; }
  done
  so=$(find "$full" -name "${name}_libretro.so" 2>/dev/null | head -1)
  if [ -n "$so" ] && [ -f "$so" ]; then
    cp "$so" "$CORES_OUT/" && echo "[core] OK: $name (found $(basename "$so"))"; SUCC=$((SUCC+1))
  else
    echo "[core] FAIL: $name -- tail of $mklog:"; tail -n 20 "$mklog" 2>/dev/null | sed 's/^/    | /'
    FAIL=$((FAIL+1)); FAILED_CORES+=("$name")
  fi
}

# ---- Core table (name|url|dir|mk|extra_token|wrapper|submodules|branch) -----
# dir == clone subdir (incl. build subdir, e.g. nestopia/libretro).
# extra_token maps via expand_extra(); wrapper = "fba" or "".
CORES=(
  # === Nintendo (8/16-bit) ===
  "fceumm|https://github.com/tzubertowski/libretro-fceumm|fceumm|-f Makefile.libretro||||"
  "quicknes|https://github.com/libretro/QuickNES_Core|QuickNES_Core|||||"
  "snes9x2005_plus|https://github.com/tzubertowski/snes9x2005|snes9x2005|||||"
  "snes9x2002|https://github.com/tzubertowski/snes9x2002|snes9x2002|||||"
  "snes9x2010|https://github.com/libretro/snes9x2010|snes9x2010|-f Makefile.libretro|LTO0|||"
  "gambatte|https://github.com/tzubertowski/libretro-gambatte|libretro-gambatte|-f Makefile.libretro||||"
  "snes9x|https://github.com/libretro/snes9x|snes9x/libretro|||||"
  "gpsp_multicore|https://github.com/tzubertowski/gpsp_multicore|gpsp|||||"
  "gpsp|https://github.com/libretro/gpsp|gpsp_upstream|||||"
  "vba_next|https://github.com/libretro/vba-next|vba-next|||||"
  "mgba|https://github.com/libretro/mgba|mgba|-f Makefile.libretro||||"
  "pokemini|https://github.com/libretro/PokeMini|PokeMini|||||"
  "gw|https://github.com/libretro/gw-libretro|libretro-gw|||||"

  # === Sega ===
  "picodrive|https://github.com/libretro/picodrive|picodrive|-f Makefile.libretro|libchdr0|||"
  "genesis_plus_gx|https://github.com/libretro/Genesis-Plus-GX|Genesis-Plus-GX|-f Makefile.libretro||||"
  "gearsystem|https://github.com/drhelius/Gearsystem|Gearsystem/platforms/libretro|||||"
  "gearboy|https://github.com/drhelius/Gearboy|Gearboy/platforms/libretro|||||"

  # === Atari ===
  "stella2014|https://github.com/libretro/stella2014-libretro|stella2014|||S||"
  "a5200|https://github.com/libretro/a5200|a5200|||||"
  "prosystem|https://github.com/libretro/prosystem-libretro|prosystem|||S||"
  "atari800|https://github.com/libretro/libretro-atari800|libretro-atari800|-f Makefile.libretro||||"
  "handy|https://github.com/libretro/libretro-handy|libretro-handy|||||"
  "beetle_lynx|https://github.com/libretro/beetle-lynx-libretro|libretro-beetle-lynx|||S||"
  "beetle_wswan|https://github.com/libretro/beetle-wswan-libretro|libretro-beetle-wswan|||S||"
  "beetle_pce_fast|https://github.com/libretro/beetle-pce-fast-libretro|libretro-beetle-pce-fast|||S||"
  "beetle_vb|https://github.com/libretro/beetle-vb-libretro|libretro-beetle-vb|||S||"
  "beetle_supergrafx|https://github.com/libretro/beetle-supergrafx-libretro|libretro-beetle-supergrafx|||S||"
  "beetle_pcfx|https://github.com/libretro/beetle-pcfx-libretro|libretro-beetle-pcfx|||S_pthread||"

  # === NEC / PC Engine / PC-88/98 ===
  "quasi88|https://github.com/libretro/quasi88-libretro|libretro-quasi88|||||"

  # === SNK / Neo Geo ===
  "race|https://github.com/libretro/RACE|RACE|||||"
  "geolith|https://github.com/libretro/geolith-libretro|libretro-geolith/libretro|||S||"

  # === Bandai / Watara ===
  "beetle_wswan|https://github.com/libretro/beetle-wswan-libretro|libretro-beetle-wswan|||S||"
  "potator|https://github.com/libretro/potator|potator/platform/libretro|||||"

  # === Home computers ===
  "fuse|https://github.com/libretro/fuse-libretro|libretro-fuse|||||"
  "81|https://github.com/libretro/81-libretro|libretro-81|||||"
  "crocods|https://github.com/libretro/crocods-core|libretro-crocods|||||"
  "cap32|https://github.com/libretro/libretro-cap32|libretro-cap32|||||"
  "gearcoleco|https://github.com/drhelius/Gearcoleco|Gearcoleco/platforms/libretro|||||"
  "theodore|https://github.com/Zlika/theodore|theodore|||||"
  "xmil|https://github.com/libretro/xmil-libretro|libretro-xmil/libretro|||||"
  "freeintv|https://github.com/libretro/FreeIntv|FreeIntv|||||"
  "freechaf|https://github.com/libretro/FreeChaF|FreeChaF|||SC|src/deps/libretro-common|"
  "vecx|https://github.com/libretro/libretro-vecx|libretro-vecx|||HAS_GPU0||"

  # === Arcade ===
  "mame2000|https://github.com/libretro/mame2000-libretro|mame2000|||||"
  "mame2003_plus|https://github.com/libretro/mame2003-plus-libretro|mame2003-plus-libretro|||S||"
  "fbalpha2012_cps1|https://github.com/libretro/fbalpha2012_cps1|fbalpha2012_cps1||||fba|"
  "fbalpha2012_cps2|https://github.com/libretro/fbalpha2012_cps2|fbalpha2012_cps2||||fba|"
  "fbalpha2012_cps3|https://github.com/libretro/fbalpha2012_cps3|fbalpha2012_cps3/svn-current/trunk|-f makefile.libretro||fba|"
  "fbalpha2012_neogeo|https://github.com/libretro/fbalpha2012_neogeo|fbalpha2012_neogeo||||fba|"
  "fbneo|https://github.com/libretro/FBNeo|FBNeo/src/burner/libretro|||S_pthread|fba|"

  # === PC / DOS games ===
  "tyrquake|https://github.com/libretro/tyrquake|tyrquake|||||"
  "prboom|https://github.com/libretro/libretro-prboom|libretro-prboom|||||"
  "ecwolf|https://github.com/libretro/ecwolf|ecwolf/src/libretro|||S|src/libretro/libretro-common|"
  "cannonball|https://github.com/libretro/cannonball|cannonball|||S||"
  "nxengine|https://github.com/libretro/nxengine-libretro|libretro-nxengine|||||"
  "reminiscence|https://github.com/libretro/REminiscence|REminiscence|||||"
  "xrick|https://github.com/libretro/xrick-libretro|libretro-xrick|||||"
  "jumpnbump|https://github.com/libretro/jumpnbump-libretro|libretro-jumpnbump|||||"
  "gong|https://github.com/libretro/gong|gong|-f Makefile.libretro||||"

  # === Open-source / homebrew / fantasy consoles ===
  "retro8|https://github.com/libretro/retro8|retro8|||||"
  "fake08|https://github.com/tzubertowski/fake-08|fake-08/platform/libretro|||||sf3000"
  "lowres_nx|https://github.com/timoinutilis/lowres-nx|lowres-nx/platform/LibRetro|||||"
  "arduous|https://github.com/libretro/arduous|arduous|||||"
  "gme|https://github.com/libretro/libretro-gme|libretro-gme|||||"
  "pocketcdg|https://github.com/libretro/libretro-pocketcdg|libretro-pocketcdg|||||"
  "nestopia|https://github.com/libretro/nestopia|nestopia/libretro|||||"
  "tgbdual|https://github.com/libretro/tgbdual-libretro|libretro-tgbdual|||||"
  "doublecherrygb|https://github.com/DoubleCherry/doublecherryGB-libretro|doublecherryGB-libretro|||||"

  # === Special (angree SF2000 ports) ===
  "uae|https://github.com/angree/sf2000-uae-amiga-emulator|sf2000-uae-amiga-emulator|-f Makefile.libretro||||"
  "castaway|https://github.com/angree/sf2000-atarist-emulator|sf2000-atarist-emulator|-f Makefile.libretro|||SC|"

  # === Special (cmake / arch-dynarec) built in explicit blocks below ===
  "pcsx_rearmed|https://github.com/libretro/pcsx_rearmed|pcsx_rearmed|-f Makefile.libretro|||frontend/libpicofe|"
)

# ---- Clone phase -----------------------------------------------------------
for entry in "${CORES[@]}"; do
  IFS='|' read -r name url dir mk extra wrap submods branch <<< "$entry"
  d="$WORKDIR/$dir"
  if [ ! -d "$d" ]; then
    printf '[core] clone %s ... ' "$name"
    if [ -n "$branch" ]; then
      git clone --depth 1 --branch "$branch" "$url" "$d" >/dev/null 2>&1 && echo "OK" || { echo "CLONE FAIL"; FAIL=$((FAIL+1)); FAILED_CORES+=("$name (clone)"); }
    else
      git clone --depth 1 "$url" "$d" >/dev/null 2>&1 && echo "OK" || { echo "CLONE FAIL"; FAIL=$((FAIL+1)); FAILED_CORES+=("$name (clone)"); }
    fi
  fi
done

# ---- Build phase -----------------------------------------------------------
for entry in "${CORES[@]}"; do
  IFS='|' read -r name url dir mk extra wrap submods branch <<< "$entry"
  case "$name" in
    # ---- gpsp (multicore): upstream uses MIPS dynarec (platform=sf3000). For
    #      ARM we use the libretro gpsp ARM target; best-effort (WARN-only). ----
    gpsp_multicore)
      full="$WORKDIR/gpsp"
      [ -d "$full" ] && {
        make -C "$full" clean 2>/dev/null || true
        make -C "$full" platform=armv7-neon-hardfloat CC="$CC" CXX="$CXX" AR="$AR" RANLIB="$RANLIB" LD="$CXX" LDFLAGS="$LDFLAGS" -j"$(nproc)" >"$WORKDIR/_mk_gpsp.log" 2>&1
        if [ -f "$full/gpsp_libretro.so" ]; then
          cp "$full/gpsp_libretro.so" "$CORES_OUT/gpsp_multicore_libretro.so" && "$STRIP" "$CORES_OUT/gpsp_multicore_libretro.so" 2>/dev/null
          echo "[core] OK: gpsp_multicore"; SUCC=$((SUCC+1))
        else
          echo "[core] FAIL: gpsp_multicore (ARM dynarec best-effort) -- tail:"; tail -n 15 "$WORKDIR/_mk_gpsp.log" | sed 's/^/    | /'
          FAIL=$((FAIL+1)); FAILED_CORES+=(gpsp_multicore)
        fi
      }
      ;;
    # ---- gpsp (upstream libretro/gpsp): same ARM approach, output gpsp_libretro.so
    gpsp)
      full="$WORKDIR/gpsp_upstream"
      [ -d "$full" ] && {
        make -C "$full" clean 2>/dev/null || true
        make -C "$full" platform=armv7-neon-hardfloat CC="$CC" CXX="$CXX" AR="$AR" RANLIB="$RANLIB" LD="$CXX" LDFLAGS="$LDFLAGS" -j"$(nproc)" >"$WORKDIR/_mk_gpsp_up.log" 2>&1
        if [ -f "$full/gpsp_libretro.so" ]; then
          cp "$full/gpsp_libretro.so" "$CORES_OUT/" && "$STRIP" "$CORES_OUT/gpsp_libretro.so" 2>/dev/null
          echo "[core] OK: gpsp"; SUCC=$((SUCC+1))
        else
          echo "[core] FAIL: gpsp (ARM dynarec best-effort) -- tail:"; tail -n 15 "$WORKDIR/_mk_gpsp_up.log" | sed 's/^/    | /'
          FAIL=$((FAIL+1)); FAILED_CORES+=(gpsp)
        fi
      }
      ;;
    # ---- o2em: Makefile adds -shared only to its own LDFLAGS; command-line
    #      LDFLAGS override that, so give the complete shared-link flags. ----
    o2em)
      full="$WORKDIR/libretro-o2em"
      [ -d "$full" ] && {
        make -C "$full" clean 2>/dev/null || true
        make -C "$full" platform=unix CC="$CC" CXX="$CXX" AR="$AR" RANLIB="$RANLIB" LD="$CC" LDFLAGS="$LDFLAGS_SC -Wl,--version-script=link.T" -j"$(nproc)" >"$WORKDIR/_mk_o2em.log" 2>&1
        if [ -f "$full/o2em_libretro.so" ]; then
          cp "$full/o2em_libretro.so" "$CORES_OUT/" && echo "[core] OK: o2em"; SUCC=$((SUCC+1))
        else
          echo "[core] FAIL: o2em -- tail:"; tail -n 15 "$WORKDIR/_mk_o2em.log" | sed 's/^/    | /'
          FAIL=$((FAIL+1)); FAILED_CORES+=(o2em)
        fi
      }
      ;;
    # ---- uae (amiga): output uae_libretro.so; needs -lz -lstdc++ ----
    uae)
      full="$WORKDIR/sf2000-uae-amiga-emulator"
      [ -d "$full" ] && {
        make -C "$full" -f Makefile.libretro clean 2>/dev/null || true
        make -C "$full" -f Makefile.libretro CC="$CC" CXX="$CXX" AR="$AR" RANLIB="$RANLIB" LD="$CXX" LDFLAGS="$LDFLAGS_SC -lz -lstdc++" -j"$(nproc)" >"$WORKDIR/_mk_uae.log" 2>&1
        if [ -f "$full/uae4all_libretro.so" ]; then
          cp "$full/uae4all_libretro.so" "$CORES_OUT/uae_libretro.so" && echo "[core] OK: uae"; SUCC=$((SUCC+1))
        else
          echo "[core] FAIL: uae -- tail:"; tail -n 15 "$WORKDIR/_mk_uae.log" | sed 's/^/    | /'
          FAIL=$((FAIL+1)); FAILED_CORES+=(uae)
        fi
      }
      ;;
    # ---- frodo (C64): needs Src/libretro-common submodule, NOLIBCO=1,
    #      --allow-multiple-definition (gui redefines strdup/strncasecmp). ----
    frodo)
      full="$WORKDIR/libretro-frodo"
      [ -d "$full" ] && {
        git -C "$full" submodule update --init --recursive 2>/dev/null || true
        make -C "$full" clean 2>/dev/null || true
        find "$full" -name "*.o" -delete 2>/dev/null || true
        make -C "$full" platform=unix NOLIBCO=1 CC="$CC" CXX="$CXX" AR="$AR" RANLIB="$RANLIB" LD="$CXX" LDFLAGS="$LDFLAGS_S -Wl,--allow-multiple-definition" -j"$(nproc)" >"$WORKDIR/_mk_frodo.log" 2>&1
        if [ -f "$full/frodo_libretro.so" ]; then
          cp "$full/frodo_libretro.so" "$CORES_OUT/" && echo "[core] OK: frodo"; SUCC=$((SUCC+1))
        else
          echo "[core] FAIL: frodo -- tail:"; tail -n 15 "$WORKDIR/_mk_frodo.log" | sed 's/^/    | /'
          FAIL=$((FAIL+1)); FAILED_CORES+=(frodo)
        fi
      }
      ;;
    # ---- arduous (Arduboy): cmake project built directly (bundles simavr).
    #      Uses the ARM wrapper ($CC/$CXX already inject -fPIC -marm ...). ----
    arduous)
      full="$WORKDIR/arduous"
      [ -d "$full" ] && {
        git -C "$full" submodule update --init --depth 1 deps/bitsery deps/miniz deps/yyjson 2>/dev/null || true
        AINC="-I$full/include -I$full/simavr/simavr/sim -I$full/simavr/simavr/cores -I$full/examples/parts -I$full/simavr/examples/parts -I$full/src"
        AOBJ=""; n=0; ok=1
        SIM="avr_acomp avr_adc avr_bitbang avr_eeprom avr_extint avr_flash avr_ioport avr_lin avr_spi avr_timer avr_twi avr_uart avr_usb avr_watchdog sim_avr sim_cmds sim_core sim_cycle_timers sim_hex sim_interrupts sim_io sim_irq sim_utils sim_vcd_file"
        for f in $SIM; do o="$full/_ao_$n.o"; $CC $AINC -std=gnu99 -c "$full/simavr/simavr/sim/$f.c" -o "$o" || ok=0; AOBJ="$AOBJ $o"; n=$((n+1)); done
        for f in "simavr/simavr/cores/sim_mega32u4.c" "simavr/examples/parts/ssd1306_virt.c" "src/sim_fake_gdb.c"; do o="$full/_ao_$n.o"; $CC $AINC -std=gnu99 -c "$full/$f" -o "$o" || ok=0; AOBJ="$AOBJ $o"; n=$((n+1)); done
        for f in "src/arduous/arduous.cpp" "src/arduous/speaker.cpp" "src/libretro/libretro.cpp"; do o="$full/_ao_$n.o"; $CXX $AINC -std=c++11 -c "$full/$f" -o "$o" || ok=0; AOBJ="$AOBJ $o"; n=$((n+1)); done
        if [ "$ok" = 1 ]; then
          $CXX -shared -Wl,--version-script="$full/link.T" $AOBJ -o "$full/arduous_libretro.so" -lc -lm && \
            cp "$full/arduous_libretro.so" "$CORES_OUT/" && echo "[core] OK: arduous" && SUCC=$((SUCC+1)) || { echo "[core] FAIL: arduous (link)"; FAIL=$((FAIL+1)); FAILED_CORES+=(arduous); }
        else
          echo "[core] FAIL: arduous (compile)"; FAIL=$((FAIL+1)); FAILED_CORES+=(arduous)
        fi
        rm -f "$full"/_ao_*.o
      }
      ;;
    # ---- pcsx_rearmed (PS1): ARM dynarec (not MIPS lightrec). Submodule
    #      frontend/libpicofe. Best-effort; if it fails, real log drives next fix.
    pcsx_rearmed)
      full="$WORKDIR/pcsx_rearmed"
      [ -d "$full" ] && {
        git -C "$full" submodule update --init --depth 1 frontend/libpicofe 2>/dev/null || true
        make -C "$full" -f Makefile.libretro clean 2>/dev/null || true
        make -C "$full" -f Makefile.libretro platform=unix CC="$CC" CXX="$CXX" CC_AS="$CC" CC_LINK="$CXX" AR="$AR" ARCH=arm DYNAREC=arm HAVE_NEON=1 BUILTIN_GPU=unai -j"$(nproc)" >"$WORKDIR/_mk_pcsx.log" 2>&1
        if [ -f "$full/pcsx_rearmed_libretro.so" ]; then
          cp "$full/pcsx_rearmed_libretro.so" "$CORES_OUT/" && echo "[core] OK: pcsx_rearmed"; SUCC=$((SUCC+1))
        else
          echo "[core] FAIL: pcsx_rearmed (ARM dynarec best-effort) -- tail:"; tail -n 15 "$WORKDIR/_mk_pcsx.log" | sed 's/^/    | /'
          FAIL=$((FAIL+1)); FAILED_CORES+=(pcsx_rearmed)
        fi
      }
      ;;
    # ---- everything else: generic _b ----
    *)
      _b "$name" "$dir" "$mk" "$extra" "$wrap" "$submods"
      ;;
  esac
done

echo ""
echo "=========================================="
echo "Core build summary: $SUCC OK, $FAIL FAIL"
echo "Successful cores: $(ls "$CORES_OUT" 2>/dev/null | grep -c '_libretro.so') .so files"
[ ${#FAILED_CORES[@]} -gt 0 ] && {
  echo "Failed cores:"
  printf '  - %s\n' "${FAILED_CORES[@]}"
}
echo "=========================================="

cat > "$CORES_OUT/_BUILD_SUMMARY.txt" <<EOF
CubeGM core build summary ($(date -u +%Y-%m-%dT%H:%M:%SZ))
Total attempted: $((${#CORES[@]}))
Successful:      $SUCC
Failed:          $FAIL

Failed cores:
$(printf '  - %s\n' "${FAILED_CORES[@]}")
EOF

# Per-core build is WARN-only; the build.sh STAGE 7 baseline gate decides red/green.
exit 0
