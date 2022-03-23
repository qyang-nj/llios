import Foundation

// MySwiftMaterial is depended by an Objc class MyObjcProduct
public class MySwiftMaterial: NSObject {
    @objc public func type() -> String {
        return "SwiftMaterial"
    }
}


// -- The following classes demonstrate how the objc interfaces are generated --

//
// ## a public class subclasses NSObject, @objc annotation on both class and function ##
//
// @interface FooClass0 : NSObject
// - (NSString * _Nonnull)barFunction SWIFT_WARN_UNUSED_RESULT;
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end
//
@objc public class FooClass0: NSObject {
    @objc public func barFunction() -> String {
        return "barFunction"
    }
}

//
// ## a public class subclasses NSObject, @objc annotation on class
//
// @interface FooClass1 : NSObject
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end
//
@objc public class FooClass1: NSObject {
    public func barFunction() -> String {
        return "barFunction"
    }
}

//
// ## a public class subclasses NSObject, @objc annotation on function
//
// @interface FooClass2 : NSObject
// - (NSString * _Nonnull)barFunction SWIFT_WARN_UNUSED_RESULT;
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end
//
public class FooClass2: NSObject {
    @objc public func barFunction() -> String {
        return "barFunction"
    }
}

//
// ## a public class subclasses NSObject, no @objc annotation
// ## Even without @objc annotation, a class interface and its init method are still generated.
//
// @interface FooClass3 : NSObject
// - (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
// @end
//
public class FooClass3: NSObject {
    public func barFunction() -> String {
        return "barFunction"
    }
}

//
// ## an internal class subclasses NSObject, @objc annotation on method
//
// No objc interface is generated
//
class FooClass4: NSObject {
    @objc func barFunction() -> String {
        return "barFunction"
    }
}

//
// ## an internal class doesn't subclass NSObject, no @objc annotation
//
// No objc interface is generated
//
public class FooClass5 {
    public func barFunction() -> String {
        return "barFunction"
    }
}

//
// ## an internal class doesn't subclass NSObject, @objc annotation on function
//
// No objc interface is generated
//
class FooClass6 {
    @objc func barFunction() -> String {
        return "barFunction"
    }
}
