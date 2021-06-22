#!/bin/bash
set -e

clang -o parser main.c argument.c util.c segment_64.c \
    symtab.c dysymtab.c dyld_info.c linkedit_data.c build_version.c
