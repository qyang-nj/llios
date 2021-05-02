#!/bin/bash
# This script is tested on XCode 12.5
set -e

SDKROOT=$(xcrun --show-sdk-path --sdk iphonesimulator)
PLATFORM_DIR="/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform"
TOOLCHAIN_DIR="/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain"
TARGET="x86_64-apple-ios14.0-simulator"

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
    -lSystem

# To set environment variables in the simulator, use SIMCTL_CHILD_ prefix
# export SIMCTL_CHILD_DYLD_PRINT_ENV=1
# export SIMCTL_CHILD_DYLD_PRINT_LIBRARIES=1

xcrun simctl spawn --standalone "iPhone 8" \
    "$PLATFORM_DIR/Developer/Library/Xcode/Agents/xctest" \
    $(realpath build/Test.xctest)
