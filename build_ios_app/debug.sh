#!/bin/zsh
set -e

# Modify these variables for any other apps.
APP_BUNDLE="Build/SampleApp.app"
SIM_NAME="iPhone 12 Pro"

function error() {
    echo "Error: $1" >&2
    exit 1
}

if ! command -v realpath >/dev/null 2>&1; then
    error "Command 'realpath' could not be found. Install it via 'brew install coreutils'."
fi

app_binary="$APP_BUNDLE/$(basename ${APP_BUNDLE%%.*})"
if [[ ! -f "$app_binary" ]]; then
    error "cannot find binary file $app_binary."
fi

# Check the target platform of the binary.
# 2 is iOS device and 7 is iOS simulator. See <mach-o/loader.h>.
platform=$(otool -l "$app_binary" | grep 'LC_BUILD_VERSION' -A 7 | grep 'platform' | awk '{print $2}')

if [[ "$platform" == 2 ]]; then
    # Call launch.sh since ios-deploy will attach debugger automatically
    ./launch.sh
elif [[ "$platform" != 7 ]]; then
    error "The app bundle is not built for simulator."
fi

app_bundle_id=$(defaults read "$(realpath $APP_BUNDLE/Info.plist)" CFBundleIdentifier)
app_name=$(defaults read "$(realpath $APP_BUNDLE/Info.plist)" CFBundleName)
sim_device=$(xcrun simctl list devices | grep "$SIM_NAME" | head -1)

if [ -z "$sim_device" ]; then
    echo "Info: Cannot find $SIM_NAME simulator. Creating one."
    sim_udid=$(xcrun simctl create "$SIM_NAME" "com.apple.CoreSimulator.SimDeviceType.${SIM_NAME// /-}" "com.apple.CoreSimulator.SimRuntime.iOS-15-0")
else
    sim_udid=$(echo "$sim_device" | grep -E -o -i "([0-9a-f]{8}-([0-9a-f]{4}-){3}[0-9a-f]{12})")
fi

echo "simulator: $sim_device"

if [[ "$sim_device" != *"(Booted)"* ]]; then
    # boot the simulator if it's not booted
    xcrun simctl boot "$sim_udid"
fi
xcrun simctl install "$sim_udid" "$APP_BUNDLE"

# It seems there isn't a way to launch an app in the simulator directly from lldb
# (https://forums.swift.org/t/using-lldb-with-ios-simulator-from-cli/33990).
# so we wait n seconds so that lldb finishes setup and launch the app using simctl.
# Adjust the n number depending on how big the application is.
(sleep 10; xcrun simctl launch "$sim_udid" "$app_bundle_id") &

open $(xcode-select -p)/Applications/Simulator.app

# Save initial lldb commands to a temp file.
lldb_source=$(mktemp)
cat > "$lldb_source" <<EOD
breakpoint set -f AppDelegate.swift -l 12
process attach --name "$app_name" --waitfor
EOD

xcrun lldb --source "$lldb_source" "$APP_BUNDLE"
