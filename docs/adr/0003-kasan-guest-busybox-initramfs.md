# KASAN guest with busybox initramfs from the ~/linux tree

The Host (xuefeiz2-server) runs the production vLLM deployment under a zero-touch constraint, so the module must run elsewhere. We boot a QEMU/KVM guest with the bzImage from `~/linux` (vanilla 6.17.0, `CONFIG_KASAN=y` already set) plus a busybox initramfs carrying `vgpu.ko` and statically linked test binaries, reusing the existing learn-kernel QEMU pipeline (`-enable-kvm`, serial console, `oops=panic`, optional gdb on `:1234`).

**Why**: guest kernel and module come from the same source tree — exact version match, no vermagic/signing issues; KASAN is already in `.config` (a free memory-error radar for a DMA/IRQ-heavy driver); the boot pipeline already exists (zero new infrastructure); the Host only runs a QEMU process (~2G RAM), preserving the vLLM safety rule.

**Consequences**: no package manager in the guest; test binaries must be statically linked; the "disk" is the initramfs (rebuild on every change); a KASAN-off performance baseline requires a second kernel build and is deferred to the stretch roadmap.
