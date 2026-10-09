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
};

DEFINE_DRM_GEM_FOPS(vgpu_drm_fops);

static const struct drm_driver vgpu_drm_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_RENDER,
	.ioctls			= vgpu_ioctls,
	.num_ioctls		= ARRAY_SIZE(vgpu_ioctls),
	.fops			= &vgpu_drm_fops,
	.dumb_create		= drm_gem_shmem_dumb_create,
	.gem_prime_import	= drm_gem_shmem_prime_import_no_map,
	.name			= "vgpu",
	.desc			= "Virtual GPU command submission (learning driver)",
	.major			= 1,
	.minor			= 0,
};

/* ------------------------------------------------------------- debugfs */

static int vgpu_dbg_stats_show(struct seq_file *s, void *unused)
{
	struct vgpu *v = s->private;
	unsigned long flags;

	spin_lock_irqsave(&v->lock, flags);
	seq_printf(s, "submitted  %u\n", v->stats_submitted);
	seq_printf(s, "completed  %u\n", v->stats_completed);
	seq_printf(s, "errors     %u\n", v->stats_errors);
	seq_printf(s, "ring_used  %u / %u bytes\n", v->ring_used, VGPU_RING_SIZE);
	spin_unlock_irqrestore(&v->lock, flags);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(vgpu_dbg_stats);

static int vgpu_dbg_ring_show(struct seq_file *s, void *unused)
{
	struct vgpu *v = s->private;
	struct page *page;
	u32 *p;
	int i;

	if (!v->ring_bo || !v->ring_bo->pages || !v->ring_bo->pages[0]) {
		seq_puts(s, "ring not pinned\n");
		return 0;
	}
	page = v->ring_bo->pages[0];
	p = kmap_local_page(page);
	for (i = 0; i < VGPU_RING_SIZE; i += 16)
		seq_printf(s, "%04x: %08x %08x\n", i, p[i / 4], p[i / 4 + 1]);
	kunmap_local(p);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(vgpu_dbg_ring);

static int vgpu_dbg_pending_show(struct seq_file *s, void *unused)
{
	struct vgpu *v = s->private;
	struct vgpu_submission *sub;
	unsigned long flags;
	bool any = false;

	spin_lock_irqsave(&v->lock, flags);
	list_for_each_entry(sub, &v->pending, node) {
		seq_printf(s, "pending   seq#%u ncmds=%u\n", sub->seqno, sub->ncmds);
		any = true;
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
	INIT_LIST_HEAD(&v->pending);
	INIT_LIST_HEAD(&v->in_flight);
	mutex_init(&v->fence_mutex);
	idr_init(&v->fence_idr);
	v->timeline = dma_fence_context_alloc(4);
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

	/* command fetch area: coherent (CPU fetches, engine consumes) */
	v->scratch = dma_alloc_coherent(&pdev->dev, VGPU_SCRATCH_SIZE,
					&v->scratch_dma, GFP_KERNEL);
	if (!v->scratch) {
		ret = -ENOMEM;
		goto err_devres;
	}

	/* the command ring: a shmem GEM BO, exactly one page, pinned */
	v->ring_bo = drm_gem_shmem_create(&v->dev, VGPU_RING_SIZE);
	if (IS_ERR(v->ring_bo)) {
		ret = PTR_ERR(v->ring_bo);
		goto err_devres;
	}
	v->ring_bo->map_wc = true;
	ret = drm_gem_shmem_pin(v->ring_bo);
	if (ret)
		goto err_devres;

	vgpu_engine_init(v);

	v->dbg = debugfs_create_dir("vgpu", NULL);
	debugfs_create_file("stats", 0444, v->dbg, v, &vgpu_dbg_stats_fops);
	debugfs_create_file("ring", 0444, v->dbg, v, &vgpu_dbg_ring_fops);
	debugfs_create_file("pending", 0444, v->dbg, v, &vgpu_dbg_pending_fops);
	debugfs_create_file("regs", 0444, v->dbg, v, &vgpu_dbg_regs_fops);

	ret = drm_dev_register(&v->dev, 0);
	if (ret)
		goto err_devres;

	/*
	 * One vma offset for the ring BO, shared by all VGPU_RING_GET
	 * callers (the offset must outlive every fd handle to the ring).
	 */
	drm_gem_create_mmap_offset(&v->ring_bo->base);

	platform_set_drvdata(pdev, v);
	dev_info(&pdev->dev,
		 "[vgpu:probe] ready: /dev/dri/card%d (renderD128), ring %u bytes\n",
		 v->dev.primary->index, VGPU_RING_SIZE);
	return 0;

err_devres:
	devres_release_group(&pdev->dev, NULL);
	return ret;
}

/* -------------------------------------------------------------- remove */

static void vgpu_drain(struct vgpu *v)
{
	/*
	 * Reset path: finish everything that was submitted.
	 * Order matters:
	 *  1. flush both workqueues (engine + bottom half) so nothing
	 *     is executing while we take the lists,
	 *  2. finish remaining submissions (signals their fences),
	 *  3. reap every fence still in the idr.
	 */
	unsigned long flags;

	flush_work(&v->engine_work);
	flush_workqueue(v->irq_wq);

	spin_lock_irqsave(&v->lock, flags);
	while (!list_empty(&v->pending) || !list_empty(&v->in_flight)) {
		struct vgpu_submission *sub;
		struct list_head *l = list_empty(&v->in_flight) ?
				      &v->pending : &v->in_flight;

		sub = list_first_entry(l, struct vgpu_submission, node);
		list_del_init(&sub->node);
		spin_unlock_irqrestore(&v->lock, flags);
		if (!sub->error)
			sub->error = -EIO;
		vgpu_finish_submission(v, sub);
		spin_lock_irqsave(&v->lock, flags);
	}
	spin_unlock_irqrestore(&v->lock, flags);

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

	dev_info(&pdev->dev, "[vgpu:remove] draining and shutting down\n");

	vgpu_drain(v);
	drm_dev_unregister(&v->dev);

	spin_lock_irqsave(&v->lock, flags);
	s = v->stats_submitted;
	c = v->stats_completed;
	e = v->stats_errors;
	spin_unlock_irqrestore(&v->lock, flags);

	debugfs_remove_recursive(v->dbg);
	drm_gem_shmem_unpin(v->ring_bo);
	drm_gem_shmem_free(v->ring_bo);
	dma_free_coherent(&pdev->dev, VGPU_SCRATCH_SIZE,
			  v->scratch, v->scratch_dma);
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
 * VGPU_RING_GET: hand the app a per-fd handle to the device command
 * ring (one shmem GEM BO, 64 x 64B slots).
 */
static int vgpu_ring_get_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file)
{
	struct vgpu *v = to_vgpu(dev);
	struct vgpu_ring_get *arg = data;
	struct drm_gem_object *obj = &v->ring_bo->base;
	int ret;

	drm_gem_object_get(obj);
	ret = drm_gem_handle_create(file, obj, &arg->handle);
	if (ret) {
		drm_gem_object_put(obj);
		return ret;
	}
	/* the vma offset was allocated once in probe */
	arg->mmap_offset = drm_vma_node_offset_addr(&obj->vma_node);
	/*
	 * The handle holds its own reference from now on; drop the
	 * temporary one taken above. probe's creation reference is
	 * separate and is released in vgpu_remove().
	 */
	drm_gem_object_put(obj);

	dev_info(dev->dev, "[vgpu:ring] fd handle=%u mmap_offset=0x%llx\n",
		 arg->handle, (unsigned long long)arg->mmap_offset);
	return 0;
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
	struct vgpu_submission *sub;
	struct vgpu_cmd *cmds;
	struct vgpu_fence *fence;
	struct page *ring_page;
	void *ring_kmap;
	u32 bytes;
	u32 seqno;
	unsigned long flags;
	int ret;

	if (arg->ncmds == 0 || arg->ncmds > VGPU_RING_SLOTS)
		return -EINVAL;
	if (arg->ring_off % VGPU_CMD_SIZE)
		return -EINVAL;
	bytes = arg->ncmds * VGPU_CMD_SIZE;
	if (arg->ring_off + bytes > VGPU_RING_SIZE)
		return -EINVAL;

	sub = kzalloc(sizeof(*sub), GFP_KERNEL);
	if (!sub)
		return -ENOMEM;
	sub->v = v;

	/* the ring is one pinned shmem page; kmap it for the fetch */
	ring_page = v->ring_bo->pages[0];
	ring_kmap = kmap_local_page(ring_page);

	spin_lock_irqsave(&v->lock, flags);
	if (v->ring_used + bytes > VGPU_RING_SIZE ||
	    v->scratch_used + bytes > VGPU_SCRATCH_SIZE) {
		spin_unlock_irqrestore(&v->lock, flags);
		kunmap_local(ring_kmap);
		kfree(sub);
		return -EBUSY;	/* ring full */
	}
	sub->seqno = v->next_seqno++;
	sub->ncmds = arg->ncmds;
	sub->cmds_off = v->scratch_used;
	sub->submit_jiffies = jiffies;
	v->ring_used += bytes;
	v->scratch_used += bytes;
	/* the "GPU fetch": copy commands ring -> scratch */
	memcpy(v->scratch + sub->cmds_off,
	       ring_kmap + arg->ring_off, bytes);
	/*
	 * Not on pending yet: the fence must be published (idr) first,
	 * otherwise the engine could execute and free sub before it is
	 * fully set up.
	 */
	spin_unlock_irqrestore(&v->lock, flags);
	kunmap_local(ring_kmap);

	cmds = (struct vgpu_cmd *)(v->scratch + sub->cmds_off);
	ret = vgpu_resolve_blit(file, cmds, sub->ncmds, sub);
	if (ret) {
		spin_lock_irqsave(&v->lock, flags);
		v->ring_used -= bytes;
		v->scratch_used -= bytes;
		spin_unlock_irqrestore(&v->lock, flags);
		kfree(sub);
		return ret;
	}

	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (!fence) {
		spin_lock_irqsave(&v->lock, flags);
		v->ring_used -= bytes;
		v->scratch_used -= bytes;
		spin_unlock_irqrestore(&v->lock, flags);
		kfree(sub);
		return -ENOMEM;
	}
	fence->seqno = sub->seqno;
	fence->created_jiffies = jiffies;
	spin_lock_init(&fence->lock);
	dma_fence_init(&fence->base, &vgpu_fence_ops, &fence->lock,
		       v->timeline, sub->seqno);
	dma_fence_get(&fence->base);

	mutex_lock(&v->fence_mutex);
	ret = idr_alloc(&v->fence_idr, fence, sub->seqno, sub->seqno + 1,
			GFP_KERNEL);
	mutex_unlock(&v->fence_mutex);
	if (ret < 0) {
		dma_fence_put(&fence->base);
		kfree(fence);
		spin_lock_irqsave(&v->lock, flags);
		v->ring_used -= bytes;
		v->scratch_used -= bytes;
		spin_unlock_irqrestore(&v->lock, flags);
		kfree(sub);
		return ret;
	}

	/* sub is fully set up and its fence is published: hand it over */
	seqno = sub->seqno;
	spin_lock_irqsave(&v->lock, flags);
	v->stats_submitted++;
	list_add_tail(&sub->node, &v->pending);
	spin_unlock_irqrestore(&v->lock, flags);

	dev_info(dev->dev, "[vgpu:cs] seq#%u ring_off=0x%llx ncmds=%u\n",
		 seqno, (unsigned long long)arg->ring_off, arg->ncmds);

	queue_work(v->engine_wq, &v->engine_work);
	arg->seqno = seqno;
	return 0;
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
		arg->status = (int)max_t(s64, fence->error, 0);
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
	unsigned long flags;

	spin_lock_irqsave(&v->lock, flags);
	arg->submitted = v->stats_submitted;
	arg->completed = v->stats_completed;
	arg->errors = v->stats_errors;
	arg->ring_used = v->ring_used;
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
