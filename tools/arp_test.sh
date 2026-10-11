#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Host test of the arpeggiator (firmware/src/arp.c): tools/arp_test.sh (inside nix develop, like golden.sh)
set -e
cd "$(dirname "$0")/.."
mkdir -p build
gcc -O1 -Wall -Wno-unused-function -Wno-unused-variable -o build/arp_test firmware/test/arp_test.c
build/arp_test
