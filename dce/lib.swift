// Public symbols are always no_dead_strip.
public struct PublicStruct {
    public func f1() {
        print("print public")
    }
}

// Non-public sysmbols can be stripped by linker if unused.
struct InternalStruct {
    func f2() {
        print("print internal")
    }
}
