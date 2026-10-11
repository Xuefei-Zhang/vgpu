// SPDX-License-Identifier: GPL-2.0
/*
 * vgpu - a virtual GPU command submission driver (for learning).
 *
 * This file: platform driver + DRM device + ioctls + debugfs.
 * The command engine, the simulated interrupt path and the fences
 * live in vgpu_engine.c.
 *
 * Modelled on drivers/gpu/drm/vgem (see docs/DESIGN.md for the full
 * design and for the differences from that driver).
 */

#include <linux/debugfs.h>
#include <linux/pm_runtime.h>
#include <linux/ktime.h>
#include <linux/idr.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>

#include <drm/drm_drv.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_vma_manager.h>

#include "vgpu.h"

/* ---------------------------------------------------------------- ioctls */

static int vgpu_bo_create_ioctl(struct drm_device *dev, void *data,
				struct drm_file *file);
static int vgpu_ring_get_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file);
static int vgpu_queue_create_ioctl(struct drm_device *dev, void *data,
				   struct drm_file *file);
static int vgpu_queue_destroy_ioctl(struct drm_device *dev, void *data,
				    struct drm_file *file);
static int vgpu_doorbell_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file);
static int vgpu_module_load_ioctl(struct drm_device *dev, void *data,
				  struct drm_file *file);
static int vgpu_module_destroy_ioctl(struct drm_device *dev, void *data,
				     struct drm_file *file);
/* phase G: vgpu_sync.c */
int vgpu_sem_bind_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file);
int vgpu_graph_ioctl(struct drm_device *dev, void *data,
		     struct drm_file *file);
/* phase C: vgpu_vm.c */
static int vgpu_vm_alloc_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file);
static int vgpu_vm_bind_ioctl(struct drm_device *dev, void *data,
			      struct drm_file *file);
static int vgpu_vm_unbind_ioctl(struct drm_device *dev, void *data,
				struct drm_file *file);
static int vgpu_cs_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file);
static int vgpu_wait_ioctl(struct drm_device *dev, void *data,
			   struct drm_file *file);
static int vgpu_query_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file);
static void vgpu_pm_submit_begin(struct vgpu *v);

/*
 * Runtime PM callbacks. QEMU has no GPU power domains, so there is no
 * hardware to gate — the callbacks exist to make the power-gate
 * observable and countable, which is what a real driver's rpm
 * discipline is FOR: the device sleeps when nobody submits, and the
 * wake is measured at the submit path.
 */
static int vgpu_runtime_suspend(struct device *dev)
{
	struct vgpu *v = dev_get_drvdata(dev);

	v->rpm_suspends++;
	dev_info(dev, "[vgpu:rpm] power-gated (#%u)\n", v->rpm_suspends);
	return 0;
}

static int vgpu_runtime_resume(struct device *dev)
{
	return 0;
}

static const struct dev_pm_ops vgpu_pm_ops = {
	.runtime_suspend = vgpu_runtime_suspend,
	.runtime_resume	= vgpu_runtime_resume,
};

static const struct drm_ioctl_desc vgpu_ioctls[] = {
	DRM_IOCTL_DEF_DRV(VGPU_BO_CREATE, vgpu_bo_create_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VGPU_RING_GET, vgpu_ring_get_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VGPU_CS, vgpu_cs_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VGPU_WAIT, vgpu_wait_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VGPU_QUERY, vgpu_query_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VGPU_QUEUE_CREATE, vgpu_queue_create_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VGPU_QUEUE_DESTROY, vgpu_queue_destroy_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VGPU_DOORBELL, vgpu_doorbell_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VGPU_MODULE_LOAD, vgpu_module_load_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VGPU_MODULE_DESTROY, vgpu_module_destroy_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VGPU_SEM_BIND, vgpu_sem_bind_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VGPU_GRAPH, vgpu_graph_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VGPU_VM_ALLOC, vgpu_vm_alloc_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VGPU_VM_BIND, vgpu_vm_bind_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VGPU_VM_UNBIND, vgpu_vm_unbind_ioctl, DRM_RENDER_ALLOW),
};

DEFINE_DRM_GEM_FOPS(vgpu_drm_fops);

/* defined with the queue subsystem below; the driver struct wants them */
static int vgpu_open(struct drm_device *dev, struct drm_file *file);
static void vgpu_postclose(struct drm_device *dev, struct drm_file *file);

static const struct drm_driver vgpu_drm_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_RENDER,
	.ioctls			= vgpu_ioctls,
	.num_ioctls		= ARRAY_SIZE(vgpu_ioctls),
	.fops			= &vgpu_drm_fops,
	.open			= vgpu_open,
	.postclose		= vgpu_postclose,
	.dumb_create		= drm_gem_shmem_dumb_create,
	.gem_prime_import	= drm_gem_shmem_prime_import_no_map,
	.name			= "vgpu",
	.desc			= "Virtual GPU command submission (learning driver)",
	.major			= 1,
	.minor			= 0,
};

/* ------------------------------------------------------------- debugfs */

static const char *const vgpu_engine_names[VGPU_ENGINE_COUNT] = {
	"gfx", "copy", "compute",
};

static int vgpu_dbg_stats_show(struct seq_file *s, void *unused)
{
	struct vgpu *v = s->private;
	unsigned long flags;
	int i;

	seq_printf(s, "clock      %u MHz\n", v->clock_hz);
	seq_printf(s, "rpm        suspends %u, last resume %llu us\n",
		   v->rpm_suspends, v->rpm_last_resume_ns / 1000);

	spin_lock_irqsave(&v->lock, flags);
	seq_printf(s, "submitted  %llu\n", v->stats_submitted);
	seq_printf(s, "completed  %llu\n", v->stats_completed);
	seq_printf(s, "errors     %llu\n", v->stats_errors);
	for (i = 0; i < VGPU_ENGINE_COUNT; i++)
		seq_printf(s, "scratch[%s] %u / %u bytes\n",
			   vgpu_engine_names[i], v->scratch_used[i],
			   VGPU_SCRATCH_SIZE);
	for (i = 0; i < VGPU_ENGINE_COUNT; i++) {
		struct vgpu_submission *sub;
		u32 backlog = 0;

		list_for_each_entry(sub, &v->engine_pending[i], node)
			backlog++;
		seq_printf(s, "engine[%-8s] backlog %u\n",
			   vgpu_engine_names[i], backlog);
	}
	spin_unlock_irqrestore(&v->lock, flags);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(vgpu_dbg_stats);

/*
 * The DVFS knob: /sys/kernel/debug/vgpu/clock_hz. Writing a frequency
 * changes the compute engine's modelled memory latency on the next
 * launch (see vgpu_lat_ns()), which is how a real frequency governor
 * shows up in kernel runtimes.
 */
static ssize_t vgpu_dbg_clock_read(struct file *f, char __user *buf,
				   size_t n, loff_t *off)
{
	struct vgpu *v = file_inode(f)->i_private;
	char tmp[64];
	int len;

	len = snprintf(tmp, sizeof(tmp), "%u\n", v->clock_hz);
	return simple_read_from_buffer(buf, n, off, tmp, len);
}

static ssize_t vgpu_dbg_clock_write(struct file *f, const char __user *buf,
				    size_t n, loff_t *off)
{
	struct vgpu *v = file_inode(f)->i_private;
	u32 hz;
	int ret;

	ret = kstrtou32_from_user(buf, n, 0, &hz);
	if (ret)
		return ret;
	if (hz < 100 || hz > 4000)
		return -EINVAL;

	spin_lock(&v->lock);
	v->clock_hz = hz;
	spin_unlock(&v->lock);
	dev_info(&v->pdev->dev, "[vgpu:dvfs] clock set to %u MHz\n", hz);
	return n;
}

static const struct file_operations vgpu_dbg_clock_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.read	= vgpu_dbg_clock_read,
	.write	= vgpu_dbg_clock_write,
};

/*
 * The per-device queue view: every live queue (any fd) with its engine,
 * priority, ring occupancy and per-queue counters — the driver-side
 * picture of what userspace calls streams.
 */
static int vgpu_dbg_queues_show(struct seq_file *s, void *unused)
{
	struct vgpu *v = s->private;
	struct vgpu_queue *q;
	unsigned long flags;

	spin_lock_irqsave(&v->lock, flags);
	list_for_each_entry(q, &v->queue_list, dev_node) {
		seq_printf(s,
			   "q#%u engine=%-8s prio=%u ring %u/%u B  sub=%llu done=%llu err=%llu launches=%llu compute %llu ns%s\n",
			   q->qid, vgpu_engine_names[q->engine], q->prio,
			   q->ring_used, q->ring_size,
			   q->stats_submitted, q->stats_completed,
			   q->stats_errors, q->cs_launches, q->cs_time_ns,
			   q->destroyed ? " (draining)" : "");
	}
	spin_unlock_irqrestore(&v->lock, flags);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(vgpu_dbg_queues);

static int vgpu_dbg_compute_show(struct seq_file *s, void *unused)
{
	struct vgpu *v = s->private;
	struct vgpu_queue *q;
	unsigned long flags;

	spin_lock_irqsave(&v->lock, flags);
	seq_printf(s, "kernels   %llu\n", v->cs_kernels_launched);
	seq_printf(s, "blocks    %llu\n", v->cs_blocks);
	seq_printf(s, "warps     %llu\n", v->cs_warps);
	seq_printf(s, "threads   %llu\n", v->cs_threads);
	seq_printf(s, "waves     %llu\n", v->cs_waves);
	/*
	 * Integer fixed-point on purpose: the kernel builds without SSE
	 * ("SSE register return with SSE disabled" is what a stray double
	 * earns you), so percentages are scaled by 10.
	 */
	seq_printf(s, "instrs    %llu  (%u.%u per thread)\n", v->cs_instrs,
		   v->cs_threads ?
		   (u32)(v->cs_instrs / v->cs_threads) : 0,
		   v->cs_threads ?
		   (u32)((v->cs_instrs * 10 / v->cs_threads) % 10) : 0);
	seq_printf(s, "mem_ops   %llu\n", v->cs_mem_ops);
	/*
	 * The memory transaction taxonomy, per warp: broadcast (all
	 * lanes same word = 1 transaction), coalesced (consecutive
	 * words = 1), strided (the rest = one per lane).
	 */
	seq_printf(s, "broadcast warps    %llu / %llu\n",
		   v->cs_bcast_warps, v->cs_warps);
	seq_printf(s, "coalesced warps    %llu / %llu\n",
		   v->cs_coalesced_warps, v->cs_warps);
	seq_printf(s, "strided warps      %llu / %llu\n",
		   v->cs_uncoalesced_warps, v->cs_warps);
	seq_printf(s, "modelled mem latency %llu ns\n", v->cs_mem_lat_ns);

	/*
	 * phase F: the scheduler's view of the compute engine. ctx
	 * switches is every hand-off at a safe point; preemptions /
	 * timeslices say why. Interleaving between two long launches
	 * shows up as ctx switches > 0.
	 */
	seq_printf(s, "ctx switches %llu\n", v->cs_ctx_switches);
	seq_printf(s, "preemptions  %llu\n", v->cs_preemptions);
	seq_printf(s, "timeslices   %llu\n", v->cs_timeslices);
	seq_puts(s, "per-context compute (q engine prio launches blocks ms preempted switches)\n");
	list_for_each_entry(q, &v->queue_list, dev_node) {
		if (q->engine != VGPU_ENGINE_COMPUTE)
			continue;
		seq_printf(s, "q#%u %s prio=%u launches=%llu blocks=%llu %llu ms preempted=%llu switches=%llu\n",
			   q->qid, vgpu_engine_names[q->engine], q->prio,
			   q->cs_launches, q->cs_blocks,
			   q->cs_time_ns / 1000000, q->cs_preempted,
			   q->cs_switches);
	}
	spin_unlock_irqrestore(&v->lock, flags);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(vgpu_dbg_compute);

/*
 * Phase G's counters: how much of the device's synchronization is
 * memory-semantics (timeline values written in BO memory the CPU
 * polls) rather than kernel-side waits.
 */
static int vgpu_dbg_sync_show(struct seq_file *s, void *unused)
{
	struct vgpu *v = s->private;
	unsigned long flags;

	spin_lock_irqsave(&v->lock, flags);
	seq_printf(s, "graphs    %llu  (one doorbell per DAG)\n",
		   v->stats_graphs);
	seq_printf(s, "signals   %llu  (timeline values written)\n",
		   v->stats_signals);
	seq_printf(s, "waits     %llu  (timeline values waited on)\n",
		   v->stats_waits);
	seq_printf(s, "events    %llu  (event slots written)\n",
		   v->stats_events);
	spin_unlock_irqrestore(&v->lock, flags);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(vgpu_dbg_sync);

static int vgpu_dbg_pending_show(struct seq_file *s, void *unused)
{
	struct vgpu *v = s->private;
	struct vgpu_submission *sub;
	unsigned long flags;
	bool any = false;
	int i;

	spin_lock_irqsave(&v->lock, flags);
	for (i = 0; i < VGPU_ENGINE_COUNT; i++) {
		list_for_each_entry(sub, &v->engine_pending[i], node) {
			seq_printf(s, "pending   seq#%u ncmds=%u engine=%s\n",
				   sub->seqno, sub->ncmds,
				   vgpu_engine_names[i]);
			if (sub->cs)
				seq_printf(s,
					   "  launch  seq#%u at block %u/%u warp %u pc %u%s",
					   sub->seqno, sub->cs->block,
					   sub->grid, sub->cs->warp,
					   sub->cs->pc,
					   sub->cs->resuming ? " (mid-warp)\n" : "\n");
			any = true;
		}
	}
	list_for_each_entry(sub, &v->in_flight, node) {
		seq_printf(s, "in-flight seq#%u ncmds=%u err=%d%s\n",
			   sub->seqno, sub->ncmds, (int)sub->error,
			   sub->is_graph ? " graph" : "");
		if (sub->cs)
			seq_printf(s,
				   "  launch  seq#%u at block %u/%u warp %u pc %u%s",
				   sub->seqno, sub->cs->block, sub->grid,
				   sub->cs->warp, sub->cs->pc,
				   sub->cs->resuming ? " (mid-warp)\n" : "\n");
		any = true;
	}
	spin_unlock_irqrestore(&v->lock, flags);
	if (!any)
		seq_puts(s, "(none)\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(vgpu_dbg_pending);

/*
 * The phase-C view: per context (VMID), every reserved VA range with
 * its residency state, and the device's VRAM budget + eviction
 * counters — the nvidia-smi-shaped half of this phase.
 *
 * Writable too: writing a new budget applies it immediately, which is
 * how the guest test drives eviction without a module parameter.
 */
static int vgpu_dbg_vm_open(struct inode *inode, struct file *file)
{
	return single_open(file, vgpu_vm_show, inode->i_private);
}

static ssize_t vgpu_dbg_vm_write(struct file *file, const char __user *ubuf,
				 size_t len, loff_t *ppos)
{
	struct vgpu *v = file->f_inode->i_private;
	char buf[32];
	u64 val;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';
	if (kstrtou64(strim(buf), 0, &val))
		return -EINVAL;

	mutex_lock(&v->vm_lock);
	v->vram_budget = val;
	mutex_unlock(&v->vm_lock);
	vgpu_vm_evict_all(v);		/* a smaller budget bites at once */

	return len;
}

static const struct file_operations vgpu_dbg_vm_fops = {
	.owner	= THIS_MODULE,
	.open	= vgpu_dbg_vm_open,
	.read	= seq_read,
	.write	= vgpu_dbg_vm_write,
	.llseek	= seq_lseek,
	.release = single_release,
};

static int vgpu_dbg_regs_show(struct seq_file *s, void *unused)
{
	struct vgpu *v = s->private;
	unsigned long flags;

	spin_lock_irqsave(&v->lock, flags);
	seq_printf(s, "status = 0x%08x\n", v->regs->status);
	seq_printf(s, "seqno  = %u\n", v->regs->seqno);
	seq_printf(s, "error  = %u\n", v->regs->error);
	seq_printf(s, "user   = 0x%08x\n", v->regs->user);
	spin_unlock_irqrestore(&v->lock, flags);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(vgpu_dbg_regs);

/* ---------------------------------------------------------------- probe */

static int vgpu_probe(struct platform_device *pdev)
{
	struct vgpu *v;
	int ret;
	int i;

	dev_info(&pdev->dev, "[vgpu:probe] probing virtual GPU\n");

	if (!devres_open_group(&pdev->dev, NULL, GFP_KERNEL))
		return -ENOMEM;

	/* behave like a 64-bit DMA-capable device (vgem does the same) */
	dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));

	v = devm_drm_dev_alloc(&pdev->dev, &vgpu_drm_driver, struct vgpu, dev);
	if (IS_ERR(v)) {
		ret = PTR_ERR(v);
		goto err_devres;
	}
	v->pdev = pdev;
	v->clock_hz = VGPU_REF_CLOCK_HZ;	/* DVFS starts at the reference */
	spin_lock_init(&v->lock);
	INIT_LIST_HEAD(&v->in_flight);
	INIT_LIST_HEAD(&v->queue_list);
	INIT_LIST_HEAD(&v->kernel_list);
	mutex_init(&v->fence_mutex);
	idr_init(&v->fence_idr);
	v->next_seqno = 1;	/* dma_fence seqno 0 is reserved */

	/* phase C: the GPU address space / VRAM manager */
	mutex_init(&v->vm_lock);
	INIT_LIST_HEAD(&v->vm_list);
	INIT_LIST_HEAD(&v->vram_lru);
	ida_init(&v->vm_ida);
	v->vram_budget = VGPU_VM_VRAM_BUDGET;
	v->vram_resident = 0;

	/*
	 * The "BAR0" register file: coherent memory, like a real MMIO
	 * region must be (CPU and "device" access it at all times).
	 */
	v->regs = dma_alloc_coherent(&pdev->dev, sizeof(*v->regs),
				     &v->regs_dma, GFP_KERNEL);
	if (!v->regs) {
		ret = -ENOMEM;
		goto err_devres;
	}

	/*
	 * Command fetch areas: one per engine (see vgpu.h: per-engine
	 * scratch is what keeps the reclamation watermark FIFO when
	 * engines complete out of order).
	 */
	for (i = 0; i < VGPU_ENGINE_COUNT; i++) {
		v->scratch[i] = dma_alloc_coherent(&pdev->dev,
						   VGPU_SCRATCH_SIZE,
						   &v->scratch_dma[i],
						   GFP_KERNEL);
		if (!v->scratch[i]) {
			ret = -ENOMEM;
			goto err_devres;
		}
	}

	/*
	 * Runtime PM: the device powers down after 2s idle (autosuspend)
	 * and the next submission wakes it — the probe/remove and
	 * get/put discipline is the lesson; QEMU has no real power domains,
	 * the measurable thing is the wake cost, not the watts.
	 */
	pm_runtime_enable(&pdev->dev);
	pm_runtime_set_autosuspend_delay(&pdev->dev, 2000);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_put_autosuspend(&pdev->dev);	/* idle until first submit */

	/* per-engine workqueues + the bottom-half queue + watchdog */
	vgpu_engine_init(v);

	v->dbg = debugfs_create_dir("vgpu", NULL);
	debugfs_create_file("stats", 0444, v->dbg, v, &vgpu_dbg_stats_fops);
	debugfs_create_file("queues", 0444, v->dbg, v, &vgpu_dbg_queues_fops);
	debugfs_create_file("compute", 0444, v->dbg, v, &vgpu_dbg_compute_fops);
	debugfs_create_file("sync", 0444, v->dbg, v, &vgpu_dbg_sync_fops);
	debugfs_create_file("pending", 0444, v->dbg, v, &vgpu_dbg_pending_fops);
	debugfs_create_file("regs", 0444, v->dbg, v, &vgpu_dbg_regs_fops);
	debugfs_create_file("vm", 0644, v->dbg, v, &vgpu_dbg_vm_fops);
	debugfs_create_file("clock_hz", 0644, v->dbg, v, &vgpu_dbg_clock_fops);

	ret = drm_dev_register(&v->dev, 0);
	if (ret)
		goto err_devres;

	platform_set_drvdata(pdev, v);
	dev_info(&pdev->dev,
		 "[vgpu:probe] ready: /dev/dri/card%d (renderD128), %d engines\n",
		 v->dev.primary->index, VGPU_ENGINE_COUNT);
	return 0;

err_devres:
	devres_release_group(&pdev->dev, NULL);
	return ret;
}

/* ------------------------------------------------- the queue subsystem */

/*
 * Packet type -> the command class an engine accepts. The packet
 * vocabulary grew past the legacy opcodes in phase B; the engine
 * class check is still about *what kind of work* it is, so translate
 * rather than duplicate the table.
 */
u32 vgpu_pkt_opcode(u32 type)
{
	switch (type) {
	case VGPU_PKT_COPY:		return VGPU_OP_BLIT;
	case VGPU_PKT_WRITE_REG:	return VGPU_OP_SET;
	case VGPU_PKT_LAUNCH:		return VGPU_OP_LAUNCH;
	case VGPU_PKT_SIGNAL_TIMELINE:	return VGPU_OP_SIGNAL_TIMELINE;
	case VGPU_PKT_WAIT_TIMELINE:	return VGPU_OP_WAIT_TIMELINE;
	case VGPU_PKT_WRITE_EVENT:	return VGPU_OP_WRITE_EVENT;
	default:			return VGPU_OP_NOP;
	}
}

/* the phase G memory-semantics packets */
bool vgpu_pkt_is_sync(u32 type)
{
	return type == VGPU_PKT_SIGNAL_TIMELINE ||
	       type == VGPU_PKT_WAIT_TIMELINE ||
	       type == VGPU_PKT_WRITE_EVENT;
}

/*
 * Which commands does an engine accept? A real device's engines accept
 * disjoint command classes (a DMA engine cannot execute a shader; a
 * compute engine is not where you put a blit). Rejecting at the
 * doorbell keeps the error visible to the submitter instead of feeding
 * garbage to the engine.
 */
static bool vgpu_engine_allows(u32 engine, u32 opcode)
{
	switch (engine) {
	case VGPU_ENGINE_COPY:
		return opcode == VGPU_OP_NOP || opcode == VGPU_OP_BLIT ||
		       opcode == VGPU_OP_SIGNAL_TIMELINE ||
		       opcode == VGPU_OP_WAIT_TIMELINE ||
		       opcode == VGPU_OP_WRITE_EVENT ||
		       opcode == VGPU_OP_VM_COPY;
	case VGPU_ENGINE_COMPUTE:
		return opcode == VGPU_OP_NOP || opcode == VGPU_OP_SET ||
		       opcode == VGPU_OP_LAUNCH ||
		       opcode == VGPU_OP_SIGNAL_TIMELINE ||
		       opcode == VGPU_OP_WAIT_TIMELINE ||
		       opcode == VGPU_OP_WRITE_EVENT;
	case VGPU_ENGINE_GFX:
		return opcode == VGPU_OP_NOP || opcode == VGPU_OP_BLIT ||
		       opcode == VGPU_OP_SET || opcode == VGPU_OP_LAUNCH ||
		       opcode == VGPU_OP_SIGNAL_TIMELINE ||
		       opcode == VGPU_OP_WAIT_TIMELINE ||
		       opcode == VGPU_OP_WRITE_EVENT ||
		       opcode == VGPU_OP_VM_COPY;
	}
	return false;
}

/*
 * Find a queue by id inside this fd. Not static: the graph ioctl
 * (vgpu_sync.c) resolves the target queue the same way.
 */
struct vgpu_queue *vgpu_queue_find(struct vgpu_file *vf, u32 qid)
{
	struct vgpu_queue *q;

	list_for_each_entry(q, &vf->queues, node)
		if (q->qid == qid)
			return q;
	return NULL;
}

/*
 * Free a queue's ring BO. Ring BOs are owned by the queue, not by any
 * GEM handle: the drop-creation-reference-after-handle-create rule
 * (commit 452872a) applies at creation, so a plain put() here releases
 * the final reference once no fd handle points at it.
 */

static void vgpu_queue_free(struct vgpu *v, struct vgpu_queue *q)
{
	if (q->ring_bo) {
		drm_gem_shmem_unpin(q->ring_bo);
		drm_gem_object_put(&q->ring_bo->base);
	}
	kfree(q);
}

struct vgpu_kernel *vgpu_kernel_find(struct vgpu_file *vf, u32 kid)
{
	struct vgpu_kernel *k;

	list_for_each_entry(k, &vf->kernels, file_node)
		if (k->kid == kid)
			return k;
	return NULL;
}static void vgpu_file_free_kernels(struct vgpu_file *vf)
{
	struct vgpu_kernel *k, *tmp;
	unsigned long flags;
	struct vgpu *v = vf->vf_dev;
	LIST_HEAD(to_free);

	spin_lock_irqsave(&v->lock, flags);
	list_for_each_entry_safe(k, tmp, &vf->kernels, file_node) {
		list_del_init(&k->file_node);
		list_del_init(&k->node);
		list_add_tail(&k->node, &to_free);
	}
	spin_unlock_irqrestore(&v->lock, flags);

	list_for_each_entry_safe(k, tmp, &to_free, node) {
		list_del(&k->node);
		kfree(k->code);
		kfree(k);
	}
}

/*
 * Is this queue still doing work? Walk the device-wide engine lists
 * for submissions that belong to it.
 *
 * ring_used alone is not enough any more: a VGPU_GRAPH submission
 * carries no ring bytes at all (the nodes came in through the ioctl,
 * not through the queue's ring), so "ring_used == 0" would happily
 * free a queue out from under a running graph — and the engine's
 * bottom half writes through sub->q.
 */
/*
 * Is anything from this queue in flight? MUST be called with v->lock
 * already held — taking it here made vgpu_queue_destroy_ioctl()
 * (which holds it) deadlock on itself, which is exactly what lockdep
 * reported ("possible recursive locking detected").
 */
static bool vgpu_queue_busy_locked(struct vgpu *v, struct vgpu_queue *q)
{
	struct vgpu_submission *sub;
	bool busy = false;
	int i;

	for (i = 0; i < VGPU_ENGINE_COUNT && !busy; i++)
		list_for_each_entry(sub, &v->engine_pending[i], node)
			if (sub->q == q) {
				busy = true;
				break;
			}
	if (!busy)
		list_for_each_entry(sub, &v->in_flight, node)
			if (sub->q == q)
				busy = true;
	return busy;
}

static void vgpu_file_destroy_all_queues(struct vgpu_file *vf)
{
	struct vgpu *v = vf->vf_dev;
	struct list_head to_free = LIST_HEAD_INIT(to_free);
	struct vgpu_queue *q, *tmp;
	unsigned long flags;
	int i;

	/*
	 * Wait for anything in flight on these queues to retire. The
	 * engines run first (a submission may still be executing, and
	 * the bottom half writes through sub->q), then the bottom
	 * halves.
	 */
	for (i = 0; i < VGPU_ENGINE_COUNT; i++)
		flush_workqueue(v->engine_wq[i]);
	flush_workqueue(v->irq_wq);

	spin_lock_irqsave(&v->lock, flags);
	list_for_each_entry_safe(q, tmp, &vf->queues, node) {
		list_del_init(&q->node);	/* file list */
		list_del_init(&q->dev_node);	/* device list */
		list_add_tail(&q->node, &to_free);
	}
	spin_unlock_irqrestore(&v->lock, flags);

	list_for_each_entry_safe(q, tmp, &to_free, node) {
		list_del(&q->node);
		vgpu_queue_free(v, q);
	}
}

static int vgpu_open(struct drm_device *dev, struct drm_file *file)
{
	struct vgpu *v = to_vgpu(dev);
	struct vgpu_file *vf;

	vf = kzalloc(sizeof(*vf), GFP_KERNEL);
	if (!vf)
		return -ENOMEM;

	INIT_LIST_HEAD(&vf->queues);
	INIT_LIST_HEAD(&vf->kernels);
	vf->default_q = NULL;
	vf->vm = NULL;
	vf->drm_file = file;
	vf->vf_dev = v;
	vf->next_qid = 1;
	vf->next_kid = 1;
	file->driver_priv = vf;
	return 0;
}

static void vgpu_postclose(struct drm_device *dev, struct drm_file *file)
{
	struct vgpu_file *vf = file->driver_priv;

	if (!vf)
		return;

	vgpu_file_destroy_all_queues(vf);
	vgpu_file_free_kernels(vf);
	/*
	 * Phase C: the fd's GPU address space. Queues first — a queue in
	 * flight may still be resolving VAs against this VM's tables, and
	 * destroying the queues drains them.
	 */
	vgpu_vm_destroy(vf);
	kfree(vf);
	file->driver_priv = NULL;
}

static void vgpu_drain(struct vgpu *v)
{
	/*
	 * Reset path: finish everything that was submitted.
	 * Order matters:
	 *  1. flush every engine workqueue (one per engine kind) plus
	 *     the bottom half, so nothing is executing while we take
	 *     the lists,
	 *  2. finish remaining submissions (signals their fences),
	 *  3. reap every fence still in the idr.
	 */
	unsigned long flags;
	int i;

	for (i = 0; i < VGPU_ENGINE_COUNT; i++)
		flush_work(&v->engine_work[i]);
	flush_workqueue(v->irq_wq);

	for (i = 0; i < VGPU_ENGINE_COUNT; i++) {
		spin_lock_irqsave(&v->lock, flags);
		while (!list_empty(&v->engine_pending[i]) ||
		       !list_empty(&v->in_flight)) {
			struct vgpu_submission *sub;
			struct list_head *l =
				list_empty(&v->in_flight) ?
				&v->engine_pending[i] : &v->in_flight;

			sub = list_first_entry(l, struct vgpu_submission,
					       node);
			list_del_init(&sub->node);
			spin_unlock_irqrestore(&v->lock, flags);
			if (!sub->error)
				sub->error = -EIO;
			vgpu_finish_submission(v, sub);
			spin_lock_irqsave(&v->lock, flags);
		}
		spin_unlock_irqrestore(&v->lock, flags);
	}

	timer_delete_sync(&v->watchdog);
	flush_work(&v->watchdog_work);

	mutex_lock(&v->fence_mutex);
	{
		struct vgpu_fence *vf;
		int id;

		idr_for_each_entry(&v->fence_idr, vf, id) {
			if (!dma_fence_is_signaled(&vf->base)) {
				vf->base.error = -EIO;
				dma_fence_signal(&vf->base);
			}
			idr_remove(&v->fence_idr, id);
			dma_fence_put(&vf->base);
		}
		idr_destroy(&v->fence_idr);
	}
	mutex_unlock(&v->fence_mutex);
}

static void vgpu_remove(struct platform_device *pdev)
{
	struct vgpu *v = platform_get_drvdata(pdev);
	unsigned long flags;
	u32 s, c, e;
	int i;

	dev_info(&pdev->dev, "[vgpu:remove] draining and shutting down\n");

	/*
	 * Runtime PM teardown: stop the autosuspend first, then bring the
	 * device back up if it powered down while idle — the resources we
	 * are about to free must belong to an active device.
	 */
	pm_runtime_disable(&pdev->dev);
	if (pm_runtime_suspended(&pdev->dev))
		pm_runtime_resume(&pdev->dev);

	vgpu_drain(v);
	drm_dev_unregister(&v->dev);

	/*
	 * Phase C: any fd still open at rmmod would hold object
	 * references against a device that is going away.
	 */
	vgpu_vm_destroy_all(v);
	ida_destroy(&v->vm_ida);

	spin_lock_irqsave(&v->lock, flags);
	s = v->stats_submitted;
	c = v->stats_completed;
	e = v->stats_errors;
	spin_unlock_irqrestore(&v->lock, flags);

	debugfs_remove_recursive(v->dbg);
	for (i = 0; i < VGPU_ENGINE_COUNT; i++)
		dma_free_coherent(&pdev->dev, VGPU_SCRATCH_SIZE,
				  v->scratch[i], v->scratch_dma[i]);
	dma_free_coherent(&pdev->dev, sizeof(*v->regs), v->regs, v->regs_dma);
	vgpu_engine_fini(v);

	devres_release_group(&pdev->dev, NULL);
	dev_info(&pdev->dev,
		 "[vgpu:remove] done (submitted=%u completed=%u errors=%u)\n",
		 s, c, e);
}

static struct platform_driver vgpu_platform_driver = {
	.driver = {
		.name = "vgpu",
		.pm = &vgpu_pm_ops,
	},
	.probe = vgpu_probe,
	.remove = vgpu_remove,
};

/* ------------------------------------------------- the ioctl handlers */

/*
 * VGPU_BO_CREATE: create a shmem-backed GEM object ("VRAM" allocation)
 * and give the app a handle + mmap_offset.
 */
static int vgpu_bo_create_ioctl(struct drm_device *dev, void *data,
				struct drm_file *file)
{
	struct vgpu_bo_create *arg = data;
	struct drm_gem_shmem_object *shmem;
	int ret;

	if (arg->size < PAGE_SIZE || arg->align || arg->flags)
		return -EINVAL;

	shmem = drm_gem_shmem_create(dev, arg->size);
	if (IS_ERR(shmem))
		return PTR_ERR(shmem);

	/* "VRAM" maps write-combined, like real GPU memory */
	shmem->map_wc = true;

	ret = drm_gem_handle_create(file, &shmem->base, &arg->handle);
	if (ret) {
		drm_gem_shmem_free(shmem);
		return ret;
	}
	drm_gem_create_mmap_offset(&shmem->base);
	arg->mmap_offset = drm_vma_node_offset_addr(&shmem->base.vma_node);
	/*
	 * drm_gem_handle_create() took over the creation reference: the
	 * handle owns it now and releases it when the last handle for
	 * this object goes away at close time (exynos-style GEM
	 * refcounting). Drop ours. Without this put the object — and
	 * its vma offset node — outlives the fd, and the DRM teardown
	 * finds the node still registered ("Memory manager not clean
	 * during takedown").
	 */
	drm_gem_object_put(&shmem->base);

	dev_info(dev->dev, "[vgpu:gem] bo#%u size=%llu mmap_offset=0x%llx\n",
		 arg->handle, (unsigned long long)arg->size,
		 (unsigned long long)arg->mmap_offset);
	return 0;
}

/*
 * The actual queue creation, shared by VGPU_QUEUE_CREATE and by the
 * lazy default queue of VGPU_RING_GET.
 *
 * MUST be called WITHOUT v->lock held: drm_gem_shmem_create/pin
 * allocate pages (GFP_KERNEL) and the WC mapping path runs
 * set_pages_array -> smp_call_function, none of which may run in a
 * spinlock-critical section ("BUG: Invalid wait context" is what you
 * get otherwise — FTRACE-GUIDE.md's lesson about checking context
 * before every call). Only the list bookkeeping takes the lock.
 */
static struct vgpu_queue *vgpu_queue_alloc(struct drm_file *file,
					   struct vgpu *v,
					   struct vgpu_file *vf,
					   u32 engine, u32 prio, u32 nslots)
{
	struct drm_device *dev = &v->dev;
	struct vgpu_queue *q;
	unsigned long flags;
	u32 handle;	/* for this fd's mmap allow-list only; the queue
			 * keeps its own object reference (see below) */

	q = kzalloc(sizeof(*q), GFP_KERNEL);
	if (!q)
		return ERR_PTR(-ENOMEM);

	q->ring_size = nslots * VGPU_CMD_SIZE;
	q->ring_bo = drm_gem_shmem_create(dev, q->ring_size);
	if (IS_ERR(q->ring_bo)) {
		void *e = ERR_CAST(q->ring_bo);

		kfree(q);
		return e;
	}
	q->ring_bo->map_wc = true;
	if (drm_gem_shmem_pin(q->ring_bo)) {
		drm_gem_object_put(&q->ring_bo->base);
		kfree(q);
		return ERR_PTR(-ENOMEM);
	}

	/*
	 * One mmap offset for the whole queue lifetime: it must outlive
	 * any per-fd handle (the same rule that fixed the ring offsets).
	 */
	if (drm_gem_create_mmap_offset(&q->ring_bo->base)) {
		drm_gem_shmem_unpin(q->ring_bo);
		drm_gem_object_put(&q->ring_bo->base);
		kfree(q);
		return ERR_PTR(-ENOMEM);
	}

	/*
	 * And a GEM handle for this fd: mmap() of a fake offset is only
	 * allowed for files on the vma node's allow-list, and the way an
	 * fd gets on that list is by holding a handle (EACCES otherwise —
	 * the reason QUEUE_CREATE alone was not enough).
	 *
	 * But unlike a plain BO (whose lifetime is exactly its handle's),
	 * the queue ring outlives any handle: drm_file_free() reaps the
	 * handles BEFORE postclose, so the queue must own its own
	 * reference instead of transferring it to the handle. Keep both
	 * refs here; vgpu_queue_free() drops ours.
	 */
	if (drm_gem_handle_create(file, &q->ring_bo->base, &handle)) {
		drm_gem_shmem_unpin(q->ring_bo);
		drm_gem_object_put(&q->ring_bo->base);
		kfree(q);
		return ERR_PTR(-ENOMEM);
	}
	q->gem_handle = handle;

	INIT_LIST_HEAD(&q->node);
	INIT_LIST_HEAD(&q->dev_node);
	q->file = vf;
	q->engine = engine;
	q->prio = prio;

	/* only the bookkeeping is atomic */
	spin_lock_irqsave(&v->lock, flags);
	q->qid = vf->next_qid++;
	q->fence_context = dma_fence_context_alloc(1);
	list_add_tail(&q->node, &vf->queues);
	list_add_tail(&q->dev_node, &v->queue_list);
	spin_unlock_irqrestore(&v->lock, flags);

	dev_info(dev->dev,
		 "[vgpu:queue] q#%u engine=%s prio=%u ring %u B\n",
		 q->qid, vgpu_engine_names[engine], prio, q->ring_size);
	return q;
}

/*
 * VGPU_RING_GET: legacy entry — hands out this fd's default queue
 * (a normal-priority GFX stream, created on first use). Kept so the
 * original single-stream test path and older callers keep working;
 * everything new should use VGPU_QUEUE_CREATE.
 */
static int vgpu_ring_get_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file)
{
	struct vgpu *v = to_vgpu(dev);
	struct vgpu_ring_get *arg = data;
	struct vgpu_file *vf = file->driver_priv;
	struct vgpu_queue *q;
	struct drm_gem_object *obj;
	unsigned long flags;
	int ret;

	if (!vf)
		return -EINVAL;

	spin_lock_irqsave(&v->lock, flags);
	q = vf->default_q;
	spin_unlock_irqrestore(&v->lock, flags);

	if (!q) {
		q = vgpu_queue_alloc(file, v, vf, VGPU_ENGINE_GFX, 1, 64);
		if (IS_ERR(q))
			return PTR_ERR(q);
		spin_lock_irqsave(&v->lock, flags);
		if (vf->default_q) {
			/* lost the race: another RING_GET got there first */
			spin_unlock_irqrestore(&v->lock, flags);
			vgpu_queue_free(v, q);
			q = vf->default_q;
		} else {
			vf->default_q = q;
			spin_unlock_irqrestore(&v->lock, flags);
		}
	}

	obj = &q->ring_bo->base;
	drm_gem_object_get(obj);
	ret = drm_gem_handle_create(file, obj, &arg->handle);
	drm_gem_object_put(obj);
	arg->mmap_offset = drm_vma_node_offset_addr(&obj->vma_node);
	if (ret)
		return ret;

	dev_info(dev->dev, "[vgpu:ring] fd handle=%u (q#%u) offset=0x%llx\n",
		 arg->handle, q->qid,
		 (unsigned long long)arg->mmap_offset);
	return 0;
}

/*
 * VGPU_QUEUE_CREATE: an in-order command stream on one engine.
 *
 * The queue owns its command ring (a shmem GEM BO), mmap'able through
 * the returned offset. This is the driver half of "streams": one fd
 * can hold many queues, each bound to one engine, and each rings its
 * own doorbell.
 */
static int vgpu_queue_create_ioctl(struct drm_device *dev, void *data,
				   struct drm_file *file)
{
	struct vgpu *v = to_vgpu(dev);
	struct vgpu_queue_create *arg = data;
	struct vgpu_file *vf = file->driver_priv;
	struct vgpu_queue *q;

	if (!vf)
		return -EINVAL;
	if (arg->engine >= VGPU_ENGINE_COUNT)
		return -EINVAL;
	if (arg->prio > 2)
		return -EINVAL;
	if (arg->nslots < 16 || arg->nslots > 64)
		return -EINVAL;

	q = vgpu_queue_alloc(file, v, vf, arg->engine, arg->prio,
				    arg->nslots);
	if (IS_ERR(q))
		return PTR_ERR(q);

	arg->qid = q->qid;
	arg->ring_size = q->ring_size;
	arg->mmap_offset = drm_vma_node_offset_addr(&q->ring_bo->base.vma_node);
	return 0;
}

/*
 * VGPU_QUEUE_DESTROY: retire a queue. Refuses while the queue still
 * has submissions in flight (ring space not reclaimed) — the caller
 * must wait for its fences first. This is exactly the "you cannot
 * destroy a stream with pending work" rule real APIs enforce.
 */
static int vgpu_queue_destroy_ioctl(struct drm_device *dev, void *data,
				    struct drm_file *file)
{
	struct vgpu *v = to_vgpu(dev);
	struct vgpu_queue_destroy *arg = data;
	struct vgpu_file *vf = file->driver_priv;
	struct vgpu_queue *q;
	unsigned long flags;
	int ret = 0;

	if (!vf)
		return -EINVAL;

	spin_lock_irqsave(&v->lock, flags);
	q = vgpu_queue_find(vf, arg->qid);
	if (!q) {
		spin_unlock_irqrestore(&v->lock, flags);
		return -ENOENT;
	}
	if (q->ring_used || vgpu_queue_busy_locked(v, q)) {
		spin_unlock_irqrestore(&v->lock, flags);
		return -EBUSY;		/* submissions still in flight */
	}
	q->destroyed = true;
	list_del_init(&q->node);
	list_del_init(&q->dev_node);
	if (vf->default_q == q)
		vf->default_q = NULL;
	spin_unlock_irqrestore(&v->lock, flags);

	dev_info(dev->dev, "[vgpu:queue] q#%u destroyed\n", arg->qid);

	/* unpin/put may sleep: outside the lock */
	vgpu_queue_free(v, q);
	return ret;
}

/*
 * Resolve one BLIT: handles -> objects, pin the pages, record the
 * range.
 *
 * Real submission drivers do the same: the ioctl context has the
 * drm_file, so it resolves handles -> objects, takes references and
 * pins the pages; the engine (workqueue context, no file) can then
 * execute without any lookup. Constraint: one BLIT per submission.
 *
 * Shared by the doorbell (one BLIT per submission) and by the graph
 * ioctl (one per COPY node) - that sharing is phase G.
 */
int vgpu_blit_pin(struct drm_file *file, struct vgpu_blit_ref *ref,
		  u32 src_bo, u32 dst_bo, u32 off, u32 len)
{
	struct drm_gem_shmem_object *src, *dst;
	u32 npages;
	int j;
	int ret;

	if (ref->src_obj)
		return -EINVAL;	/* one BLIT per submission */

	if (off % PAGE_SIZE || len % PAGE_SIZE || len == 0 ||
	    len > 16 * PAGE_SIZE)
		return -EINVAL;

	ref->src_obj = drm_gem_object_lookup(file, src_bo);
	if (!ref->src_obj)
		return -ENOENT;
	ref->dst_obj = drm_gem_object_lookup(file, dst_bo);
	if (!ref->dst_obj) {
		drm_gem_object_put(ref->src_obj);
		ref->src_obj = NULL;
		return -ENOENT;
	}
	if (off + len > ref->src_obj->size || off + len > ref->dst_obj->size) {
		ret = -ERANGE;
		goto err_put;
	}

	src = to_drm_gem_shmem_obj(ref->src_obj);
	dst = to_drm_gem_shmem_obj(ref->dst_obj);
	ret = drm_gem_shmem_pin(src);
	if (ret)
		goto err_put;
	ret = drm_gem_shmem_pin(dst);
	if (ret) {
		drm_gem_shmem_unpin(src);
		goto err_put;
	}

	ref->blit_off = off >> PAGE_SHIFT;
	npages = len >> PAGE_SHIFT;
	ref->npages = npages;
	ref->src_pages = kcalloc(npages, sizeof(*ref->src_pages), GFP_KERNEL);
	ref->dst_pages = kcalloc(npages, sizeof(*ref->dst_pages), GFP_KERNEL);
	if (!ref->src_pages || !ref->dst_pages) {
		ret = -ENOMEM;
		goto err_unpin;
	}
	for (j = 0; j < npages; j++) {
		ref->src_pages[j] = src->pages[ref->blit_off + j];
		ref->dst_pages[j] = dst->pages[ref->blit_off + j];
		get_page(ref->src_pages[j]);
		get_page(ref->dst_pages[j]);
	}
	return 0;

err_unpin:
	kfree(ref->src_pages);
	kfree(ref->dst_pages);
	ref->src_pages = ref->dst_pages = NULL;
	ref->npages = 0;
	drm_gem_shmem_unpin(dst);
	drm_gem_shmem_unpin(src);
err_put:
	drm_gem_object_put(ref->dst_obj);
	drm_gem_object_put(ref->src_obj);
	ref->src_obj = ref->dst_obj = NULL;
	return ret;
}

void vgpu_blit_unref(struct vgpu *v, struct vgpu_blit_ref *ref)
{
	u32 i;

	if (!ref->src_obj)
		return;
	/*
	 * The arrays hold exactly the range's pages (see
	 * vgpu_blit_pages), so the reference is dropped per entry —
	 * adding blit_off here as well would unbalance the refcount of a
	 * page that was never taken.
	 */
	for (i = 0; i < ref->npages; i++) {
		put_page(ref->src_pages[i]);
		put_page(ref->dst_pages[i]);
	}
	kfree(ref->src_pages);
	kfree(ref->dst_pages);
	drm_gem_shmem_unpin(to_drm_gem_shmem_obj(ref->src_obj));
	drm_gem_shmem_unpin(to_drm_gem_shmem_obj(ref->dst_obj));
	drm_gem_object_put(ref->src_obj);
	drm_gem_object_put(ref->dst_obj);
	ref->src_obj = ref->dst_obj = NULL;
	ref->src_pages = ref->dst_pages = NULL;
	ref->npages = ref->blit_off = 0;
}

/*
 * Phase C: resolve a VA -> VA DMA at submit time.
 *
 * This is where the page-table walk happens, and it has to be here:
 * the ioctl context is the only one that has the drm_file, and the fd
 * is what selects the VMID. The engine (workqueue context, no fd) then
 * just moves bytes between the pages we hand it — which is exactly the
 * division of labour in a real driver, where the submit path programs
 * the MMU and the engine sees DMA addresses.
 *
 * Both sides take the submission's own object + page references, so an
 * unbind or an eviction racing the engine cannot pull the pages away.
 */
static int vgpu_resolve_vm_copy(struct vgpu_file *vf, struct vgpu_packet *p,
				struct vgpu_submission *sub)
{
	u32 npages = sub->vm_npages;
	int ret;

	ret = vgpu_vm_resolve(vf, p->vm_copy.src_va, npages, &sub->vm_src_obj,
			      sub->vm_src_pages);
	if (ret)
		return ret;

	ret = vgpu_vm_resolve(vf, p->vm_copy.dst_va, npages, &sub->vm_dst_obj,
			      sub->vm_dst_pages);
	if (ret) {
		vgpu_vm_release(sub->vm_src_obj, sub->vm_src_pages, npages);
		sub->vm_src_obj = NULL;
		sub->vm_src_pages = NULL;
		return ret;
	}
	return 0;
}

/*
 * VGPU_MODULE_LOAD: register a compute kernel. The "module" a launch
 * references — the driver's job is the same as a real one's: validate
 * the resource request (registers, shared memory), keep a driver-side
 * copy of the code, and hand out an id the scheduler can look up.
 */
static int vgpu_module_load_ioctl(struct drm_device *dev, void *data,
				  struct drm_file *file)
{
	struct vgpu *v = to_vgpu(dev);
	struct vgpu_module_load *arg = data;
	struct vgpu_file *vf = file->driver_priv;
	struct vgpu_kernel *k;
	unsigned long flags;

	if (!vf)
		return -EINVAL;
	if (arg->nregs < 1 || arg->nregs > VGPU_NREGS_MAX ||
	    arg->smem > 4096 || arg->ninstr < 1 || arg->ninstr > 64 ||
	    !arg->code)
		return -EINVAL;

	k = kzalloc(sizeof(*k), GFP_KERNEL);
	if (!k)
		return -ENOMEM;
	k->code = kcalloc(arg->ninstr, sizeof(struct vgpu_instr), GFP_KERNEL);
	if (!k->code) {
		kfree(k);
		return -ENOMEM;
	}
	if (copy_from_user(k->code, u64_to_user_ptr(arg->code),
			   arg->ninstr * sizeof(struct vgpu_instr))) {
		kfree(k->code);
		kfree(k);
		return -EFAULT;
	}

	k->nregs = arg->nregs;
	k->smem = arg->smem;
	k->ninstr = arg->ninstr;
	INIT_LIST_HEAD(&k->node);
	INIT_LIST_HEAD(&k->file_node);

	spin_lock_irqsave(&v->lock, flags);
	k->kid = vf->next_kid++;
	list_add_tail(&k->file_node, &vf->kernels);
	list_add_tail(&k->node, &v->kernel_list);
	spin_unlock_irqrestore(&v->lock, flags);

	arg->kid = k->kid;
	dev_info(dev->dev,
		 "[vgpu:module] kid=%u nregs=%u smem=%u instrs=%u\n",
		 k->kid, k->nregs, k->smem, k->ninstr);
	return 0;
}

static int vgpu_module_destroy_ioctl(struct drm_device *dev, void *data,
				     struct drm_file *file)
{
	struct vgpu *v = to_vgpu(dev);
	struct vgpu_module_destroy *arg = data;
	struct vgpu_file *vf = file->driver_priv;
	struct vgpu_kernel *k;
	unsigned long flags;

	if (!vf)
		return -EINVAL;

	spin_lock_irqsave(&v->lock, flags);
	k = vgpu_kernel_find(vf, arg->kid);
	if (!k) {
		spin_unlock_irqrestore(&v->lock, flags);
		return -ENOENT;
	}
	list_del_init(&k->file_node);
	list_del_init(&k->node);
	spin_unlock_irqrestore(&v->lock, flags);

	dev_info(dev->dev, "[vgpu:module] kid=%u destroyed\n", arg->kid);
	kfree(k->code);
	kfree(k);
	return 0;
}

/*
 * VGPU_DOORBELL: phase B's submission path.
 *
 * Userspace writes PACKETS into the queue's ring (mmap, no syscall per
 * packet) and then rings the doorbell with the new put offset. The
 * kernel does NOT fetch here: it validates the packet range, resolves
 * the buffer handles (this needs the drm_file, and real drivers also
 * validate in submit context), publishes the fence, and hands the
 * packet RANGE to the engine. The engine then does the fetch and the
 * decode — the "hardware" reads the queue, not the syscall. That is
 * the whole point of the phase: per-packet syscalls would cost more
 * than the work itself.
 */
static int vgpu_doorbell_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file)
{
	struct vgpu *v = to_vgpu(dev);
	struct vgpu_doorbell *arg = data;
	struct vgpu_file *vf = file->driver_priv;
	struct vgpu_submission *sub;
	struct vgpu_queue *q;
	struct vgpu_fence *fence;
	struct vgpu_packet *pk;
	struct page *ring_page;
	u32 bytes, i, ncopies = 0, nlaunches = 0;
	struct vgpu_kernel *kernel = NULL;
	unsigned long flags;
	int ret;

	if (!vf)
		return -EINVAL;

	spin_lock_irqsave(&v->lock, flags);
	q = arg->qid ? vgpu_queue_find(vf, arg->qid) : vf->default_q;
	spin_unlock_irqrestore(&v->lock, flags);
	if (!q)
		return -ENOENT;

	/* the doorbell may only move put forward, 32B-aligned, in range */
	if (arg->put % sizeof(struct vgpu_packet) ||
	    arg->put > q->ring_size || arg->put <= q->get)
		return -EINVAL;

	sub = kzalloc(sizeof(*sub), GFP_KERNEL);
	if (!sub)
		return -ENOMEM;
	sub->v = v;
	sub->q = q;
	sub->engine = q->engine;
	sub->ring_off = q->get;
	sub->npkts = (arg->put - q->get) / sizeof(struct vgpu_packet);
	sub->from_doorbell = true;
	sub->ring_bytes = sub->npkts * sizeof(struct vgpu_packet);

	/*
	 * Validate and resolve in submit context (the fd is here, the
	 * engine work has no fd to look handles up with). Two passes:
	 * first the cheap per-packet checks (engine classes, packet
	 * count), then the BLIT resolution — so a rejected doorbell
	 * leaves nothing pinned behind.
	 */
	ring_page = q->ring_bo->pages[sub->ring_off / PAGE_SIZE];
	pk = kmap_local_page(ring_page) + (sub->ring_off % PAGE_SIZE);

	for (i = 0; i < sub->npkts; i++) {
		struct vgpu_packet *p = &pk[i];

		switch (p->type) {
		case VGPU_PKT_NOP:
			break;
		case VGPU_PKT_COPY:
			if (!vgpu_engine_allows(q->engine, VGPU_OP_BLIT) ||
			    ncopies++) {
				ret = -EINVAL;
				goto err_unmap;
			}
			break;
		case VGPU_PKT_WRITE_REG:
			if (!vgpu_engine_allows(q->engine, VGPU_OP_SET)) {
				ret = -EINVAL;
				goto err_unmap;
			}
			break;
		case VGPU_PKT_LAUNCH: {
			u32 kid = p->launch.kid;

			kernel = vgpu_kernel_find(vf, kid);
			if (!kernel ||
			    !vgpu_engine_allows(q->engine, VGPU_OP_LAUNCH) ||
			    nlaunches++ ||
			    p->launch.block > 256 ||
			    p->launch.block % 32 ||
			    p->launch.grid < 1 ||
			    p->launch.args_off % 4) {
				ret = -EINVAL;
				goto err_unmap;
			}
			sub->kernel = kernel;
			sub->grid = p->launch.grid;
			sub->block = p->launch.block;
			sub->args_off = p->launch.args_off;
			/*
			 * The launch's resumable state (phase F): a launch
			 * may be paused mid-grid and resumed after other
			 * contexts have run, so the submission itself owns
			 * the cursor and the saved register file.
			 */
			sub->cs = kzalloc(sizeof(*sub->cs), GFP_KERNEL);
			if (!sub->cs) {
				ret = -ENOMEM;
				goto err_unmap;
			}
			break;
		}
		/*
		 * Phase G: the memory-semantics vocabulary. Validated
		 * here, resolved (handle -> pinned object) below — the
		 * engine must never touch a handle.
		 */
		case VGPU_PKT_SIGNAL_TIMELINE:
		case VGPU_PKT_WAIT_TIMELINE:
		case VGPU_PKT_WRITE_EVENT:
			if (!vgpu_engine_allows(q->engine,
						vgpu_pkt_opcode(p->type))) {
				ret = -EINVAL;
				goto err_unmap;
			}
			break;
		case VGPU_PKT_VM_COPY: {
			u32 npages;

			if (!vgpu_engine_allows(q->engine, VGPU_OP_VM_COPY) ||
			    ncopies++) {
				ret = -EINVAL;
				goto err_unmap;
			}
			if (p->vm_copy.src_va % PAGE_SIZE ||
			    p->vm_copy.dst_va % PAGE_SIZE ||
			    p->vm_copy.len == 0 ||
			    p->vm_copy.len % PAGE_SIZE ||
			    p->vm_copy.len > VGPU_VM_MAX_COPY_PAGES * PAGE_SIZE) {
				ret = -EINVAL;
				goto err_unmap;
			}
			npages = p->vm_copy.len / PAGE_SIZE;
			sub->vm_npages = npages;
			sub->vm_src_pages = kcalloc(npages,
						    sizeof(struct page *),
						    GFP_KERNEL);
			sub->vm_dst_pages = kcalloc(npages,
						    sizeof(struct page *),
						    GFP_KERNEL);
			if (!sub->vm_src_pages || !sub->vm_dst_pages) {
				ret = -ENOMEM;
				goto err_unmap;
			}
			break;
		}
		default:
			ret = -EINVAL;
			goto err_unmap;
		}
	}

	for (i = 0; i < sub->npkts; i++) {
		struct vgpu_packet *p = &pk[i];

		if (p->type == VGPU_PKT_LAUNCH) {
			/*
			 * The argument buffer: the kernel's global memory.
			 * It is a pool buffer named by a GEM handle, pinned
			 * for the submission's lifetime (see the BLIT note
			 * below); the phase-C way to reach one by GPU VA is
			 * VM_BIND + VGPU_PKT_VM_COPY, which walks the
			 * submitter's page tables exactly as this resolve
			 * does.
			 */
			sub->args_obj = drm_gem_object_lookup(file,
							      p->launch.args_bo);
			if (!sub->args_obj) {
				ret = -ENOENT;
				goto err_unmap;
			}
			/*
			 * Pin for the submission's lifetime: the engine
			 * resolves each memory access through the BO's
			 * pages, and a driver must guarantee they are
			 * still there (this is what "validate and pin at
			 * submit" means everywhere in the DRM world).
			 */
			ret = drm_gem_shmem_pin(to_drm_gem_shmem_obj(sub->args_obj));
			if (ret) {
				drm_gem_object_put(sub->args_obj);
				sub->args_obj = NULL;
				goto err_unmap;
			}
			continue;
		}
		if (p->type == VGPU_PKT_VM_COPY) {
			ret = vgpu_resolve_vm_copy(vf, p, sub);
			if (ret)
				goto err_unmap;
			continue;
		}
		if (p->type != VGPU_PKT_COPY)
			continue;
		ret = vgpu_blit_pin(file, &sub->blit, p->copy.src_bo,
				    p->copy.dst_bo, p->copy.off, p->copy.len);
		if (ret)
			goto err_unmap;
	}

	/*
	 * Phase G: the semaphore page. Handles -> one pinned object per
	 * submission (all sync packets must name the page this fd
	 * bound, so there can only be one). Resolved here because the
	 * engine has no fd to look handles up with.
	 */
	for (i = 0; i < sub->npkts; i++) {
		struct vgpu_packet *p = &pk[i];

		switch (p->type) {
		case VGPU_PKT_SIGNAL_TIMELINE:
		case VGPU_PKT_WAIT_TIMELINE:
		case VGPU_PKT_WRITE_EVENT:
			ret = vgpu_timeline_resolve(file, sub,
						    p->timeline.sem_bo,
						    p->timeline.sem_off);
			if (ret)
				goto err_unmap;
			break;
		default:
			break;
		}
	}

	kunmap_local(pk);

	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (!fence) {
		ret = -ENOMEM;
		goto err_free_sub;
	}

	spin_lock_irqsave(&v->lock, flags);
	/*
	 * Scratch stride is VGPU_CMD_SIZE (64B) per command — NOT
	 * sizeof(struct vgpu_cmd) (48B) and not sizeof(struct
	 * vgpu_packet) (40B). The finish path decrements with
	 * ncmds * VGPU_CMD_SIZE, so the increment must use the very
	 * same stride; mixing the two sizes underflows the watermark
	 * (it did: -48 bytes per doorbell batch).
	 */
	bytes = sub->npkts * VGPU_CMD_SIZE;
	if (v->scratch_used[q->engine] + bytes > VGPU_SCRATCH_SIZE) {
		spin_unlock_irqrestore(&v->lock, flags);
		ret = -EBUSY;
		goto err_free_fence;
	}
	sub->seqno = v->next_seqno++;
	sub->cmds_off = v->scratch_used[q->engine];
	v->scratch_used[q->engine] += bytes;
	q->ring_used += sub->ring_bytes;
	q->put = arg->put;	/* the writer's position is now committed */
	spin_unlock_irqrestore(&v->lock, flags);

	fence->seqno = sub->seqno;
	fence->created_jiffies = jiffies;
	spin_lock_init(&fence->lock);
	dma_fence_init(&fence->base, &vgpu_fence_ops, &fence->lock,
		       q->fence_context, sub->seqno);
	dma_fence_get(&fence->base);

	mutex_lock(&v->fence_mutex);
	ret = idr_alloc(&v->fence_idr, fence, sub->seqno, sub->seqno + 1,
			GFP_KERNEL);
	mutex_unlock(&v->fence_mutex);
	if (ret < 0) {
		dma_fence_put(&fence->base);
		kfree(fence);
		spin_lock_irqsave(&v->lock, flags);
		v->scratch_used[q->engine] -= bytes;
		q->ring_used -= sub->ring_bytes;
		spin_unlock_irqrestore(&v->lock, flags);
		kfree(sub->cs);
		kfree(sub);
		return ret;
	}

	spin_lock_irqsave(&v->lock, flags);
	v->stats_submitted++;
	q->stats_submitted++;
	list_add_tail(&sub->node, &v->engine_pending[q->engine]);
	spin_unlock_irqrestore(&v->lock, flags);

	vgpu_pm_submit_begin(v);

	queue_work(v->engine_wq[q->engine], &v->engine_work[q->engine]);
	arg->seqno = sub->seqno;
	return 0;

err_free_fence:
	kfree(fence);
err_free_sub:
	kfree(sub->cs);
	kfree(sub);
	return ret;
err_unmap:
	kunmap_local(pk);
	/*
	 * Nothing is published yet, so release what this pass pinned
	 * (blit, semaphore page, argument buffer, VM copy pages) here.
	 */
	vgpu_blit_unref(v, &sub->blit);
	vgpu_sync_release(v, sub);
	vgpu_vm_release_sub(sub);
	if (sub->args_obj) {
		drm_gem_shmem_unpin(to_drm_gem_shmem_obj(sub->args_obj));
		drm_gem_object_put(sub->args_obj);
		sub->args_obj = NULL;
	}
	kfree(sub->cs);		/* NULL-safe: no LAUNCH in a rejected batch */
	kfree(sub);
	return ret;
}

/*
 * VGPU_VM_ALLOC: reserve GPU virtual addresses in this fd's space.
 *
 * The VMID is what makes this meaningful: two fds asking for the same
 * size at the same alignment get the same VA and mean different memory,
 * because the tag a memory access carries selects the tables.
 */
static int vgpu_vm_alloc_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file)
{
	struct vgpu_vm_alloc *arg = data;
	struct vgpu_file *vf = file->driver_priv;
	struct vgpu *v = to_vgpu(dev);
	int ret;

	/*
	 * align is a reservation alignment, so the same rules a CPU mmap
	 * has: 0 means "anywhere", otherwise a power of two >= PAGE_SIZE.
	 * Rejected here (and not silently fixed up) so a caller that
	 * asks for a bogus alignment hears about it.
	 */
	if (!vf || arg->size < PAGE_SIZE ||
	    arg->align & (arg->align - 1))
		return -EINVAL;

	ret = vgpu_vm_alloc(vf, arg->size, arg->align, &arg->va);
	if (ret)
		return ret;

	arg->vmid = vgpu_vm_vmid(vf);
	dev_info(&v->pdev->dev,
		 "[vgpu:vm] vmid %u: reserved va 0x%llx (%llu B)\n",
		 arg->vmid, (unsigned long long)arg->va,
		 (unsigned long long)arg->size);
	return 0;
}

/*
 * VGPU_VM_BIND: attach a buffer to a reserved VA range. This is the
 * moment the buffer's pages become reachable to the device — the PTE
 * install happens here, and if the device's VRAM budget cannot take
 * it, someone else's mapping is evicted to make room.
 */
static int vgpu_vm_bind_ioctl(struct drm_device *dev, void *data,
			      struct drm_file *file)
{
	struct vgpu_vm_bind *arg = data;
	struct vgpu_file *vf = file->driver_priv;

	if (!vf)
		return -EINVAL;
	return vgpu_vm_bind(vf, arg->va, arg->bo_handle, arg->flags);
}

/*
 * VGPU_VM_UNBIND: detach the buffer at va. The VA range itself stays
 * reserved, so a later bind can reuse it — the same "unmap but keep
 * the reservation" split that putting a page table entry into an
 * invalid state (rather than removing the VMA) gives you on a CPU.
 */
static int vgpu_vm_unbind_ioctl(struct drm_device *dev, void *data,
				struct drm_file *file)
{
	struct vgpu_vm_unbind *arg = data;
	struct vgpu_file *vf = file->driver_priv;

	if (!vf)
		return -EINVAL;
	vgpu_vm_unbind(vf, arg->va);
	return 0;
}

/*
 * VGPU_CS: the doorbell.
 *
 *  1. validate,
 *  2. under spinlock: claim ring space, assign the seqno, fetch the
 *     commands from the ring into the coherent scratch area,
 *  3. resolve BLIT buffers (needs the drm_file),
 *  4. create the per-submission fence and publish it in the idr,
 *  5. queue the engine.
 */
static int vgpu_cs_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file)
{
	struct vgpu *v = to_vgpu(dev);
	struct vgpu_cs *arg = data;
	struct vgpu_file *vf = file->driver_priv;
	struct vgpu_submission *sub;
	struct vgpu_queue *q;
	struct vgpu_cmd *cmds;
	struct vgpu_fence *fence;
	struct page *ring_page;
	void *ring_kmap;
	u32 bytes;
	u32 seqno;
	unsigned long flags;
	int ret;

	if (!vf)
		return -EINVAL;
	if (arg->ncmds == 0 || arg->ncmds > VGPU_RING_SLOTS)
		return -EINVAL;
	if (arg->ring_off % VGPU_CMD_SIZE)
		return -EINVAL;

	spin_lock_irqsave(&v->lock, flags);
	q = arg->qid ? vgpu_queue_find(vf, arg->qid) : vf->default_q;
	spin_unlock_irqrestore(&v->lock, flags);
	if (!q) {
		dev_err(dev->dev, "[vgpu:cs] qid %u not found in this fd\n",
			arg->qid);
		return -ENOENT;
	}

	bytes = arg->ncmds * VGPU_CMD_SIZE;
	if (arg->ring_off + bytes > q->ring_size)
		return -EINVAL;

	sub = kzalloc(sizeof(*sub), GFP_KERNEL);
	if (!sub)
		return -ENOMEM;
	sub->v = v;
	sub->q = q;
	sub->engine = q->engine;
	sub->ring_bytes = bytes;

	/* the queue's own ring is one pinned shmem page set; kmap it */
	ring_page = q->ring_bo->pages[0];
	ring_kmap = kmap_local_page(ring_page);

	spin_lock_irqsave(&v->lock, flags);
	if (q->ring_used + bytes > q->ring_size ||
	    v->scratch_used[q->engine] + bytes > VGPU_SCRATCH_SIZE) {
		spin_unlock_irqrestore(&v->lock, flags);
		kunmap_local(ring_kmap);
		kfree(sub);
		return -EBUSY;		/* this queue's ring is full */
	}
	sub->seqno = v->next_seqno++;
	sub->ncmds = arg->ncmds;
	sub->ring_off = arg->ring_off;
	sub->cmds_off = v->scratch_used[q->engine];
	sub->submit_jiffies = jiffies;
	q->ring_used += bytes;
	v->scratch_used[q->engine] += bytes;
	/* the "GPU fetch": copy commands ring -> this engine's scratch */
	memcpy(v->scratch[q->engine] + sub->cmds_off,
	       ring_kmap + arg->ring_off, bytes);
	/*
	 * Not on any engine list yet: the fence must be published (idr)
	 * first, otherwise the engine could execute and free sub while
	 * it is still being set up.
	 */
	spin_unlock_irqrestore(&v->lock, flags);
	kunmap_local(ring_kmap);

	cmds = (struct vgpu_cmd *)(v->scratch[q->engine] + sub->cmds_off);

	/*
	 * Per-engine command classes: a DMA engine rejects shader-ish
	 * work at the doorbell, like a real device would reject a packet
	 * the target engine cannot decode (the alternative — feeding it
	 * through and failing later — is how real drivers end up with
	 * hung rings).
	 *
	 * The phase G sync opcodes are packet-only: the legacy 64B
	 * command has no field that names the semaphore page, so VGPU_CS
	 * refuses them here rather than handing the engine a NULL page.
	 */
	for (u32 i = 0; i < sub->ncmds; i++) {
		if (cmds[i].opcode >= VGPU_OP_SIGNAL_TIMELINE) {
			ret = -EINVAL;
			goto err_rollback;
		}
		if (!vgpu_engine_allows(q->engine, cmds[i].opcode)) {
			ret = -EINVAL;
			goto err_rollback;
		}
	}

	for (u32 i = 0; i < sub->ncmds; i++) {
		struct vgpu_cmd *c = &cmds[i];

		if (c->opcode != VGPU_OP_BLIT)
			continue;
		if (c->src_off != c->dst_off) {
			ret = -EINVAL;
			goto err_rollback;
		}
		ret = vgpu_blit_pin(file, &sub->blit, c->src_bo, c->dst_bo,
				    c->src_off, c->length);
		if (ret)
			goto err_rollback;
	}

	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (!fence) {
		spin_lock_irqsave(&v->lock, flags);
		q->ring_used -= bytes;
		v->scratch_used[q->engine] -= bytes;
		spin_unlock_irqrestore(&v->lock, flags);
		kfree(sub);
		return -ENOMEM;
	}
	fence->seqno = sub->seqno;
	fence->created_jiffies = jiffies;
	spin_lock_init(&fence->lock);
	dma_fence_init(&fence->base, &vgpu_fence_ops, &fence->lock,
		       q->fence_context, sub->seqno);
	dma_fence_get(&fence->base);

	mutex_lock(&v->fence_mutex);
	ret = idr_alloc(&v->fence_idr, fence, sub->seqno, sub->seqno + 1,
			GFP_KERNEL);
	mutex_unlock(&v->fence_mutex);
	if (ret < 0) {
		dma_fence_put(&fence->base);
		kfree(fence);
		spin_lock_irqsave(&v->lock, flags);
		q->ring_used -= bytes;
		v->scratch_used[q->engine] -= bytes;
		spin_unlock_irqrestore(&v->lock, flags);
		kfree(sub);
		return ret;
	}

	/* sub is fully set up and its fence is published: hand it over */
	seqno = sub->seqno;
	spin_lock_irqsave(&v->lock, flags);
	v->stats_submitted++;
	q->stats_submitted++;
	list_add_tail(&sub->node, &v->engine_pending[q->engine]);
	spin_unlock_irqrestore(&v->lock, flags);

	dev_info(dev->dev, "[vgpu:cs] q#%u seq#%u ring_off=0x%llx ncmds=%u\n",
		 q->qid, seqno, (unsigned long long)arg->ring_off,
		 arg->ncmds);

	vgpu_pm_submit_begin(v);

	queue_work(v->engine_wq[q->engine], &v->engine_work[q->engine]);
	arg->seqno = seqno;
	return 0;

err_rollback:
	spin_lock_irqsave(&v->lock, flags);
	q->ring_used -= bytes;
	v->scratch_used[q->engine] -= bytes;
	spin_unlock_irqrestore(&v->lock, flags);
	kfree(sub);
	return ret;
}

/*
 * VGPU_WAIT: wait for a submission, in kernel context.
 *
 * The fence stays in the idr until it is consumed by a successful
 * WAIT (or reaped by the watchdog), so a slightly late waiter still
 * finds it.
 */
static int vgpu_wait_ioctl(struct drm_device *dev, void *data,
			   struct drm_file *file)
{
	struct vgpu *v = to_vgpu(dev);
	struct vgpu_wait *arg = data;
	struct dma_fence *fence;
	long timeout;
	bool consumed = false;

	mutex_lock(&v->fence_mutex);
	fence = idr_find(&v->fence_idr, arg->seqno);
	if (fence)
		dma_fence_get(fence);
	mutex_unlock(&v->fence_mutex);

	if (!fence) {
		arg->status = -ENOENT;
		return 0;
	}

	timeout = arg->timeout_ms ? msecs_to_jiffies(arg->timeout_ms)
				  : MAX_SCHEDULE_TIMEOUT;
	if (!dma_fence_wait_timeout(fence, true, timeout)) {
		arg->status = -ETIMEDOUT;
	} else {
		/*
		 * A dma_fence error is 0 or a negative errno. Surface it
		 * as-is: clamping with max_t(error, 0) here would swallow
		 * a failed submission and report success (DESIGN §5:
		 * status = 0 done, negative = the engine's error).
		 */
		arg->status = fence->error < 0 ? (int)fence->error : 0;
		consumed = true;
	}

	if (consumed) {
		mutex_lock(&v->fence_mutex);
		if (idr_find(&v->fence_idr, arg->seqno) == fence)
			idr_remove(&v->fence_idr, arg->seqno);
		mutex_unlock(&v->fence_mutex);
	}
	dma_fence_put(fence);

	dev_info(dev->dev, "[vgpu:wait] seq#%u status=%d\n",
		 arg->seqno, (int)arg->status);
	return 0;
}

/* VGPU_QUERY: statistics + live MMIO register values. */
static int vgpu_query_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file)
{
	struct vgpu *v = to_vgpu(dev);
	struct vgpu_query *arg = data;
	struct vgpu_queue *qtmp;
	unsigned long flags;

	spin_lock_irqsave(&v->lock, flags);
	arg->submitted = v->stats_submitted;
	arg->completed = v->stats_completed;
	arg->errors = v->stats_errors;
	arg->ring_used = 0;
	list_for_each_entry(qtmp, &v->queue_list, dev_node)
		arg->ring_used += qtmp->ring_used;
	arg->r_status = v->regs->status;
	arg->r_seqno = v->regs->seqno;
	arg->r_error = v->regs->error;
	arg->r_user = v->regs->user;
	arg->graphs = v->stats_graphs;
	arg->signals = v->stats_signals;
	arg->waits = v->stats_waits;
	arg->events = v->stats_events;
	spin_unlock_irqrestore(&v->lock, flags);
	return 0;
}

/*
 * Take the device out of a power-gate for one submission. The get is
 * what resumes it; if it was asleep we measure and log the wake cost —
 * the only number a real power-management discussion can be had with.
 * Balanced by vgpu_pm_submit_end() once the submission completes.
 */
static void vgpu_pm_submit_begin(struct vgpu *v)
{
	if (pm_runtime_suspended(&v->pdev->dev)) {
		ktime_t t0 = ktime_get_ns();

		pm_runtime_get_sync(&v->pdev->dev);
		v->rpm_last_resume_ns = ktime_get_ns() - t0;
		v->rpm_suspends++;
		dev_info(&v->pdev->dev,
			 "[vgpu:rpm] resumed for seq#%u in %llu us\n",
			 v->next_seqno, v->rpm_last_resume_ns / 1000);
	} else {
		pm_runtime_get(&v->pdev->dev);
	}
}

/* -------------------------------------------------------- module glue */

/*
 * The device itself: no resources (x86, no device tree — the platform
 * bus is the x86 analogue of DT matching, docs/adr/0002). Static: the
 * struct lives in module data for the module's lifetime; the no-op
 * release keeps device_release() from WARNing (and kfree'ing a
 * non-slab pointer) when vgpu_exit drops the last reference.
 */
static void vgpu_device_release(struct device *dev)
{
	/* nothing to free: static device */
}

static struct platform_device vgpu_device = {
	.name = "vgpu",
	.dev = {
		.release = vgpu_device_release,
	},
};

static int __init vgpu_init(void)
{
	int ret;

	ret = platform_driver_register(&vgpu_platform_driver);
	if (ret)
		return ret;

	/* register the device after the driver: probe() runs here */
	ret = platform_device_register(&vgpu_device);
	if (ret)
		platform_driver_unregister(&vgpu_platform_driver);
	return ret;
}

static void __exit vgpu_exit(void)
{
	platform_device_unregister(&vgpu_device);
	platform_driver_unregister(&vgpu_platform_driver);
}

module_init(vgpu_init);
module_exit(vgpu_exit);

MODULE_AUTHOR("Xuefei Zhang");
MODULE_DESCRIPTION("Virtual GPU command submission driver (learning)");
MODULE_LICENSE("GPL");
