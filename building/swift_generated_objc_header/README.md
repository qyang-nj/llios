# Swift Generated ObjC Header

The header file generated from Swift code, often seen as `*-Swift.h`, is crucial for Swift Objective-C interoperability. We use `-emit-objc-header` to instruct swift compiler to generate the header, which can be used to import Swift code into Objective-C. It's probably not obvious that the access modifiers, the placement of `@objc` annotation and the inheritance of NSObject all affect how the header file is generated.

I created [a sample file](FooClass.swift) and enumerated some variances to observe the different behaviors. Many compiler flags can affect how the header is generated, the ones I used are below. I will talk about some other flags at the end of this article.
```
xcrun swiftc -emit-object -parse-as-library -emit-objc-header -emit-objc-header-path FooClass-Swift.h FooClass.swift
```

### public class, public method, NSObject subclass

``` swift
// @interface FooClass01 : NSObject
// - (void)barFunction;
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end

@objc public class FooClass00: NSObject {
    @objc public func barFunction() { }
}
```

``` swift
// @interface FooClass01 : NSObject
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end

@objc public class FooClass01: NSObject {
    public func barFunction() { }
}
```

``` swift
// @interface FooClass02 : NSObject
// - (void)barFunction;
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end

public class FooClass02: NSObject {
    @objc public func barFunction() { }
}
```

⚠️ Even without `@objc` annotation, the NSObject subclass still generates its ObjC interface.
``` swift
// @interface FooClass03 : NSObject
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end

public class FooClass03: NSObject {
    public func barFunction() { }
}
```

### public class, public method, not NSObject subclass

⛔️ Build error: "only classes that inherit from NSObject can be declared @objc"
``` swift
@objc public class FooClass04 {
    @objc public func barFunction() { }
}
```

⛔️ Build error: "only classes that inherit from NSObject can be declared @objc"
``` swift
@objc public class FooClass05 {
    public func barFunction() { }
}
```

⚠️ Surprisingly this code can be built.
``` swift
// No ObjC interface is generated

public class FooClass06 {
    @objc public func barFunction() { }
}
```

``` swift
// No ObjC interface is generated

public class FooClass07 {
    public func barFunction() { }
}
```

### public class, internal method, NSObject subclass

⚠️ Internal methods won't generate ObjC interfaces even they are annotated by `@objc`.

``` swift
// @interface FooClass : NSObject
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end

@objc public class FooClass08: NSObject {
    @objc func barFunction() { }
}
```

``` swift
// @interface FooClass : NSObject
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end

@objc public class FooClass09: NSObject {
    func barFunction() { }
}
```

``` swift
// @interface FooClass : NSObject
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end

public class FooClass10: NSObject {
    @objc func barFunction() { }
}
```

``` swift
// @interface FooClass : NSObject
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end

public class FooClass11: NSObject {
    func barFunction() { }
}
```

### internal class, internal method, NSObject subclass
⚠️ Internal classes won't generate ObjC interfaces at all.

``` swift
// No ObjC interface is generated

@objc class FooClass12: NSObject {
    @objc func barFunction() { }
}
```

``` swift
// No ObjC interface is generated

@objc class FooClass13: NSObject {
    func barFunction() { }
}
```

``` swift
// No ObjC interface is generated

class FooClass14: NSObject {
    @objc func barFunction() { }
}
```

``` swift
// No ObjC interface is generated

class FooClass15: NSObject {
    func barFunction() { }
}
```

### private class, NSObject subclass
⚠️ Since private classes are not visible outside of the file, no ObjC interface is generated.

## Compiler Flags
As mentioned at the beginning, many compiler flags can affect the generation behavior, especially about internal classes or methods.
* **`-parse-as-library`** is necessary to build a library. With it, no internal class or method will generate the ObjC interface.
* Even with `-parse-as-library`, **`-import-objc-header`** can make internal classes and methods generate the ObjC interfaces. You don't need to provide a real bridging header. `-import-objc-header /dev/null` can do the trick. Please note that this flag cannot be used with `-import-underlying-module`.
* Weirdly, **`-application-extension`** can also make internal class and method generate the ObjC interface. I don't know if this is an intention or a bug.
