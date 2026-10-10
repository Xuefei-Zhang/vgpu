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
	__u32 pad2[2];		/* (SET/SIGNAL/WAIT/EVENT) see the opcodes */
};

enum vgpu_opcodes {
	VGPU_OP_NOP	= 0,	/* no-op; burns a few us so it shows in ftrace */
	VGPU_OP_BLIT	= 1,	/* DMA copy: src_bo@src_off -> dst_bo@dst_off */
	VGPU_OP_SET	= 2,	/* write set_data into the MMIO region at set_off */
	VGPU_OP_LAUNCH	= 3,	/* run a compute kernel (phase D: grid x block) */
	/*
	 * Phase G: the memory-semantics synchronization vocabulary. These
	 * three operate on a *semaphore page* (see VGPU_SEM_BIND), not on
	 * the MMIO region: the "device" writes a word the CPU can poll,
	 * instead of the CPU asking the kernel to wait for it.
	 *
	 * They exist only on the packet path (VGPU_DOORBELL / VGPU_GRAPH):
	 * the legacy 64B command has no field that names the semaphore
	 * page, so VGPU_CS rejects them (see vgpu_cs_ioctl).
	 *
	 * In the decoded command struct the operands reuse the SET fields:
	 * set_off = byte offset of the word in the semaphore page,
	 * set_data = the value (written, or waited for).
	 */
	VGPU_OP_SIGNAL_TIMELINE	= 4,	/* word = value */
	VGPU_OP_WAIT_TIMELINE	= 5,	/* stall until word >= value */
	VGPU_OP_WRITE_EVENT	= 6,	/* event slot = value */
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
 * VGPU_SEM_BIND: install this fd's semaphore page (phase G).
 *
 * The app allocates a shmem BO ("VRAM"), mmaps it, and hands the driver
 * its handle + the byte offset of the timeline word. From then on the
 * driver treats that word as the context's timeline value: the engine
 * writes it (SIGNAL_TIMELINE) and stalls on it (WAIT_TIMELINE), while
 * the CPU just reads the mmap. This is the whole of "memory-semantics
 * synchronization": the completion lives in memory both sides can see,
 * not in a syscall.
 *
 * The binding is validated once here (handle must exist, offset must be
 * a word inside the BO); the packets carry the handle so a submission
 * is self-describing, but the driver rejects any handle that is not
 * the bound one — a GPU packet naming undeclared memory is a bug.
 */
struct vgpu_sem_bind {
	__u32 bo_handle;	/* in, the semaphore page */
	__u32 sem_off;		/* in, byte offset of the timeline word */
	__u32 bound;		/* out, 1 once bound */
	__u32 pad;
};

/*
 * VGPU_GRAPH: submit a DAG of nodes with ONE doorbell (phase G).
 *
 * A node is one unit of work (COPY / LAUNCH / NOP / a timeline signal).
 * The dependency edges say which nodes must retire first; the driver
 * topologically sorts them at submit time and the engine runs them in
 * that order, signalling ONE fence at the end.
 *
 * The point being measured: N separate doorbells become 1 syscall, and
 * the dependency structure becomes explicit data instead of "submit
 * them in this order and hope".
 *
 * Edges may be spelled two equivalent ways — as predecessor lists
 * inside the node descriptors (ndeps/deps[]), or as an explicit
 * (from, to) array. Give one or the other.
 */
#define VGPU_GRAPH_MAX_NODES	16
#define VGPU_GRAPH_MAX_DEPS	3
#define VGPU_GRAPH_MAX_EDGES	32

struct vgpu_graph_node {
	__u32 type;			/* enum vgpu_packet_type (work nodes) */
	__u32 ndeps;			/* how many deps[] are used */
	__u32 deps[VGPU_GRAPH_MAX_DEPS];	/* node indices that run first */
	union {
		struct {
			__u32 src_bo;	/* GEM handle of the source */
			__u32 dst_bo;	/* GEM handle of the destination */
			__u32 off;	/* page-aligned offset in both BOs */
			__u32 len;	/* bytes, page aligned, <= 64K */
		} copy;
		struct {
			__u32 kid;	/* kernel id (VGPU_MODULE_LOAD) */
			__u32 grid;
			__u32 block;
			__u32 args_bo;	/* GEM handle of the argument buffer */
			__u32 args_off;
		} launch;
		struct {
			__u32 sem_bo;	/* GEM handle of the semaphore page */
			__u32 sem_off;	/* byte offset of the word */
			__u32 pad;
			__u64 value;
		} sem;
	};
};

struct vgpu_graph_edge {
	__u32 from;			/* node index that must retire first */
	__u32 to;			/* node index that runs after it */
};

struct vgpu_graph_submit {
	__u64 nodes;		/* in: struct vgpu_graph_node[nnodes] */
	__u64 edges;		/* in: struct vgpu_graph_edge[nedges], or 0 */
	__u32 qid;		/* in, target queue */
	__u32 nnodes;		/* in, 1..VGPU_GRAPH_MAX_NODES */
	__u32 nedges;		/* in, 0..VGPU_GRAPH_MAX_EDGES */
	__u32 seqno;		/* out, one fence for the whole graph */
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
	/*
	 * Phase G: memory-semantics sync. All three name a word in the
	 * semaphore page bound by VGPU_SEM_BIND. Word 0 of that page is
	 * conventionally the timeline value; any other word can serve as
	 * an event slot. The engine writes the word (SIGNAL/WRITE_EVENT)
	 * or stalls on it (WAIT) — and userspace discovers all of it by
	 * POLLING THE MMAP, with no ioctl in the completion path.
	 */
	VGPU_PKT_SIGNAL_TIMELINE	= 4,
	VGPU_PKT_WAIT_TIMELINE		= 5,
	VGPU_PKT_WRITE_EVENT		= 6,
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
		 * buffer, the kernel to run).
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
		 * Phase G sync operands: which word of the bound
		 * semaphore page, and the value. The handle is carried
		 * per packet (that is what a GPU VA / semaphore address
		 * is in a real packet) and must name the page the
		 * context bound with VGPU_SEM_BIND.
		 */
		struct {
			__u32 sem_bo;	/* GEM handle of the semaphore page */
			__u32 sem_off;	/* byte offset of the word (4-aligned) */
			__u32 pad;
			__u64 value;	/* written (SIGNAL/EVENT) or awaited */
		} timeline;
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
	/* phase G: counters for the memory-semantics vocabulary */
	__u32 graphs;		/* out, graphs submitted (one doorbell each) */
	__u32 signals;		/* out, timeline values written by the engine */
	__u32 waits;		/* out, timeline waits executed */
	__u32 events;		/* out, event slots written */
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
#define DRM_VGPU_SEM_BIND	0x0b
#define DRM_VGPU_GRAPH		0x0c

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
#define DRM_IOCTL_VGPU_SEM_BIND		DRM_IOWR(VGPU_IOC(DRM_VGPU_SEM_BIND), struct vgpu_sem_bind)
#define DRM_IOCTL_VGPU_GRAPH		DRM_IOWR(VGPU_IOC(DRM_VGPU_GRAPH), struct vgpu_graph_submit)

#if defined(__cplusplus)
}
#endif

#endif /* _UAPI_VGPU_DRM_H_ */
