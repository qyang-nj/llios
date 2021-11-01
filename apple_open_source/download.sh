#!/bin/bash
# Download apple open source tools.
set -e

CCTOOLS_URL="https://opensource.apple.com/tarballs/cctools/cctools-973.0.1.tar.gz"
LD64_URL="https://opensource.apple.com/tarballs/ld64/ld64-609.tar.gz"
DYLD_URL="https://opensource.apple.com/tarballs/dyld/dyld-851.27.tar.gz"
XNU_URL="https://opensource.apple.com/tarballs/xnu/xnu-7195.101.1.tar.gz"
LIB_SECURITY_CODESIGNING="https://opensource.apple.com/tarballs/libsecurity_codesigning/libsecurity_codesigning-55037.15.tar.gz"

for url in "$CCTOOLS_URL" "$LD64_URL" "$DYLD_URL" "$XNU_URL" "$LIB_SECURITY_CODESIGNING"; do
    echo "$url"

    tarfile=$(basename $url)
    fullname=${tarfile%".tar.gz"}
    name=${fullname%"-"*}
    version=${fullname##*"-"}

    rm -rf "$name"

    curl $url --output "$tarfile"
    tar -zxf "$tarfile"

    mv "$name-$version" "$name"

    git add "$name"
    [[ -n $(git status -s $name) ]] && git commit -m "$name-$version"
done
