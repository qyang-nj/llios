# XCTest

The word "xctest" is used everywhere in the world of iOS test, the test bundle (`.xctest`), the test framework (`XCTest.framework`), and the test runner (`Xcode/Agents/xctest`). This sample will dive into some details about iOS test.

## Build and Run
The script `build_and_run.sh` in this directory will build a simple test bundle and run it on the simulator directly without Xcode or other full-fledged test runners.
```
Test Suite 'All tests' started at 2021-05-01 11:05:52.723
Test Suite 'Test.xctest' started at 2021-05-01 11:05:52.724
Test Suite 'Tests' started at 2021-05-01 11:05:52.724
Test Case '-[Test.Tests testExample]' started.
Test Case '-[Test.Tests testExample]' passed (0.002 seconds).
Test Suite 'Tests' passed at 2021-05-01 11:05:52.727.
         Executed 1 test, with 0 failures (0 unexpected) in 0.002 (0.002) seconds
Test Suite 'Test.xctest' passed at 2021-05-01 11:05:52.727.
         Executed 1 test, with 0 failures (0 unexpected) in 0.002 (0.003) seconds
Test Suite 'All tests' passed at 2021-05-01 11:05:52.727.
         Executed 1 test, with 0 failures (0 unexpected) in 0.002 (0.004) seconds
```

The test bundle structure is as simple as `Test.xctest/Test`. Even `Info.plist` is not needed in the bundle to run the test.

## `MH_BUNDLE`
The type of the test binary is neither executable (`MH_EXECUTE`) nor dylib (`MH_DYLIB`). It actually is a bundle (`MH_BUNDLE`). The type is defined in the Mach-O header. To build it, `-bundle`

```
$ otool -vh Test.xctest/Test
      magic  cputype cpusubtype  caps    filetype ncmds sizeofcmds      flags
MH_MAGIC_64   X86_64        ALL  0x00      BUNDLE    27       3176   NOUNDEFS DYLDLINK TWOLEVEL
```

## `XCTest.framework`
`XCTest.framework` is the key for iOS testing. Almost all tests import `XCTest` and depend on this framework. For the obvious reason, `XCTest.framework` needs to be present at compiling and linking time. **The interesting thing is how it's loaded at runtime.**

From `otool -L` we can see that the test indeed depends on `XCTest.framework`.
```
$ otool -L Test.xctest/Test
...
@rpath/XCTest.framework/XCTest (compatibility version 1.0.0, current version 18141.0.0)
...
```

However, we didn't package the framework into our test bundle, and test file doesn't have any `LC_RPATH` at all. So, how on earth can `dyld` find the `XCTest.framework` at runtime?

One guess is through environment variable like `DYLD_FALLBACK_FRAMEWORK_PATH`. Luckily we can `export SIMCTL_CHILD_DYLD_PRINT_ENV=1` to see what the environment variables are. *(Adding `SIMCTL_CHILD_` prefix is the way to pass environment variable from host machine to simulator.)*

As we can see, `DYLD_FALLBACK_FRAMEWORK_PATH` is indeed set, but those paths don't contain `XCTest.framework`.
```
DYLD_FALLBACK_FRAMEWORK_PATH=/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Library/Developer/CoreSimulator/Profiles/Runtimes/iOS.simruntime/Contents/Resources/RuntimeRoot/System/Library/Frameworks
```

We can also `export SIMCTL_CHILD_DYLD_PRINT_LIBRARIES=1` to see what libraries are loaded.
```
dyld: loaded: <B22BCD8B-784D-365D-BCCB-3441E205D133> /Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform/Developer/Library/Xcode/Agents/../../Frameworks/XCTest.framework/XCTest
```

It looks like the `XCTest.framework` is loaded from a relative path of the test runner. Then double check the runner's dependencies.
```
$ otool -L "$PLATFORM_DIR/Developer/Library/Xcode/Agents/xctest"
...
@rpath/XCTest.framework/XCTest (compatibility version 1.0.0, current version 18141.0.0)
...

$ otool -l "$PLATFORM_DIR/Developer/Library/Xcode/Agents/xctest" | grep -A2 LC_RPATH
    cmd LC_RPATH
cmdsize 48
   path @executable_path/../../Frameworks/ (offset 12)
```

Yes, it's confirmed that the runner will load `XCTest.framework`. As we described in the above section, the test is actually a `MH_BUNDLE`, which will be loaded into the executable's memory space. Thus xctest can also access the APIs provided by `XCTest.framework`. That's why we don't need package `XCTest.framework` into our own test bundle.
