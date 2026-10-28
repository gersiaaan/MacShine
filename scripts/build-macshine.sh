#!/usr/bin/env bash
# Build MacShine (Sunshine with per-client virtual displays) and install it as /Applications/MacShine.app.
#
# Run from anywhere inside the MacShine clone:
#   bash scripts/build-macshine.sh            incremental build + install
#   bash scripts/build-macshine.sh --deps     also install/update Homebrew build dependencies
#
# The app gets its own bundle identifier, so macOS keeps its permissions (Screen Recording,
# Accessibility) separate from an official Sunshine install.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
APP_DEST="/Applications/MacShine.app"
BUNDLE_ID="dev.gersiaaan.macshine"
WITH_DEPS=0
[[ "${1:-}" == "--deps" ]] && WITH_DEPS=1

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }

[[ "$(uname -s)" == "Darwin" && "$(uname -m)" == "arm64" ]] || { echo "Apple Silicon Mac required."; exit 1; }
command -v brew >/dev/null || { echo "Homebrew is required: https://brew.sh"; exit 1; }
xcode-select -p >/dev/null 2>&1 || { xcode-select --install; echo "Rerun after the Command Line Tools are installed."; exit 1; }

cd "$REPO"
say "Updating submodules"
git submodule update --init --recursive --depth 1 --jobs 8

if [[ $WITH_DEPS -eq 1 ]] || ! brew list --versions qtbase >/dev/null 2>&1; then
  say "Installing build dependencies (Homebrew)"
  ./scripts/macos_build.sh --step=deps --skip-codesign
fi

if [[ ! -f build/CMakeCache.txt ]]; then
  say "Configuring"
  ./scripts/macos_build.sh --step=cmake --skip-codesign --skip-tests
fi

say "Building"
./scripts/macos_build.sh --step=build --skip-codesign --skip-tests

APP_BUILT="$(find "$REPO/build" -maxdepth 2 -name 'Sunshine.app' -type d | head -n1)"
[[ -n "$APP_BUILT" ]] || { echo "Sunshine.app not found under $REPO/build"; exit 1; }
[[ -x "$APP_BUILT/Contents/MacOS/sunshine_vd_helper" ]] || { echo "Virtual display helper missing from the bundle"; exit 1; }

# Optional LaunchAgent that keeps MacShine running (starts at login, restarts after a crash).
AGENT_LABEL="$BUNDLE_ID"
AGENT_PLIST="$HOME/Library/LaunchAgents/$AGENT_LABEL.plist"
AGENT_DOMAIN="gui/$(id -u)"
AGENT_LOADED=0
launchctl print "$AGENT_DOMAIN/$AGENT_LABEL" >/dev/null 2>&1 && AGENT_LOADED=1

say "Installing as $APP_DEST"
if [[ $AGENT_LOADED -eq 1 ]]; then
  # Unload first, or launchd restarts MacShine while the bundle is being replaced.
  launchctl bootout "$AGENT_DOMAIN/$AGENT_LABEL" 2>/dev/null || true
fi
osascript -e 'tell application id "'"$BUNDLE_ID"'" to quit' >/dev/null 2>&1 || true
pkill -f "$APP_DEST/Contents/MacOS/" 2>/dev/null || true
rm -rf "$APP_DEST"
ditto "$APP_BUILT" "$APP_DEST"

PLIST="$APP_DEST/Contents/Info.plist"
for key in CFBundleName CFBundleDisplayName; do
  /usr/libexec/PlistBuddy -c "Set :$key MacShine" "$PLIST" 2>/dev/null ||
    /usr/libexec/PlistBuddy -c "Add :$key string MacShine" "$PLIST"
done
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier $BUNDLE_ID" "$PLIST"

# Sign with a local self-signed certificate instead of ad hoc: macOS ties Screen Recording and
# Accessibility to the signing certificate, so the permissions survive rebuilds. The certificate
# lives in its own keychain file; the login keychain, trust settings and keychain search list are
# left untouched.
SIGN_DIR="$HOME/Library/Application Support/MacShine/signing"
SIGN_KEYCHAIN="$SIGN_DIR/macshine-signing.keychain-db"
SIGN_IDENTITY="MacShine Local Signing"
if [[ ! -f "$SIGN_KEYCHAIN" ]]; then
  say "Creating local signing certificate ($SIGN_IDENTITY)"
  mkdir -p "$SIGN_DIR"
  chmod 700 "$SIGN_DIR"
  /usr/bin/openssl rand -hex 24 > "$SIGN_DIR/keychain-password"
  chmod 600 "$SIGN_DIR/keychain-password"
  SIGN_PASSWORD="$(cat "$SIGN_DIR/keychain-password")"
  SIGN_TMP="$(mktemp -d)"
  cat > "$SIGN_TMP/cert.cnf" <<EOF
[req]
distinguished_name=dn
x509_extensions=ext
prompt=no
[dn]
CN=$SIGN_IDENTITY
[ext]
basicConstraints=critical,CA:false
keyUsage=critical,digitalSignature
extendedKeyUsage=critical,codeSigning
EOF
  /usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -config "$SIGN_TMP/cert.cnf" \
    -keyout "$SIGN_TMP/key.pem" -out "$SIGN_TMP/cert.pem" 2>/dev/null
  /usr/bin/openssl pkcs12 -export -name "$SIGN_IDENTITY" -inkey "$SIGN_TMP/key.pem" -in "$SIGN_TMP/cert.pem" \
    -out "$SIGN_TMP/identity.p12" -passout "pass:$SIGN_PASSWORD"
  security create-keychain -p "$SIGN_PASSWORD" "$SIGN_KEYCHAIN"
  security set-keychain-settings "$SIGN_KEYCHAIN"
  security unlock-keychain -p "$SIGN_PASSWORD" "$SIGN_KEYCHAIN"
  security import "$SIGN_TMP/identity.p12" -k "$SIGN_KEYCHAIN" -P "$SIGN_PASSWORD" -T /usr/bin/codesign >/dev/null
  security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$SIGN_PASSWORD" "$SIGN_KEYCHAIN" >/dev/null
  rm -rf "$SIGN_TMP"
fi
security unlock-keychain -p "$(cat "$SIGN_DIR/keychain-password")" "$SIGN_KEYCHAIN"

say "Signing ($SIGN_IDENTITY)"
codesign --force --keychain "$SIGN_KEYCHAIN" --sign "$SIGN_IDENTITY" --identifier "$BUNDLE_ID.vd-helper" "$APP_DEST/Contents/MacOS/sunshine_vd_helper"
codesign --force --deep --keychain "$SIGN_KEYCHAIN" --sign "$SIGN_IDENTITY" --identifier "$BUNDLE_ID" "$APP_DEST"

if [[ $AGENT_LOADED -eq 1 ]]; then
  say "Starting MacShine again ($AGENT_LABEL)"
  launchctl bootstrap "$AGENT_DOMAIN" "$AGENT_PLIST"
fi

say "Done: $APP_DEST"
cat <<EOF

Next:
  1. System Settings > Privacy & Security: allow MacShine under
     "Screen & System Audio Recording" and "Accessibility" (once; rebuilds keep the permissions).
  2. Quit and reopen MacShine.
  3. Web UI: https://localhost:47990
Logs live: "$APP_DEST/Contents/MacOS/Sunshine"
EOF
