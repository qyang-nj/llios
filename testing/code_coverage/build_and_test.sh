#!/bin/zsh
# This script was tested on Xcode 14.3
set -e

# Change the target to arm64 to run tests natively on M1 machine
TARGET="arm64-apple-ios15.2-simulator"

SDKROOT=$(xcrun --show-sdk-path --sdk iphonesimulator)
PLATFORM_DIR="$(xcode-select -p)/Platforms/iPhoneSimulator.platform"
TOOLCHAIN_DIR="$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain"
ARCH="$(echo $TARGET | awk -F "-" '{print $1}')"
TEST_BINARY="build/Test.xctest/Test"

rm -rf build
mkdir -p build/Test.xctest

xcrun swiftc -c -o build/Test.o Test.swift Lib.swift \
    -wmo \
    -sdk "$SDKROOT" -target "$TARGET" \
    -F "$PLATFORM_DIR/Developer/Library/Frameworks" \
    -I "$PLATFORM_DIR/Developer/usr/lib" \
    -coverage-prefix-map $PWD=. \
    -profile-coverage-mapping \
    -profile-generate

xcrun clang -bundle -o $TEST_BINARY build/Test.o \
    -target "$TARGET" \
    -isysroot "$SDKROOT" \
    -L "$SDKROOT/usr/lib" \
    -L "$SDKROOT/usr/lib/swift" \
    -L "$TOOLCHAIN_DIR/usr/lib/swift/iphonesimulator" \
    -L "$PLATFORM_DIR/Developer/usr/lib" \
    -F "$PLATFORM_DIR/Developer/Library/Frameworks" \
    -F "$SDKROOT/System/Library/Frameworks" \
    -lSystem \
    -fprofile-instr-generate

PROFRAW_FILE="/tmp/test.profraw"
PROFDATA_FILE="/tmp/test.profdata"

export SIMCTL_CHILD_LLVM_PROFILE_FILE="$PROFRAW_FILE"

rm -f $PROFRAW_FILE $PROFDATA_FILE

xcrun simctl spawn --arch=$ARCH --standalone "iPhone 14 Pro"  \
    "$PLATFORM_DIR/Developer/Library/Xcode/Agents/xctest" \
    $(realpath build/Test.xctest)

xcrun llvm-profdata merge -sparse "$PROFRAW_FILE" -o "$PROFDATA_FILE"

# Show report in terminal
# xcrun llvm-cov report -instr-profile "$PROFDATA_FILE" build/Test.xctest/Test

# Generate HTML report
xcrun llvm-cov show --format=html --output-dir=report --instr-profile $PROFDATA_FILE --compilation-dir $PWD $TEST_BINARY
echo "Generated HTML report at $(realpath report/index.html)"
