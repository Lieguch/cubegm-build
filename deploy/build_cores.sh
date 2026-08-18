#!/usr/env bash
# =============================================================================
#  build_cores.sh -- per-core build orchestrator for all libretro cores
# =============================================================================
#  Driven by deploy/build.sh (STAGE 7). Cross-compiles every core listed in
#  treefrog-ui/cores.md (clone_cores.sh) for RK3036G (armhf, glibc<=2.17).
#
#  Per-core recipe table fields (pipe-separated):
#    name|repo|build_path|makefile|output_so|extra_flags|needs_submodules
#
#  - name:             short identifier (used for clone dir + config.xml key)
#  - repo:             git URL (tzubertowski forks used where they exist)
#  - build_path:       subdir to cd into before make ("." for root)
#  - makefile:         "Makefile.libretro" or "Makefile" or "" (default)
#  - output_so:        expected .so filename after successful build
#  - extra_flags:      extra make flags (e.g. HAVE_LIGHTREC=1)
#  - needs_submodules: "1" if --recursive clone needed
#
#  Failures are WARN-only (not fatal) -- the build report records success/fail.
#  Expect ~35-50/66 to build cleanly on armhf; the rest have ARM-incompatible
#  assembly, missing deps, or cmake/build systems we don't fully support here.
#  The 5 baseline cores (mgba snes9x fceumm picodrive nestopia) are hard-gated
#  by build.sh STAGE 7 even though this script is WARN-only per-core.
# =============================================================================
set -uo pipefail

: "${CC:=arm-linux-gnueabihf-gcc}"
: "${CXX:=arm-linux-gnueabihf-g++}"
: "${WORKDIR:?WORKDIR must be set}"
: "${CORES_OUT:=$WORKDIR/cores}"
: "${CFLAGS:?CFLAGS must be set}"
: "${CXXFLAGS:?CXXFLAGS must be set}"
: "${LDFLAGS:?LDFLAGS must be set}"

mkdir -p "$CORES_OUT"
SUCC=0; FAIL=0
FAILED_CORES=()

# ---- Per-core recipe table (treefrog-ui/cores.md) --------------------------
# fmt: name|repo|build_path|makefile|output_so|extra_flags|submodules
CORES=(
  # === Nintendo (8/16-bit) ===
  "fceumm|https://github.com/tzubertowski/libretro-fceumm|.|Makefile.libretro|fceumm_libretro.so||0"
  "quicknes|https://github.com/libretro/QuickNES_Core|.|Makefile|quicknes_libretro.so||0"
  "nestopia|https://github.com/libretro/nestopia|libretro|Makefile|nestopia_libretro.so||0"
  "snes9x2005|https://github.com/tzubertowski/snes9x2005|.|Makefile.libretro|snes9x2005_plus_libretro.so|USE_BLARGG_APU=1|0"
  "snes9x2002|https://github.com/tzubertowski/snes9x2002|.|Makefile.libretro|snes9x2002_libretro.so||0"
  "snes9x2010|https://github.com/libretro/snes9x2010|.|Makefile.libretro|snes9x2010_libretro.so||0"
  "snes9x|https://github.com/libretro/snes9x|libretro|Makefile|snes9x_libretro.so||0"
  "gambatte|https://github.com/tzubertowski/libretro-gambatte|.|Makefile.libretro|gambatte_libretro.so||0"
  "gearboy|https://github.com/drhelius/Gearboy|platform/libretro|Makefile|gearboy_libretro.so||0"
  "tgbdual|https://github.com/libretro/libretro-tgbdual|.|Makefile|tgbdual_libretro.so||0"
  "doublecherrygb|https://github.com/DoubleCherry/doublecherryGB-libretro|.|Makefile|doublecherrygb_libretro.so||0"
  "gpsp|https://github.com/tzubertowski/gpsp_multicore|.|Makefile|gpsp_libretro.so||0"
  "vba_next|https://github.com/libretro/vba-next|.|Makefile.libretro|vba_next_libretro.so||0"
  "mgba|https://github.com/libretro/mgba|.|Makefile.libretro|mgba_libretro.so||0"
  "pokemini|https://github.com/libretro/PokeMini|.|Makefile.libretro|pokemini_libretro.so||0"
  "gw|https://github.com/libretro/gw-libretro|.|Makefile|gw_libretro.so||0"

  # === Sega ===
  "picodrive|https://github.com/libretro/picodrive|.|Makefile.libretro|picodrive_libretro.so|use_libchdr=0|1"
  "genesis_plus_gx|https://github.com/libretro/Genesis-Plus-GX|.|Makefile.libretro|genesis_plus_gx_libretro.so||0"
  "gearsystem|https://github.com/drhelius/Gearsystem|.|Makefile|gearsystem_libretro.so||0"

  # === Atari ===
  "stella2014|https://github.com/libretro/stella2014-libretro|.|Makefile|stella2014_libretro.so||0"
  "a5200|https://github.com/libretro/a5200|.|Makefile|a5200_libretro.so||0"
  "prosystem|https://github.com/libretro/prosystem-libretro|.|Makefile|prosystem_libretro.so||0"
  "atari800|https://github.com/libretro/libretro-atari800|.|Makefile|atari800_libretro.so||0"
  "handy|https://github.com/libretro/libretro-handy|.|Makefile|handy_libretro.so||0"
  "beetle_lynx|https://github.com/libretro/beetle-lynx-libretro|.|Makefile.libretro|mednafen_lynx_libretro.so||0"

  # === NEC / PC Engine ===
  "beetle_pce_fast|https://github.com/libretro/beetle-pce-fast-libretro|.|Makefile.libretro|mednafen_pce_fast_libretro.so||0"
  "beetle_supergrafx|https://github.com/libretro/beetle-supergrafx-libretro|.|Makefile.libretro|mednafen_supergrafx_libretro.so||0"
  "beetle_pcfx|https://github.com/libretro/beetle-pcfx-libretro|.|Makefile.libretro|mednafen_pcfx_libretro.so||0"
  "quasi88|https://github.com/libretro/quasi88-libretro|.|Makefile|quasi88_libretro.so||0"

  # === SNK ===
  "race|https://github.com/libretro/RACE|.|Makefile|race_libretro.so||0"
  "geolith|https://github.com/libretro/geolith-libretro|.|Makefile|geolith_libretro.so||0"

  # === Bandai ===
  "beetle_wswan|https://github.com/libretro/beetle-wswan-libretro|.|Makefile.libretro|mednafen_wswan_libretro.so||0"
  "potator|https://github.com/libretro/potator|.|Makefile|potator_libretro.so||0"

  # === Home computers ===
  "fuse|https://github.com/libretro/fuse-libretro|.|Makefile|fuse_libretro.so||0"
  "libretro_81|https://github.com/libretro/81-libretro|.|Makefile|81_libretro.so||0"
  "crocods|https://github.com/libretro/crocods-core|.|Makefile|crocods_libretro.so||0"
  "cap32|https://github.com/libretro/libretro-cap32|.|Makefile|cap32_libretro.so||0"
  "gearcoleco|https://github.com/drhelius/Gearcoleco|.|Makefile|gearcoleco_libretro.so||0"
  "theodore|https://github.com/Zlika/theodore|.|Makefile|theodore_libretro.so||0"
  "xmil|https://github.com/libretro/xmil-libretro|.|Makefile|x68k_libretro.so||0"
  "vice|https://github.com/libretro/vice-libretro|.|Makefile|vice_x64_libretro.so||0"
  "frodo|https://github.com/tzubertowski/libretro-frodo|.|Makefile|frodo_libretro.so||0"

  # === Arcade ===
  "mame2000|https://github.com/libretro/mame2000-libretro|.|Makefile|mame2000_libretro.so||0"
  "fbalpha2012_cps1|https://github.com/libretro/fbalpha2012_cps1|.|Makefile.libretro|fbalpha2012_cps1_libretro.so||0"
  "fbalpha2012_cps2|https://github.com/libretro/fbalpha2012_cps2|.|Makefile.libretro|fbalpha2012_cps2_libretro.so||0"
  "fbalpha2012_cps3|https://github.com/libretro/fbalpha2012_cps3|.|Makefile.libretro|fbalpha2012_cps3_libretro.so||0"
  "fbalpha2012_neogeo|https://github.com/libretro/fbalpha2012_neogeo|.|Makefile.libretro|fbalpha2012_neogeo_libretro.so||0"

  # === PC / DOS games ===
  "tyrquake|https://github.com/libretro/tyrquake|.|Makefile|tyrquake_libretro.so||0"
  "prboom|https://github.com/libretro/libretro-prboom|.|Makefile|prboom_libretro.so||0"
  "ecwolf|https://github.com/libretro/ecwolf|.|Makefile|ecwolf_libretro.so||0"
  "cannonball|https://github.com/libretro/cannonball|.|Makefile|cannonball_libretro.so||0"

  # === Open-source / homebrew ===
  "nxengine|https://github.com/libretro/nxengine-libretro|.|Makefile|nxengine_libretro.so||0"
  "reminiscence|https://github.com/libretro/REminiscence|.|Makefile|reminiscence_libretro.so||0"
  "xrick|https://github.com/libretro/xrick-libretro|.|Makefile|xrick_libretro.so||0"
  "jumpnbump|https://github.com/libretro/jumpnbump-libretro|.|Makefile|jumpnbump_libretro.so||0"
  "gong|https://github.com/libretro/gong|.|Makefile|gong_libretro.so||0"
  "fake08|https://github.com/tzubertowski/fake-08|platform/libretro|Makefile|fake08_libretro.so||0"
  "pcsx_rearmed|https://github.com/libretro/pcsx_rearmed|.|Makefile.libretro|pcsx_rearmed_libretro.so|HAVE_LIGHTREC=1|0"
  "retro8|https://github.com/libretro/retro8|.|Makefile|retro8_libretro.so||0"
  "lowres_nx|https://github.com/timoinutilis/lowres-nx|.|Makefile|lowresnx_libretro.so||0"
  "ardens|https://github.com/tiberiusbrown/Ardens|.|Makefile|ardens_libretro.so||0"
  "arduous|https://github.com/libretro/arduous|.|Makefile|arduous_libretro.so||0"

  # === Misc ===
  "freeintv|https://github.com/libretro/FreeIntv|.|Makefile|freeintv_libretro.so||0"
  "freechaf|https://github.com/libretro/FreeChaF|.|Makefile|freechaf_libretro.so||0"
  "vecx|https://github.com/libretro/libretro-vecx|.|Makefile|vecx_libretro.so||0"
  "pocketcdg|https://github.com/libretro/libretro-pocketcdg|.|Makefile|pocketcdg_libretro.so||0"
  "gme|https://github.com/libretro/libretro-gme|.|Makefile|gme_libretro.so||0"
)

# ---- Build loop -----------------------------------------------------------
for entry in "${CORES[@]}"; do
  IFS='|' read -r name repo build_path makefile output flags submods <<< "$entry"
  d="$WORKDIR/libretro-$name"
  clone_args="--depth 1"
  [ "$submods" = "1" ] && clone_args="$clone_args --recursive"

  if [ ! -d "$d" ]; then
    printf '\033[1;32m[core]\033[0m clone %s ... ' "$name"
    if git clone $clone_args "$repo" "$d" >/dev/null 2>&1; then
      echo "OK"
    else
      echo "CLONE FAIL"
      FAIL=$((FAIL+1)); FAILED_CORES+=("$name (clone)"); continue
    fi
  fi

  builddir="$d"
  [ "$build_path" != "." ] && builddir="$d/$build_path"
  [ -d "$builddir" ] || builddir="$d"

  pushd "$builddir" >/dev/null 2>&1 || { FAIL=$((FAIL+1)); FAILED_CORES+=("$name (cd)"); continue; }
  make clean >/dev/null 2>&1 || true

  so=""
  case "$name" in
    mgba)
      # mgba needs cmake on this toolchain (plain Makefile.libretro is fragile).
      rm -rf build-cubegm
      cmake -B build-cubegm -DCMAKE_BUILD_TYPE=Release \
            -DLIBMGBA_ONLY=ON -DBUILD_LIBRETRO=ON \
            -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm \
            -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
            -DCMAKE_C_FLAGS="$CFLAGS" -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
            -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS" \
            -DCMAKE_SHARED_LINKER_FLAGS="$LDFLAGS" . >/dev/null 2>&1 \
        && cmake --build build-cubegm --target mgba_libretro -- -j"$(nproc)" >/dev/null 2>&1
      so=$(find build-cubegm -name "mgba_libretro.so" 2>/dev/null | head -1)
      ;;
    *)
      make_args=(CC="$CC" CXX="$CXX")
      [ -n "$makefile" ] && make_args+=(-f "$makefile")
      # NOTE: platform MUST be "armv-neon-hardfloat" -- that is libretro's real
      # target name. "armv7-neon-hardfloat" matches NO platform block, so the
      # core's Makefile falls back to a broken/host config and the build dies.
      # We do NOT override CFLAGS/CXXFLAGS/LDFLAGS here: each core's Makefile
      # platform block sets its own correct flags, and the PATH compiler wrapper
      # (built by build.sh) injects -fPIC -marm -march/-mfpu/-mfloat-abi for us.
      make_args+=(platform=armv-neon-hardfloat
                  $flags -j"$(nproc)")
      mklog="$WORKDIR/_mk_${name}.log"
      make "${make_args[@]}" >"$mklog" 2>&1
      rc=$?
      so=$(find . -name "$output" 2>/dev/null | head -1)
      ;;
  esac

  if [ -n "$so" ] && [ -f "$so" ]; then
    cp "$so" "$CORES_OUT/" && echo "[core] OK: $name -> $(basename "$so")"; SUCC=$((SUCC+1))
  elif [ -n "$so" ]; then
    echo "[core] FAIL: $name (built but $output missing)"; FAIL=$((FAIL+1)); FAILED_CORES+=("$name (no .so)")
  else
    echo "[core] FAIL: $name (build error rc=${rc:-?}) -- tail of $mklog:"
    tail -n 25 "$mklog" 2>/dev/null | sed 's/^/    | /'
    FAIL=$((FAIL+1)); FAILED_CORES+=("$name (build)")
  fi
  popd >/dev/null 2>&1
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

# Write summary for the CI build report
cat > "$CORES_OUT/_BUILD_SUMMARY.txt" <<EOF
CubeGM core build summary ($(date -u +%Y-%m-%dT%H:%M:%SZ))
Total attempted: $((${#CORES[@]}))
Successful:      $SUCC
Failed:          $FAIL

Failed cores:
$(printf '  - %s\n' "${FAILED_CORES[@]}")
EOF
