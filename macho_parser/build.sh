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
    LDFLAGS="$LDFLAGS -lssl -lcrypto -L/usr/local/opt/openssl/lib -D OPENSSL"
fi

if [ "$OPT_DEBUG" == 1 ]; then
    CFLAGS="$CFLAGS -g"
fi



mkdir -p build/
rm -f build/*.o

for src in sources/*.cpp; do
    xcrun clang++ --std=c++14 -c -o "build/$(basename $src).o" $CFLAGS $src
done

for src in sources/*.c; do
    xcrun clang -c -o "build/$(basename $src).o" $CFLAGS $src
done

xcrun clang++ --std=c++14 -o macho_parser -framework CoreFoundation -framework Security $LDFLAGS build/*.o
