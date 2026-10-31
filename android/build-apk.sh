#!/usr/bin/env bash
# Builds SmashTV.apk (arm64 phones/tablets). Needs: Android SDK + NDK (Android Studio installs both),
# JDK 17, git, make, python3, perl. Set ANDROID_HOME (SDK) and ANDROID_NDK_HOME (NDK) first, e.g.
#   export ANDROID_HOME=~/Android/Sdk
#   export ANDROID_NDK_HOME=$ANDROID_HOME/ndk/27.2.12479018
# The app contains no ROMs: you pick your smashtv.zip on the phone the first time it starts.
set -euo pipefail
cd "$(dirname "$0")"
HERE=$PWD
ROOT=$(cd .. && pwd)
: "${ANDROID_HOME:?Set ANDROID_HOME to your Android SDK folder}"
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to your Android NDK folder}"
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
SDL_TAG=release-3.2.24
MAME_COMMIT=5f5a07748ce5c239c4118463ce8e6473743d4859   # libretro/mame, MAME 0.289 (same as desktop)

# 1. SDL3 source for the native build
if [ ! -f app/jni/SDL/CMakeLists.txt ]; then
  echo ">> Fetching SDL3 $SDL_TAG"
  git clone -q --depth 1 --branch "$SDL_TAG" https://github.com/libsdl-org/SDL.git app/jni/SDL
fi
# SDL's own Java classes must match its native code
mkdir -p app/src/main/java/org/libsdl/app
cp app/jni/SDL/android-project/app/src/main/java/org/libsdl/app/*.java app/src/main/java/org/libsdl/app/

# 2. the emulator core for Android (arm64)
CORE=app/libs/arm64-v8a/libmame_libretro.so
if [ ! -f "$CORE" ]; then
  SRC=$ROOT/third_party/mame-android
  if [ ! -f "$SRC/Makefile.libretro" ]; then
    echo ">> Fetching MAME source"
    mkdir -p "$SRC"
    git -C "$SRC" init -q
    git -C "$SRC" fetch -q --depth 1 https://github.com/libretro/mame.git "$MAME_COMMIT"
    git -C "$SRC" checkout -q FETCH_HEAD
  fi
  perl -pi -e 's/ROM_LOAD \( "(a_5346_3044_\d\.u\d+)"/ROM_LOAD_OPTIONAL ( "$1"/' "$SRC/src/mame/williams/midyunit.cpp"
  echo ">> Building the emulator core for Android (this takes a while)"
  make -C "$SRC" -f Makefile.libretro platform=android-arm64 SUBTARGET=smashtv SOURCES=src/mame/williams/midyunit.cpp \
       NOWERROR=1 PYTHON_EXECUTABLE="$(command -v python3)" -j"$JOBS"
  BUILT=$(ls -t "$SRC"/*.so | head -1)
  mkdir -p "$(dirname "$CORE")"
  cp "$BUILT" "$CORE"
  "$ANDROID_NDK_HOME"/toolchains/llvm/prebuilt/*/bin/llvm-strip "$CORE" || true
fi
# the core may use the shared C++ runtime: ship it next to the core
if "$ANDROID_NDK_HOME"/toolchains/llvm/prebuilt/*/bin/llvm-readelf -d "$CORE" | grep -q libc++_shared; then
  cp "$ANDROID_NDK_HOME"/toolchains/llvm/prebuilt/*/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so app/libs/arm64-v8a/
fi

# 3. the app
chmod +x gradlew
./gradlew --no-daemon assembleDebug
mkdir -p "$ROOT/dist"
cp app/build/outputs/apk/debug/app-debug.apk "$ROOT/dist/SmashTV.apk"
echo ">> Built: $ROOT/dist/SmashTV.apk  (copy it to your phone and open it to install)"
