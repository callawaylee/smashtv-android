#!/usr/bin/env bash
#
# Make a self-contained SmashTV APK that opens straight into the game.
#
#   bundle-rom.sh  SmashTV.apk  smashtv.zip  [SmashTV-full.apk]
#
#   SmashTV.apk    the base app downloaded from the build (no ROM inside)
#   smashtv.zip    YOUR MAME "smashtv" ROM set
#   output         SmashTV-full.apk (default)
#
# Your ROM is injected and the APK is re-signed entirely on this machine; nothing is uploaded.
# Needs: python3 (already on Fedora) and either Android's apksigner, or Java
#        (sudo dnf install -y java-17-openjdk-headless) so this can fetch a one-file signer.
set -euo pipefail
cd "$(dirname "$0")"

BASE=${1:-}; ROM=${2:-}; OUT=${3:-SmashTV-full.apk}
if [ -z "$BASE" ] || [ -z "$ROM" ]; then
    sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 1
fi
[ -f "$BASE" ] || { echo "Base APK not found: $BASE"; exit 1; }
[ -f "$ROM" ]  || { echo "ROM zip not found: $ROM"; exit 1; }
command -v python3 >/dev/null || { echo "python3 is required."; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
UNSIGNED="$WORK/unsigned.apk"

python3 bundle_rom.py "$BASE" "$ROM" "$UNSIGNED"

echo ">> Signing"
if command -v apksigner >/dev/null 2>&1; then
    KS="$HOME/.android/debug.keystore"
    if [ ! -f "$KS" ]; then
        mkdir -p "$HOME/.android"
        keytool -genkeypair -keystore "$KS" -storepass android -keypass android \
            -alias androiddebugkey -dname "CN=Android Debug,O=Android,C=US" \
            -keyalg RSA -keysize 2048 -validity 10000 >/dev/null 2>&1
    fi
    if command -v zipalign >/dev/null 2>&1; then
        zipalign -f 4 "$UNSIGNED" "$WORK/aligned.apk"; IN="$WORK/aligned.apk"
    else
        IN="$UNSIGNED"
    fi
    apksigner sign --ks "$KS" --ks-pass pass:android --ks-key-alias androiddebugkey \
        --key-pass pass:android --out "$OUT" "$IN"
else
    command -v java >/dev/null || {
        echo "Need Android 'apksigner' or Java. On Fedora/Nobara:"
        echo "    sudo dnf install -y java-17-openjdk-headless"
        exit 1; }
    JAR="$HOME/.cache/uber-apk-signer-1.3.0.jar"
    if [ ! -f "$JAR" ]; then
        echo ">> Fetching the one-file signer (first time only)"
        mkdir -p "$HOME/.cache"
        curl -fL -o "$JAR" \
          https://github.com/patrickfav/uber-apk-signer/releases/download/v1.3.0/uber-apk-signer-1.3.0.jar
    fi
    java -jar "$JAR" --apks "$UNSIGNED" --out "$WORK/signed" --allowResign >/dev/null
    mv "$WORK"/signed/*[Ss]igned*.apk "$OUT"
fi

echo
echo ">> Done:  $OUT"
echo "   Copy it to your phone and open it to install. It boots straight into the game."
