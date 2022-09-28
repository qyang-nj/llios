#!/bin/zsh
# This script was tested on Xcode 14.0

TARGET="x86_64-apple-ios14.0-simulator"
SDKROOT=$(xcrun --show-sdk-path --sdk iphonesimulator)
PLATFORM_DIR="$(xcode-select -p)/Platforms/iPhoneSimulator.platform"
TOOLCHAIN_DIR="$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain"
ARCH="$(echo $TARGET | awk -F "-" '{print $1}')"

rm -rf build
mkdir -p build/HostApp.app

# Building the host app
xcrun swiftc AppDelegate.swift ViewController.swift \
    -sdk "$SDKROOT" -target "$TARGET" \
    -emit-executable \
    -o "build/HostApp.app/HostApp"

cp App-Info.plist build/HostApp.app/Info.plist

# Building the test
mkdir -p build/HostApp.app/PlugIns/HostedTests.xctest

xcrun swiftc -c -o build/HostedTests.o HostedTests.swift \
    -sdk "$SDKROOT" -target "$TARGET" \
    -F "$PLATFORM_DIR/Developer/Library/Frameworks" \
    -I "$PLATFORM_DIR/Developer/usr/lib"

xcrun ld -bundle -o build/HostApp.app/PlugIns/HostedTests.xctest/HostedTests build/HostedTests.o \
    -syslibroot "$SDKROOT" \
    -L "$SDKROOT/usr/lib" \
    -L "$SDKROOT/usr/lib/swift" \
    -L "$TOOLCHAIN_DIR/usr/lib/swift/iphonesimulator" \
    -L "$PLATFORM_DIR/Developer/usr/lib" \
    -F "$PLATFORM_DIR/Developer/Library/Frameworks" \
    -F "$SDKROOT/System/Library/Frameworks" \
    -lSystem


# Install the app
xcrun simctl install "iPhone 14 Pro" "$(realpath build/HostApp.app)"

# Launch the app and run the tests
export SIMCTL_CHILD_DYLD_INSERT_LIBRARIES=$PLATFORM_DIR/Developer/usr/lib/libXCTestBundleInject.dylib
export SIMCTL_CHILD_DYLD_LIBRARY_PATH=$PLATFORM_DIR/Developer/usr/lib
xcrun simctl launch --console-pty "iPhone 14 Pro" me.qyang.HostApp
