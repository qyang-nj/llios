# LC_BUILD_VERSION
``` c
struct build_version_command {
    uint32_t	cmd;		/* LC_BUILD_VERSION */
    uint32_t	cmdsize;
    uint32_t	platform;	/* platform */
    uint32_t	minos;		/* X.Y.Z is encoded in nibbles xxxx.yy.zz */
    uint32_t	sdk;		/* X.Y.Z is encoded in nibbles xxxx.yy.zz */
    uint32_t	ntools;		/* number of tool entries following this */
};
```
`LC_BUILD_VERSION` tells what platform and version a binary is built for. It consolidates the old `LC_VERSION_MIN_*` commands and adds version for a few tools (`clang`, `swift`, `ld`).
* `LC_VERSION_MIN_MACOSX`
* `LC_VERSION_MIN_IPHONEOS`
* `LC_VERSION_MIN_WATCHOS`
* `LC_VERSION_MIN_TVOS`

Previously, to differentiate a binary that is built for macOS or iOS simulator (both are x86_64), we need to check if `MIN_MACOSX` or `MIN_IPHONEOS` presents. The M1 chip makes things more complicated. It's impossible to differentiate arm64 iOS simulator build and iOS device build in the old way. With the new `LC_BUILD_VERSION`, combining the architecture in the Mach-O header, we are able to tell which build of a binary through the `platform` field (`IOS`, `IOSSIMULATOR`, `MACOS` and [more](https://github.com/qyang-nj/llios/blob/1f111edc87adbca68c336d3ab501e3ca4a1f2356/apple_open_source/cctools/include/mach-o/loader.h#L1265-L1275)).

Another intersting thing is that the version number is encoded in a 32-bit integer (16 bits for major version, 8 bits for minor version and 8 bits for patch version), so **the maximum of minor version is 15**. This is probably why Apple decided to set macOS version from 10.15 (Catalina) straight to 11 (Big Sur), after being version 10.x for about twenty years.