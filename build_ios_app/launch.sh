#!/bin/zsh
set -e

APP_BUNDLE="Build/SampleApp.app"
SIM_NAME="iPhone 12 Pro"

function error() {
    echo "Error: $1" >&2
    exit 1
}

if [[ ! -d "$APP_BUNDLE" ]]; then
    ./build.sh
fi

app_binary="$APP_BUNDLE/$(basename ${APP_BUNDLE%%.*})"
if [[ ! -f "$app_binary" ]]; then
    error "cannot find binary file $app_binary."
fi

# Check the target platform of the binary.
# 2 is iOS device and 7 is iOS simulator. See <mach-o/loader.h>.
platform=$(otool -l "$app_binary" | grep 'LC_BUILD_VERSION' -A 7 | grep 'platform' | awk '{print $2}')

if [[ "$platform" == 7 ]]; then
    echo "Launching simulator ..."

    APP_BUNDLE_ID=$(defaults read "$(realpath Build/SampleApp.app/Info.plist)" CFBundleIdentifier)

    # Launch app in the simulator
    xcrun simctl shutdown all
    xcrun simctl boot "$SIM_NAME"
    xcrun simctl install booted "$APP_BUNDLE"
    xcrun simctl launch booted "$APP_BUNDLE_ID"

    open $(xcode-select -p)/Applications/Simulator.app

elif [ "$platform" == 2 ]; then
    echo "Launching on device ..."
    if ! command -v ios-deploy &> /dev/null
    then
        error "'ios-deploy' is needed to launch the app on device. \nCheck out this awesome tool (https://github.com/ios-control/ios-deploy)."
    fi
    ios-deploy --debug --bundle "Build/SampleApp.app"
else
    error "The target platform of the binary is neither iOS simulator nor device."
fi
