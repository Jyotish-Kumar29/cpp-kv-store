#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

echo "Configuring server..."
cmake -S . -B build/server \
    -DCMAKE_BUILD_TYPE=Release

echo "Building kvstore server..."
cmake --build build/server --target kvstore --parallel

# manage_server.sh expects the binary at build/kvstore.
ln -sfn server/kvstore build/kvstore

echo "Done: build/server/kvstore"