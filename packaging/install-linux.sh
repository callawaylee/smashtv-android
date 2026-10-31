#!/usr/bin/env bash
# Adds "Smash T.V." to your application menu (no root needed).
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$APPS"
cat > "$APPS/smashtv-native.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=Smash T.V.
Comment=Arcade twin-stick shooter
Exec="$DIR/smashtv"
Path=$DIR
Icon=applications-games
Terminal=false
Categories=Game;ActionGame;
DESK
chmod +x "$DIR/smashtv"
# Install bundled ROMs (if this build has them) so the game starts straight away.
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/SmashTVNative/smashtv/roms"
if [ -f "$DIR/smashtv.zip" ]; then
  mkdir -p "$DATA" && cp "$DIR/smashtv.zip" "$DATA/smashtv.zip" && echo "Installed ROMs."
fi
echo "Added Smash T.V. to your app menu. You can also run: $DIR/smashtv"
