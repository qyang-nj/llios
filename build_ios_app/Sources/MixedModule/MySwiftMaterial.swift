import Foundation

// MySwiftMaterial is depended by an Objc class MyObjcProduct
public class MySwiftMaterial: NSObject {
    @objc public func type() -> String {
        return "SwiftMaterial"
    }
}
