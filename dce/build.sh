#!/bin/bash
set -e

[ -z "$DEAD_STRIP" ] && DEAD_STRIP=1
[ -z "$USE_STATIC_LIB" ] && USE_STATIC_LIB=0

# Swift symbols are preserved by default for relection, so we need to disable reflection
xcrun swiftc -parse-as-library -c -o lib.o -Xfrontend -disable-reflection-metadata lib.swift
libtool -static lib.o -o lib.a

xcrun swiftc -c -Xfrontend -disable-reflection-metadata -o main.o main.swift

[ "$DEAD_STRIP" == 1 ] && dead_strip_flag="-dead_strip" || dead_strip_flag=""
[ "$USE_STATIC_LIB" == 1 ] && lib_bin=lib.a || lib_bin=lib.o

# We can toggle `-dead_strip` to see what can be stripped.
xcrun ld -syslibroot /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX11.1.sdk \
    -L /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX11.1.sdk/usr/lib/swift \
    -lobjc -lSystem \
    $dead_strip_flag \
    $lib_bin main.o

# Verify what symbols are no_dead_strip
# nm -nm lib.o | grep "no dead strip"

# Observe the preserved strings. Others are stripped.
strings a.out
