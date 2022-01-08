#!/bin/bash
set -e

[ -z "$TWO_LEVEL_NAMESAPCE" ] && TWO_LEVEL_NAMESAPCE=1
[ -x "$FIXUP_CHAINS" ] && FIXUP_CHAINS=0

mkdir -p build

xcrun clang -dynamiclib -install_name "@rpath/my_dylib.dylib" -o build/my_dylib.dylib sample/my_dylib.c

xcrun clang -c -g -o build/main.o sample/main.c
xcrun clang -c -fmodules -o build/objc.o sample/objc.m

[ "$TWO_LEVEL_NAMESAPCE" == 1 ] && two_level_flag="" || two_level_flag="-flat_namespace"
[ "$FIXUP_CHAINS" == 1 ] && fixup_chains_flag="-Xlinker -fixup_chains" || fixup_chains_flag=""
xcrun clang $two_level_flag $fixup_chains_flag \
    -o sample.out \
    -Xlinker -U -Xlinker "_c_extern_weak_function" \
    -Xlinker -dyld_env -Xlinker DYLD_VERSIONED_FRAMEWORK_PATH=/path/to/llios \
    -rpath "build" \
    build/main.o build/objc.o build/my_dylib.dylib
