// Public symbols are always no_dead_strip.
public struct PublicStruct {
    public func f1() {
        print("public print")
    }
}

// Non-public sysmbols can be stripped by linker if unused.
struct InternalStruct {
    func f2() {
        print("internal print")
    }
}
