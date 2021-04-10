var barGlobalVariable = "bar"

public func barMethod () {
    fooMethod()
}

public class BarClass {
    public init() {}

    func barInstanceMethod() {
        barGlobalVariable = "BAR"
    }
}
