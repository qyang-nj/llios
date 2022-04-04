#!/bin/bash

[ -x "$FIXUP_CHAINS" ] && FIXUP_CHAINS=0

[ "$FIXUP_CHAINS" == 1 ] && fixup_chains_flag="-Xlinker -fixup_chains" || fixup_chains_flag=""

# Make sure we're build for x86_64 arch, because the article is based on x86_64 assembly.
target="x86_64-apple-macos"

clang -target $target -dynamiclib -o build/lib.dylib lib.c
clang -target $target -c -o build/main.o main.c

clang -target $target $fixup_chains_flag build/main.o build/lib.dylib
