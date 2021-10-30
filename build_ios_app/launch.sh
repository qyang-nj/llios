#!/bin/bash
set -e

APP_BUNDLE="Build/SampleApp.app"
APP_BINARY="$APP_BUNDLE/SampleApp"

if [ ! -d "$APP_BUNDLE" ]; then
    ./build.sh
fi

# Check the target platform of the binary.
# 2 is iOS device and 7 is iOS simulator. See <mach-o/loader.h>.
platform=$(otool -l "$APP_BINARY" | grep 'LC_BUILD_VERSION' -A 7 | grep 'platform' | awk '{print $2}')

if [ "$platform" == 7 ]; then
    echo "Launching simulator ..."

    APP_BUNDLE_ID=$(defaults read "$(realpath Build/SampleApp.app/Info.plist)" CFBundleIdentifier)

    # Launch app in the simulator
    xcrun simctl shutdown all
    xcrun simctl boot "iPhone 12"
    xcrun simctl install booted "$APP_BUNDLE"
    xcrun simctl launch booted "$APP_BUNDLE_ID"
elif [ "$platform" == 2 ]; then
    echo "Launching on device ..."
    if ! command -v ios-deploy &> /dev/null
    then
        echo "Error: 'ios-deploy' is needed to launch the app on device."
        echo "Check out this aweseome tool (https://github.com/ios-control/ios-deploy)."
        exit 1
    fi
    ios-deploy --debug --bundle "Build/SampleApp.app"
else
    echo "Error: The target platform of the binary is neither iOS simulator nor device."
    exit 1
fi
