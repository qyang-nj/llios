#!/bin/zsh
# This script was tested on Xcode 16.0
set -e

TARGET="arm64-apple-ios16.0-simulator"

SDKROOT=$(xcrun --show-sdk-path --sdk iphonesimulator)
PLATFORM_DIR="$(xcode-select -p)/Platforms/iPhoneSimulator.platform"
TOOLCHAIN_DIR="$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain"
ARCH="$(echo $TARGET | awk -F "-" '{print $1}')"

rm -rf build
mkdir -p build/Test.xctest

xcrun swiftc -c -o build/Test.o Test.swift \
    -sdk "$SDKROOT" -target "$TARGET" \
    -F "$PLATFORM_DIR/Developer/Library/Frameworks" \
    -I "$PLATFORM_DIR/Developer/usr/lib"

xcrun ld -bundle -o build/Test.xctest/Test build/Test.o \
    -syslibroot "$SDKROOT" \
    -L "$SDKROOT/usr/lib" \
    -L "$SDKROOT/usr/lib/swift" \
    -L "$TOOLCHAIN_DIR/usr/lib/swift/iphonesimulator" \
    -L "$PLATFORM_DIR/Developer/usr/lib" \
    -F "$PLATFORM_DIR/Developer/Library/Frameworks" \
    -F "$SDKROOT/System/Library/Frameworks" \
    -lXCTestSwiftSupport

# To set environment variables in the simulator, use SIMCTL_CHILD_ prefix
# export SIMCTL_CHILD_DYLD_PRINT_ENV=1
# export SIMCTL_CHILD_DYLD_PRINT_LIBRARIES=1

xcrun simctl spawn --arch=$ARCH --standalone "iPhone 16"  \
    "$PLATFORM_DIR/Developer/Library/Xcode/Agents/xctest" \
    $(realpath build/Test.xctest)
