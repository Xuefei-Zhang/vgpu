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
static int vgpu_cs_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file);
static int vgpu_wait_ioctl(struct drm_device *dev, void *data,
			   struct drm_file *file);
static int vgpu_query_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file);

static const struct drm_ioctl_desc vgpu_ioctls[] = {
	DRM_IOCTL_DEF_DRV(VGPU_BO_CREATE, vgpu_bo_create_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VGPU_RING_GET, vgpu_ring_get_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VGPU_CS, vgpu_cs_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VGPU_WAIT, vgpu_wait_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VGPU_QUERY, vgpu_query_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VGPU_QUEUE_CREATE, vgpu_queue_create_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VGPU_QUEUE_DESTROY, vgpu_queue_destroy_ioctl, DRM_RENDER_ALLOW),
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
			   "q#%u engine=%-8s prio=%u ring %u/%u B  sub=%llu done=%llu err=%llu%s\n",
			   q->qid, vgpu_engine_names[q->engine], q->prio,
			   q->ring_used, q->ring_size,
			   q->stats_submitted, q->stats_completed,
			   q->stats_errors,
			   q->destroyed ? " (draining)" : "");
	}
	spin_unlock_irqrestore(&v->lock, flags);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(vgpu_dbg_queues);

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
			any = true;
		}
	}
	list_for_each_entry(sub, &v->in_flight, node) {
		seq_printf(s, "in-flight seq#%u ncmds=%u err=%d\n",
			   sub->seqno, sub->ncmds, (int)sub->error);
		any = true;
	}
	spin_unlock_irqrestore(&v->lock, flags);
	if (!any)
		seq_puts(s, "(none)\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(vgpu_dbg_pending);

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
	spin_lock_init(&v->lock);
	INIT_LIST_HEAD(&v->in_flight);
	INIT_LIST_HEAD(&v->queue_list);
	mutex_init(&v->fence_mutex);
	idr_init(&v->fence_idr);
	v->next_seqno = 1;	/* dma_fence seqno 0 is reserved */

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

	/* per-engine workqueues + the bottom-half queue + watchdog */
	vgpu_engine_init(v);

	v->dbg = debugfs_create_dir("vgpu", NULL);
	debugfs_create_file("stats", 0444, v->dbg, v, &vgpu_dbg_stats_fops);
	debugfs_create_file("queues", 0444, v->dbg, v, &vgpu_dbg_queues_fops);
	debugfs_create_file("pending", 0444, v->dbg, v, &vgpu_dbg_pending_fops);
	debugfs_create_file("regs", 0444, v->dbg, v, &vgpu_dbg_regs_fops);

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
 * Which commands does an engine accept? A real device's engines accept
 * disjoint command classes (a DMA engine cannot execute a shader; a
 * compute engine is not where you put a blit). Rejecting at the
 * doorbell keeps the error visible to the submitter instead of feeding
 * garbage to the engine.
 */
static bool vgpu_engine_allows(u32 engine, u32 opcode)
{
	switch (engine) {
	case VGPU_ENGINE_GFX:
		return opcode == VGPU_OP_NOP || opcode == VGPU_OP_BLIT ||
		       opcode == VGPU_OP_SET;
	case VGPU_ENGINE_COPY:
		return opcode == VGPU_OP_NOP || opcode == VGPU_OP_BLIT;
	case VGPU_ENGINE_COMPUTE:
		/* LAUNCH lands in phase D; for now compute runs control ops */
		return opcode == VGPU_OP_NOP || opcode == VGPU_OP_SET;
	}
	return false;
}

static struct vgpu_queue *vgpu_queue_find(struct vgpu_file *vf, u32 qid)
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

static void vgpu_file_destroy_all_queues(struct vgpu_file *vf)
{
	struct vgpu *v = vf->vf_dev;
	struct list_head to_free = LIST_HEAD_INIT(to_free);
	struct vgpu_queue *q, *tmp;
	unsigned long flags;

	/* wait for anything in flight on these queues to retire */
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
	vf->default_q = NULL;
	vf->next_qid = 1;
	vf->vf_dev = v;
	file->driver_priv = vf;
	return 0;
}

static void vgpu_postclose(struct drm_device *dev, struct drm_file *file)
{
	struct vgpu_file *vf = file->driver_priv;

	if (!vf)
		return;

	vgpu_file_destroy_all_queues(vf);
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

	vgpu_drain(v);
	drm_dev_unregister(&v->dev);

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
	if (q->ring_used) {
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
 * Resolve the BLIT's buffer handles at submit time.
 *
 * Real submission drivers do the same: the ioctl context has the
 * drm_file, so it resolves handles -> objects, takes references and
 * pins the pages; the engine (workqueue context, no file) can then
 * execute without any lookup. Constraint: one BLIT per submission.
 */
static int vgpu_resolve_blit(struct drm_file *file, struct vgpu_cmd *cmds,
			     u32 ncmds, struct vgpu_submission *sub)
{
	u32 i;
	int j;

	for (i = 0; i < ncmds; i++) {
		struct vgpu_cmd *c = &cmds[i];
		struct drm_gem_shmem_object *src, *dst;
		u32 npages, off;
		int ret;

		if (c->opcode != VGPU_OP_BLIT)
			continue;
		if (sub->src_obj)
			return -EINVAL;	/* one BLIT per submission */

		if (c->src_off != c->dst_off ||
		    (c->src_off % PAGE_SIZE) ||
		    (c->length % PAGE_SIZE) ||
		    c->length == 0 || c->length > 16 * PAGE_SIZE)
			return -EINVAL;

		sub->src_obj = drm_gem_object_lookup(file, c->src_bo);
		if (!sub->src_obj)
			return -ENOENT;
		sub->dst_obj = drm_gem_object_lookup(file, c->dst_bo);
		if (!sub->dst_obj) {
			drm_gem_object_put(sub->src_obj);
			sub->src_obj = NULL;
			return -ENOENT;
		}
		if (c->src_off + c->length > sub->src_obj->size ||
		    c->dst_off + c->length > sub->dst_obj->size) {
			ret = -ERANGE;
			goto err_put;
		}

		src = to_drm_gem_shmem_obj(sub->src_obj);
		dst = to_drm_gem_shmem_obj(sub->dst_obj);
		ret = drm_gem_shmem_pin(src);
		if (ret)
			goto err_put;
		ret = drm_gem_shmem_pin(dst);
		if (ret) {
			drm_gem_shmem_unpin(src);
			goto err_put;
		}

		off = c->src_off >> PAGE_SHIFT;
		npages = c->length >> PAGE_SHIFT;
		sub->nblit_pages = npages;
		sub->blit_off = off;
		sub->src_pages = kcalloc(npages, sizeof(*sub->src_pages),
					 GFP_KERNEL);
		sub->dst_pages = kcalloc(npages, sizeof(*sub->dst_pages),
					 GFP_KERNEL);
		if (!sub->src_pages || !sub->dst_pages) {
			ret = -ENOMEM;
			goto err_unpin;
		}
		for (j = 0; j < npages; j++) {
			sub->src_pages[j] = src->pages[off + j];
			sub->dst_pages[j] = dst->pages[off + j];
			get_page(sub->src_pages[j]);
			get_page(sub->dst_pages[j]);
		}
		return 0;

err_unpin:
		kfree(sub->src_pages);
		kfree(sub->dst_pages);
		sub->src_pages = sub->dst_pages = NULL;
		drm_gem_shmem_unpin(dst);
		drm_gem_shmem_unpin(src);
err_put:
		drm_gem_object_put(sub->dst_obj);
		drm_gem_object_put(sub->src_obj);
		sub->src_obj = sub->dst_obj = NULL;
		return ret;
	}
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
	 */
	for (u32 i = 0; i < sub->ncmds; i++) {
		if (!vgpu_engine_allows(q->engine, cmds[i].opcode)) {
			ret = -EINVAL;
			goto err_rollback;
		}
	}

	ret = vgpu_resolve_blit(file, cmds, sub->ncmds, sub);
	if (ret)
		goto err_rollback;

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
	spin_unlock_irqrestore(&v->lock, flags);
	return 0;
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
