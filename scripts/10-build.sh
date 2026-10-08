#!/usr/bin/env bash
# Build vgpu.ko (against ~/linux) and the static userspace test.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KSRC="${KSRC:-$HOME/linux}"

make -C "$KSRC" M="$ROOT/driver" modules -j"$(nproc)"
make -C "$ROOT/userspace"

echo "built: $ROOT/driver/vgpu.ko"
echo "built: $ROOT/userspace/vgpu_test"
