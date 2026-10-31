# Smash T.V. Native

A standalone, double-click game app for the 1990 Williams arcade game **Smash T.V.**, for Linux, macOS and Windows.

It's a small C program (SDL3) that runs the real arcade game through a cut-down MAME core containing only the Smash T.V. hardware driver. It opens straight into the game in its own window, with no emulator menus.

- Twin-stick play out of the box: left stick moves, right stick fires (any SDL-supported pad: Xbox, PlayStation, Switch Pro, 8BitDo…)
- Keyboard: WASD move, arrows or IJKL fire
- 2 players with two gamepads
- Unlimited continues (Start drops a coin for you), toggle in the menu
- Save / load state, fullscreen, sharp or smooth picture, scanlines
- Skips the arcade's ~20 s power-on self-test (toggle in the menu)
- High scores and arcade settings are kept between sessions

**ROMs aren't included.** On first launch, drag your `smashtv.zip` (MAME set, rev 8.00) or a folder of the 14 ROM files onto the window. Files are matched by checksum, so odd file names are fine. The 4 extra PLD files that newer MAME versions list aren't needed.

## Build

### Linux (Fedora / Nobara) and macOS

```bash
./build.sh --run
```

On Fedora this installs `SDL3-devel`, `cmake` and `gcc` with dnf if they're missing, builds the app in a few seconds, and puts everything in `dist/`. Then run `dist/install.sh` once to add **Smash T.V.** to your app menu.

A prebuilt Linux core (`core/mame_libretro.so`, needs glibc ≥ 2.38, i.e. Fedora 39+) is included, so you skip the long core build. On macOS, or if you delete `core/`, `build.sh` first builds the core from source (`scripts/build-core.sh`, 20–60 min, once). On macOS install Xcode command line tools and `brew install cmake` first.

### Windows

1. Install [MSYS2](https://www.msys2.org/), open the **MSYS2 MINGW64** shell and run:
   `pacman -S git make perl zip mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-python mingw-w64-x86_64-ninja`
2. `./build.sh` (builds the core, then the app). If it complains about missing DLLs when started outside MSYS2, copy `libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll` from `C:\msys64\mingw64\bin` next to `smashtv.exe`.

### All three at once (GitHub Actions)

Push this folder to a GitHub repo. Then go to **Actions → Build Smash T.V. Native → Run workflow**, or push a tag like `v1.0.0`. It produces:

- `SmashTV-windows-x64.zip`: unzip and double-click `smashtv.exe`
- `SmashTV-macos-arm64.zip`: Apple Silicon `.app` (first open: right-click → Open)
- `SmashTV-linux-x86_64.tar.gz`

With a tag, they're attached to a GitHub Release. The first run takes about an hour, mostly the core build; later runs reuse the cached core.

### Android (phones and tablets, arm64)

The game is centred at its original shape; the empty sides hold the touch controls: a movement stick
on the left, an 8-direction fire stick on the right, and pause / coin / start buttons in the top corners.
(On 4:3 tablets the sticks sit over the bottom corners.) A Bluetooth gamepad works too.

- **GitHub:** the workflow's `android` job builds `SmashTV.apk` (Actions tab, artifact `SmashTV-android`).
- **On your PC:** install Android Studio (it brings the SDK; add the NDK from its SDK Manager), then
  `export ANDROID_HOME=~/Android/Sdk ANDROID_NDK_HOME=~/Android/Sdk/ndk/<version>` and run
  `android/build-apk.sh`. The APK lands in `dist/`.

Copy the APK to the phone and open it to install (allow "install unknown apps" for your file manager).
The app contains no ROMs: on first start, tap the screen and pick your `smashtv.zip`.
To try the touch controls on desktop: `./smashtv --touch`.

## Layout

| Path | What |
|---|---|
| `src/main.c` | The app: window, audio, input, menu, ROM setup, libretro frontend |
| `src/romset.c` | ROM checksum verification and zip import |
| `src/libretro.h` | libretro API header (MIT) |
| `scripts/build-core.sh` | Builds the MAME core with only `midyunit.cpp`, pinned to MAME 0.289 |
| `core/` | Prebuilt core(s) `build.sh` picks up |
| `.github/workflows/build.yml` | Windows / macOS / Linux release builds |

Player data (settings, high scores, save state, `log.txt`):
Linux `~/.local/share/SmashTVNative/smashtv/`, macOS `~/Library/Application Support/SmashTVNative/smashtv/`, Windows `%APPDATA%\SmashTVNative\smashtv\`.

## Licensing

App code: yours. The MAME core is BSD-3/GPL-2 (see the MAME source). If you share builds, ship the core's licence with them, and never ship the ROMs: those are copyrighted by their owners, and each player has to supply their own.
