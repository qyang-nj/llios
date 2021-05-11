#!/bin/bash
set -e

clang -o parser main.c argument.c symtab.c dysymtab.c
