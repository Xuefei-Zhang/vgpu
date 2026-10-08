#!/usr/bin/env bash
# Boot the KVM guest: the ~/linux bzImage (KASAN) + our initramfs.
#
#   scripts/30-run.sh             # interactive (shell at the end)
#   touch $PWD/autorun && scripts/20-initramfs.sh && scripts/30-run.sh
#                                 # = auto-run the tests on boot
#   DEBUG=1 scripts/30-run.sh     # pause at start; attach gdb on :1234
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KSRC="${KSRC:-$HOME/linux}"
BZIMAGE="$KSRC/arch/x86/boot/bzImage"
INITRD="$ROOT/initramfs/initramfs.cpio.gz"

[ -f "$BZIMAGE" ] || { echo "ERR: $BZIMAGE missing (build ~/linux first)"; exit 1; }
[ -f "$INITRD" ] || { echo "ERR: run scripts/20-initramfs.sh first"; exit 1; }

if [ -f "$ROOT/autorun" ]; then
	echo "autorun marker set: the test will run on boot"
fi

DEBUG=${DEBUG:-0}
GDB_FLAGS=""
if [ "$DEBUG" = "1" ]; then
	GDB_FLAGS="-s -S"
	echo "[DEBUG] guest will pause at start. Attach with:"
	echo "        cd $KSRC && gdb-multiarch vmlinux -ex 'target remote :1234'"
fi

exec qemu-system-x86_64 \
	-enable-kvm -cpu host -smp 4 -m 2G \
	-kernel "$BZIMAGE" \
	-initrd "$INITRD" \
	-append "console=ttyS0 nokaslr panic=-1 oops=panic loglevel=8 rdinit=/init" \
	-nographic -no-reboot \
	-device virtio-rng-pci \
	$GDB_FLAGS
