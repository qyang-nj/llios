# Behind the scenes: iOS Testing
This article uncovers what happens behind the scenes when building and running iOS tests. It explains what "xctest" can mean and three kinds of comment tests. To simplify the complexities, here we just talk about tests on iOS simulator. Other platforms are very similar.

## XCTest
The xctest is everywhere in the realm of iOS testing, and it can mean different things. It's a test framework, a test bundle, or a test runner.

### XCTest is a framework
If you have written any iOS tests, it’s impossible that you don’t know `XCTest.framework`. It’s a dynamic framework which provides some basic classes for writing test cases, e.g. `XCTestCase`. It's located in `PLATFORM_DIR/Developer/Library/Frameworks`[^1] and is the only framework in that directory. Other common frameworks, like `Foundation.framework`, are in `PLATFORM_DIR/Developer/SDKs/iPhoneSimulator.sdk/System/Library/Frameworks`. Because of the unique location, if your application (not test target) accidentally linked with `XCTest.framework` (which is a pretty common mistake), you will notice a runtime crash immediately

`XCTest.framework` links with other test related frameworks, `XCTestCore.framework`, `XCUnit.framework` and `XCUIAutomation.framework`. They are private frameworks, and we normally don’t need to deal with them directly.

[^1]: `PLATFORM_DIR=/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform`

### xctest is a test bundle
The build product of a test target is something like `MyLibrary.xctest`. Same as .app and .framework, .xctest is also a bundle, which is a specially purposed directory. Inside the directory, there are a binary (which is the executable code), a Info.plist file, and some resources. For more details of bundle, see [Bundles and Packages](https://nshipster.com/bundles-and-packages/#bundles).

Other than a directory, “bundle” is also a Mach-O type. Interestingly, it’s the case for xctest as well. By examining the binary inside the xctest bundle with `otool` , we will see the filetype is BUNDLE.
```
$ otool -vh MyTest.xctest/MyTest
      magic  cputype cpusubtype  caps    filetype ncmds sizeofcmds      flags
MH_MAGIC_64   X86_64        ALL  0x00      BUNDLE    36       4096   NOUNDEFS DYLDLINK TWOLEVEL
```
EXECUTE, DYLIB, and BUNDLE are all Mach-O types.
* EXECUTE means the binary is an executable, it can be launched directly by `dyld`.
* DYLIB indicates the binary is a dynamic library which can be loaded by other Mach-O types. A dynamic library provides some features needed by the loader.
* Similar to DYLIB, BUNDLE cannot be executed independently. Different than DYLIB, it doesn’t provide a feature. Instead, it needs some features from the loader to run. The loader is known as the host. BUNDLEs are usually used as plugins.

Test cases cannot be run by themselves. They need to a test runner to run. In that sense, the test cases are the plugins of the test runner.

### xctest is a test runner
Speaking of test runner, `xctest` is also a test runner. It’s shipped within Xcode,  `PLATFORM_DIR/Developer/Library/Xcode/Agents/xctest`. It’s needed to run logic tests. We will talk more about this next.

## Types of tests
There are three common types of tests, logic tests, hosted tests, and UI tests.
### Logic Tests
Logic test is also known as unit test, the most common type. They are loaded into the aforementioned xctest test runner and run. They are self sufficient and don’t require an app. They don’t even need to launch a simulator.
```
xcrun simctl spawn SIMULATOR PLATFORM_DIR/Developer/Library/Xcode/Agents/xctest /path/to/MyTest.xctest
```
[Here](../testing/logic_test/build_and_test.sh) is a full sample to build and run a logic test without Xcode.

### Hosted Tests
Some tests require an app to run, known as hosted app. They usually depend on some features provided by the host app, like UIApplication. They are called hosted tests or app tests. The runner is the host app instead of `xctest` and the test bundle is loaded into the host app.

How does the host app know it should load and run the test? It’s through the injected `libXCTestBundleInject.dylib`. Check [Dynamic Interposing](../dynamic_linking/dynamic_interposing.md) for more details.
```
export SIMCTL_CHILD_DYLD_INSERT_LIBRARIES=$PLATFORM_DIR/Developer/usr/lib/libXCTestBundleInject.dylib
xcrun simctl launch --console-pty "iPhone 14 Pro" me.qyang.HostApp
```

[Here](../testing/hosted_test/build_and_test.sh) is a full sample to build and run a hosted test without Xcode.

### UI Tests
UI tests let us test our app like the end user. It can mimic the user behaviors, e.g. tapping a button. In this case, our app is a standalone application, called target app. There is a separate runner app that loads the test bundle to test against our application. I'll add more details for this one later.

## xcodebuild test-without-building
A more common scenario is to use `xcodebuild test-without-build` to run `.xctest`. [Here](../testing/xcodebuild/run_test.py) is a script to show how this works.
