
import XCTest
import Testing

class XCTestDemo: XCTestCase {

    func testMathOperations() {
        let a = 5
        let b = 3

        XCTAssertEqual(a + b, 8)
        XCTAssertEqual(a * b, 15)
        XCTAssertEqual(a - b, 2)
    }
}

struct SwiftTestingDemo {

  @Test func verifyMathOperations() {
      let a = 5
      let b = 3

      #expect(a + b == 8)
      #expect(a * b == 15)
      #expect(a - b == 2)
  }

}
