# Xcode.app Directory Structure

Xcode.app is huge! The installation size is more than 12GB as of Xcode 14.0, with millions of files in it. Most of time we can treat it as a black box, but sometimes we need to know where the important files are, e.g. system libraries/frameworks, especially when providing the search paths to compiler.

Here is an extremely simplified view to see where the common files are inside Xcode.app.[^1]

## Platforms
Platforms have the dynamic libraries/frameworks that are used at runtime.

```
$DEVELOPER_DIR/Platforms
├── iPhoneSimulator.platform
│  └── Developer
│      ├── usr/lib
│      │  ├── libXCTestBundleInject.dylib
│      │  ├── libXCTestSwiftSupport.dylib
│      │  └── XCTest.swiftmodule
│      ├── Library/Frameworks
│      │  └── XCTest.framework
│      └── SDKs/iPhoneSimulator.sdk
│         └── System/Library/Frameworks
│         │  ├── UIKit.framework
│         │  ├── Foundation.framework
│         │  └── ... (more system frameworks)
│         ├── /usr/lib
│         │  ├── libc++.tbd
│         │  ├── libobjc.tb
│         │  └── ... (more lib*.tb)
│         └── /usr/lib/swift
│            ├── Swift.swiftmodule
│            ├── libswiftCore.tbd
│            └── ... (more libswift*.tbd and *.swiftmodule)
├── iPhoneOS.platform
├── MacOSX.platform
└── ... (more platforms)
```

Compiler Flags
* SDK path: `-sdk $DEVELOPER_DIR/Platforms/iPhoneSimulator.platform/Developer/SDKs/iPhoneSimulator.sdk`
* System framework search path `-F $DEVELOPER_DIR/Platforms/iPhoneSimulator.platform/Developer/SDKs/iPhoneSimulator.sdk/System/Library/Frameworks`
* `XCTest.framework` search path `-F $DEVELOPER_DIR/Platforms/iPhoneSimulator.platform/Developer/Library/Frameworks`
* XCTest swift support search path `-I $DEVELOPER_DIR/Platforms/iPhoneSimulator.platform/Developer/usr/lib`

Notes
* `XCTest.framework` is at a special location. Unlike other frameworks, `XCTest.framework` shouldn't be linked in production.
* Most system dylibs are actually `.tbd` instead of real binaries.

## Toolchains
Toolchains contain the build tools, e.g. compiler, linker, and support static libraries at compile time.

```
$DEVELOPER_DIR/Toolchains/XcodeDefault.xctoolchain
├── usr/bin
│  ├── clang
|  ├── cc -> clang (symlink)
|  ├── c++ -> clang (symlink)
│  ├── swift-frontend
│  ├── swift -> swift-frontend (symlink)
│  ├── swiftc -> swift-frontend (symlink)
│  ├── swift-drivier
│  └── ...
├── usr/lib
│  └── ...
├── usr/lib/swift
│  ├── iphonesimulator
│  │  ├── libswiftCompatibility50.a
│  │  ├── libswiftCompatibility51.a
│  │  ├── libswiftCompatibilityConcurrency.a
│  │  ├── libswiftCompatibilityDynamicReplacements.a
│  │  └── ...
│  ├── iphoneos
│  └── ...
└── Developer/Platforms (only 3 platforms relevant to development)
   ├── iPhoneSimulator.platform
   ├── MacOSX.platform
   └── AppleTVSimulator.platform
```

Notes
* `$DEVELOPER_DIR/Toolchains/XcodeDefault.xctoolchain/usr/bin` is where `xcrun` to locate tools.
* `swift` and `swiftc` are just symlinks to `swift-frontend`.
* `clang++`, `c++`, `cc` are symlinks to `clang`.

[^1]: `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer/`
