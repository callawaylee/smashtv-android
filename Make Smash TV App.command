#!/bin/bash
# Double-click this file (once) on your Mac. It builds "Smash TV.app" with your ROMs
# inside, puts it in Applications, and opens it. Put smashtv.zip next to this file or in Downloads.
cd "$(dirname "$0")"
ROOT=$PWD
clear
echo "=== Making Smash TV.app ==="
echo

fail() { echo; echo "!! $1"; echo; echo "Press Return to close."; read -r; exit 1; }

# 1. Apple's command line tools (compiler)
if ! xcode-select -p >/dev/null 2>&1 || ! xcrun --sdk macosx --show-sdk-path >/dev/null 2>&1; then
  xcode-select --install >/dev/null 2>&1
  fail "Apple's developer tools are needed. Click Install in the window that just opened, wait for it to finish, then double-click this file again."
fi

# 2. your ROMs
if [ ! -e smashtv.zip ] && [ ! -e roms ]; then
  for c in "$HOME/Downloads/smashtv.zip" "$HOME/Desktop/smashtv.zip" "$ROOT/../smashtv.zip"; do
    [ -f "$c" ] && { cp "$c" smashtv.zip; echo "Using ROMs from $c"; break; }
  done
fi
[ -e smashtv.zip ] || [ -e roms ] || fail "Couldn't find smashtv.zip. Put it in this folder or in Downloads, then double-click this file again."

# 3. CMake (private copy, no Homebrew needed)
if ! command -v cmake >/dev/null 2>&1; then
  if [ ! -x .tools/bin/cmake ]; then
    echo "Getting CMake..."
    python3 -m venv .tools && .tools/bin/pip install -q cmake || fail "Couldn't install CMake (needs internet)."
  fi
  export PATH="$ROOT/.tools/bin:$PATH"
fi

# 4. build (first time ~30-60 min for the emulator core)
echo "Building. The first time takes 30-60 minutes; you can leave this window open and walk away."
echo
rm -rf build dist
./build.sh > build.log 2>&1 &
pid=$!
while kill -0 $pid 2>/dev/null; do
  line=$(grep -E "^(>>|Compiling|Linking|embed_roms)" build.log 2>/dev/null | tail -1 | cut -c1-70)
  printf "\r  %-72s" "$line"
  sleep 2
done
wait $pid || { echo; tail -25 build.log; fail "Build failed. Send the lines above (or the file build.log) to get it fixed."; }
echo

# 5. install and open
rm -rf "/Applications/Smash TV.app"
cp -R "dist/Smash TV.app" /Applications/ || fail "Couldn't copy to Applications."
xattr -dr com.apple.quarantine "/Applications/Smash TV.app" 2>/dev/null
rm -rf build third_party/mame/build
echo
echo "=== Done! Smash TV is in your Applications folder. Starting it now... ==="
open "/Applications/Smash TV.app"
sleep 3
