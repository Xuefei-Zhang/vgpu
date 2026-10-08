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

/* One doorbell: ncmds commands fetched from the ring into scratch. */
struct vgpu_submission {
	struct list_head node;
	struct vgpu *v;		/* back-pointer for the per-sub finish work */
	u32 seqno;
	u32 ncmds;
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
};

struct vgpu {
	struct platform_device *pdev;
	struct drm_device dev;

	/* "GPU core": one ordered workqueue, one submission at a time */
	struct workqueue_struct *engine_wq;
	struct work_struct engine_work;
	/* bottom half: process completions, signal fences */
	struct workqueue_struct *irq_wq;
	struct work_struct watchdog_work;

	struct vgpu_regs *regs;		/* coherent: virtual BAR0 */
	dma_addr_t regs_dma;
	u8 *scratch;			/* coherent: fetched commands */
	dma_addr_t scratch_dma;
	u32 scratch_used;

	struct drm_gem_shmem_object *ring_bo;	/* the command ring */

	spinlock_t lock;		/* ring/scratch bookkeeping + lists */
	struct list_head pending;	/* submitted, not yet executing */
	struct list_head in_flight;	/* executing / waiting for "interrupt" */
	u32 next_seqno;
	u32 ring_used;

	/* seqno -> fence, so VGPU_WAIT can find its fence */
	struct mutex fence_mutex;
	struct idr fence_idr;

	u64 timeline;		/* one fence context for all fences */

	struct timer_list watchdog;	/* fence timeout backstop (10s) */

	u32 stats_submitted, stats_completed, stats_errors;
	struct dentry *dbg;
};

#define to_vgpu(drm_dev) container_of(drm_dev, struct vgpu, dev)

/* vgpu_engine.c */
extern const struct dma_fence_ops vgpu_fence_ops;
void vgpu_engine_init(struct vgpu *v);
void vgpu_engine_fini(struct vgpu *v);
irqreturn_t vgpu_irq_handler(int irq, void *dev_id);
void vgpu_finish_submission(struct vgpu *v, struct vgpu_submission *sub);

#endif /* _VGPU_DRV_H_ */
