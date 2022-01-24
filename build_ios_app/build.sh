#!/bin/zsh
set -e

# -d/--device: build for an iOS device instead of simulator
# -r/--release: build for release instead of debug
# --minos: set minimum os version
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

APP_NAME="SampleApp"

if [[ $OPT_DEVICE == 1 ]]; then
    SDK_PATH=$(xcrun --show-sdk-path --sdk iphoneos)
    SWIFT_FLAGS+=(-sdk $SDK_PATH -target arm64-apple-ios${OPT_MINOS})
    CFLAGS+=(-isysroot $SDK_PATH -arch arm64 -miphoneos-version-min=${OPT_MINOS})
else
    SDK_PATH=$(xcrun --show-sdk-path --sdk iphonesimulator)
    SWIFT_FLAGS+=(-sdk $(xcrun --show-sdk-path --sdk $SDK_PATH) -target x86_64-apple-ios${OPT_MINOS}-simulator)
    CFLAGS+=(-isysroot $SDK_PATH -arch x86_64 -mios-simulator-version-min=${OPT_MINOS})
fi

if [[ "$OPT_RELEASE" == 1 ]]; then
    SWIFT_FLAGS+=($SWIFT_FLAGS -O)
    CFLAGS+=($CFLAGS -O)
else
    SWIFT_FLAGS+=(-g -Onone)
    CFLAGS+=($CFLAGS -g -O0)
fi

function prepare() {
    rm -rf Build
    mkdir -p Build
}

function build_swift_static_lib() {
    echo ">>> Build Swift static library."
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

    xcrun swiftc ${SWIFT_FLAGS[@]} ${PARAMS[@]}
    xcrun libtool -static -o Build/StaticLib.a Build/foo.o Build/bar.o
}

function build_mixed_module() {
    echo ">>> Build mixed language module."
    local BUILD_DIR="Build/MixedModule"
    mkdir -p $BUILD_DIR

    local SWIFT_PARAMS=(
        -parse-as-library
        -emit-object
        -emit-module
        -module-name MixedModule
        -emit-module-path $BUILD_DIR/MixedModule.swiftmodule
        -import-underlying-module
        -I Sources/MixedModule # for underlying module's module.modulemap
        -emit-objc-header
        -emit-objc-header-path $BUILD_DIR/MixedModule-Swift.h
        -o $BUILD_DIR/MixedModule_Swift.o
        -wmo # generate a single .o file with multiple .swift source files
        Sources/MixedModule/MySwiftProducer.swift
        Sources/MixedModule/MySwiftMaterial.swift
    )
    xcrun swiftc ${SWIFT_FLAGS[@]} ${SWIFT_PARAMS[@]}

    local OBJC_PARAMS=(
        -c
        -fmodules
        -ObjC
        -I $BUILD_DIR
        -o $BUILD_DIR/MixedModule_MyObjcProduct.o
        Sources/MixedModule/MyObjcProduct.m
    )
    xcrun clang ${CFLAGS[@]} ${OBJC_PARAMS[@]}
    xcrun libtool -static -o $BUILD_DIR/MixedModule.a \
        $BUILD_DIR/MixedModule_Swift.o $BUILD_DIR/MixedModule_MyObjcProduct.o
}

function build_swift_dylib() {
    echo ">>> Build Swift dynamic library."
    local PARAMS=(
        -emit-library
        -emit-module
        -module-name SwiftDylib
        -emit-module-path Build/SwiftDylib.swiftmodule
        -o Build/SwiftDylib.dylib
        -Xlinker -install_name
        -Xlinker @rpath/Frameworks/SwiftDylib.dylib
        Sources/SwiftDylib/SwiftDylib.swift
    )
    xcrun swiftc ${SWIFT_FLAGS[@]} ${PARAMS[@]}
}

function build_objc_dylib() {
    echo ">>> Build Objective-C dynamic library."
    local PARAMS=(
        -shared
        -fmodules  # enable modules feature
        -ObjC
        -install_name @rpath/Frameworks/ObjcDylib.dylib
        -o Build/ObjcDylib.dylib
        Sources/ObjcDylib/LLIOSObjcDylib.m
    )
    xcrun clang ${CFLAGS[@]} ${PARAMS[@]}
}

function build_executable() {
    echo ">>> Build executable"
    local PARAMS=(
        -emit-executable
        -I Build
        -I Sources/ObjcDylib
        -I Build/MixedModule # for MixedModule.swiftmodule
        -I Sources/MixedModule # for MixedModule underlying module
        -o "Build/$APP_NAME"
        -Xlinker -rpath -Xlinker @executable_path/
        Build/StaticLib.a
        BUild/MixedModule/MixedModule.a
        Build/SwiftDylib.dylib
        Build/ObjcDylib.dylib
        Sources/AppDelegate.swift Sources/ViewController.swift Sources/SwiftUIView.swift
    )
    xcrun swiftc ${SWIFT_FLAGS[@]} ${PARAMS[@]}
}

function process_info_plist() {
    echo ">>> Process Info.plist."
    PLIST_BUDDY="/usr/libexec/PlistBuddy"
    cp Sources/Info.plist Build/Info.plist
    $PLIST_BUDDY -c "Set :CFBundleDevelopmentRegion en" Build/Info.plist
    $PLIST_BUDDY -c "Set :CFBundleExecutable $APP_NAME" Build/Info.plist
    $PLIST_BUDDY -c "Set :CFBundleName $APP_NAME" Build/Info.plist
    $PLIST_BUDDY -c "Set :CFBundleIdentifier me.qyang.$APP_NAME" Build/Info.plist
    $PLIST_BUDDY -c "Set :CFBundlePackageType APPL" Build/Info.plist
}

function package_app_bundle() {
    echo ">>> Package app bundle."
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
    echo ">>> Sign the app"
    identity=$(security find-identity -v -p codesigning | grep "Apple Development" | head -1 | awk '{print $2}')
    # The embedded dylibs need to be signed separately and before signing the app bundle.
    codesign --force --sign "$identity" "Build/$APP_NAME.app/Frameworks/"*.dylib
    codesign --force --sign "$identity" "Build/$APP_NAME.app" --entitlements Sources/Entitlements.plist
}

prepare
build_swift_static_lib
build_mixed_module
build_swift_dylib
build_objc_dylib
build_executable
process_info_plist
package_app_bundle
if [[ "$OPT_DEVICE" == 1 ]]; then sign_app; fi

echo "Done. Build/$APP_NAME.app"
