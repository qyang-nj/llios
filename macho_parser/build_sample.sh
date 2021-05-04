#!/bin/bash
set -e

[ -z "$TWO_LEVEL_NAMESAPCE" ] && TWO_LEVEL_NAMESAPCE=1

mkdir -p build

clang -dynamiclib -o build/my_dylib.dylib sample/my_dylib.c

clang -c -o build/main.o sample/main.c
clang -c -fmodules -o build/objc.o sample/objc.m

[ "$TWO_LEVEL_NAMESAPCE" == 1 ] && two_level_flag="" || two_level_flag="-flat_namespace"
clang $two_level_flag \
    -o sample.out \
    -Xlinker -U -Xlinker "_c_extern_weak_function" \
    build/main.o build/objc.o build/my_dylib.dylib
