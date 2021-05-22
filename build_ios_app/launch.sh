#!/bin/bash
set -e

APP_BUNDLE="Build/SampleApp.app"

if [ ! -d "$APP_BUNDLE" ]; then
    ./build.sh
fi

APP_BUNDLE_ID=$(defaults read "$(realpath Build/SampleApp.app/Info.plist)" CFBundleIdentifier)

# Launch app in the simulator
xcrun simctl shutdown all
xcrun simctl boot "iPhone 12"
xcrun simctl install booted "$APP_BUNDLE"
xcrun simctl launch booted "$APP_BUNDLE_ID"
