
import XCTest

class Tests: XCTestCase {
  func testFailed() throws {
    XCTAssert(false)
  }

  func testWindow() throws {
    // Accessing the host app
    print(UIApplication.shared.windows)
    XCTAssert(true)
  }
}
