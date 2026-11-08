#!/bin/sh
# Streaming STX decoder vs. the old decode-all + SDL-shim-resample path (bit-exact). Run from n3ds_port/.
set -e
cd "$(dirname "$0")/../.."
g++ -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-unused-parameter -pthread \
    tools/host_test/test_stx.cpp platform/n3ds_stx.cpp tools/host_test/stx_mock.cpp -o /tmp/test_stx
/tmp/test_stx
