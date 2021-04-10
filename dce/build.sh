#!/bin/bash

# Swift symbols are preserved by default for relection, so we need to disable reflection
xcrun swiftc -parse-as-library -c -o lib.o -Xfrontend -disable-reflection-metadata lib.swift
xcrun swiftc -c -Xfrontend -disable-reflection-metadata -o main.o main.swift

# We can toggle `-dead_strip` to see what can be stripped.
xcrun ld -syslibroot /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX11.1.sdk \
    -L /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX11.1.sdk/usr/lib/swift \
    -lobjc -lSystem \
    -dead_strip \
    lib.o main.o


# Verify what symbols are no_dead_strip
nm -nm lib.o | grep "no dead strip"

# Observe the strings are preserved. Others are stripped.
strings a.out
