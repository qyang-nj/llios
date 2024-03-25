public protocol FooProtocol {}
public struct FooStruct : FooProtocol {}

extension FooProtocol where Self == FooStruct {
  public static var foo: String {
    "Hello, world!"
  }
}
