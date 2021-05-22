#!/bin/sh
set -e

APP_BUNDLE="Build/SampleApp.app"
SIM_NAME="iPhone 12 Pro"

if [ ! -d "$APP_BUNDLE" ]; then
    ./build.sh
fi

APP_BUNDLE_ID=$(defaults read "$(realpath Build/SampleApp.app/Info.plist)" CFBundleIdentifier)
APP_NAME=$(defaults read "$(realpath Build/SampleApp.app/Info.plist)" CFBundleName)
SIM_DEVICE=$(xcrun simctl list devices | grep "$SIM_NAME" | head -1)
SIM_UDID=$(echo "$SIM_DEVICE" | grep -E -o -i "([0-9a-f]{8}-([0-9a-f]{4}-){3}[0-9a-f]{12})")

echo "simulator: $SIM_DEVICE"

if [[ "$SIM_DEVICE" != *"(Booted)"* ]]; then
    # boot the simulator if it's not booted
    xcrun simctl boot "$SIM_UDID"
fi
xcrun simctl install "$SIM_UDID" "$APP_BUNDLE"

# It seems there isn't a way to launch an app in the simulator directly from lldb
# (https://forums.swift.org/t/using-lldb-with-ios-simulator-from-cli/33990).
# so we wait n seconds so that lldb finishes setup and launch the app using simctl.
# Adjust the n number depending on how big the application is.
(sleep 10; xcrun simctl launch "$SIM_UDID" "$APP_BUNDLE_ID") &

# Save initial lldb commands to a temp file.
lldb_source=$(mktemp)
cat > "$lldb_source" <<EOD
breakpoint set -f AppDelegate.swift -l 12
process attach --name "$APP_NAME" --waitfor
EOD

xcrun lldb --source "$lldb_source" "$APP_BUNDLE"
