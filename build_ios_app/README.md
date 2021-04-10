# Build iOS app without Xcode
This demonstrates how to build static library, dynamic library, exectuable and app bundle without Xcode. To build and launch the app, run `make launch_app`.

## Notes

### -wmo (-whole-module-optimization)
Here is the [Apple's words for WMO]( https://github.com/apple/swift/blob/master/docs/OptimizationTips.rst#whole-module-optimizations-wmo).

WMO can take multiple source files and generate one object.

`swiftc -wmo -o one.o source1.swift source2.swift`

Because of this, `-wmo` doesn't work with some other flags. For exmaple, with `--index-store-path` it will yeild error "index output filenames do not match input source files".

### -incremental
`-incremental` is the opposite to `-wmo`. It generates each `.o` file every `.swift` source files. `-incremental` requires an output file map provided by `-output-file-map`.
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
Parse the input file(s) as libraries, not scripts. Without this, swiftc will include main, which causes duplicate symbols.

### Install Name
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

Reference:
- [Mach-O linking and loading tricks](http://blog.darlinghq.org/2018/07/mach-o-linking-and-loading-tricks.html)
