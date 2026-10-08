#!/bin/sh
echo "=== single process full chain ==="
/root/vgpu_test || exit 1
echo "=== 4-process concurrent submission ==="
/root/vgpu_test -m 4 || exit 1
echo "=== debugfs stats ==="
cat /sys/kernel/debug/vgpu/stats
echo "--- regs ---"
cat /sys/kernel/debug/vgpu/regs
echo "=== all tests passed ==="
