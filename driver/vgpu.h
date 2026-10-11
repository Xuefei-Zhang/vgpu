/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vgpu - internal driver structures and helpers.
 *
 * Memory model (see docs/DESIGN.md):
 *  - Ring    = one shmem GEM BO (4K = 64 x 64B slots), written by the app
 *  - scratch = coherent DMA area where submitted commands are fetched
 *  - regs    = coherent DMA area emulating the BAR0 register file
 */
#ifndef _VGPU_DRV_H_
#define _VGPU_DRV_H_

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dma-fence.h>
#include <linux/highmem.h>
#include <linux/idr.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

#include <drm/drm_drv.h>
#include <drm/drm_gem_shmem_helper.h>

#include "uapi/vgpu_drm.h"

#define VGPU_RING_SIZE		4096	/* one page: 64 x 64B command slots */
#define VGPU_CMD_SIZE		64
#define VGPU_RING_SLOTS		(VGPU_RING_SIZE / VGPU_CMD_SIZE)
#define VGPU_SCRATCH_SIZE	(16 * 1024)	/* command fetch area */
#define VGPU_FENCE_TIMEOUT	(10 * HZ)	/* hang-detection backstop */

/*
 * The SIMT geometry (phase D). Both live here, not in vgpu_compute.c,
 * because phase F's context switch saves a warp's register file and
 * that struct is part of struct vgpu_submission.
 */
#define VGPU_WARP		32	/* lanes per warp */
#define VGPU_NREGS		16	/* registers per thread */

/* virtual BAR0: the MMIO register file (coherent DMA) */
struct vgpu_regs {
	u32 status;
	u32 seqno;
	u32 error;
	u32 user;
};
#define VGPU_STATUS_DONE	BIT(0)
#define VGPU_STATUS_ERR	BIT(1)

/*
 * A dma_fence for one submission. Standalone object: freed by its own
 * release callback once the last reference (idr + waiters) is dropped.
 */
struct vgpu_fence {
	struct dma_fence base;
	spinlock_t lock;
	u32 seqno;
	unsigned long created_jiffies;	/* for the watchdog */
};

/*
 * What the scheduler decided at the last safe point of a run (phase F).
 * VGPU_SCHED_DONE doubles as "no yield requested" — the check runs at
 * every instruction boundary and the executor only stops on the other
 * two.
 */
enum vgpu_sched {
	VGPU_SCHED_DONE = 0,	/* run on / the submission finished */
	VGPU_SCHED_TIMESLICE,	/* this slice is spent: round-robin yield */
	VGPU_SCHED_PREEMPT,	/* a higher-priority context is waiting */
};

/*
 * One warp's register file: 32 lanes x 16 registers plus the per-lane
 * EXIT mask. This is what a context switch saves and restores — the
 * driver-side analogue of the wavefront state a real driver writes into
 * the context's MQD before it can let another channel run.
 */
struct vgpu_warp_frame {
	u32 regs[VGPU_WARP][VGPU_NREGS];
	bool done[VGPU_WARP];
};

/*
 * The resumable state of an in-flight kernel launch (phase F).
 *
 * A launch no longer runs to completion inside one work item: it can be
 * paused at any instruction boundary and resumed later, possibly after
 * another context's kernel has run in between. Everything needed to
 * continue is here:
 *   block / warp  which block and warp the engine is dispatching
 *   pc / resuming the instruction to resume at, and whether the
 *                 register file holds a partially executed warp
 *   slice_blocks  blocks executed since the launch last had the engine
 *                 (the timeslice accounting)
 *   fw            the saved register file, NULL once the grid retired
 */
struct vgpu_launch {
	u32 block;
	u32 warp;
	u32 pc;
	bool started;		/* the register file is allocated */
	bool resuming;		/* warp+pc hold live state: resume mid-warp */
	u32 slice_blocks;	/* blocks executed since the last switch */
	u64 start_ns;		/* when this slice of execution began */
	struct vgpu_warp_frame *fw;
};

/*
 * A resolved BLIT: the handles became objects (pinned), the pages are
 * captured, and the range inside them is recorded. The doorbell does
 * this once per COPY packet; a graph node uses the very same struct,
 * which is why it is shared instead of living in the submission.
 */
struct vgpu_blit_ref {
	struct drm_gem_object *src_obj, *dst_obj;
	/*
	 * The range's pages: exactly npages entries, starting at the
	 * range's first page (blit_off) — so consumers index them from
	 * 0 and never add blit_off again.
	 */
	struct page **src_pages, **dst_pages;
	u32 npages;
	u32 blit_off;		/* page index of the range inside the BOs */
};

/*
 * One graph node, fully resolved by the GRAPH ioctl: handles became
 * objects (pinned), the kernel id became a struct vgpu_kernel. The
 * engine just walks order[] and executes these.
 */
struct vgpu_gnode {
	u32 type;			/* enum vgpu_packet_type */
	struct vgpu_blit_ref blit;		/* VGPU_PKT_COPY */
	struct vgpu_kernel *kernel;		/* VGPU_PKT_LAUNCH */
	struct drm_gem_object *args_obj;	/* pinned argument buffer */
	u32 args_off, grid, block;
	u32 sem_off;				/* sync packets */
	u64 value;
};

/*
 * One doorbell. Three shapes:
 *  - CS ioctl (legacy): commands are fetched into scratch at submit
 *    time, ncmds set, npkts == 0.
 *  - DOORBELL ioctl (phase B): carries a packet RANGE in the queue
 *    ring; the engine fetches and decodes it, setting ncmds from npkts.
 *  - GRAPH ioctl (phase G): gnodes/order carry resolved nodes; the
 *    queue's ring is not touched at all — one doorbell for the DAG.
 */
struct vgpu_submission {
	struct list_head node;		/* engine_pending[] or in_flight */
	struct vgpu *v;		/* back-pointer for the per-sub finish work */
	struct vgpu_queue *q;	/* the queue it came from (ring accounting) */
	u32 seqno;
	u32 engine;			/* engine the queue was bound to */
	u32 npkts;			/* packets left to fetch (0 once fetched) */
	u32 ncmds;			/* commands the engine will/did execute */
	u32 ring_off;		/* byte offset of the first packet */
	bool from_doorbell;		/* packet path vs legacy CS path */
	/*
	 * Ring bytes claimed (packets are 32B, commands are 64B — these
	 * two sizes are NOT interchangeable: mixing them up makes the
	 * ring watermark underflow on the packet path).
	 */
	u32 ring_bytes;
	u32 cmds_off;		/* offset inside the coherent scratch area */
	u32 error;		/* set by the engine, propagated to the fence */
	u32 finished;		/* cmpxchg guard: finished exactly once */
	unsigned long submit_jiffies;

	/* at most one BLIT per submission, resolved at submit time */
	struct vgpu_blit_ref blit;
	struct work_struct finish_work;	/* bottom half: one work per sub */

	/* compute: a LAUNCH packet resolved at submit time */
	struct vgpu_kernel *kernel;
	struct drm_gem_object *args_obj;	/* the argument buffer (pinned) */
	u32 args_off;
	u32 grid, block;
	/*
	 * Phase E: the VA-named launch (VGPU_PKT_LAUNCH_VA). The kernel's
	 * global memory is the MANAGED range [launch_va, launch_va +
	 * launch_size) in the submitting fd's address space, and every
	 * LD/ST is resolved through that context's page tables — so the
	 * submission has to carry the context itself, which is what the
	 * engine (workqueue context, no fd) would otherwise have no way
	 * to learn.
	 *
	 * @vm is BORROWED from vf->vm: the submission never outlives it,
	 * because vgpu_postclose() flushes every engine workqueue before
	 * destroying the fd's address space, and the rmmod path drains
	 * first too (see vgpu_drain). Taking a reference on the object
	 * behind the range is what keeps the PAGES alive, not the VM.
	 */
	struct vgpu_vm *vm;			/* the submitting fd's VM */
	u64 launch_va;				/* 0: handle-named launch */
	u64 launch_size;			/* bytes of the managed range */
	struct drm_gem_object *va_obj;		/* its BO, pinned (like args) */
	u32 va_faults;				/* demand faults this launch */
	/*
	 * phase F: the resumable launch state (NULL unless this submission
	 * carries a LAUNCH). While a launch is paused it stays on the
	 * in_flight list — its fence is NOT signaled, its ring bytes and
	 * its pinned argument buffer stay claimed — and cs carries the
	 * saved cursor the engine resumes from.
	 */
	struct vgpu_launch *cs;
	u32 cmd_idx;		/* next command to execute (the scheduler PC) */

	/*
	 * Phase G: the semaphore page (pinned for this submission, like
	 * the argument buffer) and, for a VGPU_GRAPH submission, the
	 * resolved node array. Everything the engine needs is resolved
	 * at submit time, so the graph path never touches a handle.
	 */
	struct drm_gem_object *sem_obj;		/* pinned semaphore page */
	struct vgpu_gnode *gnodes;		/* VGPU_GRAPH only */
	u32 *order;				/* topological node order */
	u32 ngnodes;
	bool is_graph;
	/*
	 * Which node of order[] the graph resumes at. Phase F made a
	 * LAUNCH resumable: a graph's launch node can be paused mid-grid
	 * and the engine re-picks the submission later, so the graph has
	 * to re-enter at the node it stopped inside — not back at node 0
	 * (which would re-run the copy-in and, worse, run the copy-out
	 * before the launch has written its output).
	 */
	u32 gnode_idx;

	/*
	 * Phase C: a DMA between GPU virtual addresses. Resolved at
	 * submit time, exactly like the BLIT above: the doorbell is the
	 * only place that has the drm_file (and therefore the VMID), so
	 * the page-table walk happens here rather than in the engine.
	 * The submission owns its own object + page references, which is
	 * what makes an unbind/eviction racing the engine harmless.
	 */
	struct drm_gem_object *vm_src_obj, *vm_dst_obj;
	struct page **vm_src_pages, **vm_dst_pages;
	u32 vm_npages;};

/*
 * A submission queue: one in-order command stream bound to one engine.
 * This is the driver-side analogue of a userspace stream
 * (cudaStreamCreate) — and, like real drivers, the queue owns its own
 * command ring instead of sharing one device-wide scratch buffer
 * (CONTEXT.md's "shared scratch" note was the deliberate simplification
 * this replaces).
 */
struct vgpu_queue {
	struct list_head node;		/* vgpu_file's queue list */
	struct list_head dev_node;	/* device-wide list (debugfs) */
	struct vgpu_file *file;		/* back-pointer for teardown */
	struct drm_gem_shmem_object *ring_bo;	/* the queue's command ring */
	u32 gem_handle;		/* this fd's handle (mmap allow-list only) */
	u32 qid;
	u32 engine;			/* enum vgpu_engines */
	u32 prio;			/* 0 low, 1 normal, 2 high */
	u32 ring_size;			/* bytes */
	u32 ring_used;			/* bytes claimed by in-flight work */
	/*
	 * The packet ring's read/write offsets (bytes, 32B aligned):
	 * userspace advances put (implicitly, by writing packets and then
	 * ringing the doorbell with the new put), the engine advances get
	 * once the fetch is done. This is the host-side queue descriptor a
	 * real GPU walks (MQD / GPFIFO entry) — in host memory, written by
	 * userspace without a syscall in the fast path.
	 */
	u32 get, put;
	u64 fence_context;		/* one timeline per queue */
	u64 stats_submitted, stats_completed, stats_errors;

	/*
	 * phase F: per-context compute accounting (debugfs "compute").
	 * This is the driver-side source of what an nvidia-smi-alike
	 * shows per process: how much engine time this context burned,
	 * and how often it was bounced off the engine.
	 */
	u64 cs_launches, cs_blocks, cs_time_ns;
	u64 cs_switches, cs_preempted;

	/* in-order: the last seqno handed out on this queue */
	u32 tail_seqno;
	bool destroyed;			/* set by QUEUE_DESTROY; drained */
};

/*
 * A loaded compute kernel: the "module" a launch references. The code
 * is our tiny ISA (see uapi); the driver is the loader/scheduler for
 * it, exactly like a real driver loads a compute module and schedules
 * its grids.
 */
struct vgpu_kernel {
	struct list_head node;
	struct list_head file_node;
	u32 kid;
	u32 nregs;
	u32 smem;
	u32 ninstr;
	struct vgpu_instr *code;	/* driver-side copy, ninstr entries */
};

/* ------------------------------------------------- phase C: the VM */

/*
 * Page-table geometry. Two levels, both kernel pages:
 *
 *   PDE page : 1024 x u32 entries, each the PFN of a PTE page.
 *              (va >> 22) selects the entry -> a PTE page covers 4M.
 *   PTE page : 1024 x u32 entries, each the PFN of a data page.
 *              (va >> 12) selects the entry.
 *
 * So the driver performs exactly the walk a GPU's GCMC (or an SMMU)
 * performs, and the reason a multi-page buffer needs a page table at
 * all is visible: the second level is per 4M of VA and is allocated
 * lazily, so a context that has bound one page still costs one page.
 */
#define VGPU_VM_PDE_ENTRIES	(PAGE_SIZE / sizeof(u32))
#define VGPU_VM_PTE_ENTRIES	(PAGE_SIZE / sizeof(u32))
#define VGPU_VM_PDE_SHIFT	(PAGE_SHIFT + 10)	/* 22 */
#define VGPU_VM_PDE_IDX(va)	(((va) >> VGPU_VM_PDE_SHIFT) & \
				 (VGPU_VM_PDE_ENTRIES - 1))
#define VGPU_VM_PTE_IDX(va)	(((va) >> PAGE_SHIFT) & \
				 (VGPU_VM_PTE_ENTRIES - 1))

#define VGPU_PTE_VALID		BIT(31)
#define VGPU_PTE_PFN_MASK	0x3fffffffu
#define VGPU_PDE_VALID		BIT(31)
#define VGPU_PDE_PFN_MASK	0x3fffffffu

/* The VA space handed out: 32-bit, and page 0 is never a target. */
#define VGPU_VM_VA_BASE		((u64)PAGE_SIZE)
#define VGPU_VM_VA_END		(1ULL << 32)

#define VGPU_VMID_MIN		1
#define VGPU_VMID_MAX		65535

/* The device's "VRAM" budget: a TTM-lite LRU, not a real allocator. */
#define VGPU_VM_VRAM_BUDGET	(16ULL * 1024 * 1024)
#define VGPU_VM_MAX_COPY_PAGES	64		/* 256K per VA copy */
/*
 * Phase E: the largest managed range a VA-named launch may name. A
 * launch walks its tables on every LD/ST, so the ceiling is both a
 * validation convenience and a statement that this is a teaching
 * driver: 64 pages is 16K demands of headroom for a saxpy-sized
 * argument buffer.
 */
#define VGPU_VM_MAX_MANAGED_PAGES	64

/*
 * One buffer mapped into a VM: the driver-side record of "bo#N is at
 * VA X in context Y". This is the VRAM manager's unit of residency
 * and eviction — mapped == the PTEs are installed right now.
 */
struct vgpu_vm_range {
	struct list_head node;		/* vm->ranges, address order */
	struct list_head lru_node;	/* v->vram_lru, MRU at the tail */
	u64 start;			/* VA, page aligned */
	u64 size;			/* bytes */
	struct vgpu_vm *vm;		/* owner: whose tables hold the PTEs */
	struct drm_gem_object *obj;	/* our reference (a handle's is its own) */
	u32 handle;			/* the fd handle it was bound with */
	bool mapped;			/* resident: PTEs installed */
	/*
	 * Phase E: bound with VGPU_VM_BIND_MANAGED. A managed range is
	 * part of the context's unified address space, so it is the only
	 * kind a VGPU_PKT_LAUNCH_VA may name: the kernel's LD/ST walk
	 * these tables and fault a page back in, which is the migration
	 * that makes "one address space for CPU and device" true rather
	 * than a slogan.
	 */
	bool managed;
};

/*
 * A per-fd GPU address space. This IS the GPU context's VM: one VMID
 * per fd, its own page tables, its own reserved VA ranges. The point
 * of the phase is that the same VA in two of these means different
 * memory, and the VMID is the tag a real memory access carries to say
 * which tables to walk.
 */
struct vgpu_vm {
	struct list_head node;		/* v->vm_list */
	struct list_head ranges;	/* struct vgpu_vm_range, address order */
	struct vgpu *v;
	struct vgpu_file *file;		/* the owning fd */
	u32 vmid;
	struct page *pde;		/* level-1 table: 1024 PTE pages */
	u64 resident;			/* bytes currently mapped in this VM */
	u32 nranges;
};

/* Per-fd state: every open() owns its queues (drm_file.driver_priv). */
struct vgpu_file {
	struct list_head queues;	/* struct vgpu_queue */
	struct list_head kernels;	/* struct vgpu_kernel */
	struct vgpu_queue *default_q;	/* lazily created GFX queue (qid 0) */
	struct vgpu_vm *vm;		/* phase C: this fd's GPU address space */
	struct vgpu *vf_dev;
	struct drm_file *drm_file;	/* for handle -> object lookups */
	u32 next_qid;
	u32 next_kid;
	/*
	 * Phase G: this context's semaphore page. Only the handle and
	 * the byte offset of the timeline word are remembered — no
	 * reference is taken here; each submission resolves (and pins)
	 * it the same way it resolves a BLIT buffer, so the object's
	 * lifetime stays tied to the submission, not to the fd.
	 */
	u32 sem_handle;
	u32 sem_off;
};

struct vgpu {
	struct platform_device *pdev;
	struct drm_device dev;

	/*
	 * "GPU engines": one ordered workqueue per engine kind, so work
	 * from different engines can run concurrently (one submission
	 * at a time per engine — exactly like a single GPU pipe).
	 */
	struct workqueue_struct *engine_wq[VGPU_ENGINE_COUNT];
	struct work_struct engine_work[VGPU_ENGINE_COUNT];
	struct list_head engine_pending[VGPU_ENGINE_COUNT];	/* to execute */

	/* bottom half: process completions, signal fences */
	struct workqueue_struct *irq_wq;
	struct work_struct watchdog_work;

	struct vgpu_regs *regs;		/* coherent: virtual BAR0 */
	dma_addr_t regs_dma;
	/*
	 * One coherent fetch area PER ENGINE, not one per device. With a
	 * single bump-pointer + a device-wide watermark, the reclamation
	 * order followed COMPLETION order, not ALLOCATION order: the
	 * moment two engines ran concurrently, engine A's bottom half
	 * could lower the watermark while engine B still had a live
	 * submission there, and the next submission overwrote B's
	 * commands (found the hard way: a compute submission mysteriously
	 * executed a BLIT another engine's rejected doorbell had left
	 * behind). Per-engine areas keep each engine's watermark FIFO —
	 * an engine is a single pipe, its own command RAM is the honest
	 * model.
	 */
	u8 *scratch[VGPU_ENGINE_COUNT];		/* coherent: fetched commands */
	dma_addr_t scratch_dma[VGPU_ENGINE_COUNT];
	u32 scratch_used[VGPU_ENGINE_COUNT];

	spinlock_t lock;		/* engine lists / seqno / scratch */
	struct list_head in_flight;	/* executing / waiting for "interrupt" */
	struct list_head queue_list;	/* every live queue, any fd (debugfs) */
	u32 next_seqno;		/* device-global submission id (fence idr key) */

	/* seqno -> fence, so VGPU_WAIT can find its fence */
	struct mutex fence_mutex;
	struct idr fence_idr;

	struct timer_list watchdog;	/* fence timeout backstop (10s) */

	u64 stats_submitted, stats_completed, stats_errors;
	struct dentry *dbg;

	/* phase G: memory-semantics synchronization counters */
	u64 stats_graphs, stats_signals, stats_waits, stats_events;

	/*
	 * Phase C: the "VRAM" manager. A device-wide byte budget with an
	 * LRU: a bind that does not fit evicts the least recently used
	 * mapping (unwind its PTEs, keep the shmem pages — the BO does
	 * not move, host memory IS its backing) and a later access maps
	 * it straight back. That is TTM's eviction model with CPU swap
	 * as the destination, and it is deliberately NOT a buddy
	 * allocator (docs/GPGPU-EVOLUTION-PLAN.md §6).
	 *
	 * A mutex and not a spinlock, unlike the other locks here:
	 * walking the tables can allocate a PTE page, and the fix for the
	 * sleeping-in-spinlock bug in this repo's history is to not be in
	 * one. Phase E made the compute engine a client of the walk (see
	 * vgpu_vm_loan): that is safe precisely because an engine is a
	 * workqueue context, where sleeping is legal, and it takes the
	 * lock for one page at a time rather than for a whole launch.
	 */
	struct mutex vm_lock;
	struct list_head vm_list;	/* every live struct vgpu_vm */
	struct list_head vram_lru;	/* mapped ranges, MRU at the tail */
	struct ida vm_ida;		/* VMID allocation */
	u64 vram_budget, vram_resident;
	u64 stats_evicted;		/* ranges unwound to the host */
	u64 stats_vm_faults;		/* VAs resolved through a demand re-map */
	u64 stats_remap_pages;		/* pages re-installed by those faults */
	u64 stats_bound;		/* ranges bound since boot */
	/*
	 * Phase E: demand faults serviced by the COMPUTE engine, i.e. a
	 * kernel's own LD/ST finding a non-resident page and the driver
	 * migrating it in. They also bump phase C's stats_vm_faults (the
	 * migration is the same code path) — this counter is the
	 * engine-side view of it, so a debugfs reader can tell a VA->VA
	 * DMA that faulted from a kernel that faulted.
	 */
	u64 cs_vm_faults;
	/* compute statistics (debugfs "compute") */
	u64 cs_kernels_launched, cs_blocks, cs_warps, cs_threads;
	u64 cs_instrs, cs_mem_ops, cs_waves;
	u64 cs_coalesced_warps, cs_bcast_warps, cs_uncoalesced_warps;
	u64 cs_mem_lat_ns;		/* modelled memory latency, ns */

	/*
	 * Phase I: runtime power management, DVFS and per-context
	 * accounting. clock_hz is the engine clock in MHz (writable
	 * debugfs knob); the compute engine's modelled memory latency
	 * scales inversely with it, which is what DVFS means for kernel
	 * time. rpm_* records the power-gate history: how many times the
	 * device fell asleep and what the last wake cost.
	 */
	u32 clock_hz;
	u32 rpm_suspends;
	u64 rpm_last_resume_ns;
	struct list_head kernel_list;	/* every live kernel (debugfs) */

	/*
	 * phase F: the scheduler's counters. cs_ctx_switches counts
	 * every time an engine gave its pipe up at a safe point;
	 * cs_preemptions / cs_timeslices split that by reason.
	 */
	u64 cs_ctx_switches, cs_preemptions, cs_timeslices;
};

#define to_vgpu(drm_dev) container_of(drm_dev, struct vgpu, dev)

/* phase D's SM geometry lives in vgpu_compute.c; the warp register
 * file above needs these two at struct-definition time. */
#define VGPU_NREGS_MAX		16

/* the compute engine's memory-latency model and its reference clock */
#define VGPU_MEM_LAT_NS		40
#define VGPU_REF_CLOCK_HZ	1000	/* the clock VGPU_MEM_LAT_NS is quoted at */

/* how long the engine stalls on a WAIT_TIMELINE before giving up */
#define VGPU_TIMELINE_WAIT_MS	2000

/* word-addressed access into a pinned BO (vgpu_sync.c, vgpu_compute.c) */
#define VGPU_WORDS_PER_PAGE	(PAGE_SIZE / sizeof(u32))

/*
 * vgpu_vm.c — phase C: per-fd GPU address space, the two-level
 * software page table walk, and the VRAM budget/LRU eviction.
 * All of it runs with v->vm_lock held (a mutex: the walk allocates).
 */
struct vgpu_vm *vgpu_vm_get(struct vgpu_file *vf);
void vgpu_vm_destroy(struct vgpu_file *vf);
/* This fd's VMID, or 0 if it has no address space yet. */
u32 vgpu_vm_vmid(struct vgpu_file *vf);
int vgpu_vm_alloc(struct vgpu_file *vf, u64 size, u64 align, u64 *va_out);
int vgpu_vm_bind(struct vgpu_file *vf, u64 va, u32 handle, u32 flags);
void vgpu_vm_unbind(struct vgpu_file *vf, u64 va);
/*
 * Resolve npages of GPU VA in this fd. On success the caller owns the
 * returned object reference and one reference on each page — the same
 * "pin at submit" rule the BLIT path follows, which is what keeps a
 * concurrent unbind or eviction from pulling the pages away.
 */
int vgpu_vm_resolve(struct vgpu_file *vf, u64 va, u32 npages,
		    struct drm_gem_object **objp, struct page **pages);
/*
 * Phase E: one word of GPU VA, through the tables, for the compute
 * engine. Resolves the page (faulting it back in if it is not
 * resident), takes references on the page and on the buffer behind it,
 * and reports whether the access needed a migration. The caller touches
 * @page and then returns it with vgpu_vm_unloan() — the "loan" is
 * exactly as long as one LD/ST, which is why a launch can be paused
 * and resumed (phase F) without holding a page across a context
 * switch.
 */
struct vgpu_va_page {
	struct drm_gem_object *obj;	/* the range's buffer, referenced */
	struct page *page;		/* referenced: the word lives here */
	bool migrated;			/* the driver just migrated it in */
};
int vgpu_vm_loan(struct vgpu_vm *vm, u64 va, struct vgpu_va_page *out);
void vgpu_vm_unloan(struct vgpu_va_page *out);

/*
 * Phase E: attach to the range at [va, va + size) so a VA-named launch
 * can pin the buffer behind it for its lifetime. Validates that the
 * range is bound, and — when @managed is asked for — that it was bound
 * with VGPU_VM_BIND_MANAGED. Unlike vgpu_vm_resolve() this does NOT
 * walk the PTEs and does NOT fault anything in: a managed launch
 * faults lazily, from the engine, on first touch.
 */
int vgpu_vm_attach(struct vgpu_vm *vm, u64 va, u64 size, bool managed,
		   struct drm_gem_object **objp);
void vgpu_vm_release(struct drm_gem_object *obj, struct page **pages,
		     u32 npages);
void vgpu_vm_release_sub(struct vgpu_submission *sub);
void vgpu_vm_evict_all(struct vgpu *v);
int vgpu_vm_show(struct seq_file *s, void *data);
void vgpu_vm_destroy_all(struct vgpu *v);
/* vgpu_compute.c */
struct vgpu_instr;
enum vgpu_sched vgpu_compute_run(struct vgpu *v, struct vgpu_submission *sub);

/* vgpu_drv.c: BLIT resolution, shared by the doorbell and the graph */
struct vgpu_queue *vgpu_queue_find(struct vgpu_file *vf, u32 qid);
struct vgpu_kernel *vgpu_kernel_find(struct vgpu_file *vf, u32 kid);
u32 vgpu_pkt_opcode(u32 type);
bool vgpu_pkt_is_sync(u32 type);
int vgpu_blit_pin(struct drm_file *file, struct vgpu_blit_ref *ref,
		  u32 src_bo, u32 dst_bo, u32 off, u32 len);
void vgpu_blit_unref(struct vgpu *v, struct vgpu_blit_ref *ref);

/* vgpu_sync.c (phase G) */
int vgpu_sem_bind_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file);
int vgpu_graph_ioctl(struct drm_device *dev, void *data,
		     struct drm_file *file);
int vgpu_timeline_resolve(struct drm_file *file, struct vgpu_submission *sub,
			  u32 sem_bo, u32 sem_off);
void vgpu_sync_exec_cmd(struct vgpu *v, struct vgpu_submission *sub,
			u32 op, u32 sem_off, u64 val);
/*
 * Walk the graph's nodes in topological order. Returns VGPU_SCHED_DONE
 * when the whole DAG retired; otherwise the reason a LAUNCH node was
 * paused, with sub->gnode_idx left at that node so the next entry
 * resumes it (phase F's scheduler). The submission must NOT be
 * completed in that case: no fence, no release of gnodes.
 */
enum vgpu_sched vgpu_graph_execute(struct vgpu *v,
				   struct vgpu_submission *sub);
void vgpu_sync_release(struct vgpu *v, struct vgpu_submission *sub);
u32 vgpu_bo_word(struct drm_gem_object *obj, u32 word, bool write, u32 val);

/* vgpu_engine.c */
extern const struct dma_fence_ops vgpu_fence_ops;
void vgpu_engine_init(struct vgpu *v);
void vgpu_engine_fini(struct vgpu *v);
irqreturn_t vgpu_irq_handler(int irq, void *dev_id);
void vgpu_finish_submission(struct vgpu *v, struct vgpu_submission *sub);
void vgpu_blit_pages(struct vgpu *v, struct page **src_pages,
		     struct page **dst_pages, u32 npages);
/* the cooperative yield point a running launch polls (phase F) */
enum vgpu_sched vgpu_sched_check(struct vgpu *v, struct vgpu_submission *sub);
#endif /* _VGPU_DRV_H_ */
