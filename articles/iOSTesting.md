# Behind the scenes: iOS Testing
This article uncovers what happens behind the scenes when building and running iOS tests. It explains what "xctest" can mean and three kinds of comment tests. To simplify the complexities, here we just talk about tests on iOS simulator. Other platforms are very similar.

## XCTest
The xctest is everywhere in the realm of iOS testing, and it can mean different things. It's a test framework, a test bundle, or a test runner.

### XCTest is a framework
If you have written any iOS tests, it’s impossible that you don’t know `XCTest.framework`. It’s a dynamic framework which provides some basic classes for writing test cases, e.g. `XCTestCase`. It's located in `PLATFORM_DIR/Developer/Library/Frameworks`[^1] and is the only framework in that directory. Other common frameworks, like `Foundation.framework`, are in `PLATFORM_DIR/Developer/SDKs/iPhoneSimulator.sdk/System/Library/Frameworks`. Because of the unique location, if your application (not test target) accidentally linked with `XCTest.framework` (which is a pretty common mistake), you will notice a runtime crash immediately

`XCTest.framework` links with other test related frameworks, `XCTestCore.framework`, `XCUnit.framework` and `XCUIAutomation.framework`. They are private frameworks, and we normally don’t need to deal with them directly.

[^1]: `PLATFORM_DIR=/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform`

### xctest is a test bundle
#### bundle is a directory
If you have paid attention to the build product of a test target, you know its name is like `MyLibrary.xctest`. Same as .app and .framework, .xctest is also a bundle, which is a specially purposed directory. The .xctest bundle contains a binary (which is the code) and resources.

#### bundle is a Mach-O type
Other than a directory, “bundle” is also a Mach-O type. Interestingly, it’s the case for xctest as well. By examining the binary inside the xctest bundle with `otool` , we will see the filetype is “BUNDLE”. “EXECUTE”, “DYLIB” and “BUNDLE” are all Mach-O types.

```
$ otool -vh MyTest.xctest/MyTest
      magic  cputype cpusubtype  caps    filetype ncmds sizeofcmds      flags
MH_MAGIC_64   X86_64        ALL  0x00      BUNDLE    36       4096   NOUNDEFS DYLDLINK TWOLEVEL
```

* “EXECUTE” means the binary is an executable, it can be launched directly.
*  “DYLIB” indicates the binary is a dynamic library which can be loaded by other Mach-O types. A dynamic library provides some features to the loader.
* Similar to  “DYLIB”, “BUNDLE” cannot be executed directly. Different than “DYLIB”, it doesn’t provide a feature. Instead, it needs some features from the loader to run. The loader is known as the host. “BUNDLE” are usually used as plugins.

Test cases cannot be run by themself. They need to be loaded into the test runner’s process. In that sense, the test cases are the plugins of the test runner.

### xctest is a test runner
Speaking of test runner, `xctest` is also a test runner. It’s shipped within Xcode,  `/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform/Developer/Library/Xcode/Agents/xctest`. It’s needed to run logic tests. We will talk more about this later.

## Types of tests
There are three common types of tests, logic tests, hosted tests, and UI tests.
### Logic Tests
Logic test is also known as unit test, the most common type. They are loaded into previously mentioned xctest test runner and run. They don’t require an app. They don’t even need to launch a simulator.
```
PLATFORM_DIR="$(xcode-select -p)/Platforms/iPhoneSimulator.platform" \
xcrun simctl spawn --arch=x86_64 --standalone "iPhone 14" "$PLATFORM_DIR/Developer/Library/Xcode/Agents/xctest" /path/to/MyTest.xctest
```
[Here](../testing/logic_test/build_and_test.sh) is a full sample to build and run a logic test without Xcode.

### Hosted Tests
These tests require an app to run, known as hosted app. They usually depend on some features provided by the hosted app, like the size of a view. The runner is the host app instead of `xctest` and the test bundle is loaded into the host app.

How does the host app know it should load and run the test? It’s through the injected `libXCTestBundleInject.dylib`. Check [Dynamic Interposing](../dynamic_linking/dynamic_interposing.md) for more details.
```
DYLD_INSERT_LIBRARIES=/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform/Developer/usr/lib/libXCTestBundleInject.dylib
```

### UI Tests
UI tests let us test our app like the end user. It can mimic user behaviors, like tapping a button. In this case, our app is a standalone application, called target app. There is a separate runner app that loads the test bundle to test against our application. I'll add more details for this one later.
