
import XCTest
import Testing

class XCTestDemo: XCTestCase {
  func testFailed() throws {
    XCTAssert(false)
  }

  func testWindow() throws {
    // Accessing the host app
    print(UIApplication.shared.windows)
    XCTAssert(true)
  }
}

@Suite struct SwiftTestingDemo {
    @Test func failed() {
        #expect(Bool(false))
    }

    @Test func window() {
        // Accessing the host app
        print(UIApplication.shared.windows)
        #expect(Bool(true))
    }
}
