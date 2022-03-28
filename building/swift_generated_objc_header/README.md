# Swift Generated ObjC Header

By using `-emit-objc-header` on swift compiler, we can generate a header file from swift code, which can be used to import Swift code into Objective-C. It's probably not obvious that the access modifiers, the placement of `@objc` annotation and the inheritance of NSObject all affect how the header file is generated.

We can create [a sample file](FooClass.swift) and enumerate all the variances to observe the different behaviors.

#### public class, public method, NSObject subclass

``` swift
// @interface FooClass : NSObject
// - (void)barFunction;
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end

@objc public class FooClass: NSObject {
    @objc public func barFunction() { }
}
```

``` swift
// @interface FooClass : NSObject
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end

@objc public class FooClass: NSObject {
    public func barFunction() { }
}
```

``` swift
// @interface FooClass : NSObject
// - (void)barFunction;
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end

public class FooClass: NSObject {
    @objc public func barFunction() { }
}
```

⚠️ Even without `@objc` annotation, NSObject subclass still generates its ObjC interface.
``` swift
// @interface FooClass : NSObject
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end

public class FooClass: NSObject {
    public func barFunction() { }
}
```

#### public class, public method, not NSObject subclass

⛔️ Build error: only classes that inherit from NSObject can be declared @objc
``` swift
@objc public class FooClass {
    @objc public func barFunction() { }
}
```

⛔️ Build error: only classes that inherit from NSObject can be declared @objc
``` swift
@objc public class FooClass {
    public func barFunction() { }
}
```

⚠️ Surprisingly this code can be built.
``` swift
// No ObjC interface is generated

public class FooClass {
    @objc public func barFunction() { }
}
```

``` swift
// No ObjC interface is generated

public class FooClass {
    public func barFunction() { }
}
```

#### public class, internal method, NSObject subclass

⚠️ Those are actually the same as public method above, so the access modifiers on the method don't really matter.

``` swift
// @interface FooClass : NSObject
// - (void)barFunction;
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end

@objc public class FooClass: NSObject {
    @objc func barFunction() { }
}
```

``` swift
// @interface FooClass : NSObject
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end

@objc public class FooClass: NSObject {
    func barFunction() { }
}
```

``` swift
// @interface FooClass : NSObject
// - (void)barFunction;
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end

public class FooClass: NSObject {
    @objc func barFunction() { }
}
```

``` swift
// @interface FooClass : NSObject
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end

public class FooClass: NSObject {
    func barFunction() { }
}
```

#### internal class, internal method, NSObject subclass
⚠️ Non-public classes won't generate ObjC interface, even with `@objc` annotation.
``` swift
// No ObjC interface is generated

@objc class FooClass: NSObject {
    @objc func barFunction() { }
}
```

``` swift
// No ObjC interface is generated

@objc class FooClass: NSObject {
    func barFunction() { }
}
```

``` swift
// No ObjC interface is generated

class FooClass: NSObject {
    @objc func barFunction() { }
}
```

``` swift
// No ObjC interface is generated

class FooClass: NSObject {
    func barFunction() { }
}
```
