// xcrun swiftc -c -emit-objc-header -emit-objc-header-path FooClass-Swift.h FooClass.swift

import Foundation // required to use @objc

// public class, public method, NSObject subclass

@objc public class FooClass0: NSObject {
    @objc public func barFunction() { }
}

@objc public class FooClass1: NSObject {
    public func barFunction() { }
}

public class FooClass2: NSObject {
    @objc public func barFunction() { }
}

public class FooClass3: NSObject {
    public func barFunction() { }
}

// public class, public method, not NSObject subclass

// !! Build Error
// @objc public class FooClass4 {
//     @objc public func barFunction() { }
// }

// !! Build Error
// @objc public class FooClass5 {
//     public func barFunction() { }
// }

public class FooClass6 {
    @objc public func barFunction() { }
}

public class FooClass7 {
    public func barFunction() { }
}

// public class, internal method, NSObject subclass

@objc public class FooClass8: NSObject {
    @objc func barFunction() { }
}

@objc public class FooClass9: NSObject {
    func barFunction() { }
}

public class FooClass10: NSObject {
    @objc func barFunction() { }
}

public class FooClass11: NSObject {
    func barFunction() { }
}

// internal class, internal method, NSObject subclass

@objc class FooClass12: NSObject {
    @objc func barFunction() { }
}

@objc class FooClass13: NSObject {
    func barFunction() { }
}

class FooClass14: NSObject {
    @objc func barFunction() { }
}

class FooClass15: NSObject {
    func barFunction() { }
}
