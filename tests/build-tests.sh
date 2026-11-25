#!/usr/bin/env bash
# Builds dist/frametest.exe (the ETW frame capture test run by CI on Windows).
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -z "${ZIG:-}" ]]; then
  if command -v zig >/dev/null 2>&1; then ZIG=zig; else ZIG="python3 -m ziglang"; fi
fi
mkdir -p dist
$ZIG c++ -target x86_64-windows-gnu -std=c++17 -O2 -municode -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN \
  tests/frametest.cpp src/frames.cpp src/util.cpp -o dist/frametest.exe \
  -ld3d11 -ld3d9 -ldxgi -ladvapi32 -lshell32 -lole32 -luuid -lbcrypt -lversion -lshlwapi -luser32
$ZIG c++ -target x86_64-windows-gnu -std=c++17 -O2 -municode -DFAKEGAME -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN \
  tests/frametest.cpp src/frames.cpp src/util.cpp -o dist/fakegame.exe -Wl,--subsystem,windows \
  -ld3d11 -ld3d9 -ldxgi -ladvapi32 -lshell32 -lole32 -luuid -lbcrypt -lversion -lshlwapi -luser32
rm -f dist/frametest.pdb dist/fakegame.pdb
