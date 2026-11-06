#!/bin/sh
# Pakai: ANDROID_NDK=/path/ke/ndk ./build.sh   -> build/libVkLayer_khronos_validation.so
set -e
: "${ANDROID_NDK:?isi ANDROID_NDK dulu}"
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ls -la build/libVkLayer_khronos_validation.so
