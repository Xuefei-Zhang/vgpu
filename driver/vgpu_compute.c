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
 */
#include <linux/module.h>
#include <linux/slab.h>

#include "vgpu.h"

#define VGPU_SM_COUNT		2	/* simulated SMs */
#define VGPU_WARP		32	/* lanes per warp */
#define VGPU_MAX_THREADS_SM	256	/* occupancy ceiling per SM */
#define VGPU_MEM_LAT_NS		40	/* global-memory latency model */

#define VGPU_NREGS		16

#define R(i)			regs[(i) & (VGPU_NREGS - 1)]
#define VGPU_WORDS_PER_PAGE	(PAGE_SIZE / sizeof(u32))

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
	struct drm_gem_shmem_object *sh =
		to_drm_gem_shmem_obj(sub->args_obj);
	struct page *page;
	u32 *p;

	page = sh->pages[addr / VGPU_WORDS_PER_PAGE];
	if (!page)
		return 0;
	p = kmap_local_page(page);
	if (write)
		p[addr % VGPU_WORDS_PER_PAGE] = val;
	else
		val = p[addr % VGPU_WORDS_PER_PAGE];
	kunmap_local(p);
	return val;
}

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
 */
/*
 * One warp's register file. Heap-allocated once per launch (the engine
 * executes one submission at a time, so a single frame is reused by
 * every warp): 32 lanes x 16 registers on the kernel stack would
 * exceed the 2048-byte frame limit.
 */
struct vgpu_warp_frame {
	u32 regs[VGPU_WARP][VGPU_NREGS];
	bool done[VGPU_WARP];
};

static void vgpu_run_warp(struct vgpu *v, struct vgpu_submission *sub,
			  struct vgpu_warp_frame *fw, u32 base_tid)
{
	struct vgpu_kernel *k = sub->kernel;
	u32 (*regs)[VGPU_NREGS] = fw->regs;
	bool *done = fw->done;
	bool all_done = false;
	u32 pc, lane;

	memset(fw, 0, sizeof(*fw));
	for (lane = 0; lane < VGPU_WARP; lane++)
		regs[lane][0] = base_tid + lane;

	for (pc = 0; pc < k->ninstr && !all_done; pc++) {
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
				v->cs_mem_lat_ns += VGPU_MEM_LAT_NS / 8;
				break;
			case VGPU_ISA_ST:
				addr[lane] = R[in->a & (VGPU_NREGS - 1)] + in->b;
				vgpu_mem_word(sub, addr[lane], true,
					      R[in->dst & (VGPU_NREGS - 1)]);
				v->cs_mem_lat_ns += VGPU_MEM_LAT_NS;
				break;
			case VGPU_ISA_EXIT:
				done[lane] = true;
				break;
			default:
				sub->error = -EINVAL;
				return;
			}
		}

		if (!is_mem || sub->error)
			continue;

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
}

void vgpu_compute_run(struct vgpu *v, struct vgpu_submission *sub)
{
	u32 warps_per_block = sub->block / VGPU_WARP;
	struct vgpu_warp_frame *fw;
	u32 block, warp;

	if (!warps_per_block || sub->block % VGPU_WARP) {
		sub->error = -EINVAL;
		return;
	}

	fw = kzalloc(sizeof(*fw), GFP_KERNEL);
	if (!fw) {
		sub->error = -ENOMEM;
		return;
	}

	for (block = 0; block < sub->grid && !sub->error; block++) {
		for (warp = 0; warp < warps_per_block; warp++) {
			u32 tid = block * sub->block + warp * VGPU_WARP;

			vgpu_run_warp(v, sub, fw, tid);
			if (sub->error)
				return;
			v->cs_warps++;
			v->cs_threads += VGPU_WARP;
		}
		v->cs_blocks++;
	}

	kfree(fw);
	v->cs_kernels_launched++;
	v->cs_waves += DIV_ROUND_UP(sub->grid * sub->block,
				    VGPU_SM_COUNT * VGPU_MAX_THREADS_SM);
}
