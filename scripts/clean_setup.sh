#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

# Removes all build artifacts and the persistent store file.
# Run this to start completely fresh before a clean build or benchmark run.

echo "[CLEAN] Removing build/ ..."
rm -rf build/

echo "[CLEAN] Removing data/kvstore.aof ..."
rm -f data/kvstore.aof

echo "[CLEAN] Done."