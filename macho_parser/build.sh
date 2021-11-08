#!/bin/bash
# Usage: build.sh --openssl
#   --openssl    Build with OpenSSL library, enabling printing more details of code signature.
set -e

OPT_OPENSSL=0
OPT_DEBUG=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --openssl)
            OPT_OPENSSL=1
            shift
            ;;
        -d|--debug)
            OPT_DEBUG=1
            shift
            ;;
    esac
done

CFLAGS=""

if [ "$OPT_OPENSSL" == 1 ]; then
    CFLAGS="$CFLAGS -lssl -lcrypto -L/usr/local/opt/openssl/lib -D OPENSSL"
fi

if [ "$OPT_DEBUG" == 1 ]; then
    CFLAGS="$CFLAGS -g"
fi

SRCS=$(ls sources/*.c)
clang -o macho_parser -framework CoreFoundation -framework Security $CFLAGS $SRCS
