#!/bin/bash
# Builds the "MacShine" virtual microphone, a renamed BlackHole driver, with the Command Line Tools only.
#
# Device 1 "MacShine"        : visible input, what apps (Claude, Zoom, ...) record from.
# Device 2 "MacShine Sender" : hidden output, MacShine plays the client's microphone into it.
#
# Usage: build-driver.sh [output-dir]   -> produces <output-dir>/MacShine.driver
set -euo pipefail

BLACKHOLE_TAG="v0.7.1"
BUNDLE_ID="dev.macshine.MacShineMic"
FACTORY_UUID="7D1F3B2A-5C4E-4E8B-9A61-2F0D8C3E6B57"
OUT_DIR="${1:-$(pwd)/cmake-build-mic-driver}"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$BLACKHOLE_TAG" \
  https://github.com/ExistentialAudio/BlackHole.git "$WORK_DIR/BlackHole"
SRC="$WORK_DIR/BlackHole/BlackHole"

BUNDLE="$WORK_DIR/MacShine.driver"
mkdir -p "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources"

clang -bundle -O2 -arch arm64 -arch x86_64 -mmacosx-version-min=11.0 \
  -DkDriver_Name='"MacShine"' \
  -DkPlugIn_BundleID="\"$BUNDLE_ID\"" \
  -DkPlugIn_Icon='"MacShine.icns"' \
  -DkHas_Driver_Name_Format=false \
  -DkDevice_Name='"MacShine"' \
  -DkDevice2_Name='"MacShine Sender"' \
  -DkDevice_HasInput=true -DkDevice_HasOutput=false -DkDevice_IsHidden=false \
  -DkDevice2_HasInput=false -DkDevice2_HasOutput=true -DkDevice2_IsHidden=true \
  -DkManufacturer_Name='"MacShine"' \
  -DkNumber_Of_Channels=1 \
  -DkSampleRates=48000 \
  -DkCanBeDefaultSystemDevice=false \
  -framework CoreAudio -framework CoreFoundation -framework Accelerate \
  -o "$BUNDLE/Contents/MacOS/MacShine" "$SRC/BlackHole.c"

cp "$SRC/BlackHole.icns" "$BUNDLE/Contents/Resources/MacShine.icns"

cat > "$BUNDLE/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>English</string>
  <key>CFBundleExecutable</key><string>MacShine</string>
  <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>MacShine</string>
  <key>CFBundlePackageType</key><string>BNDL</string>
  <key>CFBundleShortVersionString</key><string>${BLACKHOLE_TAG#v}</string>
  <key>CFBundleSignature</key><string>????</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>CFPlugInFactories</key>
  <dict><key>$FACTORY_UUID</key><string>BlackHole_Create</string></dict>
  <key>CFPlugInTypes</key>
  <dict>
    <key>443ABAB8-E7B3-491A-B985-BEB9187030DB</key>
    <array><string>$FACTORY_UUID</string></array>
  </dict>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
</dict>
</plist>
PLIST

codesign --force --sign - "$BUNDLE"

mkdir -p "$OUT_DIR"
rm -rf "$OUT_DIR/MacShine.driver"
cp -R "$BUNDLE" "$OUT_DIR/MacShine.driver"
echo "$OUT_DIR/MacShine.driver"
