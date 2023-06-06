#!/bin/zsh
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

CFLAGS=()
LDFLAGS=("-lz")

if [[ "$OPT_OPENSSL" == 1 ]]; then
    CFLAGS+=("-DOPENSSL" "-I$(brew --prefix openssl)/include")
    LDFLAGS+=("-lssl" "-lcrypto" "-L$(brew --prefix openssl)/lib" "-DOPENSSL")
fi

if [[ "$OPT_DEBUG" == 1 ]]; then
    CFLAGS+=("-g")
fi

mkdir -p build/
find build -name "*.o" -delete

for src in sources/**/*.cpp; do
    xcrun clang++ --std=c++17 -c -o "build/$(basename $src).o" $CFLAGS $src
done

xcrun clang++ --std=c++17 -o macho_parser -framework CoreFoundation -framework Security $LDFLAGS build/*.o
