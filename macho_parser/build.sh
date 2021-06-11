#!/bin/bash
set -e

clang -o parser main.c argument.c util.c symtab.c dysymtab.c dyld_info.c linkedit_data.c
