#!/bin/bash
set -e

srcs=$(ls sources/*.c)
clang -o parser -lssl -lcrypto -L/usr/local/opt/openssl/lib -framework CoreFoundation -framework Security $srcs
