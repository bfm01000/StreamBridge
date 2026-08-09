#!/bin/bash
# Build FFmpeg for Android arm64-v8a using NDK toolchain
# Usage: bash scripts/build-ffmpeg-android.sh
set -euo pipefail

FFMPEG_VERSION="${FFMPEG_VERSION:-7.0.2}"
API="${API:-21}"
NDK_ROOT="${ANDROID_NDK_HOME:-D:/soft/AS_sdk/ndk/28.2.13676358}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
THIRD_PARTY_DIR="${PROJECT_DIR}/third_party"
FFMPEG_SRC_DIR="${THIRD_PARTY_DIR}/ffmpeg-${FFMPEG_VERSION}"
BUILD_OUTPUT="${THIRD_PARTY_DIR}/ffmpeg-android/arm64-v8a"

# Convert D:/path to /d/path for Git Bash compatibility
NDK_ROOT_UNIX="$(echo "${NDK_ROOT}" | sed 's|^D:/|/d/|;s|^C:/|/c/|')"
TOOLCHAIN="${NDK_ROOT_UNIX}/toolchains/llvm/prebuilt/windows-x86_64"
MAKE="${NDK_ROOT_UNIX}/prebuilt/windows-x86_64/bin/make.exe"
TARGET="aarch64-linux-android"

# Verify NDK exists
if [ ! -d "${TOOLCHAIN}" ]; then
    echo "ERROR: NDK toolchain not found at ${TOOLCHAIN}"
    echo "Set ANDROID_NDK_HOME environment variable to your NDK root."
    exit 1
fi

echo "=== NDK: ${NDK_ROOT}"
echo "=== API: ${API}"
echo "=== Target: ${TARGET}"
echo "=== Source: ${FFMPEG_SRC_DIR}"
echo "=== Output: ${BUILD_OUTPUT}"

mkdir -p "${THIRD_PARTY_DIR}"

# Download FFmpeg if needed
if [ ! -d "${FFMPEG_SRC_DIR}" ]; then
    echo "=== Downloading FFmpeg ${FFMPEG_VERSION} ==="
    cd "${THIRD_PARTY_DIR}"
    FFMPEG_TAR="ffmpeg-${FFMPEG_VERSION}.tar.xz"
    if [ ! -f "${FFMPEG_TAR}" ]; then
        curl -L --retry 3 -o "${FFMPEG_TAR}" \
            "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"
    fi
    echo "=== Extracting FFmpeg ==="
    tar xf "${FFMPEG_TAR}"
    echo "=== Download complete ==="
fi

cd "${FFMPEG_SRC_DIR}"

# Clean previous build if any
if [ -f "Makefile" ] || [ -f "config.h" ]; then
    echo "=== Cleaning previous build ==="
    "${MAKE}" clean 2>/dev/null || true
    # Don't distclean - we want to preserve the source
fi

# Host compiler: MSYS2 UCRT64 gcc (for building FFmpeg host tools during configure)
HOST_CC="/c/msys64/ucrt64/bin/gcc.exe"
if [ ! -x "${HOST_CC}" ]; then
    echo "ERROR: Host C compiler not found at ${HOST_CC}"
    echo "Install MSYS2 UCRT64 gcc or MinGW-w64."
    exit 1
fi

# MSYS2 MUST be first in PATH: gcc's internal tools (cc1.exe, as.exe, collect2.exe)
# need MSYS2 DLLs, and Windows DLL search searches PATH in order.
# We compensate by passing explicit --cc/--cxx to FFmpeg configure so
# it uses the Android clang (not the MSYS2 one that PATH would pick up).
export PATH="/c/msys64/ucrt64/bin:${TOOLCHAIN}/bin:${PATH}"
echo "=== Host CC: ${HOST_CC} (found)"

# Use clang.exe directly with --target (bypass NDK wrapper script to avoid fork/hang issues)
CC_WRAPPER="${PROJECT_DIR}/scripts/android-cc.sh"
CXX_WRAPPER="${PROJECT_DIR}/scripts/android-cxx.sh"

# Create minimal C compiler wrapper
cat > "${CC_WRAPPER}" << 'EOF'
#!/bin/sh
exec /d/soft/AS_sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe --target=aarch64-linux-android21 "$@"
EOF
chmod +x "${CC_WRAPPER}"

# Create minimal C++ compiler wrapper
cat > "${CXX_WRAPPER}" << 'EOF'
#!/bin/sh
exec /d/soft/AS_sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe --target=aarch64-linux-android21 "$@"
EOF
chmod +x "${CXX_WRAPPER}"

CC="${CC_WRAPPER}"
CXX="${CXX_WRAPPER}"
SYSROOT="${TOOLCHAIN}/sysroot"

export AR="${TOOLCHAIN}/bin/llvm-ar"
export AS="${TOOLCHAIN}/bin/llvm-as"
export RANLIB="${TOOLCHAIN}/bin/llvm-ranlib"
export STRIP="${TOOLCHAIN}/bin/llvm-strip"
export NM="${TOOLCHAIN}/bin/llvm-nm"

# Fix Windows path issue: FFmpeg configure needs a Unix-style TMPDIR
export TMPDIR="${PROJECT_DIR}/third_party/tmp"
mkdir -p "${TMPDIR}"

echo "=== CC: ${CC}"
echo "=== CXX: ${CXX}"
echo "=== SYSROOT: ${SYSROOT}"
echo "=== TMPDIR: ${TMPDIR}"

echo "=== Configuring FFmpeg ==="
./configure \
    --prefix="${BUILD_OUTPUT}" \
    --enable-cross-compile \
    --target-os=android \
    --arch=aarch64 \
    --cpu=armv8-a \
    --cc="${CC}" \
    --cxx="${CXX}" \
    --host-cc="${HOST_CC}" \
    --pkg-config=false \
    --disable-shared \
    --enable-static \
    --disable-programs \
    --disable-doc \
    --disable-avdevice \
    --disable-avfilter \
    --disable-postproc \
    --disable-all \
    --enable-avcodec \
    --enable-avformat \
    --enable-avutil \
    --enable-swscale \
    --enable-swresample \
    --enable-decoder=h264,aac \
    --enable-parser=h264,aac \
    --enable-demuxer=flv,live_flv \
    --enable-protocol=rtmp,tcp,file \
    --enable-muxer=null \
    --enable-bsf=h264_mp4toannexb,aac_adtstoasc \
    --disable-asm \
    --extra-cflags="-O2 -fPIC -DANDROID"

echo "=== Building FFmpeg (this may take several minutes) ==="
"${MAKE}" -j4

echo "=== Installing FFmpeg ==="
"${MAKE}" install

echo "=== Building shared libraries from static archives ==="
# Link each .a into a .so for Android System.loadLibrary()
cd "${BUILD_OUTPUT}/lib"

CLANG_BIN="${TOOLCHAIN}/bin/clang.exe"
TARGET_FLAG="--target=aarch64-linux-android21"
SHARED_FLAGS="-shared -Wl,-z,noexecstack -Wl,--as-needed"

echo "--- libavutil.so ---"
"${CLANG_BIN}" ${TARGET_FLAG} ${SHARED_FLAGS} \
    -Wl,--whole-archive libavutil.a -Wl,--no-whole-archive \
    -o libavutil.so -pthread -lm -latomic -landroid

echo "--- libswresample.so ---"
"${CLANG_BIN}" ${TARGET_FLAG} ${SHARED_FLAGS} \
    -Wl,--whole-archive libswresample.a -Wl,--no-whole-archive \
    -L. -lavutil -o libswresample.so -lm -latomic

echo "--- libswscale.so ---"
"${CLANG_BIN}" ${TARGET_FLAG} ${SHARED_FLAGS} \
    -Wl,--whole-archive libswscale.a -Wl,--no-whole-archive \
    -L. -lavutil -o libswscale.so -lm -latomic

echo "--- libavcodec.so ---"
"${CLANG_BIN}" ${TARGET_FLAG} ${SHARED_FLAGS} \
    -Wl,--whole-archive libavcodec.a -Wl,--no-whole-archive \
    -L. -lswresample -lavutil -o libavcodec.so -pthread -lm -latomic

echo "--- libavformat.so ---"
"${CLANG_BIN}" ${TARGET_FLAG} ${SHARED_FLAGS} \
    -Wl,--whole-archive libavformat.a -Wl,--no-whole-archive \
    -L. -lavcodec -lswresample -lavutil -o libavformat.so -lm -latomic

echo "=== Stripping debug symbols ==="
"${STRIP}" --strip-unneeded *.so

echo "=== Build complete ==="
echo "Libraries installed to: ${BUILD_OUTPUT}"
ls -la "${BUILD_OUTPUT}/lib/"
