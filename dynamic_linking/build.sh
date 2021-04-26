#!/bin/bash

clang -dynamiclib -o build/lib.dylib lib.c
clang -c -o build/main.o main.c
clang build/main.o build/lib.dylib
