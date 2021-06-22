#!/bin/bash
set -e

srcs=$(ls sources/*.c)
clang -o parser $srcs
