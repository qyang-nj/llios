#!/bin/sh
set -e

APP_BUNDLE="Build/SampleApp.app"
APP_BINARY="$APP_BUNDLE/SampleApp"
SIM_NAME="iPhone 12 Pro"

if [ ! -d "$APP_BUNDLE" ]; then
    ./build.sh
fi

# Check the target platform of the binary.
# 2 is iOS device and 7 is iOS simulator. See <mach-o/loader.h>.
platform=$(otool -l "$APP_BINARY" | grep 'LC_BUILD_VERSION' -A 7 | grep 'platform' | awk '{print $2}')

if [ "$platform" == 2 ]; then
    # Call launch.sh since ios-deploy will attach debugger automatically
    ./launch.sh
elif [ "$platform" != 7 ]; then
    echo "Error: The app bundle is not built for simulator."
    exit 1
fi

APP_BUNDLE_ID=$(defaults read "$(realpath Build/SampleApp.app/Info.plist)" CFBundleIdentifier)
APP_NAME=$(defaults read "$(realpath Build/SampleApp.app/Info.plist)" CFBundleName)
SIM_DEVICE=$(xcrun simctl list devices | grep "$SIM_NAME" | head -1)

if [ -z "$SIM_DEVICE" ]; then
    echo "Info: Cannot find $SIM_NAME simulator. Creating one."
    SIM_UDID=$(xcrun simctl create "$SIM_NAME" "com.apple.CoreSimulator.SimDeviceType.${SIM_NAME// /-}" "com.apple.CoreSimulator.SimRuntime.iOS-15-0")
else
    SIM_UDID=$(echo "$SIM_DEVICE" | grep -E -o -i "([0-9a-f]{8}-([0-9a-f]{4}-){3}[0-9a-f]{12})")
fi

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
