# XCTest

> [!WARNING]
> This note is deprecated. See [Behind the scenes: iOS Testing](../articles/iOSTesting.md)

The word "xctest" is extensively used in the realm of iOS testing, the test bundle (`.xctest`), the test framework (`XCTest.framework`), and the test runner (`xctest`). This article along with a sample will dive into some details about iOS testing.

## Build and Run
The script `build_and_run.sh` in this directory will build a simple xctest bundle and run it on the simulator directly without Xcode or other full-fledged test runners.
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

The test bundle structure is as simple as `Test.xctest/Test`. Even `Info.plist` is not needed to run the test.

## `XCTest.framework`
Almost all tests import `XCTest` and depend on `XCTest.framework`. For the obvious reason, it needs to be present at compiling and linking time. **The really interesting thing is how it's loaded at runtime.**

From `otool -L` we can see that the test indeed depends on `XCTest.framework`.
```
$ otool -L Test.xctest/Test
...
@rpath/XCTest.framework/XCTest (compatibility version 1.0.0, current version 18141.0.0)
...
```

However, we didn't package the framework into our test bundle, and test file doesn't have any `LC_RPATH` at all. How on earth can `dyld` loads `XCTest.framework` at runtime?

One guess is through environment variable like `DYLD_FALLBACK_FRAMEWORK_PATH`. Luckily we can `export SIMCTL_CHILD_DYLD_PRINT_ENV=1` to see what the environment variables are. *(Adding `SIMCTL_CHILD_` prefix is the way to pass environment variable from host machine to simulator.)*

As we can see, `DYLD_FALLBACK_FRAMEWORK_PATH` is indeed set, but those paths don't lead to `XCTest.framework`.
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

Well, it seems that the test runner loads `XCTest.framework` already, so the test doesn't need to load it again. If this is true, tests and test runner must share the same memory space, but how does this work? (Keep reading!)

## `libXCTestSwiftSupport.dylib`
One of the changes introduced by Xcode 12.5 is requiring `libXCTestSwiftSupport.dylib`.

> Xcode no longer includes XCTest’s legacy Swift overlay library (libswiftXCTest.dylib). Use the library’s replacement, libXCTestSwiftSupport.dylib, instead.

Similar to `XCTest.framework`, our test binary depends on it through @rpath, and we didn't package it into test bundle either.
```
@rpath/libXCTestSwiftSupport.dylib (compatibility version 1.0.0, current version 1.0.0)
```
Unlike `XCTest.framework`, the test runner doesn't depend on `libXCTestSwiftSupport.dylib` (no show at `otool -L xctest`). Still, our test can run successfully and we can see from the log that `libXCTestSwiftSupport.dylib` is loaded.
```
dyld: loaded: <3585C82D-EBB7-3D65-8FA3-00BBAF113C52> /Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform/Developer/Library/Xcode/Agents/../../../usr/lib/libXCTestSwiftSupport.dylib
```
This is even more mysterious, How does this happen? (Keep reading!)


## `MH_BUNDLE`
One fact, usually being overlooked, is that the type of the test binary is neither executable (`MH_EXECUTE`) nor dylib (`MH_DYLIB`). **It actually is a bundle (`MH_BUNDLE`)**. Distinct from the directory structure, this bundle is a file type defined in the Mach-O header. When we build the test bundle, we passed `-bundle` to the static linker.

```
$ otool -vh Test.xctest/Test
...  filetype  ...
...    BUNDLE  ...
```

Bundles provide the Mach-O mechanism for loading extension (or plugin-in) code into an application at runtime. More details about `MH_BUNDLE` can be found through Google. The important thing we need to know here is that bundle is loaded by the loader (usually the executable) through `dlopen`.

I spent some time digging into the `dyld` source code. It's an eureka moment when I saw this snippet of code ([link](https://github.com/opensource-apple/dyld/blob/3f928f32597888c5eac6003b9199d972d49857b5/src/dyldAPIs.cpp#L1428-L1432)).

``` c
// for dlopen, use rpath from caller image and from main executable
if ( callerImage != NULL )
    callerImage->getRPaths(dyld::gLinkContext, rpathsFromCallerImage);
if ( callerImage != dyld::mainExecutable() )
    dyld::mainExecutable()->getRPaths(dyld::gLinkContext, rpathsFromCallerImage);
```

Just reading the comment, we know that when a bundle is opened, not only its own `rpath` but also the `rpath` of its loader and main executable are appended to the list. In our case, the `rpath` of the test runner (executable) are also used for searching. They include `@executable_path/../../Frameworks/` and `@executable_path/../../../usr/lib`, where the `XCTest.framework` and `libXCTestSwiftSupport.dylib` are. Okay, I believe the mystery is solved.

## Host App
Some tests require a host app. Therefore the executable is no longer `xctest`. Instead, it's the host app, which is unlikely to have the right `rpath` for test libraries. In this case, environment variable `DYLD_FALLBACK_FRAMEWORK_PATH` and `DYLD_FALLBACK_LIBRARY_PATH` come in handy. We can set them through `SIMCTL_CHILD_` prefix or the test runner's config file, like `.xctestrun`.

```
<key>TestingEnvironmentVariables</key>
<dict>
    <key>DYLD_FALLBACK_LIBRARY_PATH</key>
    <string>__PLATFORMS__/iPhoneSimulator.platform/Developer/usr/lib</string>
</dict>
```
