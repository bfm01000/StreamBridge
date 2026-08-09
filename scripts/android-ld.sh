#!/bin/sh
# Android linker wrapper - handles long command lines via response files
# Windows CreateProcess has a ~32767 char limit.
# When the command line is too long, we write .o and -l args to a response file.
set -e

CLANG="/d/soft/AS_sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
TARGET="--target=aarch64-linux-android21"
RSP_FILE=""
NEW_ARGS=""
OBJ_COUNT=0

# Check if we need a response file (args > 20000 chars is risky)
TOTAL_LEN=0
for arg in "$@"; do
    TOTAL_LEN=$((TOTAL_LEN + ${#arg} + 1))
done

if [ "$TOTAL_LEN" -gt 20000 ]; then
    # Use response file for object files and libraries
    RSP_FILE="/tmp/ffmpeg_link_$$.rsp"
    rm -f "$RSP_FILE"

    for arg in "$@"; do
        case "$arg" in
            *.o)
                # Quote paths with spaces
                echo "\"$arg\"" >> "$RSP_FILE"
                OBJ_COUNT=$((OBJ_COUNT + 1))
                ;;
            -l*)
                echo "$arg" >> "$RSP_FILE"
                ;;
            *)
                NEW_ARGS="$NEW_ARGS $arg"
                ;;
        esac
    done

    # Remove leading space
    NEW_ARGS="${NEW_ARGS# }"
    eval "exec $CLANG $TARGET $NEW_ARGS @$RSP_FILE"
else
    exec "$CLANG" "$TARGET" "$@"
fi
