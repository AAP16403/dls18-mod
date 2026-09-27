#!/bin/sh
# Build and run the host harness in WSL (x86-64 gcc, non-PIE so all game-visible pointers stay < 4 GiB).
#   wsl -e sh run_tests.sh [seasons] [seed]
set -e
cd "$(dirname "$0")"
[ -f dls18.txt ] || python3 make_dataset.py
gcc -O1 -g -std=gnu11 -Wall -Wno-unused-function -no-pie -fno-pie -I.. harness.c -o harness -lm
./harness dls18.txt "${1:-8}" "${2:-12345}"
