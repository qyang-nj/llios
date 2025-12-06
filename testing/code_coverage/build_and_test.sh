#!/bin/zsh
# This script was tested on Xcode 26.1
set -e

TARGET="$(uname -m)-apple-ios16.1-simulator"

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
    -fprofile-instr-generate

PROFRAW_FILE="/tmp/test.profraw"
PROFDATA_FILE="/tmp/test.profdata"

# %c enables a mode in which profile counter updates are continuously synced to a file
export SIMCTL_CHILD_LLVM_PROFILE_FILE="$PROFRAW_FILE%c"

rm -f $PROFRAW_FILE $PROFDATA_FILE

# Don't exit if test fails
set +e
xcrun simctl spawn --arch=$ARCH --standalone "iPhone 17"  \
    "$PLATFORM_DIR/Developer/Library/Xcode/Agents/xctest" \
    $PWD/build/Test.xctest
set -e

xcrun llvm-profdata merge -sparse "$PROFRAW_FILE" -o "$PROFDATA_FILE"

COMMON_OPTIONS=(-instr-profile $PROFDATA_FILE --compilation-dir $PWD)

# Show report in terminal
xcrun llvm-cov report $COMMON_OPTIONS $TEST_BINARY
echo ""

# Export json report
xcrun llvm-cov export $COMMON_OPTIONS --format=text $TEST_BINARY > coverage.json
echo "Generated JSON report at $PWD/coverage.json"

# Generate HTML report
xcrun llvm-cov show $COMMON_OPTIONS --format=html --output-dir=report $TEST_BINARY
echo "Generated HTML report at $PWD/report/index.html"
