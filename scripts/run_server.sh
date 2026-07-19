#!/bin/bash

# Exit immediately if any command fails
set -e 

echo -e "\n[1/3] Configuring CMake..."
cmake -S . -B build

echo -e "\n[2/3] Building KV-Store..."
cmake --build build

echo -e "\n[3/3] Starting Server..."
./build/kvstore