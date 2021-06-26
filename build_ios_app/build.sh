#!/bin/bash
set -e

SDK=iphonesimulator
SDK_PATH=$(xcrun --show-sdk-path --sdk $SDK)
BUILD_TYPE="Debug" # Debug or Release

# Targeting ios12 or lower will end up linking with libSwiftCompatibility50
# and libSwiftCompatibilityDynamicReplacements.
TARGET=x86_64-apple-ios12.0-simulator
SWIFTC="xcrun swiftc -sdk $SDK_PATH -target $TARGET"
APP_NAME="SampleApp"

if [ "$BUILD_TYPE" == "Release" ]; then
    SWIFTC="$SWIFTC -O"
else
    SWIFTC="$SWIFTC -g -Onone"
fi

# Prepare
rm -rf Build
mkdir -p Build

# Build the static library
OUTPUT_FILE_MAP_JSON="Build/StaticLib_OutputFileMap.json"
cat > $OUTPUT_FILE_MAP_JSON <<EOL
{
  "": {"swift-dependencies": "Build/StaticLib-master.swiftdeps"},
  "Sources/StaticLib/foo.swift": {"object": "Build/foo.o"},
  "Sources/StaticLib/bar.swift": {"object": "Build/bar.o"}
}
EOL

$SWIFTC \
    -incremental \
    -parse-as-library \
    -c \
    -module-name StaticLib \
    -emit-module \
    -emit-module-path Build/StaticLib.swiftmodule \
    -index-store-path Build/IndexStore \
    -output-file-map "$OUTPUT_FILE_MAP_JSON" \
    Sources/StaticLib/bar.swift Sources/StaticLib/foo.swift

xcrun libtool \
    -static \
    -o Build/StaticLib.a \
    Build/foo.o Build/bar.o

# Build the dynamic library
$SWIFTC \
    -emit-library \
    -emit-module \
    -o Build/DynamicLib.dylib \
    -module-name DynamicLib \
    -emit-module-path Build/DynamicLib.swiftmodule \
    -Xlinker -install_name -Xlinker @rpath/DynamicLib.dylib \
    Sources/DynamicLib/DynamicLib.swift

# Build the executable
$SWIFTC \
    -emit-executable \
    -I Build \
    -o "Build/$APP_NAME" \
    -Xlinker -rpath -Xlinker @executable_path/ \
    Build/StaticLib.a Build/DynamicLib.dylib \
    Sources/AppDelegate.swift Sources/ViewController.swift Sources/SwiftUIView.swift

# Process Info.plist
PLIST_BUDDY="/usr/libexec/PlistBuddy"
cp Sources/Info.plist Build/Info.plist
$PLIST_BUDDY -c "Set :CFBundleDevelopmentRegion en" Build/Info.plist
$PLIST_BUDDY -c "Set :CFBundleExecutable $APP_NAME" Build/Info.plist
$PLIST_BUDDY -c "Set :CFBundleName $APP_NAME" Build/Info.plist
$PLIST_BUDDY -c "Set :CFBundleIdentifier com.qyang-nj.$APP_NAME" Build/Info.plist
$PLIST_BUDDY -c "Set :CFBundlePackageType APPL" Build/Info.plist

# Build app bundle
mkdir -p "Build/$APP_NAME.app"
mv "Build/$APP_NAME" "Build/$APP_NAME.app"
mv "Build/DynamicLib.dylib" "Build/$APP_NAME.app"
mv "Build/Info.plist" "Build/$APP_NAME.app"

echo "Done. Build/$APP_NAME.app"
