// swift-tools-version: 5.9
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription
import CompilerPluginSupport

let package = Package(
    name: "swift_macros",
    platforms: [.macOS(.v10_15)],
    products: [
        .executable(name: "main", targets: ["main"]),
    ],
    dependencies: [
        // Depend on the latest Swift 5.9 prerelease of SwiftSyntax
        .package(url: "https://github.com/apple/swift-syntax.git", from: "509.0.0-swift-5.9-DEVELOPMENT-SNAPSHOT-2023-04-25-b"),
    ],
    targets: [
        .macro(
            name: "StringifyMacro",
            dependencies: [
                .product(name: "SwiftSyntaxMacros", package: "swift-syntax"),
                .product(name: "SwiftCompilerPlugin", package: "swift-syntax")
            ],
            path: ".",
            sources: ["StringifyMacro.swift"]
        ),
        .executableTarget(
            name: "main",
            dependencies: ["StringifyMacro"],
            path: ".",
            sources: ["main.swift"]
        ),
    ]
)
