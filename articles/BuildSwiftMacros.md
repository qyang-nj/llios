# Build Swift Macros
In WWDC 2023, Apple announced [Swift Macros](https://docs.swift.org/swift-book/documentation/the-swift-programming-language/macros), which enable code generation during compile time. Swift Macros go beyond being a regular programming language feature; they actually extend the functionality of the Swift compiler. In this discussion, we will cover two aspects, implementing macros and using macros, from the build perspective.

## Implement Macro
To implement a macro, we will write code similar to the example below. As evident from the `import` statements, this code depends on [apple/swift-syntax](https://github.com/apple/swift-syntax). It may seem counter-intuitive that the macro implementation will not be included directly in our app's build. Instead, it will be built into a **compiler plugin**, which is an executable. That's why the implementation includes `@main` to designate an entry point. Since the compiler plugin is utilized by the compiler itself, it must be built and executed on the host machine, such as macOS, rather than on iOS.

```swift
import SwiftCompilerPlugin
import SwiftSyntax
import SwiftSyntaxBuilder
import SwiftSyntaxMacros

public struct FooMacro: ExpressionMacro {
    public static func expansion(
        of node: some FreestandingMacroExpansionSyntax,
        in context: some MacroExpansionContext
    ) -> ExprSyntax {
        ...
    }
}

@main
struct FooPlugin: CompilerPlugin {
    let providingMacros: [Macro.Type] = [
        FooMacro.self,
    ]
}
```

The summary is that building a Swift Macro implementation is just building an macOS executable. Because of this, we should be able to integrate with third-party macros without requiring access the source code.

## Use Macro
Using a macro contains two parts. Firstly, we need to declare it with `macro` keyword. We also need to hook up the declaration with the implementation, through the module name and type name. The module name is specified through a compiler flag (see below) and the type name is the struct name defined in the implementation.

```swift
@freestanding(expression)
public macro FOO<T>(_ value: T) -> (T, String) = #externalMacro(module: "Foo", type: "FooMacro")
```

Secondly, we just invocate the macro by `#FOO`.

###
```swift
let foo = #FOO("bar")
```

The declaration and invocation can be in the same file or even different modules. To compile, we need to pass **`-Xfrontend -load-plugin-executable -Xfrontend /path/to/FooPlugin#Foo`** to the compiler. The value format is "\<path-to-exectuable\>#\<module-name\>". Please note that wherever the macro is declared or invoked, this flag is required.

There is another flag called `-load-plugin-library`, which accepts a dynamic library instead of an executable. The use case needs to be researched.

## Sample
There is a sample [here](../building/swift_macros/). Use `swift build -v` to see how building macros works.
