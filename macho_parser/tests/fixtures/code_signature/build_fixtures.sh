#!/bin/zsh

swiftc main.swift -o main

cp main main_sha1
codesign --force --sign - --digest-algorithm=sha1 main_sha1

cp main main_sha256
codesign --force --sign - --digest-algorithm=sha256 main_sha256

rm main
