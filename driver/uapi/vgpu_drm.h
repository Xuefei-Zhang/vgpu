/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * vgpu - userspace ABI for the virtual GPU command submission driver.
 *
 * The ABI follows the modern (>= 6.1) DRM GEM conventions:
 *  - buffer creation is a driver-specific ioctl returning a GEM handle
 *    plus a mmap_offset (mmap() the fd at that offset),
 *  - command submission is a driver-specific "CS" ioctl (the doorbell),
 *  - completion is waited on in kernel context via a dma_fence.
 */
#ifndef _UAPI_VGPU_DRM_H_
#define _UAPI_VGPU_DRM_H_

#ifdef __KERNEL__
#include <uapi/drm/drm.h>
#else
#include "drm.h"	/* libdrm's drm.h for userspace builds */
#endif

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * One "GPU command". 64 bytes. The app writes commands into the
 * mmap'ed ring (see VGPU_RING_GET), then rings the doorbell
 * with VGPU_CS.
 */
struct vgpu_cmd {
	__u32 opcode;	/* VGPU_OP_* */
	__u32 pad;
	__u32 src_bo;	/* GEM handle of the source BO (BLIT) */
	__u32 dst_bo;	/* GEM handle of the destination BO (BLIT) */
	__u32 src_off;	/* byte offset into src BO, page aligned */
	__u32 dst_off;	/* byte offset into dst BO, page aligned */
	__u32 length;	/* bytes, page aligned, <= 64K */
	__u32 set_off;	/* (SET) byte offset into the MMIO region */
	__u64 set_data;	/* (SET) value the "GPU" writes into MMIO */
	__u32 pad2[2];
};

enum vgpu_opcodes {
	VGPU_OP_NOP	= 0,	/* no-op; burns a few us so it shows in ftrace */
	VGPU_OP_BLIT	= 1,	/* DMA copy: src_bo@src_off -> dst_bo@dst_off */
	VGPU_OP_SET	= 2,	/* write set_data into the MMIO region at set_off */
	VGPU_OP_LAUNCH	= 3,	/* run a compute kernel (phase D: grid x block) */
	/*
	 * Phase C: a DMA between two GPU VIRTUAL addresses. The driver
	 * resolves both through the submitting fd's page tables, which is
	 * the same walk a GPU's GCMC/SMMU performs — this is the command
	 * that makes per-VMID isolation observable from userspace.
	 */
	VGPU_OP_VM_COPY	= 4,
};

/*
 * The vgpu "compute ISA" — deliberately tiny (7 instructions). Not a
 * real SASS/PTX: the point is to learn what a KERNEL LAUNCH is
 * (grid/block/warps, register file, memory ops) and what the driver
 * does with one, not to build a compiler.
 *
 * Register file: 16 x u32 per thread. r0 is preloaded with the global
 * thread id. Memory: the kernel's argument BO, word addressed; LD/ST
 * address = (reg[a] + b) as a WORD index into that BO.
 */
enum vgpu_isa_op {
	VGPU_ISA_MOVI	= 0,	/* rd = a (immediate) */
	VGPU_ISA_ADD	= 1,	/* rd = reg[a] + reg[b] */
	VGPU_ISA_MUL	= 2,	/* rd = reg[a] * reg[b] */
	VGPU_ISA_FMA	= 3,	/* rd = reg[a] * reg[b] + reg[c(dst)]... see layout */
	VGPU_ISA_LD	= 4,	/* rd = mem[(reg[a] + b) words] */
	VGPU_ISA_ST	= 5,	/* mem[(reg[a] + b) words] = reg[dst] */
	VGPU_ISA_EXIT	= 6,
};

/* one instruction: 16 bytes */
struct vgpu_instr {
	__u32 op;
	__u32 dst;
	__u32 a;
	__u32 b;
};

/* VGPU_MODULE_LOAD: register a kernel (the "compute module"). */
struct vgpu_module_load {
	__u32 nregs;		/* in, registers per thread (<= 16) */
	__u32 smem;		/* in, shared memory per block (<= 4096) */
	__u32 ninstr;		/* in, number of instructions */
	__u32 kid;		/* out, kernel id within this fd */
	__u64 code;		/* in, userspace pointer: ninstr * 16 bytes */
};

/* VGPU_MODULE_DESTROY: retire a kernel. */
struct vgpu_module_destroy {
	__u32 kid;		/* in */
	__u32 pad;
};

/*
 * Phase C: the GPU virtual memory interface.
 *
 * Every fd is one GPU context and gets its own address space with its
 * own VMID (a real GPU cannot describe the same VA twice — the VMID is
 * what a memory access is tagged with). A VA range is first RESERVED
 * (VM_ALLOC) and then a buffer is attached to it (VM_BIND); the pages
 * become reachable to the device only through that fd's page tables.
 *
 *   cudaMalloc        ->  VGPU_BO_CREATE     (host-side shmem BO)
 *   + VM_ALLOC        ->  a GPU VA in my context
 *   + VM_BIND         ->  the device can now dereference that VA
 *   VGPU_PKT_VM_COPY  ->  the walk happens, per page
 *
 * Choosing handle-vs-VA is the point: the SAME address in two processes
 * translatesto different memory because each VMID has its own tables,
 * and an address nobody bound is a fault, not garbage.
 */
struct vgpu_vm_alloc {
	__u64 size;		/* in, bytes, rounded up to the page size */
	__u64 align;		/* in, 0 or a power of two >= PAGE_SIZE */
	__u64 va;		/* out, the start of the reserved range */
	__u32 vmid;		/* out, this fd's VMID (assigned on first use) */
	__u32 pad;
};

/* flags: bit0 = managed (UVM-lite demand migration, phase E). */
#define VGPU_VM_BIND_MANAGED	BIT(0)

struct vgpu_vm_bind {
	__u64 va;		/* in, a VA returned by VM_ALLOC */
	__u32 bo_handle;	/* in, the GEM handle to attach there */
	__u32 flags;		/* in, VGPU_VM_BIND_* */
};

/* Detach the buffer at va. The VA range itself stays reserved. */
struct vgpu_vm_unbind {
	__u64 va;		/* in */
	__u32 pad;
};

/*
 * The device's "engines". A real GPGPU card exposes one command processor
 * per function (NVIDIA: GR + COPY + NVDEC; AMD: gfx ring + sdma + vcn);
 * a queue is an in-order stream bound to one engine, which is exactly
 * what userspace calls a stream (cudaStreamCreate).
 */
enum vgpu_engines {
	VGPU_ENGINE_GFX		= 0,	/* the general engine: NOP/BLIT/SET */
	VGPU_ENGINE_COPY	= 1,	/* DMA engine: BLIT only */
	VGPU_ENGINE_COMPUTE	= 2,	/* compute engine: NOP/SET (LAUNCH: phase D) */
	VGPU_ENGINE_COUNT	= 3,
};

/* VGPU_QUEUE_CREATE: an in-order command stream on one engine. */
struct vgpu_queue_create {
	__u32 engine;		/* in, enum vgpu_engines */
	__u32 prio;		/* in, 0 = low, 1 = normal, 2 = high */
	__u32 nslots;		/* in, ring slots, 16..64 */
	__u32 qid;		/* out, queue id within this fd */
	__u64 ring_size;	/* out, bytes of the queue's command ring */
	__u64 mmap_offset;	/* out, mmap() the ring at this offset */
};

/* VGPU_QUEUE_DESTROY: retire a queue (must have no submissions in flight). */
struct vgpu_queue_destroy {
	__u32 qid;		/* in */
	__u32 pad;
};

/* VGPU_BO_CREATE: create a shmem-backed GEM object ("VRAM" allocation). */
struct vgpu_bo_create {
	__u64 size;		/* in, bytes, >= 4K */
	__u64 align;		/* in, must be 0 (reserved) */
	__u64 flags;		/* in, must be 0 (reserved) */
	__u32 handle;		/* out, GEM handle */
	__u32 pad;
	__u64 mmap_offset;	/* out, for mmap() */
};

/* VGPU_RING_GET: get a per-fd handle to the device command ring. */
struct vgpu_ring_get {
	__u32 handle;		/* out */
	__u32 pad;
	__u64 mmap_offset;	/* out, for mmap() */
};

/*
 * Packets: what the queue's ring actually holds once submissions
 * become packet streams (phase B). 40 bytes each, so a 4096B ring
 * holds 102 of them. Note the three sizes that are easy to confuse:
 * a ring SLOT is 64B (VGPU_CMD_SIZE), a decoded command struct is
 * 48B, and a packet is 40B — the scratch stride is the slot size. OP_COPY is the old BLIT, OP_WRITE_REG the old
 * SET. The doorbell (VGPU_DOORBELL) says how far the writer got; the
 * engine fetches and decodes from there.
 */
enum vgpu_packet_type {
	VGPU_PKT_NOP		= 0,
	VGPU_PKT_COPY		= 1,	/* BO->BO DMA */
	VGPU_PKT_WRITE_REG	= 2,	/* write into the MMIO region */
	VGPU_PKT_LAUNCH		= 3,	/* run a compute kernel */
	VGPU_PKT_VM_COPY	= 4,	/* GPU-VA -> GPU-VA DMA (phase C) */
};

struct vgpu_packet {
	__u32 type;		/* enum vgpu_packet_type */
	union {
		struct {
			__u32 src_bo;	/* GEM handle of the source */
			__u64 dst_bo;	/* GEM handle of the destination */
			__u32 off;	/* page-aligned offset in both BOs */
			__u32 len;	/* bytes, page aligned, <= 64K */
		} copy;
		struct {
			__u32 reg_off;	/* byte offset in the MMIO region */
			__u32 pad;
			__u64 reg_val;	/* value */
		} reg;
		/*
		 * LAUNCH: the kernel descriptor a GPGPU driver receives
		 * for every kernel launch (grid/block dims, the argument
		 * buffer, the kernel to run). The argument buffer is
		 * named by a GEM handle: it is a pool buffer, and the
		 * phase-C way to address one by GPU VA is VM_BIND +
		 * VGPU_PKT_VM_COPY, which is the path that has to go
		 * through the submitter's page tables.
		 */
		struct {
			__u32 kid;	/* kernel id (VGPU_MODULE_LOAD) */
			__u32 grid;	/* blocks in the grid */
			__u32 block;	/* threads per block (<= 256) */
			__u32 args_bo;	/* GEM handle of the argument buffer */
			__u32 args_off;	/* byte offset of the args inside it */
			__u32 pad[3];
		} launch;
		/*
		 * Phase C: VA -> VA DMA. Both addresses are GPU virtual
		 * addresses in the submitting fd's address space; the
		 * driver walks that fd's page tables to find the pages.
		 */
		struct {
			__u64 src_va;	/* page-aligned source VA */
			__u64 dst_va;	/* page-aligned destination VA */
			__u32 len;	/* bytes, page aligned, <= 256K */
			__u32 pad[3];
		} vm_copy;
	};
};

/* VGPU_DOORBELL: "the writer has produced packets up to put". */
struct vgpu_doorbell {
	__u32 qid;		/* in, target queue */
	__u32 put;		/* in, byte offset in the queue ring */
	__u32 seqno;		/* out, sequence number for VGPU_WAIT */
	__u32 pad;
};

/* VGPU_CS: the doorbell. Execute ncmds commands starting at ring_off. */
struct vgpu_cs {
	__u64 ring_off;	/* in, byte offset of the first command, 64B aligned */
	__u32 ncmds;		/* in, number of commands (>= 1) */
	__u32 seqno;		/* out, sequence number of this submission */
	__u32 qid;		/* in, target queue; 0 = this fd's default queue */
};

/* VGPU_WAIT: wait for a submission to complete (kernel-side fence wait). */
struct vgpu_wait {
	__u32 seqno;		/* in */
	__u32 timeout_ms;	/* in, 0 = wait forever */
	__s32 status;		/* out: 0 done, -ETIMEDOUT, -EIO, -ENOENT */
	__u32 pad;
};

/* VGPU_QUERY: device statistics and live MMIO register values. */
struct vgpu_query {
	__u32 submitted;	/* out */
	__u32 completed;	/* out */
	__u32 errors;		/* out */
	__u32 ring_used;	/* out, bytes of ring currently claimed */
	__u32 r_status;		/* out, MMIO status register */
	__u32 r_seqno;		/* out, MMIO seqno register */
	__u32 r_error;		/* out, MMIO error register */
	__u32 r_user;		/* out, MMIO user register (SET target) */
	__u32 pad;
};

#define VGPU_IOC(nr)		(DRM_COMMAND_BASE + (nr))

#define DRM_VGPU_BO_CREATE	0x01
#define DRM_VGPU_RING_GET	0x02
#define DRM_VGPU_CS		0x03
#define DRM_VGPU_WAIT		0x04
#define DRM_VGPU_QUERY		0x05
#define DRM_VGPU_QUEUE_CREATE	0x06
#define DRM_VGPU_QUEUE_DESTROY	0x07
#define DRM_VGPU_DOORBELL	0x08
#define DRM_VGPU_MODULE_LOAD	0x09
#define DRM_VGPU_MODULE_DESTROY	0x0a
#define DRM_VGPU_VM_ALLOC	0x0b
#define DRM_VGPU_VM_BIND	0x0c
#define DRM_VGPU_VM_UNBIND	0x0d

#define DRM_IOCTL_VGPU_VM_ALLOC		DRM_IOWR(VGPU_IOC(DRM_VGPU_VM_ALLOC), struct vgpu_vm_alloc)
#define DRM_IOCTL_VGPU_VM_BIND		DRM_IOWR(VGPU_IOC(DRM_VGPU_VM_BIND), struct vgpu_vm_bind)
#define DRM_IOCTL_VGPU_VM_UNBIND	DRM_IOWR(VGPU_IOC(DRM_VGPU_VM_UNBIND), struct vgpu_vm_unbind)

#define DRM_IOCTL_VGPU_BO_CREATE	DRM_IOWR(VGPU_IOC(DRM_VGPU_BO_CREATE), struct vgpu_bo_create)
#define DRM_IOCTL_VGPU_RING_GET		DRM_IOWR(VGPU_IOC(DRM_VGPU_RING_GET), struct vgpu_ring_get)
#define DRM_IOCTL_VGPU_CS		DRM_IOWR(VGPU_IOC(DRM_VGPU_CS), struct vgpu_cs)
#define DRM_IOCTL_VGPU_WAIT		DRM_IOWR(VGPU_IOC(DRM_VGPU_WAIT), struct vgpu_wait)
#define DRM_IOCTL_VGPU_QUERY		DRM_IOWR(VGPU_IOC(DRM_VGPU_QUERY), struct vgpu_query)
#define DRM_IOCTL_VGPU_QUEUE_CREATE	DRM_IOWR(VGPU_IOC(DRM_VGPU_QUEUE_CREATE), struct vgpu_queue_create)
#define DRM_IOCTL_VGPU_QUEUE_DESTROY	DRM_IOWR(VGPU_IOC(DRM_VGPU_QUEUE_DESTROY), struct vgpu_queue_destroy)
#define DRM_IOCTL_VGPU_DOORBELL		DRM_IOWR(VGPU_IOC(DRM_VGPU_DOORBELL), struct vgpu_doorbell)
#define DRM_IOCTL_VGPU_MODULE_LOAD	DRM_IOWR(VGPU_IOC(DRM_VGPU_MODULE_LOAD), struct vgpu_module_load)
#define DRM_IOCTL_VGPU_MODULE_DESTROY	DRM_IOWR(VGPU_IOC(DRM_VGPU_MODULE_DESTROY), struct vgpu_module_destroy)

#if defined(__cplusplus)
}
#endif

#endif /* _UAPI_VGPU_DRM_H_ */
