#!/usr/bin/env bash
# One-step build for Linux and macOS (Windows: use the MSYS2 MINGW64 shell, same command).
#   ./build.sh            build into dist/
#   ./build.sh --run      build, then start the game
set -euo pipefail
cd "$(dirname "$(readlink -f "$0" 2>/dev/null || echo "$0")")"
ROOT=$PWD

case "$(uname -s)" in
  Linux*)  OS=linux; EXT=so ;;
  Darwin*) OS=mac;   EXT=dylib ;;
  MINGW*|MSYS*) OS=windows; EXT=dll ;;
esac

# 1. dependencies
if [ "$OS" = linux ] && command -v dnf >/dev/null; then
  pkgs=""
  for p in gcc gcc-c++ make cmake git python3 perl SDL3-devel; do rpm -q "$p" >/dev/null 2>&1 || pkgs="$pkgs $p"; done
  [ -n "$pkgs" ] && sudo dnf install -y $pkgs
elif [ "$OS" = linux ] && command -v apt-get >/dev/null; then
  command -v cmake >/dev/null || sudo apt-get install -y build-essential cmake git python3
elif [ "$OS" = mac ]; then
  command -v cmake >/dev/null || { echo "Install CMake first: brew install cmake"; exit 1; }
fi

# 2. emulator core (prebuilt one in core/ is used if present)
[ -f "core/mame_libretro.$EXT" ] || scripts/build-core.sh core

# 3. the app (uses system SDL3 on Linux when available, otherwise builds it from source)
CMAKE_EXTRA=()
if [ "$OS" = mac ]; then
  # Same CPU as the core; macOS 11+ = every Apple Silicon Mac. SDL is built in statically
  # so the app doesn't depend on Homebrew and runs on other Macs too.
  if [ "$(sysctl -n hw.optional.arm64 2>/dev/null || echo 0)" = 1 ] && [ "${MAC_ARCH:-arm64}" = arm64 ]; then MARCH=arm64; else MARCH=x86_64; fi
  CMAKE_EXTRA=(-DCMAKE_OSX_ARCHITECTURES=$MARCH -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DSMASHTV_VENDOR_SDL=ON)
fi
# YOUR ROMs, if placed in the project folder (smashtv.zip or a roms/ folder), are compiled
# into the program so it needs no ROM file. Personal use only: don't share such builds.
ROMSRC=""
for c in smashtv.zip smash-tv.zip roms; do [ -e "$c" ] && { ROMSRC="$ROOT/$c"; break; }; done
if [ -n "$ROMSRC" ]; then echo ">> Will build your ROMs into the app ($ROMSRC)"; fi
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSMASHTV_EMBED_ROMS="$ROMSRC" ${CMAKE_EXTRA[@]+"${CMAKE_EXTRA[@]}"}
cmake --build build --config Release -j

# 4. assemble dist/
rm -rf dist && mkdir -p dist
if [ "$OS" = mac ]; then
  APPDIR="dist/Smash TV.app"
  cp -R "build/Smash TV.app" "$APPDIR"
  mkdir -p "$APPDIR/Contents/Frameworks"
  cp "core/mame_libretro.dylib" "$APPDIR/Contents/Frameworks/"
  codesign --force --deep -s - "$APPDIR" 2>/dev/null || true
  RUN=("$APPDIR/Contents/MacOS/Smash TV")
else
  cp build/smashtv* dist/ 2>/dev/null || true
  cp "core/mame_libretro.$EXT" dist/
  cp packaging/README-player.txt dist/README.txt
  if [ "$OS" = linux ]; then
    cp packaging/install-linux.sh dist/install.sh
    chmod +x dist/install.sh dist/smashtv
  fi
  RUN=(dist/smashtv)
fi
# 5. on macOS also make a zip of the app, handy for copying to another Mac
if [ "$OS" = mac ]; then
  (cd dist && ditto -c -k --keepParent "Smash TV.app" "Smash TV.zip")
fi
echo ">> Built: $ROOT/dist"
if [ "${1:-}" = --run ]; then exec "${RUN[@]}"; fi
