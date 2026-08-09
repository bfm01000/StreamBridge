#!/bin/sh
exec /d/soft/AS_sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe --target=aarch64-linux-android21 "$@"
