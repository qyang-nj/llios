#!/bin/zsh

# Emit the symbol graph for Foo.swift,
# so that we can see the "SYNTHESIZED" symbols
xcrun swiftc -emit-module -o build/Foo.swiftmodule -emit-symbol-graph -emit-symbol-graph-dir build Foo.swift
