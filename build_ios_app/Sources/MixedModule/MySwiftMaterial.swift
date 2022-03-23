import Foundation

// MySwiftMaterial is depended by an Objc class MyObjcProduct
public class MySwiftMaterial: NSObject {
    @objc public func type() -> String {
        return "SwiftMaterial"
    }
}


// The following classes demonstrate how the access modifiers,
// inheritance and @objc annotation affect Objc interface generation.

//
// ## a public class subclasses NSObject, @objc annotation on both class and function ##
//
// @interface FooClass0 : NSObject
// - (void)barFunction;
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end
//
@objc public class FooClass0: NSObject {
    @objc public func barFunction() { }
}

//
// ## a public class subclasses NSObject, @objc annotation on class
//
// @interface FooClass1 : NSObject
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end
//
@objc public class FooClass1: NSObject {
    public func barFunction() { }
}

//
// ## a public class subclasses NSObject, @objc annotation on function
//
// @interface FooClass2 : NSObject
// - (void)barFunction;
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end
//
public class FooClass2: NSObject {
    @objc public func barFunction() { }
}

//
// ## a public class subclasses NSObject, @objc annotation on an internal function
// ## An internal function doesn't generate an interface, event though it's annotated.
//
// @interface FooClass3 : NSObject
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end
//
public class FooClass3: NSObject {
    @objc func barFunction() { }
}

//
// ## a public class subclasses NSObject, no @objc annotation
// ## Even without @objc annotation, a class interface and its init method are still generated.
//
// @interface FooClass4 : NSObject
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end
//
public class FooClass4: NSObject {
    public func barFunction() { }
}

//
// ## an internal class subclasses NSObject, @objc annotation on method
//
// No objc interface is generated
//
class FooClass5: NSObject {
    @objc func barFunction() { }
}

//
// ## an internal class doesn't subclass NSObject, no @objc annotation
//
// No objc interface is generated
//
public class FooClass6 {
    public func barFunction() { }
}

//
// ## an internal class doesn't subclass NSObject, @objc annotation on function
// ## Surprisingly this code can build.
//
// No objc interface is generated
//
class FooClass7 {
    @objc func barFunction() { }
}
