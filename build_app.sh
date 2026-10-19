#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")"

# Generate a .icns from the checked-in PNG if one is not present.
ICON="icon.icns"
if [ ! -f "$ICON" ] && [ -f "docs/icon.png" ]; then
    TMP_DIR="$(mktemp -d)"
    ICONSET="$TMP_DIR/AppIcon.iconset"
    mkdir -p "$ICONSET"
    for spec in "16 16x16" "32 16x16@2x" "32 32x32" "64 32x32@2x" \
                "128 128x128" "256 128x128@2x" "256 256x256" \
                "512 256x256@2x" "512 512x512" "1024 512x512@2x"; do
        size="${spec%% *}"
        name="${spec##* }"
        sips -z "$size" "$size" "docs/icon.png" \
            --out "$ICONSET/icon_${name}.png" >/dev/null
    done
    iconutil -c icns "$ICONSET" -o "$ICON"
    rm -rf "$TMP_DIR"
fi

swift build -c release
BIN_PATH="$(swift build -c release --show-bin-path)"

APP="dist/EarCue.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

cp "$BIN_PATH/EarCue" "$APP/Contents/MacOS/EarCue"
cp "Info.plist" "$APP/Contents/Info.plist"
if [ -f "$ICON" ]; then
    cp "$ICON" "$APP/Contents/Resources/icon.icns"
fi

codesign --force --deep --sign - "$APP"

echo "Built: $APP"
