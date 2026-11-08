#!/bin/sh
# Host test for platform/n3ds_gfx.cpp. Needs only g++ (no devkitPro).
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -O1 -g -Wall -Wno-unused-function -I mock -I ../../platform -o /tmp/n3ds_gfx_test test_gfx.cpp mock_bridge.cpp ../../platform/n3ds_gfx.cpp ../../platform/n3ds_tex.cpp ../../platform/n3ds_texconv.cpp ../../platform/n3ds_config.cpp ../../platform/n3ds_tev.cpp
/tmp/n3ds_gfx_test
