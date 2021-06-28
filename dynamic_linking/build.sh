#!/bin/bash

[ -x "$FIXUP_CHAINS" ] && FIXUP_CHAINS=0

[ "$FIXUP_CHAINS" == 1 ] && fixup_chains_flag="-Xlinker -fixup_chains" || fixup_chains_flag=""

clang -dynamiclib -o build/lib.dylib lib.c
clang -c -o build/main.o main.c

clang $fixup_chains_flag build/main.o build/lib.dylib
