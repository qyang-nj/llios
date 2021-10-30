#!/bin/bash
set -e

# -d/--device: build for an iOS device instead of simulator
# -r/--release: build for release instead of debug
OPT_DEVICE=0
OPT_RELEASE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -d|--device)
            OPT_DEVICE=1
            shift
            echo "Note: Build for iOS devices."
            ;;
        -r|--release)
            OPT_RELEASE=1
            echo "Note: Build for release."
            shift
            ;;
        *) # unknown option
            echo "Unknow option: ${1}"
            shift
            ;;
    esac
done

[ $OPT_DEVICE == 1 ] && SDK="iphoneos" || SDK="iphonesimulator"
SDK_PATH=$(xcrun --show-sdk-path --sdk $SDK)

# Targeting ios12 or lower will end up linking with libSwiftCompatibility50
# and libSwiftCompatibilityDynamicReplacements.
[ $OPT_DEVICE == 1 ] && TARGET=arm64-apple-ios14.0 || TARGET=x86_64-apple-ios14.0-simulator
SWIFTC="xcrun swiftc -sdk $SDK_PATH -target $TARGET"
APP_NAME="SampleApp"

if [ "$OPT_RELEASE" == 1 ]; then
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
$PLIST_BUDDY -c "Set :CFBundleIdentifier me.qyang.$APP_NAME" Build/Info.plist
$PLIST_BUDDY -c "Set :CFBundlePackageType APPL" Build/Info.plist

# Build app bundle
mkdir -p "Build/$APP_NAME.app"
mv "Build/$APP_NAME" "Build/$APP_NAME.app"
mv "Build/DynamicLib.dylib" "Build/$APP_NAME.app"
mv "Build/Info.plist" "Build/$APP_NAME.app"

# Code Signing (required for device builds)
# Note: You need to replace the signing identity with your one.
# Find a valid signing identity by `security find-identity -v -p codesigning`.
# You also need to change the app id and modify Entitlements.plist.
# (It doesn't seem to be necessary to copy the provisioning profile. Idk why.)
if [ "$OPT_DEVICE" == 1 ]; then
    # The embedded dylibs need to be signed separately and before signing the app bundle.
    codesign --force --sign '08F760DEAD51F26EE4ADC5FF40196215C85AD9DE' "$(ls Build/$APP_NAME.app/*.dylib)"
    codesign --force --sign '08F760DEAD51F26EE4ADC5FF40196215C85AD9DE' "Build/$APP_NAME.app" --entitlements Sources/Entitlements.plist
fi

echo "Done. Build/$APP_NAME.app"
