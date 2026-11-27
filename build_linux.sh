#!/usr/bin/env bash
# Experimental Linux build. Needs Qt 6 (Core Gui Widgets Network), CMake >= 3.21, a C++20 compiler;
# optional: libiio (pkg-config) for ADALM-PLUTO.
set -euo pipefail
cd "$(dirname "$0")"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNADIRSTATION_BUILD_PLUTO=ON
cmake --build build -j"$(nproc)"
echo "Built: build/NADIR_Station"
