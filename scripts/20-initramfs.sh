#!/usr/bin/env bash
# Build the guest initramfs: busybox + vgpu.ko + vgpu_test + /init.
# Reuses the same busybox approach as ~/learn-kernel (docs/adr/0003).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INITDIR="$ROOT/initramfs/root"
BUSYBOX=/usr/bin/busybox

[ -f "$ROOT/driver/vgpu.ko" ] || { echo "run scripts/10-build.sh first"; exit 1; }
[ -f "$ROOT/userspace/vgpu_test" ] || { echo "run scripts/10-build.sh first"; exit 1; }

rm -rf "$INITDIR"
mkdir -p "$INITDIR"/{bin,proc,sys,dev,root,etc}
cp "$BUSYBOX" "$INITDIR/bin/busybox"
for c in sh insmod rmmod ls cat echo mount sleep printf true false \
         lsmod dmesg grep hexdump dd test rm mkdir cp mv mknod; do
	ln -sf busybox "$INITDIR/bin/$c"
done
cp "$ROOT/driver/vgpu.ko" "$INITDIR/root/"
cp "$ROOT/userspace/vgpu_test" "$INITDIR/root/"
# autorun marker (if present next to the scripts) triggers the test on boot
[ -f "$ROOT/autorun" ] && touch "$INITDIR/root/autorun"

# the automated test run
cat > "$INITDIR/root/run_test.sh" <<'EOF'
#!/bin/sh
echo "=== single process full chain ==="
/root/vgpu_test || exit 1
echo "=== 4-process concurrent submission ==="
/root/vgpu_test -m 4 || exit 1
echo "=== multi-queue, multi-engine (one fd, 3 streams) ==="
/root/vgpu_test -q || exit 1
echo "--- queues (debugfs) ---"
cat /sys/kernel/debug/vgpu/queues
echo "=== doorbell path (packets fetched by the engine) ==="
/root/vgpu_test -p || exit 1
echo "=== negative paths (bad handle / engine fault / ring full) ==="
/root/vgpu_test -e || exit 1
echo "=== ftrace: function_graph over vgpu_* ==="
TR=/sys/kernel/debug/tracing
echo function_graph > "$TR/current_tracer"
echo 'vgpu_*' > "$TR/set_ftrace_filter"
echo 'vgpu_*' > "$TR/set_graph_function"
echo > "$TR/trace"			# clear after the filters, before the run
echo 1 > "$TR/tracing_on"
/root/vgpu_test >/dev/null 2>&1 || exit 1
echo 0 > "$TR/tracing_on"
echo "trace lines: $(wc -l < "$TR/trace")"
sed -n '1,50p' "$TR/trace"
echo nop > "$TR/current_tracer"
echo "=== debugfs stats ==="
cat /sys/kernel/debug/vgpu/stats
echo "--- regs ---"
cat /sys/kernel/debug/vgpu/regs
echo "=== rmmod drain ==="
rmmod vgpu || exit 1
lsmod | grep -q '^vgpu' && exit 1
echo "rmmod: OK (drain path clean)"
echo "=== reload for interactive use ==="
insmod /root/vgpu.ko || exit 1
for n in renderD128 card0; do
	if [ -e "/sys/class/dri/$n/dev" ]; then sys="/sys/class/dri/$n/dev";
	elif [ -e "/sys/class/drm/$n/dev" ]; then sys="/sys/class/drm/$n/dev";
	else continue; fi
	dev=$(cat "$sys"); rm -f "/dev/dri/$n"
	mknod "/dev/dri/$n" c "${dev%:*}" "${dev#*:}"
done
/root/vgpu_test >/dev/null 2>&1 && echo "device ready: /dev/dri/{card0,renderD128}" || echo "WARN: reload smoke test failed"
echo "=== all tests passed ==="
EOF
chmod +x "$INITDIR/root/run_test.sh"

# /init: mount the pseudo-filesystems, create the /dev/dri nodes by
# hand (no udev in a busybox initramfs), insmod the driver.
cat > "$INITDIR/init" <<'EOF'
#!/bin/sh
mount -t proc none /proc
mount -t sysfs none /sys
mount -t debugfs none /sys/kernel/debug

# no udev here: create the DRM device nodes from sysfs
mkdir -p /dev/dri
mknode() { # $1 = sysfs dev file, $2 = node name
	dev=$(cat "$1")
	major=${dev%:*}
	minor=${dev#*:}
	if [ ! -e "/dev/dri/$2" ]; then
		mknod "/dev/dri/$2" c "$major" "$minor" 2>/dev/null || true
	fi
}
echo "[init] vgpu guest up (kernel $(uname -r))"
if [ -f /root/vgpu.ko ]; then
	insmod /root/vgpu.ko
	# nodes only exist after the module probes — create them now (no udev)
	[ -e /sys/class/drm/card0/dev ] && mknode /sys/class/drm/card0/dev card0
	[ -e /sys/class/dri/renderD128/dev ] && mknode /sys/class/dri/renderD128/dev renderD128
	echo "[init] vgpu.ko loaded: $(ls /dev/dri 2>/dev/null)"
fi
if [ -f /root/autorun ]; then
	/root/run_test.sh
fi
echo "[init] dropping to shell"
exec /bin/sh
EOF
chmod +x "$INITDIR/init"

(cd "$INITDIR" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip) \
	> "$ROOT/initramfs/initramfs.cpio.gz"
echo "built: $ROOT/initramfs/initramfs.cpio.gz ($(du -h "$ROOT/initramfs/initramfs.cpio.gz" | cut -f1))"
