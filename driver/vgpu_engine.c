// SPDX-License-Identifier: GPL-2.0
/*
 * vgpu - the "GPU": command engine, simulated interrupt path, fences.
 *
 * Execution model (docs/DESIGN.md §6/§7):
 *
 *   CS ioctl (process)          engine workqueue        "interrupt"
 *   ┌──────────────┐            ┌──────────────┐        ┌────────────────┐
 *   │ fetch ring   │──queue────▶│ decode +     │────────│ top half:      │
 *   │ to scratch   │            │ execute cmds │  calls │ read/clear     │
 *   └──────────────┘            └──────────────┘        │ regs, hand off │
 *                                                        └───────┬────────┘
 *                                                                │ queue
 *   VGPU_WAIT (process) ◀──woken──┐        ┌─────────────────────▼────────┐
 *   dma_fence_wait_timeout()      └─────── │ bottom half: fence signal,   │
 *                                          │ stats, free the submission   │
 *                                          └──────────────────────────────┘
 *
 * The "interrupt" is simulated by calling vgpu_irq_handler() directly
 * (the mainline vgem approach, docs/adr/0001). The code keeps the real
 * constraints: the top half takes a spinlock and must not sleep.
 */

#include <linux/dma-fence.h>
#include <linux/module.h>
#include <linux/workqueue.h>

#include "vgpu.h"

/* ---------------------------------------------------------------- fence */

static const char *vgpu_fence_get_driver_name(struct dma_fence *fence)
{
	return "vgpu";
}

static const char *vgpu_fence_get_timeline_name(struct dma_fence *fence)
{
	return "vgpu-timeline";
}

static void vgpu_fence_release(struct dma_fence *fence)
{
	struct vgpu_fence *vf = container_of(fence, struct vgpu_fence, base);

	kfree(vf);
}

const struct dma_fence_ops vgpu_fence_ops = {
	.get_driver_name	= vgpu_fence_get_driver_name,
	.get_timeline_name	= vgpu_fence_get_timeline_name,
	.release		= vgpu_fence_release,
};

/* --------------------------------------------------------------- engine */

/*
 * The DMA engine: copy npages from one page array to another using the
 * streaming DMA API (dma_map_single / dma_unmap_single). On x86 without
 * an IOMMU the mapping is a no-op; with an IOMMU (or on ARM) this is
 * where real device-to-memory translation happens.
 */
static void vgpu_copy_pages(struct vgpu *v, struct page **src,
			    struct page **dst, u32 npages)
{
	u32 i;

	for (i = 0; i < npages; i++) {
		struct page *sp = src[i];
		struct page *dp = dst[i];
		void *sv = kmap_local_page(sp);
		void *dv = kmap_local_page(dp);
		dma_addr_t saddr, daddr;

		saddr = dma_map_single(&v->pdev->dev, sv, PAGE_SIZE,
				       DMA_FROM_DEVICE);
		daddr = dma_map_single(&v->pdev->dev, dv, PAGE_SIZE,
				       DMA_TO_DEVICE);
		/* the "GPU" moves the bytes */
		memcpy(dv, sv, PAGE_SIZE);
		dma_unmap_single(&v->pdev->dev, daddr, PAGE_SIZE,
				 DMA_TO_DEVICE);
		dma_unmap_single(&v->pdev->dev, saddr, PAGE_SIZE,
				 DMA_FROM_DEVICE);
		kunmap_local(dv);
		kunmap_local(sv);
	}
}

/*
 * BLIT: BO handles the submitter resolved into page arrays.
 */
static void vgpu_blit(struct vgpu *v, struct vgpu_submission *sub)
{
	if (!sub->src_obj) {
		sub->error = -EINVAL;
		return;
	}
	vgpu_copy_pages(v, sub->src_pages + sub->blit_off,
			sub->dst_pages + sub->blit_off, sub->nblit_pages);
}

/*
 * Phase C's DMA: GPU VA -> GPU VA. The pages were already walked out of
 * this submission's page tables at doorbell time (that is where the fd,
 * and therefore the VMID, is available), so the engine just moves bytes
 * — exactly like a real one, which sees DMA addresses, not file
 * descriptors.
 */
static void vgpu_vm_copy(struct vgpu *v, struct vgpu_submission *sub)
{
	if (!sub->vm_src_obj) {
		sub->error = -EINVAL;
		return;
	}
	vgpu_copy_pages(v, sub->vm_src_pages, sub->vm_dst_pages,
			sub->vm_npages);
}

/*
 * Execute one submission. Runs in the engine workqueue — the
 * "engine". One ordered workqueue per engine kind: one submission at
 * a time per engine, but GFX / COPY / COMPUTE run concurrently
 * (see vgpu_engine_init). engine_pending[] carries the in-order
 * backlog of that engine.
 */
/*
 * The fetch. This is the phase-B point: the ENGINE reads the queue's
 * ring and decodes the packets into commands, instead of the ioctl
 * having copied them for us. On real hardware this is what the
 * command processor's front end does when the doorbell rings — the
 * CPU has already left the building by now.
 */
static void vgpu_fetch_packets(struct vgpu *v, struct vgpu_submission *sub)
{
	struct vgpu_queue *q = sub->q;
	struct page *page;
	struct vgpu_packet *pk;
	struct vgpu_cmd *cmds;
	u32 i;

	/* the ring may span pages for larger queues */
	page = q->ring_bo->pages[sub->ring_off / PAGE_SIZE];
	pk = kmap_local_page(page) + (sub->ring_off % PAGE_SIZE);
	cmds = (struct vgpu_cmd *)(v->scratch[sub->engine] + sub->cmds_off);

	for (i = 0; i < sub->npkts; i++) {
		struct vgpu_cmd *c = &cmds[i];

		memset(c, 0, sizeof(*c));
		switch (pk[i].type) {
		case VGPU_PKT_NOP:
			c->opcode = VGPU_OP_NOP;
			break;
		case VGPU_PKT_COPY:
			c->opcode = VGPU_OP_BLIT;
			c->src_bo = pk[i].copy.src_bo;
			c->dst_bo = pk[i].copy.dst_bo;
			c->src_off = pk[i].copy.off;
			c->dst_off = pk[i].copy.off;
			c->length = pk[i].copy.len;
			break;
		case VGPU_PKT_WRITE_REG:
			c->opcode = VGPU_OP_SET;
			c->set_off = pk[i].reg.reg_off;
			c->set_data = pk[i].reg.reg_val;
			break;
		case VGPU_PKT_LAUNCH:
			c->opcode = VGPU_OP_LAUNCH;
			break;
		case VGPU_PKT_VM_COPY:
			/*
			 * Phase C: the engine sees GPU addresses, not
			 * file descriptors — the doorbell already walked
			 * them into page arrays. All that survives the
			 * fetch is the opcode; the pages come along in
			 * the submission.
			 */
			c->opcode = VGPU_OP_VM_COPY;
			break;
		default:
			c->opcode = 0xff;	/* engine reports the fault */
		}
	}
	kunmap_local(pk);
}

static void vgpu_execute(struct vgpu *v, struct vgpu_submission *sub)
{

	struct vgpu_cmd *cmds =
		(struct vgpu_cmd *)(v->scratch[sub->engine] + sub->cmds_off);
	u32 i;

	for (i = 0; i < sub->ncmds; i++) {
		struct vgpu_cmd *c = &cmds[i];

		switch (c->opcode) {
		case VGPU_OP_NOP:
			/* burn a few us so the step shows in ftrace */
			udelay(10);
			break;
		case VGPU_OP_SET:
			if (c->set_off % 4 || c->set_off >= sizeof(*v->regs)) {
				if (!sub->error)
					sub->error = -EINVAL;
				break;
			}
			/* "GPU" writes to its own BAR0 */
			*((u32 *)v->regs + c->set_off / 4) = c->set_data;
			break;
		case VGPU_OP_BLIT:
			if (!sub->src_obj) {
				if (!sub->error)
					sub->error = -EINVAL;
				break;
			}
			vgpu_blit(v, sub);
			break;
		case VGPU_OP_LAUNCH:
			vgpu_compute_run(v, sub);
			break;
		case VGPU_OP_VM_COPY:
			vgpu_vm_copy(v, sub);
			break;
		default:
			if (!sub->error)
				sub->error = -EINVAL;
		}
		if (sub->error)
			break;	/* "GPU fault: stop the submission" */
	}
	dev_info(&v->pdev->dev,
		 "[vgpu:engine] seq#%u (engine %u) done (%u cmds)%s\n",
		 sub->seqno, sub->engine, sub->ncmds,
		 sub->error ? " ERROR" : "");
}

static void vgpu_engine_work(struct work_struct *work, int engine);

static void vgpu_engine_work_gfx(struct work_struct *work)
{
	vgpu_engine_work(work, VGPU_ENGINE_GFX);
}

static void vgpu_engine_work_copy(struct work_struct *work)
{
	vgpu_engine_work(work, VGPU_ENGINE_COPY);
}

static void vgpu_engine_work_compute(struct work_struct *work)
{
	vgpu_engine_work(work, VGPU_ENGINE_COMPUTE);
}

typedef void (*vgpu_engine_work_fn)(struct work_struct *work);

static const vgpu_engine_work_fn engine_work_fns[VGPU_ENGINE_COUNT] = {
	vgpu_engine_work_gfx,
	vgpu_engine_work_copy,
	vgpu_engine_work_compute,
};

/*
 * The engine body. NOTE how the container is recovered: the index is
 * part of the member name (engine_work[engine]), because container_of()
 * subtracts offsetof(array member [0]) — subtracting offsetof of the
 * ARRAY BASE for element 1 yields a device pointer off by
 * index * sizeof(work_struct). That is not theoretical: doing
 * "container_of(work, struct vgpu, engine_work[0])" plus a runtime
 * "work - &v->engine_work[0]" index made the GFX engine work and the
 * COPY engine dereference a neighbouring queue object ("spinlock bad
 * magic" + KASAN slab-out-of-bounds 40 bytes right of a
 * sizeof(struct vgpu_queue) region). One wrapper per engine, each
 * naming its own index, is what real drivers do.
 */
static void vgpu_engine_work(struct work_struct *work, int engine)
{
	struct vgpu *v = container_of(work, struct vgpu, engine_work[engine]);
	struct vgpu_submission *sub;

	for (;;) {
		unsigned long flags;

		spin_lock_irqsave(&v->lock, flags);
		sub = list_first_entry_or_null(&v->engine_pending[engine],
					       struct vgpu_submission, node);
		if (sub)
			list_move_tail(&sub->node, &v->in_flight);
		spin_unlock_irqrestore(&v->lock, flags);
		if (!sub)
			break;

		/*
		 * Packet submissions carry a ring range, not commands:
		 * the fetch happens here, in engine context.
		 */
		if (sub->npkts) {
			vgpu_fetch_packets(v, sub);
			sub->ncmds = sub->npkts;
			sub->npkts = 0;
		}
		vgpu_execute(v, sub);

		/*
		 * "Hardware" completion: the device writes the status
		 * registers and rings the interrupt line.
		 *
		 * smp_wmb(): the register writes must be visible to
		 * whatever CPU the top half runs on (coherent RAM is
		 * not device-ordered like real MMIO would be).
		 */
		v->regs->seqno = sub->seqno;
		v->regs->error = sub->error;
		v->regs->status = VGPU_STATUS_DONE |
			(sub->error ? VGPU_STATUS_ERR : 0);
		smp_wmb();
		vgpu_irq_handler(0, v);	/* simulated interrupt (ADR-0001) */
	}
}

/* ----------------------------------------------------- interrupt path */

/*
 * Top half. In real hardware this runs in hardirq context: it must be
 * short, must not sleep, and takes a spinlock. Here it is invoked
 * directly by the engine (ADR-0001), but it keeps exactly those
 * constraints.
 */
static void vgpu_irq_bh_work(struct work_struct *work);

irqreturn_t vgpu_irq_handler(int irq, void *dev_id)
{
	struct vgpu *v = dev_id;
	struct vgpu_submission *sub = NULL, *pos;
	unsigned long flags;
	u32 status, seqno;

	spin_lock_irqsave(&v->lock, flags);
	status = v->regs->status;
	if (!(status & VGPU_STATUS_DONE)) {
		spin_unlock_irqrestore(&v->lock, flags);
		return IRQ_NONE;
	}
	v->regs->status = 0;	/* clear the "interrupt" flag */
	seqno = v->regs->seqno;
	list_for_each_entry(pos, &v->in_flight, node) {
		if (pos->seqno == seqno) {
			sub = pos;
			break;
		}
	}
	if (sub)
		/* unlinked now: the bottom half processes exactly this sub */
		list_del_init(&sub->node);
	spin_unlock_irqrestore(&v->lock, flags);

	if (!sub)
		return IRQ_NONE;

	dev_info(&v->pdev->dev, "[vgpu:irq] top-half seq#%u -> bottom half\n",
		 seqno);
	INIT_WORK(&sub->finish_work, vgpu_irq_bh_work);
	queue_work(v->irq_wq, &sub->finish_work);
	return IRQ_HANDLED;
}

/*
 * Finish one submission: signal its fence (wakes the VGPU_WAIT waiter)
 * and free the submission. Idempotent via the finished flag.
 *
 * The fence itself stays in the idr (it is consumed by WAIT or reaped
 * by the watchdog) — its lifetime is independent of the submission.
 */
void vgpu_finish_submission(struct vgpu *v, struct vgpu_submission *sub)
{
	struct dma_fence *fence;

	if (cmpxchg(&sub->finished, 0, 1))
		return;

	/* release the launch's argument buffer (unpin + object ref) */
	if (sub->args_obj) {
		drm_gem_shmem_unpin(to_drm_gem_shmem_obj(sub->args_obj));
		drm_gem_object_put(sub->args_obj);
		sub->args_obj = NULL;
	}
	if (sub->src_obj) {
		u32 i;

		for (i = 0; i < sub->nblit_pages; i++) {
			put_page(sub->src_pages[sub->blit_off + i]);
			put_page(sub->dst_pages[sub->blit_off + i]);
		}
		kfree(sub->src_pages);
		kfree(sub->dst_pages);
		drm_gem_shmem_unpin(to_drm_gem_shmem_obj(sub->src_obj));
		drm_gem_shmem_unpin(to_drm_gem_shmem_obj(sub->dst_obj));
		drm_gem_object_put(sub->src_obj);
		drm_gem_object_put(sub->dst_obj);
	}
	/* phase C: the VA-resolved copy's pages and buffers */
	vgpu_vm_release_sub(sub);

	mutex_lock(&v->fence_mutex);
	fence = idr_find(&v->fence_idr, sub->seqno);
	if (fence)
		dma_fence_get(fence);
	mutex_unlock(&v->fence_mutex);

	if (fence) {
		if (sub->error)
			fence->error = sub->error;
		dma_fence_signal(fence);
		dma_fence_put(fence);
	}

	{
		unsigned long flags;

		spin_lock_irqsave(&v->lock, flags);
		if (sub->error) {
			v->stats_errors++;
			sub->q->stats_errors++;
		} else {
			v->stats_completed++;
			sub->q->stats_completed++;
		}
		/* ring space is per queue now: a full queue blocks only
		 * its own stream, not the whole device */
		/*
		 * The queue's read pointer advances only on the packet
		 * path (get/put exist for the doorbell flow; the legacy
		 * CS path uses arbitrary ring offsets).
		 */
		if (sub->from_doorbell)
			sub->q->get = sub->ring_off + sub->ring_bytes;
		sub->q->ring_used -= sub->ring_bytes;
		v->scratch_used[sub->engine] -= sub->ncmds * VGPU_CMD_SIZE;
		spin_unlock_irqrestore(&v->lock, flags);
	}
	kfree(sub);
}

/*
 * Bottom half: process context, may sleep. Signals the fence, updates
 * stats, frees the submission. (In a real driver this is where the
 * expensive post-completion work lives — that is why it exists.)
 */
static void vgpu_irq_bh_work(struct work_struct *work)
{
	struct vgpu_submission *sub =
		container_of(work, struct vgpu_submission, finish_work);
	struct vgpu *v = sub->v;

	/* the top half already unlinked sub from in_flight */
	dev_info(&v->pdev->dev, "[vgpu:bh] seq#%u signaling fence\n",
		 sub->seqno);
	vgpu_finish_submission(v, sub);
}

/* ---------------------------------------------------------- watchdog */

/*
 * Hang-detection backstop (vgem does the same with a per-fence timer):
 * if a fence is not signaled within 10 s, signal it with -ETIMEDOUT so
 * waiters can never hang forever. The submission itself is still
 * completed by the normal path (or by rmmod).
 *
 * Runs in process context (queued from the timer, like any bottom
 * half work), so it may take the mutex.
 */
static void vgpu_watchdog_work(struct work_struct *work)
{
	struct vgpu *v = container_of(work, struct vgpu, watchdog_work);
	struct vgpu_fence *vf;
	int id;
	bool signaled = false;

	mutex_lock(&v->fence_mutex);
	idr_for_each_entry(&v->fence_idr, vf, id) {
		if (!dma_fence_is_signaled(&vf->base) &&
		    time_after(jiffies, vf->created_jiffies +
			       VGPU_FENCE_TIMEOUT)) {
			vf->base.error = -ETIMEDOUT;
			dma_fence_signal(&vf->base);
			signaled = true;
		}
	}
	mutex_unlock(&v->fence_mutex);

	if (signaled)
		dev_info(&v->pdev->dev,
			 "[vgpu:watchdog] signaled timed-out fence(s)\n");
}

static void vgpu_watchdog_timer(struct timer_list *t)
{
	struct vgpu *v = container_of(t, struct vgpu, watchdog);

	mod_timer(&v->watchdog, jiffies + VGPU_FENCE_TIMEOUT);
	queue_work(v->irq_wq, &v->watchdog_work);
}

/* ---------------------------------------------------------- init/fini */

void vgpu_engine_init(struct vgpu *v)
{
	static const char * const names[VGPU_ENGINE_COUNT] = {
		"vgpu-engine-gfx", "vgpu-engine-copy", "vgpu-engine-compute",
	};
	int i;

	for (i = 0; i < VGPU_ENGINE_COUNT; i++) {
		/*
		 * Ordered + unbound: submissions of one engine run one at
		 * a time (like a single GPU pipe) but any engine may run
		 * on any CPU — so two engines can overlap in time, which
		 * the ftrace lesson in FTRACE-GUIDE.md §10 uses as proof.
		 */
		v->engine_wq[i] = alloc_ordered_workqueue(names[i],
							  WQ_UNBOUND);
		INIT_LIST_HEAD(&v->engine_pending[i]);
		INIT_WORK(&v->engine_work[i], engine_work_fns[i]);
	}

	v->irq_wq = alloc_ordered_workqueue("vgpu-irq", WQ_UNBOUND);
	INIT_WORK(&v->watchdog_work, vgpu_watchdog_work);
	timer_setup(&v->watchdog, vgpu_watchdog_timer, 0);
	mod_timer(&v->watchdog, jiffies + VGPU_FENCE_TIMEOUT);
}

void vgpu_engine_fini(struct vgpu *v)
{
	int i;

	timer_delete_sync(&v->watchdog);
	flush_work(&v->watchdog_work);
	for (i = 0; i < VGPU_ENGINE_COUNT; i++)
		destroy_workqueue(v->engine_wq[i]);
	destroy_workqueue(v->irq_wq);
}
