// SPDX-License-Identifier: GPL-2.0
/*
 * vgpu - phase H: fault containment.
 *
 * The GPU-RAS story of this project (docs/GPGPU-EVOLUTION-PLAN.md §4,
 * phase H). Everything here exists to make ONE thing true: "one
 * context's wild pointer must not take down every other context's
 * work." A real datacentre card that reset the whole device on one bad
 * access would be unusable — every other tenant's in-flight kernels
 * would die with it — so real drivers do two things a "reset and print"
 * never does:
 *
 *   1. record a STRUCTURED fault. Not "the device broke" but {which
 *      submission, which VA, why, which VMID}: NVIDIA's UVM fault buffer,
 *      amdgpu's IH ring. The driver keeps a small fixed ring of those in
 *      host memory (this file), so a fault storm can never exhaust it and
 *      an nvidia-smi-alike can read it.
 *
 *   2. contain the fault to the OFFENDING CONTEXT. The context is marked
 *      poisoned; every engine then refuses to run any further submission
 *      of that context and drains it with -EIO. The device is NOT reset,
 *      other contexts keep running, and a fresh context can still submit.
 *
 * The recovery of the faulting submission itself reuses the ERROR path
 * the whole driver already has — the compute engine sets sub->error, the
 * engine rings the (simulated) interrupt, the bottom half signals the
 * fence with that error and VGPU_WAIT returns it. Phase H adds nothing
 * to that path except the record and the poison; see vgpu_compute.c.
 *
 * Locking: fb_lock is a plain spinlock (irq-safe). vgpu_fault_report()
 * runs in the compute engine's workqueue context with no other lock
 * held — it takes fb_lock for a handful of stores, sets the context's
 * poison bool, and returns. debugfs reads/clears the same ring under the
 * same lock. The vmid we record is sub->vm->vmid, which is assigned once
 * at VM create and immutable, so it is read without vm_lock (the mutex)
 * — which is what keeps this safe to call from a workqueue.
 */
#include <linux/module.h>
#include <linux/seq_file.h>

#include "vgpu.h"

/* The structured-fault reason as a string, for dmesg and the faults file. */
const char *vgpu_fault_str(u32 reason)
{
	switch (reason) {
	case VGPU_FAULT_UNBOUND:	return "unbound-va";
	case VGPU_FAULT_NOT_MANAGED: return "not-managed";
	case VGPU_FAULT_OOR:	return "out-of-range";
	case VGPU_FAULT_INJECTED:	return "injected";
	case VGPU_FAULT_IO:	return "contained-eio";
	default:		return "?";
	}
}

/*
 * Which errno the offending submission's fence should carry. A missing
 * binding is "no such translation" (-ENOENT), an access past the declared
 * range is -ERANGE, and an injected or contained fault is -EIO. The
 * distinct reasons all reach the fence as a negative, which is the one
 * property VGPU_WAIT's caller depends on.
 */
static int vgpu_fault_errno(u32 reason)
{
	switch (reason) {
	case VGPU_FAULT_OOR:	return -ERANGE;
	case VGPU_FAULT_INJECTED:
	case VGPU_FAULT_IO:	return -EIO;
	case VGPU_FAULT_UNBOUND:
	case VGPU_FAULT_NOT_MANAGED:
	default:		return -ENOENT;
	}
}

/*
 * Record one non-recoverable fault and contain its context.
 *
 * Called from the engine (workqueue context) the instant an access
 * cannot be resolved — never from a hardirq, never under v->lock. It is
 * deliberately the single funnel so the record, the [vgpu:fault] log and
 * the poison flag can never disagree.
 *
 * Returns the errno for the submission's fence and (for convenience) the
 * callers set sub->error to it themselves.
 */
int vgpu_fault_report(struct vgpu_submission *sub, u64 va, u32 reason)
{
	struct vgpu *v = sub->v;
	struct vgpu_file *vf = sub->q->file;
	u32 vmid = sub->vm ? sub->vm->vmid : 0;
	struct vgpu_fault *f;
	unsigned long flags;

	/*
	 * Contain first, record second. The context is poisoned the moment
	 * the fault is seen, so by the time this engine finishes the
	 * faulting submission every other engine picking up a sibling of
	 * this context will refuse to run it.
	 */
	vf->poisoned = true;

	spin_lock_irqsave(&v->fb_lock, flags);
	f = &v->fb[v->fb_head];
	f->seqno = sub->seqno;
	f->reason = reason;
	f->va = va;
	f->vmid = vmid;
	f->when = jiffies;
	v->fb_head = (v->fb_head + 1) % VGPU_FAULT_RING;
	if (reason == VGPU_FAULT_INJECTED)
		v->fb_injected++;
	v->fb_total++;
	spin_unlock_irqrestore(&v->fb_lock, flags);

	dev_info(&v->pdev->dev,
		 "[vgpu:fault] seq#%u vmid %u va 0x%llx reason=%s: context contained (-EIO siblings), device NOT reset, other contexts run on\n",
		 sub->seqno, vmid, va, vgpu_fault_str(reason));
	return vgpu_fault_errno(reason);
}

/* Clear the ring (debugfs is writable so a test can start from zero). */
void vgpu_fault_clear(struct vgpu *v)
{
	unsigned long flags;

	spin_lock_irqsave(&v->fb_lock, flags);
	memset(v->fb, 0, sizeof(v->fb));
	v->fb_head = 0;
	spin_unlock_irqrestore(&v->fb_lock, flags);
}

/*
 * The "faults" debugfs view: the fixed ring, oldest first, plus the
 * lifetime counters. Greppable by design — the guest test reads a
 * specific fault back out by vmid + reason rather than trusting dmesg.
 */
int vgpu_fault_show(struct seq_file *s, void *data)
{
	struct vgpu *v = s->private;
	u32 start, i;
	unsigned long flags;

	spin_lock_irqsave(&v->fb_lock, flags);
	seq_printf(s, "faults %u  (ring of %u, %u injected)\n",
		   v->fb_total, VGPU_FAULT_RING, v->fb_injected);
	/*
	 * Oldest-first: the next slot to write is the oldest entry once the
	 * ring has wrapped, so start there and walk the whole ring.
	 */
	start = v->fb_total >= VGPU_FAULT_RING ? v->fb_head : 0;
	for (i = 0; i < VGPU_FAULT_RING && i < v->fb_total; i++) {
		u32 idx = (start + i) % VGPU_FAULT_RING;
		struct vgpu_fault *f = &v->fb[idx];

		if (!f->reason && !f->seqno)
			continue;
		seq_printf(s,
			   "#%u seq#%u vmid %u va 0x%llx reason=%s jiffies %lu\n",
			   i + 1, f->seqno, f->vmid, f->va,
			   vgpu_fault_str(f->reason), f->when);
	}
	spin_unlock_irqrestore(&v->fb_lock, flags);
	return 0;
}
