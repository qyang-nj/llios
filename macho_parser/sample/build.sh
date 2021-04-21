#!/bin/bash
set -e

[ -z "$TWO_LEVEL_NAMESAPCE" ] && TWO_LEVEL_NAMESAPCE=1

clang -dynamiclib -o build/my_dylib.dylib my_dylib.c

clang -c -o build/main.o main.c
clang -c -fmodules -o build/objc.o objc.m

[ "$TWO_LEVEL_NAMESAPCE" == 1 ] && two_level_flag="" || two_level_flag="-flat_namespace"
clang $two_level_flag \
    -o sample \
    -Xlinker -U -Xlinker "_c_extern_weak_function" \
    build/main.o build/objc.o build/my_dylib.dylib
