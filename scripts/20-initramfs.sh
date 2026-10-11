#!/usr/bin/env bash
# Build the guest initramfs: busybox + vgpu.ko + vgpu_test + /init.
# Reuses the same busybox approach as ~/learn-kernel (docs/adr/0003).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INITDIR="$ROOT/initramfs/root"
BUSYBOX=/usr/bin/busybox

[ -f "$ROOT/driver/vgpu.ko" ] || { echo "run scripts/10-build.sh first"; exit 1; }
[ -f "$ROOT/userspace/vgpu_test" ] || { echo "run scripts/10-build.sh first"; exit 1; }
[ -f "$ROOT/userspace/examples/vgpu_smi" ] || { echo "run scripts/10-build.sh first"; exit 1; }

rm -rf "$INITDIR"
mkdir -p "$INITDIR"/{bin,proc,sys,dev,root,etc}
cp "$BUSYBOX" "$INITDIR/bin/busybox"
for c in sh insmod rmmod ls cat echo mount sleep printf true false \
         lsmod dmesg grep hexdump dd test rm mkdir cp mv mknod \
         sed awk wc seq; do
	ln -sf busybox "$INITDIR/bin/$c"
done
cp "$ROOT/driver/vgpu.ko" "$INITDIR/root/"
cp "$ROOT/userspace/vgpu_test" "$INITDIR/root/"
[ -f "$ROOT/userspace/examples/saxpy" ] && cp "$ROOT/userspace/examples/saxpy" "$INITDIR/root/"
[ -f "$ROOT/userspace/examples/uvm_saxpy" ] && cp "$ROOT/userspace/examples/uvm_saxpy" "$INITDIR/root/"
[ -f "$ROOT/userspace/examples/faults" ] && cp "$ROOT/userspace/examples/faults" "$INITDIR/root/"
cp "$ROOT/userspace/examples/vgpu_smi" "$INITDIR/root/"
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
echo "=== compute: saxpy kernel launch (the CUDA example) ==="
/root/saxpy || exit 1
echo "--- compute stats ---"
cat /sys/kernel/debug/vgpu/compute
echo "=== vgpu-smi-lite: device observability (phase J) ==="
mkdir -p /tmp
DBG=/sys/kernel/debug/vgpu
/root/vgpu_smi > /tmp/smi.txt || exit 1
cat /tmp/smi.txt
# Cross-validation: an observability tool is only worth its numbers,
# so every number the tool printed is compared here against the very
# debugfs file it came from (the tool also echoes its raw counters at
# the bottom of the report - these lines diff them mechanically).
xcheck_all() { # $1 = "queues" to also compare the queue table
	xf=0
	xck() {
		if [ "$2" = "$3" ]; then
			echo "OK   xcheck $1: tool=$2 debugfs=$3"
		else
			echo "FAIL xcheck $1: tool=$2 debugfs=$3"
			xf=1
		fi
	}
	tv() { line=$(grep -F "$1=" "$SMI"); echo "${line#*=}"; }
	xck submitted "$(tv 'raw stats submitted')" "$(awk '$1=="submitted"{print $2}' $DBG/stats)"
	xck completed "$(tv 'raw stats completed')" "$(awk '$1=="completed"{print $2}' $DBG/stats)"
	xck errors "$(tv 'raw stats errors')" "$(awk '$1=="errors"{print $2}' $DBG/stats)"
	for e in gfx copy compute; do
		xck "scratch[$e]" "$(tv "raw stats scratch[$e]")" "$(grep -F "scratch[$e]" $DBG/stats | awk '{print $2"/"$4}')"
		# the driver pads engine names: awk sees "engine[gfx" then "]"
		xck "backlog[$e]" "$(tv "raw stats engine[$e].backlog")" "$(grep -F "engine[$e" $DBG/stats | awk '{print $NF}')"
	done
	xck compute.kernels "$(tv 'raw compute kernels')" "$(awk '$1=="kernels"{print $2}' $DBG/compute)"
	xck compute.blocks "$(tv 'raw compute blocks')" "$(awk '$1=="blocks"{print $2}' $DBG/compute)"
	xck compute.warps "$(tv 'raw compute warps')" "$(awk '$1=="warps"{print $2}' $DBG/compute)"
	xck compute.threads "$(tv 'raw compute threads')" "$(awk '$1=="threads"{print $2}' $DBG/compute)"
	xck compute.waves "$(tv 'raw compute waves')" "$(awk '$1=="waves"{print $2}' $DBG/compute)"
	xck compute.instrs "$(tv 'raw compute instrs')" "$(awk '$1=="instrs"{print $2}' $DBG/compute)"
	xck compute.mem_ops "$(tv 'raw compute mem_ops')" "$(awk '$1=="mem_ops"{print $2}' $DBG/compute)"
	xck compute.broadcast_warps "$(tv 'raw compute warps_broadcast')" "$(awk '$1=="broadcast"{print $3}' $DBG/compute)"
	xck compute.coalesced_warps "$(tv 'raw compute warps_coalesced')" "$(awk '$1=="coalesced"{print $3}' $DBG/compute)"
	xck compute.strided_warps "$(tv 'raw compute warps_strided')" "$(awk '$1=="strided"{print $3}' $DBG/compute)"
	xck compute.latency_ns "$(tv 'raw compute modelled_mem_latency_ns')" "$(awk '$1=="modelled"{print $4}' $DBG/compute)"
	xck pending.count "$(tv 'raw pending count')" "$(grep -c 'seq#' $DBG/pending)"
	xck regs.status "$(tv 'raw regs status')" "$(awk '$1=="status"{print $3}' $DBG/regs)"
	xck regs.seqno "$(tv 'raw regs seqno')" "$(awk '$1=="seqno"{print $3}' $DBG/regs)"
	xck regs.error "$(tv 'raw regs error')" "$(awk '$1=="error"{print $3}' $DBG/regs)"
	xck regs.user "$(tv 'raw regs user')" "$(awk '$1=="user"{print $3}' $DBG/regs)"
	if [ "$1" = "queues" ]; then
		xck queues.count "$(tv 'raw queues count')" "$(grep -c '^q#' $DBG/queues)"
	fi
	if [ "$xf" = 0 ]; then
		echo "cross-validation: OK - every tool number matches the debugfs file it came from"
	else
		echo "cross-validation: FAILED"
		exit 1
	fi
}
SMI=/tmp/smi.txt
xcheck_all queues
echo "--- live streams: three queues held open while vgpu_smi observes ---"
/root/vgpu_smi --demo > /tmp/smi_demo.txt || exit 1
cat /tmp/smi_demo.txt
# (the demo's streams close when it exits, so its queue table is the
# only thing that cannot be re-checked against the files afterwards)
SMI=/tmp/smi_demo.txt
xcheck_all
echo "--- the debugfs files right after the demo closed its streams ---"
cat $DBG/stats
cat $DBG/queues
echo "=== negative paths (bad handle / engine fault / ring full) ==="
/root/vgpu_test -e || exit 1
echo "=== phase F: timeslicing + preemption ==="
/root/vgpu_test -f || exit 1
echo "--- scheduler view (debugfs compute) ---"
cat /sys/kernel/debug/vgpu/compute
echo "=== phase G: memory-semantics sync (timeline semaphore, graph) ==="
/root/vgpu_test -g || exit 1
echo "--- sync stats (graphs / signals / waits / events) ---"
cat /sys/kernel/debug/vgpu/sync
echo "=== phase C: GPU virtual memory (VMID isolation + VRAM budget) ==="
/root/vgpu_test -v || exit 1
echo "--- vm view: per-context VA ranges, budget, evictions ---"
cat /sys/kernel/debug/vgpu/vm
echo "=== phase E: unified memory (managed VA + demand migration) ==="
/root/uvm_saxpy || exit 1
echo "--- vm view after the managed range was faulted back in ---"
cat /sys/kernel/debug/vgpu/vm
echo "--- engine-side demand faults (debugfs compute) ---"
grep 'vm faults' /sys/kernel/debug/vgpu/compute
echo "=== phase I: runtime PM, DVFS clock, per-context accounting ==="
/root/vgpu_test -i || exit 1
echo "--- dvfs/rpm view (debugfs stats) ---"
cat /sys/kernel/debug/vgpu/stats
echo "=== phase H: fault containment (one context's wild pointer must not take down the others) ==="
/root/faults || exit 1
echo "--- fault buffer: the structured records the device kept ---"
cat /sys/kernel/debug/vgpu/faults
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
