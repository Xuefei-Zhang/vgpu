// SPDX-License-Identifier: GPL-2.0
/*
 * vgpu - the "compute" side of the device: kernel launches.
 *
 * A LAUNCH packet carries a kernel descriptor (grid, block, argument
 * buffer). This file is the simulated execution: blocks are fanned out
 * to SMs, threads run the tiny ISA (see uapi/vgpu_drm.h), and the run
 * produces the statistics a real profiler would show you — waves,
 * coalescing, instructions retired.
 *
 * Deliberately NOT cycle-accurate: memory latency is an integer model
 * (ns per access class), instructions retire in program order. The
 * mechanism being taught is what a launch IS, not how an SM pipelines
 * it.
 *
 * Phase F adds preemption and timeslicing: a launch no longer has to
 * retire its whole grid in one go. The executor polls
 * vgpu_sched_check() at every instruction boundary — the safe points a
 * real GPU offers its scheduler, because there is no wire the CPU can
 * pull to make a shader stop — and, when told to yield, saves the
 * cursor (block/warp/pc) plus the register file into sub->cs so the
 * engine can run somebody else's kernel and later resume this one.
 */
#include <linux/module.h>
#include <linux/slab.h>

#include "vgpu.h"

#define VGPU_SM_COUNT		2	/* simulated SMs */
#define VGPU_MAX_THREADS_SM	256	/* occupancy ceiling per SM */
/* VGPU_MEM_LAT_NS / VGPU_REF_CLOCK_HZ live in vgpu.h (the driver uses them too) */

/*
 * DVFS in one line: the modelled memory latency is inversely
 * proportional to the engine clock. Halve the clock and every kernel
 * takes (about) twice as long — the whole point of a frequency governor,
 * expressed as the number the compute engine accounts.
 */
static u32 vgpu_lat_ns(struct vgpu *v)
{
	if (!v->clock_hz)
		return VGPU_MEM_LAT_NS;
	return DIV_ROUND_UP((u64)VGPU_MEM_LAT_NS * VGPU_REF_CLOCK_HZ,
			    v->clock_hz);
}

/*
 * Modelled per-block SM time. Not a sleep: a busy delay, exactly like
 * the one VGPU_OP_NOP uses, so the modelled work has real wall-clock
 * length and a timeslice shows up in ftrace (and so a launch is still
 * in flight when a higher-priority doorbell arrives).
 */
#define VGPU_BLOCK_US		20

#define R(i)			regs[(i) & (VGPU_NREGS - 1)]

/*
 * One global-memory word, through the argument BO's pages. The BO is
 * pinned for the submission's lifetime (the doorbell does it, like a
 * real driver pins before a launch); each access still resolves its
 * page — which is exactly the walk a GPU's memory management unit
 * performs, and the reason a multi-page argument buffer needs a page
 * table at all.
 */
static u32 vgpu_mem_word(struct vgpu_submission *sub, u32 addr, bool write,
			 u32 val)
{
	return vgpu_bo_word(sub->args_obj, addr, write, val);
}

/*
 * One warp's register file — now a per-launch object, not a scratch
 * buffer: with preemption a launch can be paused mid-warp, so the
 * registers have to survive across context switches. It lives in
 * sub->cs (see vgpu.h) and is freed when the grid retires.
 */

/*
 * One WARP, executed synchronously (SIMT: lanes step the same
 * instruction — no branch instructions exist in this ISA, so lockstep
 * is exact, not an approximation).
 *
 * The point of warp-synchronous execution is the memory metric: for
 * each memory instruction we see all 32 addresses at once and can
 * classify the access the way real hardware does —
 *   broadcast  : all lanes the same word        -> 1 transaction
 *   coalesced  : lanes walk consecutive words   -> 1 transaction
 *   strided    : anything else                  -> 32 transactions
 * A per-thread interpreter can only see one address at a time and
 * would report a bogus number (an earlier version of this file did:
 * it measured the first word each thread touched, which for saxpy is
 * the same parameter word for every lane, and "measured" 100%
 * uncoalesced for a perfectly coalesced kernel).
 *
 * Resumable (phase F): runs from cs->pc to EXIT or to a yield. Returns
 * VGPU_SCHED_DONE when the warp retired; otherwise the reason the
 * scheduler wants the engine back, with pc/resuming set so the next
 * call continues this exact warp — the saved register file IS the
 * context.
 */
static enum vgpu_sched vgpu_run_warp(struct vgpu *v,
				     struct vgpu_submission *sub,
				     struct vgpu_launch *cs, u32 base_tid)
{
	struct vgpu_kernel *k = sub->kernel;
	struct vgpu_warp_frame *fw = cs->fw;
	u32 (*regs)[VGPU_NREGS] = fw->regs;
	bool *done = fw->done;
	u32 pc, lane;
	bool all_done = false;

	if (cs->resuming) {
		pc = cs->pc;		/* continue the interrupted warp */
	} else {
		memset(fw, 0, sizeof(*fw));
		for (lane = 0; lane < VGPU_WARP; lane++)
			regs[lane][0] = base_tid + lane;
		pc = 0;
	}
	cs->resuming = false;

	for (; pc < k->ninstr && !all_done; pc++) {
		struct vgpu_instr *in = &k->code[pc];
		u32 addr[VGPU_WARP];
		bool is_mem = in->op == VGPU_ISA_LD || in->op == VGPU_ISA_ST;
		bool broadcast = true, coalesced = true;
		u32 first = 0;

		all_done = true;
		for (lane = 0; lane < VGPU_WARP; lane++) {
			u32 *R = regs[lane];
			u32 *d = &R[in->dst & (VGPU_NREGS - 1)];

			if (done[lane])
				continue;
			all_done = false;
			v->cs_instrs++;

			switch (in->op) {
			case VGPU_ISA_MOVI:
				*d = in->a;
				break;
			case VGPU_ISA_ADD:
				*d = R[in->a & (VGPU_NREGS - 1)] +
				     R[in->b & (VGPU_NREGS - 1)];
				break;
			case VGPU_ISA_MUL:
				*d = R[in->a & (VGPU_NREGS - 1)] *
				     R[in->b & (VGPU_NREGS - 1)];
				break;
			case VGPU_ISA_LD:
				addr[lane] = R[in->a & (VGPU_NREGS - 1)] + in->b;
				*d = vgpu_mem_word(sub, addr[lane], false, 0);
				v->cs_mem_lat_ns += vgpu_lat_ns(v) / 8;	/* L1-hit share */
				break;
			case VGPU_ISA_ST:
				addr[lane] = R[in->a & (VGPU_NREGS - 1)] + in->b;
				vgpu_mem_word(sub, addr[lane], true,
					      R[in->dst & (VGPU_NREGS - 1)]);
				v->cs_mem_lat_ns += vgpu_lat_ns(v);
				break;
			case VGPU_ISA_EXIT:
				done[lane] = true;
				break;
			default:
				sub->error = -EINVAL;
				return VGPU_SCHED_DONE;
			}
		}

		if (sub->error)
			return VGPU_SCHED_DONE;

		if (is_mem) {
			v->cs_mem_ops += VGPU_WARP;
			for (lane = 0; lane < VGPU_WARP; lane++) {
				if (done[lane])
					continue;
				if (!lane || addr[lane] != first) {
					if (lane && addr[lane] != first)
						broadcast = false;
				}
				if (lane) {
					if (addr[lane] != addr[lane - 1] + 1)
						coalesced = false;
				} else {
					first = addr[lane];
				}
			}
			if (broadcast) {
				v->cs_bcast_warps++;
			} else if (coalesced) {
				v->cs_coalesced_warps++;
			} else {
				v->cs_uncoalesced_warps++;
			}
		}

		/*
		 * The safe point: instruction boundary, register file
		 * consistent and the memory metrics for this instruction
		 * already accounted. A real engine checks a preempt-pending
		 * bit here; we ask the scheduler directly.
		 */
		{
			enum vgpu_sched r = vgpu_sched_check(v, sub);

			if (r != VGPU_SCHED_DONE) {
				cs->pc = pc + 1;
				cs->resuming = true;
				return r;
			}
		}
	}
	return VGPU_SCHED_DONE;
}

/*
 * Account this slice of execution to the context (queue) that owns the
 * launch: engine time, blocks retired. Under v->lock because debugfs
 * reads these counters concurrently.
 */
static void vgpu_launch_account(struct vgpu *v, struct vgpu_submission *sub,
				u64 start_ns)
{
	unsigned long flags;

	spin_lock_irqsave(&v->lock, flags);
	sub->q->cs_time_ns += ktime_get_ns() - start_ns;
	spin_unlock_irqrestore(&v->lock, flags);
}

/*
 * Run a kernel launch — or a slice of one (phase F).
 *
 * Returns VGPU_SCHED_DONE when the whole grid retired; otherwise the
 * scheduler's reason for pausing, with the resume point saved in
 * sub->cs (block/warp/pc + register file). The caller must NOT
 * complete the submission in that case: no fence, no ring reclaim,
 * the argument buffer stays pinned.
 */
enum vgpu_sched vgpu_compute_run(struct vgpu *v, struct vgpu_submission *sub)
{
	struct vgpu_launch *cs = sub->cs;
	u32 warps_per_block = sub->block / VGPU_WARP;
	enum vgpu_sched r = VGPU_SCHED_DONE;

	if (!warps_per_block || sub->block % VGPU_WARP) {
		sub->error = -EINVAL;
		return VGPU_SCHED_DONE;
	}

	if (!cs->started) {
		cs->fw = kzalloc(sizeof(*cs->fw), GFP_KERNEL);
		if (!cs->fw) {
			sub->error = -ENOMEM;
			return VGPU_SCHED_DONE;
		}
		cs->started = true;
	}
	cs->start_ns = ktime_get_ns();

	while (cs->block < sub->grid && !sub->error) {
		while (cs->warp < warps_per_block) {
			u32 tid = cs->block * sub->block + cs->warp * VGPU_WARP;

			r = vgpu_run_warp(v, sub, cs, tid);
			if (r != VGPU_SCHED_DONE)
				goto out;	/* paused mid-grid */
			if (sub->error)
				break;		/* bad instruction: stop */
			v->cs_warps++;
			v->cs_threads += VGPU_WARP;
			cs->warp++;
		}
		if (sub->error)
			break;
		cs->warp = 0;
		cs->block++;
		{
			unsigned long flags;

			spin_lock_irqsave(&v->lock, flags);
			v->cs_blocks++;
			sub->q->cs_blocks++;
			spin_unlock_irqrestore(&v->lock, flags);
		}
		cs->slice_blocks++;
		udelay(VGPU_BLOCK_US);
	}

	if (!sub->error) {
		/* the whole grid retired */
		v->cs_kernels_launched++;
		v->cs_waves += DIV_ROUND_UP((u64)sub->grid * sub->block,
					    VGPU_SM_COUNT * VGPU_MAX_THREADS_SM);
		{
			unsigned long flags;

			spin_lock_irqsave(&v->lock, flags);
			sub->q->cs_launches++;
			spin_unlock_irqrestore(&v->lock, flags);
		}
		kfree(cs->fw);
		cs->fw = NULL;
	}
	r = VGPU_SCHED_DONE;

out:
	vgpu_launch_account(v, sub, cs->start_ns);
	return r;
}
