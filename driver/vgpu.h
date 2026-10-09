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
 * One doorbell. Two shapes:
 *  - CS ioctl (legacy): commands are fetched into scratch at submit
 *    time, ncmds set, npkts == 0.
 *  - DOORBELL ioctl (phase B): carries a packet RANGE in the queue
 *    ring; the engine fetches and decodes it, setting ncmds from npkts.
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
	struct drm_gem_object *src_obj, *dst_obj;
	struct page **src_pages, **dst_pages;
	u32 nblit_pages;
	u32 blit_off;		/* page index of the blit range inside the BOs */
	struct work_struct finish_work;	/* bottom half: one work per sub */

	/* compute: a LAUNCH packet resolved at submit time */
	struct vgpu_kernel *kernel;
	struct drm_gem_object *args_obj;	/* the argument buffer (pinned) */
	u32 args_off;
	u32 grid, block;
};

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

/* Per-fd state: every open() owns its queues (drm_file.driver_priv). */
struct vgpu_file {
	struct list_head queues;	/* struct vgpu_queue */
	struct list_head kernels;	/* struct vgpu_kernel */
	struct vgpu_queue *default_q;	/* lazily created GFX queue (qid 0) */
	struct vgpu *vf_dev;
	u32 next_qid;
	u32 next_kid;
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

	/* compute statistics (debugfs "compute") */
	u64 cs_kernels_launched, cs_blocks, cs_warps, cs_threads;
	u64 cs_instrs, cs_mem_ops, cs_waves;
	u64 cs_coalesced_warps, cs_bcast_warps, cs_uncoalesced_warps;
	u64 cs_mem_lat_ns;		/* modelled memory latency, ns */
	struct list_head kernel_list;	/* every live kernel (debugfs) */
};

#define to_vgpu(drm_dev) container_of(drm_dev, struct vgpu, dev)

#define VGPU_NREGS_MAX		16

/* vgpu_compute.c */
struct vgpu_instr;
void vgpu_compute_run(struct vgpu *v, struct vgpu_submission *sub);

/* vgpu_engine.c */
extern const struct dma_fence_ops vgpu_fence_ops;
void vgpu_engine_init(struct vgpu *v);
void vgpu_engine_fini(struct vgpu *v);
irqreturn_t vgpu_irq_handler(int irq, void *dev_id);
void vgpu_finish_submission(struct vgpu *v, struct vgpu_submission *sub);
#endif /* _VGPU_DRV_H_ */
