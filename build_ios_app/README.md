# Build iOS App without Xcode
This topic demonstrates how to build and debug an iOS app without an IDE or a build system.
``` bash
cd llios/build_ios_app
./build.sh [--device] [--release]   # build the iOS app
./launch.sh  # launch the app in the simulator or device
./debug.sh   # launch the app and attach lldb to it
```

The demonstration includes
* building static libraries
* building dynamic libraries
* building an executable
* packaging an app bundle
* code signing
* attaching lldb to the app
* launching an app in a simulator or a device

## swiftc

### -c (-emit-object)
Generate a object file.

### -emit-library
Directly Generate a dynamic library.

### -wmo (-whole-module-optimization)
Here is the [Apple's words for WMO]( https://github.com/apple/swift/blob/master/docs/OptimizationTips.rst#whole-module-optimizations-wmo).

WMO can take multiple source files and generate one object. Because of this, `-wmo` doesn't work with some other flags. For example, with `--index-store-path` it will yield error "index output filenames do not match input source files".

```
swiftc -wmo -o one.o source1.swift source2.swift
```

### -incremental
`-incremental` is the opposite to `-wmo`. It generates each a `.o` file for every `.swift` source file. `-incremental` requires an output file map provided by `-output-file-map`.
```
{
  "": {
    "swift-dependencies": "Build/StatiLib-master.swiftdeps"
  },
  "Sources/StaticLib/foo.swift": {
    "dependencies": "Build/foo.d",
    "diagnostics": "Build/foo.dia",
    "llvm-bc": "Build/foo.bc",
    "object": "Build/foo.o",
    "swift-dependencies": "Build/foo.swiftdeps",
    "swiftmodule": "Build/foo~partial.swiftmodule"
  },
  "Sources/StaticLib/bar.swift": {
    ...
  }
}
```

The swift driver with `-incremental` calls swift frontend with `-primary-file` for each source file and at the end it merges the module. (`-primary-file` is a frontend flag, which cannot be passed directly swift driver.)
```
> swiftc -incremental -output-file-map output_file_map.json Sources/StaticLib/bar.swift Sources/StaticLib/foo.swift -###

swiftc -frontend -c -primary-file Sources/StaticLib/bar.swift Sources/StaticLib/foo.swift -emit-module-path Build/bar.swiftmodule -parse-as-library -module-name StaticLib -o Build/bar.o
swiftc -frontend -c Sources/StaticLib/bar.swift -primary-file Sources/StaticLib/foo.swift -emit-module-path Build/foo.swiftmodule -parse-as-library -module-name StaticLib -o Build/foo.o
swiftc -frontend -merge-modules -emit-module Build/bar.swiftmodule Build/foo.swiftmodule -parse-as-library -module-name StaticLib -o Build/StaticLib.swiftmodule
```

### -parse-as-library
Parse the input file(s) as libraries, not scripts. Without this, swiftc generates the `main` method for each compilation unit.

## ld
### -install_name
Install name, which is unique on Darwin platform, provides a dylib search path. It's set by passing `-install_name` to the linker (ld) when building a dylib (not an executable). In other words, install name is an attribute of a dylib itself.
When `-install_name path/to/DynamicLib.dylib` is given, it ends up to a load command (`LC_ID_DYLIB`) in the dylib's macho-o load section.
```
Load command 3
          cmd LC_ID_DYLIB
      cmdsize 56
         name path/to/DynamicLib.dylib (offset 24)
   time stamp 1 Wed Dec 31 16:00:01 1969
      current version 0.0.0
compatibility version 0.0.0
```

When linking againt a dylib, a different load command (`LC_LOAD_DYLIB`) was added to the executable.
```
Load command 12
          cmd LC_LOAD_DYLIB
      cmdsize 56
         name path/to/DynamicLib.dylib (offset 24)
   time stamp 2 Wed Dec 31 16:00:02 1969
      current version 0.0.0
compatibility version 0.0.0
```

The install name can start with `@executable_path`, `@loader_path`, or `@rpath`, to provide a relative search path.

Note: `-install_name` is a linker flag. If calling from swift driver, we need to use `-Xlinker`.

##### Learn more
- [Mach-O linking and loading tricks](http://blog.darlinghq.org/2018/07/mach-o-linking-and-loading-tricks.html)

## libtool
Unlike `-emit-library`, I didn't find a flag for building a static library, so we need to use `libtool -static` to create a static library.

## simctl
quick reference:
```
xcrun simctl shutdown all
xcrun simctl boot "iPhone 12"
xcrun simctl install booted "Build/SampleApp.app"
xcrun simctl launch booted "com.qyang-nj.SampleApp"
```
