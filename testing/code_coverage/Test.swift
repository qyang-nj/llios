
import XCTest

class Tests: XCTestCase {
  func testExample() throws {
    let foo = Foo()
    XCTAssert(foo.func1(a: 0) == 0)
    XCTAssert(foo.func1(a: 2) == 1)
  }

  // func testForceCrash() {
  //   assertionFailure("Crash")
  // }
}
