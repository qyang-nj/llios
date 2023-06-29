@freestanding(expression)
public macro stringify<T>(_ value: T) -> (T, String) = #externalMacro(module: "StringifyMacro", type: "StringifyMacro")

let a = 17
let b = 25

let (result, code) = #stringify(a + b)

print("The value \(result) was produced by the code \"\(code)\"")
