#!/bin/zsh

# This script demonstrates how to build a mergeable library and requires Xcode 15+.

rm -rf build
mkdir -p build

PARAMS=(
    -emit-library
    -o build/Library.dylib
    -module-name Library
    -emit-module-path Build/Library.swiftmodule
    -Xlinker -make_mergeable
    Library.swift
)
xcrun swiftc ${PARAMS[@]}

# unmerged
xcrun swiftc -Ibuild Binary.swift -o build/main_unmerged -Xlinker build/Library.dylib

# merged
xcrun swiftc -Ibuild Binary.swift -o build/main_merged -Xlinker -merge_library -Xlinker build/Library.dylib
