#!/usr/bin/env bash
set -euo pipefail

mkdir -p ../public/downloads

echo "Compiling x86_64 Windows binary (cdw2png_x64.exe)..."
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra \
  -ffunction-sections -fdata-sections \
  -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN \
  main.cpp -o ../public/downloads/cdw2png_x64.exe \
  -lole32 -loleaut32 -luuid -lshell32 \
  -static -static-libgcc -static-libstdc++ -Wl,--gc-sections -s

echo "Compiling x86 (32-bit) Windows binary (cdw2png_x86.exe)..."
i686-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra \
  -ffunction-sections -fdata-sections \
  -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN \
  main.cpp -o ../public/downloads/cdw2png_x86.exe \
  -lole32 -loleaut32 -luuid -lshell32 \
  -static -static-libgcc -static-libstdc++ -Wl,--gc-sections -s

echo "Packaging source archive..."
rm -f ../public/downloads/cdw2png_src.zip
zip -q -j ../public/downloads/cdw2png_src.zip main.cpp CMakeLists.txt build_msvc.bat build_mingw.sh

echo "Build complete."
