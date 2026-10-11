# vgpu

A virtual GPU device driven entirely in software, running inside a QEMU/KVM guest. Exists to learn the complete Linux GPU-driver software chain (device registration → buffer objects → command submission → execution → interrupt → fence) without touching real GPU hardware.

## Language

### Device

**vgpu**:
The virtual GPU as a whole: one DRM card plus its simulated Engine, Ring, and MMIO region.
_Avoid_: virtual device, vGPU (vGPU is a real virtualization technology — a different thing)

**MMIO region**:
A coherent kernel buffer that emulates the BAR0 register file (status, seqno, error). On real hardware this would be an ioremap'd BAR.
_Avoid_: registers (too vague), BAR

**Engine**:
The simulated GPU command processor: a single in-kernel worker that decodes and executes Commands from the Ring, in submission order, one submission at a time.
_Avoid_: GPU, hardware, scheduler

### Submission

**Ring**:
A coherent circular buffer of Command slots that the app writes via mmap. It is shared scratch: concurrent writers must use disjoint slots (real GPUs give each context its own ring).
_Avoid_: queue, circular queue, command queue

**Command**:
One fixed-size (64B) GPU operation inside the Ring: opcode plus operands (BO handles, offsets, length).
_Avoid_: instruction, packet

**Submission**:
The set of Commands covered by one Doorbell. Identified by one Seqno and tracked by exactly one Fence.
_Avoid_: batch, job

**Doorbell**:
The VGPU_CS ioctl that tells the Engine new Commands exist at a Ring offset. On real hardware this would be an MMIO write to a register.
_Avoid_: submit, kick (as nouns; the verb "submit" is fine)

**Seqno**:
A monotonically increasing 32-bit number per device, assigned at Doorbell time. The app's handle for a Submission.

**Fence**:
The per-Submission dma_fence, signaled when the Engine finishes the Submission. Waited on by the VGPU_WAIT ioctl in kernel context; userspace never polls it.

### Memory

**BO**:
A shmem-backed GEM buffer object — one app-visible VRAM allocation, referenced by Commands through a GEM handle.
_Avoid_: buffer, memory, vram

**VRAM**:
The collective of all BOs. Emulated on system memory (shmem), not real video memory.

**Managed range**:
A VA range bound with VGPU_VM_BIND_MANAGED: one address space the CPU and the device share, where residency is decided by the access pattern rather than by the submission. A kernel launched by VGPU_PKT_LAUNCH_VA resolves its LD/ST through the submitting context's page tables and a non-resident page is migrated in on demand — the缺页 model of a CPU, on the device (CUDA calls this Unified Memory).
_Avoid_: managed memory (a range is what is managed, not memory), UVM as a verb

### Lifecycle

**Reset**:
Stop the Engine, drain the Ring, and mark outstanding Fences with an error. Triggered by the error path or by rmmod.

### Ops

**Guest**:
The QEMU/KVM VM running the KASAN kernel and vgpu. The only place this project ever executes.

**Host**:
xuefeiz2-server, the production vLLM machine. Zero-touch constraint: no modules, no reboot, no changes — the project is visible to the Host only as a QEMU process.

**Initramfs**:
The busybox rootfs carrying vgpu.ko, the test binaries, and /init. Rebuilt on change; the Guest has no disk.
