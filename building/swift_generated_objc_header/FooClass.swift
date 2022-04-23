// xcrun swiftc -c -parse-as-library -emit-objc-header -emit-objc-header-path FooClass-Swift.h FooClass.swift

import Foundation // required to use @objc

// public class, public method, NSObject subclass

@objc public class FooClass00: NSObject {
    @objc public func barFunction() { }
}

@objc public class FooClass01: NSObject {
    public func barFunction() { }
}

public class FooClass02: NSObject {
    @objc public func barFunction() { }
}

public class FooClass03: NSObject {
    public func barFunction() { }
}

// public class, public method, not NSObject subclass

// !! Build Error
// @objc public class FooClass04 {
//     @objc public func barFunction() { }
// }

// !! Build Error
// @objc public class FooClass05 {
//     public func barFunction() { }
// }

public class FooClass06 {
    @objc public func barFunction() { }
}

public class FooClass07 {
    public func barFunction() { }
}

// public class, internal method, NSObject subclass

@objc public class FooClass08: NSObject {
    @objc func barFunction() { }
}

@objc public class FooClass09: NSObject {
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

// private class, NSObject subclass

@objc private class FooClass16: NSObject {
    @objc func barFunction() { }
}

@objc private class FooClass17: NSObject {
    func barFunction() { }
}

private class FooClass18: NSObject {
    @objc func barFunction() { }
}

private class FooClass19: NSObject {
    func barFunction() { }
}
