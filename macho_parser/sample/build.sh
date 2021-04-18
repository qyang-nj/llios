#!/bin/bash
set -e

clang -c -o sample.o sample.c
clang -c -fmodules -o objc_file.o objc_file.m

clang -o sample sample.o objc_file.o
