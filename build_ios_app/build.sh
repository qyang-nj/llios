#!/bin/zsh
set -e

# -d/--device: build for an iOS device instead of simulator
# -r/--release: build for release instead of debug
OPT_DEVICE=0
OPT_RELEASE=0
OPT_MINOS="14.0"
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
        --minos)
            OPT_MINOS=$2
            shift 2
            ;;
        *) # unknown option
            echo "Unknow option: ${1}"
            shift
            ;;
    esac
done

[[ $OPT_DEVICE == 1 ]] && SDK="iphoneos" || SDK="iphonesimulator"
[[ $OPT_DEVICE == 1 ]] && ARCH="arm64" || ARCH="x86_64"
[[ $OPT_DEVICE == 1 ]] && TARGET="${ARCH}-apple-ios${OPT_MINOS}" || TARGET="${ARCH}-apple-ios${OPT_MINOS}-simulator"
[[ $OPT_DEVICE == 1 ]] && MIN_OS_OPTION="-miphoneos-version-min=$OPT_MINOS" || MIN_OS_OPTION="-mios-simulator-version-min=$OPT_MINOS"

SDK_PATH=$(xcrun --show-sdk-path --sdk $SDK)
SWIFTC="xcrun swiftc -sdk $SDK_PATH -target $TARGET"
CLANG="xcrun clang -isysroot $SDK_PATH $MIN_OS_OPTION -arch $ARCH"
APP_NAME="SampleApp"

if [[ "$OPT_RELEASE" == 1 ]]; then
    SWIFTC="$SWIFTC -O"
    CLANG="$CLANG -O"
else
    SWIFTC="$SWIFTC -g -Onone"
    CLANG="$CLANG -g -O0"
fi

function prepare() {
    rm -rf Build
    mkdir -p Build
}

function build_swift_static_lib() {
    local OUTPUT_FILE_MAP_JSON="Build/StaticLib_OutputFileMap.json"
    cat > $OUTPUT_FILE_MAP_JSON <<EOL
{
"": {"swift-dependencies": "Build/StaticLib-master.swiftdeps"},
"Sources/StaticLib/foo.swift": {"object": "Build/foo.o", "swift-dependencies": "Build/foo.swiftdeps"},
"Sources/StaticLib/bar.swift": {"object": "Build/bar.o", "swift-dependencies": "Build/bar.swiftdeps"}
}
EOL

    local PARAMS=(
        -incremental
        -parse-as-library
        -c
        -module-name StaticLib
        -emit-module
        -emit-module-path Build/StaticLib.swiftmodule
        -index-store-path Build/IndexStore
        -output-file-map "$OUTPUT_FILE_MAP_JSON"
        Sources/StaticLib/bar.swift Sources/StaticLib/foo.swift
    )

    eval ${SWIFTC} ${PARAMS[@]}

    xcrun libtool -static -o Build/StaticLib.a Build/foo.o Build/bar.o
}

function build_swift_dylib() {
    local PARAMS=(
        -emit-library
        -emit-module
        -module-name SwiftDylib
        -emit-module-path Build/SwiftDylib.swiftmodule
        -o Build/SwiftDylib.dylib
        -Xlinker -install_name -Xlinker @rpath/Frameworks/SwiftDylib.dylib
        Sources/SwiftDylib/SwiftDylib.swift
    )
    eval ${SWIFTC} ${PARAMS[@]}
}

function build_objc_dylib() {
    local PARAMS=(
        -shared
        -all_load
        -fmodules
        -install_name @rpath/Frameworks/ObjcDylib.dylib
        -o Build/ObjcDylib.dylib
        Sources/ObjcDylib/LLIOSObjcDylib.m
    )
    eval ${CLANG} ${PARAMS[@]}
}

function build_executable() {
    local PARAMS=(
        -emit-executable
        -I Build
        -I Sources/ObjcDylib
        -o "Build/$APP_NAME"
        -Xlinker -rpath -Xlinker @executable_path/
        Build/StaticLib.a
        Build/SwiftDylib.dylib
        Build/ObjcDylib.dylib
        Sources/AppDelegate.swift Sources/ViewController.swift Sources/SwiftUIView.swift
    )
    eval ${SWIFTC} ${PARAMS[@]}
}

function process_info_plist() {
    PLIST_BUDDY="/usr/libexec/PlistBuddy"
    cp Sources/Info.plist Build/Info.plist
    $PLIST_BUDDY -c "Set :CFBundleDevelopmentRegion en" Build/Info.plist
    $PLIST_BUDDY -c "Set :CFBundleExecutable $APP_NAME" Build/Info.plist
    $PLIST_BUDDY -c "Set :CFBundleName $APP_NAME" Build/Info.plist
    $PLIST_BUDDY -c "Set :CFBundleIdentifier me.qyang.$APP_NAME" Build/Info.plist
    $PLIST_BUDDY -c "Set :CFBundlePackageType APPL" Build/Info.plist
}

function package_app_bundle() {
    mkdir -p "Build/$APP_NAME.app"
    mkdir -p "Build/$APP_NAME.app/Frameworks"
    mv "Build/$APP_NAME" "Build/$APP_NAME.app"
    mv "Build/SwiftDylib.dylib" "Build/$APP_NAME.app/Frameworks"
    mv "Build/ObjcDylib.dylib" "Build/$APP_NAME.app/Frameworks"
    mv "Build/Info.plist" "Build/$APP_NAME.app"
}

# Code Signing (required for device builds)
# Note: You need to replace the signing identity with your one.
# Find a valid signing identity by `security find-identity -v -p codesigning`.
# You also need to change the app id and modify Entitlements.plist.
# (It doesn't seem to be necessary to copy the provisioning profile. Idk why.)
function sign_app() {
    identity=$(security find-identity -v -p codesigning | grep "Apple Development" | head -1 | awk '{print $2}')
    # The embedded dylibs need to be signed separately and before signing the app bundle.
    codesign --force --sign "$identity" "Build/$APP_NAME.app/Frameworks/"*.dylib
    codesign --force --sign "$identity" "Build/$APP_NAME.app" --entitlements Sources/Entitlements.plist
}

prepare
build_swift_static_lib
build_swift_dylib
build_objc_dylib
build_executable
process_info_plist
package_app_bundle
if [[ "$OPT_DEVICE" == 1 ]]; then sign_app; fi

echo "Done. Build/$APP_NAME.app"
