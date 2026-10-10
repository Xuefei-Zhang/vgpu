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
 * BLIT: the "GPU" copies src -> dst using the streaming DMA API
 * (dma_map_single / dma_unmap_single), one page at a time. On x86
 * without an IOMMU the mapping is a no-op; with an IOMMU (or on ARM)
 * this is where real device-to-memory translation happens.
 *
 * Takes the page arrays explicitly so a graph node (which resolves its
 * own buffers) can reuse it — the single-BLIT submission wrapper below
 * is what saw the light of day first.
 *
 * The arrays ARE the range: vgpu_blit_pin() captured npages pages
 * starting at the range's first page and nothing else, so they are
 * indexed from 0. Adding the range's page offset here as well read one
 * entry past the array for any copy not starting on page 0 — invisible
 * while every submission copied at offset 0, and the first thing a
 * graph node at a non-zero offset does (KASAN: slab-out-of-bounds read
 * in vgpu_blit_pages, then a GPF in memcpy on the garbage page).
 */
void vgpu_blit_pages(struct vgpu *v, struct page **src_pages,
		     struct page **dst_pages, u32 npages){
	u32 i;

	for (i = 0; i < npages; i++) {
		struct page *sp = src_pages[i];
		struct page *dp = dst_pages[i];
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
 * The BLIT path's one-submission wrapper: the submitter resolved the
 * handles into page arrays (and pinned the BOs), so the engine only
 * moves bytes. Kept as a wrapper because a graph node resolves its own
 * buffers and calls vgpu_blit_pages() directly.
 */
static void vgpu_blit(struct vgpu *v, struct vgpu_submission *sub)
{
	struct vgpu_blit_ref *b = &sub->blit;

	vgpu_blit_pages(v, b->src_pages, b->dst_pages, b->npages);
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
	vgpu_blit_pages(v, sub->vm_src_pages, sub->vm_dst_pages,
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
		/*
		 * Phase G: the sync packets carry no engine-side state
		 * beyond which word of the semaphore page and what
		 * value — the command struct's SET fields are exactly
		 * that shape (and the resolution of the page itself
		 * happened in the doorbell).
		 */
		case VGPU_PKT_SIGNAL_TIMELINE:
			c->opcode = VGPU_OP_SIGNAL_TIMELINE;
			c->set_off = pk[i].timeline.sem_off;
			c->set_data = pk[i].timeline.value;
			break;
		case VGPU_PKT_WAIT_TIMELINE:
			c->opcode = VGPU_OP_WAIT_TIMELINE;
			c->set_off = pk[i].timeline.sem_off;
			c->set_data = pk[i].timeline.value;
			break;
		case VGPU_PKT_WRITE_EVENT:
			c->opcode = VGPU_OP_WRITE_EVENT;
			c->set_off = pk[i].timeline.sem_off;
			c->set_data = pk[i].timeline.value;
			break;
		case VGPU_PKT_VM_COPY:
			/*
			 * Phase C: the engine sees GPU addresses, not file
			 * descriptors — the doorbell already walked them into
			 * page arrays. All that survives the fetch is the
			 * opcode; the pages come along in the submission.
			 */
			c->opcode = VGPU_OP_VM_COPY;
			break;
			break;
		default:
			c->opcode = 0xff;	/* engine reports the fault */
		}
	}
	kunmap_local(pk);
}

/*
 * Execute one submission — or one slice of it (phase F).
 *
 * Returns VGPU_SCHED_DONE when the submission finished; otherwise the
 * scheduler's reason for pausing, and sub->cmd_idx is left pointing at
 * the command to resume at (a LAUNCH saves its own finer-grained
 * cursor in sub->cs). The caller must not complete the submission in
 * that case: the fence stays unsignaled and the submission stays on
 * the run queue, state and all.
 */
static enum vgpu_sched vgpu_execute(struct vgpu *v, struct vgpu_submission *sub)
{
	struct vgpu_cmd *cmds =
		(struct vgpu_cmd *)(v->scratch[sub->engine] + sub->cmds_off);
	u32 i;

	/* VGPU_GRAPH: nodes were resolved and ordered by the ioctl */
	if (sub->gnodes)
		/*
		 * The graph is resumable now (phase F): a launch node
		 * inside it can be paused, in which case the graph
		 * reports where it stopped and this function passes
		 * the verdict on to the engine loop instead of
		 * completing the submission.
		 */
		return vgpu_graph_execute(v, sub);

	for (i = sub->cmd_idx; i < sub->ncmds; i++) {
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
			if (!sub->blit.src_obj) {
				if (!sub->error)
					sub->error = -EINVAL;
				break;
			}
			vgpu_blit(v, sub);
			break;
		case VGPU_OP_LAUNCH: {
			enum vgpu_sched r;

			/*
			 * Only the doorbell path resolves a launch (the
			 * legacy CS command has no kernel descriptor),
			 * so a launch without a submission-side state is
			 * a malformed command, not something to decode.
			 */
			if (!sub->cs) {
				if (!sub->error)
					sub->error = -EINVAL;
				break;
			}
			r = vgpu_compute_run(v, sub);
			if (r != VGPU_SCHED_DONE) {
				sub->cmd_idx = i;	/* resume here */
				return r;
			}
			break;
		/*
		 * Phase G: the stream can keep going after a signal — a
		 * semaphore write is just another packet, and the whole
		 * point is that more work can depend on it.
		 */
		case VGPU_OP_SIGNAL_TIMELINE:
		case VGPU_OP_WAIT_TIMELINE:
		case VGPU_OP_WRITE_EVENT:
			vgpu_sync_exec_cmd(v, sub, c->opcode, c->set_off,
					   c->set_data);
			break;
		case VGPU_OP_VM_COPY:
			vgpu_vm_copy(v, sub);
			break;
		}
		default:
			if (!sub->error)
				sub->error = -EINVAL;
		}
		if (sub->error)
			break;	/* "GPU fault: stop the submission" */
	}
	sub->cmd_idx = sub->ncmds;
	return VGPU_SCHED_DONE;
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

/* the blocks per timeslice on the compute engine (phase F) */
#define VGPU_TIMESLICE_BLOCKS	32

/*
 * The cooperative yield point (phase F).
 *
 * A running launch polls this at every instruction boundary — the
 * "safe points" a real GPU's scheduler gets, because unlike a CPU there
 * is no wire anyone can pull to make a shader stop mid-instruction:
 * preemption has to be cooperative.
 *
 * Two reasons to give the engine up:
 *   TIMESLICE — this launch has executed its share of blocks, so it
 *              goes to the BACK of its priority class and another
 *              context gets the pipe (that is the timeslice).
 *   PREEMPT   — a strictly higher-priority context has work waiting on
 *              this engine. The launch pauses at this instruction
 *              boundary and keeps its place in its own class, ahead of
 *              same-priority backlog.
 *
 * Never reports PREEMPT unless a strictly higher submission really is
 * waiting, so the engine always makes progress when it re-picks.
 */
enum vgpu_sched vgpu_sched_check(struct vgpu *v, struct vgpu_submission *sub)
{
	struct vgpu_launch *cs = sub->cs;
	struct vgpu_submission *pos;
	enum vgpu_sched r = VGPU_SCHED_DONE;
	unsigned long flags;

	/*
	 * Preemption is checked BEFORE the timeslice on purpose: it is
	 * the urgent condition (a context is waiting), and reporting a
	 * coincidentally expired timeslice instead would hide a real
	 * preemption from the counters.
	 *
	 * Cheap lockless pre-filter: when nothing else is queued on this
	 * engine there is nobody to preempt us. list_empty_careful() is
	 * the lockless-readable form; at worst we notice one instruction
	 * later than a locked check would have.
	 */
	if (!list_empty_careful(&v->engine_pending[sub->engine])) {
		spin_lock_irqsave(&v->lock, flags);
		list_for_each_entry(pos, &v->engine_pending[sub->engine],
				    node) {
			if (pos->q->prio > sub->q->prio) {
				r = VGPU_SCHED_PREEMPT;
				break;
			}
		}
		spin_unlock_irqrestore(&v->lock, flags);
		if (r != VGPU_SCHED_DONE)
			return r;
	}

	/* the timeslice: this engine context owns the launch's counters */
	if (cs->slice_blocks >= VGPU_TIMESLICE_BLOCKS) {
		cs->slice_blocks = 0;
		return VGPU_SCHED_TIMESLICE;
	}
	return VGPU_SCHED_DONE;
}

/*
 * A context switch: the engine is handing its pipe from one context's
 * launch to another's work. Nothing completes — the fence stays
 * unsignaled, the ring bytes and the pinned argument buffer stay
 * claimed, and the launch's resume point lives in sub->cs. That is
 * exactly the state a real driver saves into the channel's MQD before
 * it can let a higher-priority channel run.
 *
 * Where the victim goes back on the run queue is the whole point of
 * the split: a preempted launch keeps its place (head of its class, so
 * it resumes as soon as the higher-priority work retires), while a
 * timesliced one goes to the tail so the other contexts get their turn.
 */
static void vgpu_ctx_switch(struct vgpu *v, struct vgpu_submission *sub,
			    enum vgpu_sched why)
{
	unsigned long flags;

	spin_lock_irqsave(&v->lock, flags);
	v->cs_ctx_switches++;
	sub->q->cs_switches++;
	if (why == VGPU_SCHED_PREEMPT) {
		v->cs_preemptions++;
		sub->q->cs_preempted++;
		list_move(&sub->node, &v->engine_pending[sub->engine]);
	} else {
		v->cs_timeslices++;
		list_move_tail(&sub->node, &v->engine_pending[sub->engine]);
	}
	spin_unlock_irqrestore(&v->lock, flags);

	dev_info(&v->pdev->dev,
		 "[vgpu:sched] %s: seq#%u q#%u prio=%u yields at block %u/%u warp %u pc %u (switches %llu)\n",
		 why == VGPU_SCHED_PREEMPT ? "preempt" : "timeslice",
		 sub->seqno, sub->q->qid, sub->q->prio, sub->cs->block,
		 sub->grid, sub->cs->warp, sub->cs->pc, v->cs_ctx_switches);
}

/*
 * Pick the next submission to run on an engine: the first one in the
 * highest priority class waiting (phase F's scheduler). Strict ">"
 * keeps FIFO order inside a class, and since every submission of one
 * queue shares that queue's priority, in-order execution per queue —
 * the contract userspace depends on — is untouched. Cross-queue
 * reordering is precisely what priority classes are for.
 */
static struct vgpu_submission *vgpu_engine_next(struct vgpu *v, int engine)
{
	struct vgpu_submission *sub, *best = NULL;
	unsigned long flags;

	spin_lock_irqsave(&v->lock, flags);
	list_for_each_entry(sub, &v->engine_pending[engine], node) {
		if (!best || sub->q->prio > best->q->prio)
			best = sub;
	}
	if (best)
		list_move_tail(&best->node, &v->in_flight);
	spin_unlock_irqrestore(&v->lock, flags);
	return best;
}

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
 *
 * The loop is also phase F's scheduler: it does not drain one
 * submission at a time any more, it runs each one until a safe point
 * yields, then re-picks. A submission that yielded is still on the run
 * queue with its state saved, so "re-pick" is what resumes it.
 */
static void vgpu_engine_work(struct work_struct *work, int engine)
{
	struct vgpu *v = container_of(work, struct vgpu, engine_work[engine]);
	struct vgpu_submission *sub;

	for (;;) {
		enum vgpu_sched r;

		sub = vgpu_engine_next(v, engine);
		if (!sub)
			break;

		/*
		 * Packet submissions carry a ring range, not commands:
		 * the fetch happens here, in engine context. Only the
		 * first slice fetches — a paused launch keeps its
		 * decoded commands (and its scratch claim) across the
		 * context switch.
		 */
		if (sub->npkts) {
			vgpu_fetch_packets(v, sub);
			sub->ncmds = sub->npkts;
			sub->npkts = 0;
		}

		r = vgpu_execute(v, sub);
		if (r != VGPU_SCHED_DONE) {
			vgpu_ctx_switch(v, sub, r);
			continue;	/* let the scheduler pick the next */
		}

		dev_info(&v->pdev->dev,
			 "[vgpu:engine] seq#%u (engine %u) done (%u cmds)%s\n",
			 sub->seqno, sub->engine, sub->ncmds,
			 sub->error ? " ERROR" : "");

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

	/*
	 * Phase G first: a graph owns its own resolved resources (per
	 * node), and the semaphore page is pinned per submission. Both
	 * are released (and the pointers cleared) here, so the generic
	 * BLIT/launch cleanup below only ever sees what it resolved.
	 * It also clears sub->args_obj, which for a graph is the node's
	 * argument buffer and was already released per node — the release
	 * below is therefore live only on the doorbell path, and the two
	 * can never double-free.
	 */
	vgpu_sync_release(v, sub);

	/* release the launch's argument buffer (unpin + object ref) */
	if (sub->args_obj) {
		drm_gem_shmem_unpin(to_drm_gem_shmem_obj(sub->args_obj));
		drm_gem_object_put(sub->args_obj);
		sub->args_obj = NULL;
	}
	/* release the BLIT's pages + pins (doorbell path) */
	vgpu_blit_unref(v, &sub->blit);
	/*
	 * The saved launch state goes with it. A launch that was paused
	 * mid-grid still owns its register file here — the drain path
	 * (rmmod) and the fence path both come through here.
	 */
	if (sub->cs) {
		kfree(sub->cs->fw);
		kfree(sub->cs);
		sub->cs = NULL;
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
