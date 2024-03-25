#!/bin/zsh

xcrun swiftc -emit-module -o build/ModuleA.swiftmodule ModuleA.swift

echo "\033[34m>>> Compiler diagnostics with .swiftsourceinfo:\033[0m"
xcrun swiftc -typecheck -Ibuild ModuleB.swift

# We can also use `-avoid-emit-module-source-info` to avoid emitting .swiftsourceinfo files.
rm build/ModuleA.swiftsourceinfo

echo "\033[34m>>> Compiler diagnostics without .swiftsourceinfo:\033[0m"
xcrun swiftc -typecheck -Ibuild ModuleB.swift
