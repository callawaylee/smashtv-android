#!/usr/bin/env bash
# Builds the cut-down MAME libretro core (Williams Y-unit driver only) that
# Smash T.V. Native loads. Works on Linux, macOS, and Windows (MSYS2 MINGW64 shell).
#
#   scripts/build-core.sh [output-dir]      (default: ./core)
#
# Takes 20-60 minutes depending on CPU; only needed once.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
OUT=${1:-$ROOT/core}
MAME_COMMIT=5f5a07748ce5c239c4118463ce8e6473743d4859   # libretro/mame, MAME 0.289
SRC=$ROOT/third_party/mame

case "$(uname -s)" in
  Linux*)  EXT=so ;;
  Darwin*) EXT=dylib ;;
  MINGW*|MSYS*|CYGWIN*) EXT=dll ;;
  *) echo "Unsupported OS $(uname -s)"; exit 1 ;;
esac
JOBS=$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 2)

if [ ! -f "$SRC/Makefile.libretro" ]; then
  echo ">> Fetching MAME source ($MAME_COMMIT)"
  mkdir -p "$SRC"
  git -C "$SRC" init -q
  git -C "$SRC" fetch -q --depth 1 https://github.com/libretro/mame.git "$MAME_COMMIT"
  git -C "$SRC" checkout -q FETCH_HEAD
fi

# Newer MAME lists 4 PLD logic-chip dumps for Smash T.V. that the emulation never
# reads (they only document the board). Mark them optional so the common
# 14-file ROM set works.
perl -pi -e 's/ROM_LOAD \( "(a_5346_3044_\d\.u\d+)"/ROM_LOAD_OPTIONAL ( "$1"/' "$SRC/src/mame/williams/midyunit.cpp"

EXTRA=()
if [ "$EXT" = dylib ]; then
  # MAME's makefile hard-codes the full Xcode.app SDK path and always targets Intel.
  # Use whatever SDK is installed (Command Line Tools is enough) and build for this Mac's chip.
  SDK=$(xcrun --sdk macosx --show-sdk-path)
  if [ "$(sysctl -n hw.optional.arm64 2>/dev/null || echo 0)" = 1 ] && [ "${MAC_ARCH:-arm64}" = arm64 ]; then
    EXTRA=(CROSS_COMPILE=1 LIBRETRO_APPLE_PLATFORM=arm64-apple-macos11.0 LIBRETRO_APPLE_ISYSROOT="$SDK")
    echo ">> macOS Apple Silicon build (SDK: $SDK)"
  else
    EXTRA=(LIBRETRO_APPLE_ISYSROOT="$SDK")
    echo ">> macOS Intel build (SDK: $SDK)"
  fi
  # an earlier attempt may have generated build files for a different CPU/SDK setup
  WANT="${EXTRA[*]}"
  if [ "$(cat "$SRC/.smashtv-build-config" 2>/dev/null)" != "$WANT" ]; then
    rm -rf "$SRC/build" "$SRC/smashtv_libretro.dylib"
    echo "$WANT" > "$SRC/.smashtv-build-config"
  fi
fi

echo ">> Building core with $JOBS jobs (this takes a while)"
make -C "$SRC" -f Makefile.libretro SUBTARGET=smashtv SOURCES=src/mame/williams/midyunit.cpp \
     NOWERROR=1 PYTHON_EXECUTABLE="$(command -v python3 || command -v python)" -j"$JOBS" ${EXTRA[@]+"${EXTRA[@]}"}

mkdir -p "$OUT"
cp "$SRC/smashtv_libretro.$EXT" "$OUT/mame_libretro.$EXT"
if [ "$EXT" = dylib ]; then strip -x "$OUT/mame_libretro.$EXT"; else strip "$OUT/mame_libretro.$EXT"; fi
echo ">> Core ready: $OUT/mame_libretro.$EXT"
