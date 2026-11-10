#!/bin/zsh
set -euo pipefail
cd "$(dirname "$0")"
swift build -c release
APP="build/Canivete.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp .build/release/Canivete "$APP/Contents/MacOS/Canivete"
cp Info.plist "$APP/Contents/Info.plist"
codesign --force --sign - "$APP"
echo "Built $APP"
