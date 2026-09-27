#!/bin/sh
# Build libCareerMarket.so for armeabi-v7a with the WSL cross compiler (no NDK/zig needed).
#   wsl -e sh mod/career_market/build_native_bridge.sh [output]
# Freestanding, no libc: the bridge reaches everything through the game. Soft-float calling passes
# floats in core registers, which is what the game's softfp ABI does too.
set -e
cd "$(dirname "$0")"
OUT="${1:-../build/lib/armeabi-v7a/libCareerMarket.so}"
mkdir -p "$(dirname "$OUT")"
arm-linux-gnueabi-gcc -march=armv7-a -mthumb -mfloat-abi=soft -std=gnu11 -O2 -g0 \
  -ffreestanding -fno-builtin -ffunction-sections -fdata-sections -fPIC -fvisibility=hidden \
  -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
  -Wall -Wno-unused-function \
  -nostdlib -shared -Wl,-soname,libCareerMarket.so -Wl,--no-undefined -Wl,--gc-sections \
  -Wl,-z,max-page-size=4096 -Wl,--hash-style=both -Wl,-z,noexecstack -Wl,-z,now -Wl,-s \
  native_bridge.c bridge_runtime.c modcore.c -o "$OUT" -lgcc
echo "built $OUT ($(stat -c %s "$OUT") bytes)"
