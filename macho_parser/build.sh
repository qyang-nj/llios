#!/bin/bash
set -e

OPT_OPENSSL=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --openssl)
            OPT_OPENSSL=1
            shift
            ;;
    esac
done

CFLAGS=""

if [ "$OPT_OPENSSL" == 1 ]; then
    CFLAGS="$CFLAGS -lssl -lcrypto -L/usr/local/opt/openssl/lib -D OPENSSL"
fi

SRCS=$(ls sources/*.c)
clang -o parser -framework CoreFoundation -framework Security $CFLAGS $SRCS
