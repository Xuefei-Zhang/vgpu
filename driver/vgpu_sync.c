// SPDX-License-Identifier: GPL-2.0
/*
 * vgpu - phase G: memory-semantics synchronization.
 *
 * The rest of the driver answers "is it done?" with a syscall:
 * userspace calls VGPU_WAIT and the kernel sits in
 * dma_fence_wait_timeout() on the CPU's behalf. Every modern GPU API
 * does the opposite — the completion lives in MEMORY that both sides
 * can see (Vulkan timeline semaphores, CUDA graphs, the "sync object"
 * pages of every real driver's submission ring), and the CPU simply
 * polls it:
 *
 *   userspace                         engine
 *   ---------                         ------
 *   bind a semaphore page             writes timeline value
 *   write packets                     stalls on WAIT_TIMELINE
 *   ONE doorbell                      writes event slots
 *   poll mmap'ed memory  <----------  done (also signals the fence,
 *                                     so VGPU_WAIT still works)
 *
 * This file holds:
 *   VGPU_SEM_BIND     - install the fd's semaphore page
 *   the sync packets  - SIGNAL/WAIT_TIMELINE, WRITE_EVENT
 *   VGPU_GRAPH       - submit a DAG with one doorbell
 *
 * All handle resolution happens in the ioctls (they have the fd); the
 * engine only ever touches pinned objects.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "vgpu.h"

/* ------------------------------------------------------- BO word access */

/*
 * One 32-bit word of a pinned shmem BO, by word index. Shared with
 * vgpu_compute.c (which addresses the same kind of buffer through the
 * kernel's argument BO) — the page walk is the GPU's memory walk.
 */
u32 vgpu_bo_word(struct drm_gem_object *obj, u32 word, bool write, u32 val)
{
	struct drm_gem_shmem_object *sh = to_drm_gem_shmem_obj(obj);
	struct page *page;
	u32 *p;

	if (word >= obj->size / sizeof(u32))
		return 0;
	page = sh->pages[word / VGPU_WORDS_PER_PAGE];
	if (!page)
		return 0;
	p = kmap_local_page(page);
	if (write)
		p[word % VGPU_WORDS_PER_PAGE] = val;
	else
		val = p[word % VGPU_WORDS_PER_PAGE];
	kunmap_local(p);
	return val;
}

/* ------------------------------------------------------------ SEM_BIND */

int vgpu_sem_bind_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file)
{
	struct vgpu_sem_bind *arg = data;
	struct vgpu_file *vf = file->driver_priv;
	struct drm_gem_object *obj;

	if (!vf)
		return -EINVAL;
	if (arg->sem_off % 4)
		return -EINVAL;

	/*
	 * Validate, do not hold: the submission resolves (and pins) the
	 * page exactly like it resolves a BLIT buffer, so the object's
	 * lifetime stays tied to the submission, not to the fd. Handles
	 * are per-fd, so an fd can only ever have one page in play.
	 */
	obj = drm_gem_object_lookup(file, arg->bo_handle);
	if (!obj)
		return -ENOENT;
	if (arg->sem_off + sizeof(u32) > obj->size) {
		drm_gem_object_put(obj);
		return -ERANGE;
	}
	drm_gem_object_put(obj);

	vf->sem_handle = arg->bo_handle;
	vf->sem_off = arg->sem_off;
	arg->bound = 1;

	dev_info(dev->dev, "[vgpu:sem] handle=%u word_off=%u bound\n",
		 arg->bo_handle, arg->sem_off);
	return 0;
}

/*
 * Resolve the semaphore page for one submission. Called by the
 * doorbell (or the graph ioctl) the first time a sync packet shows
 * up; later sync packets must name the same page, which is why there
 * is only one possible handle per fd and only one pin per submission.
 */
int vgpu_timeline_resolve(struct drm_file *file, struct vgpu_submission *sub,
			  u32 sem_bo, u32 sem_off)
{
	struct vgpu_file *vf = file->driver_priv;
	struct drm_gem_object *obj;
	int ret;

	if (!vf->sem_handle) {
		pr_err("vgpu: [vgpu:sync] no semaphore page bound\n");
		return -EINVAL;
	}
	if (sem_bo != vf->sem_handle) {
		pr_err("vgpu: [vgpu:sync] packet names an unbound BO %u (bound %u)\n",
		       sem_bo, vf->sem_handle);
		return -EINVAL;
	}
	if (sem_off % 4)
		return -EINVAL;

	if (!sub->sem_obj) {
		obj = drm_gem_object_lookup(file, sem_bo);
		if (!obj)
			return -ENOENT;
		ret = drm_gem_shmem_pin(to_drm_gem_shmem_obj(obj));
		if (ret) {
			drm_gem_object_put(obj);
			return ret;
		}
		sub->sem_obj = obj;
	}
	if (sem_off + sizeof(u32) > sub->sem_obj->size)
		return -ERANGE;
	return 0;
}

/* --------------------------------------------------------- sync packets */

/*
 * Execute one sync packet in the engine (workqueue) context.
 *
 * SIGNAL / WRITE_EVENT write a word the CPU can poll. WAIT_TIMELINE
 * stalls the engine until somebody else writes it — that is the whole
 * dependency mechanism: no ioctl, no kernel-side wait.
 *
 * The stall SLEEPS rather than spins, on purpose. A spin would burn a
 * CPU even though the value we are waiting for is typically written
 * by a DIFFERENT engine (or another context); sleeping lets the
 * bottom half and every other engine make progress, which is exactly
 * how a real GPU pipeline stalls on a semaphore.
 *
 * smp_wmb() after the write: the semaphore store must be visible to
 * the polling CPU before the submission's fence is signalled (the
 * "write the sync object, then raise the fence" ordering rule of every
 * real driver).
 */
void vgpu_sync_exec_cmd(struct vgpu *v, struct vgpu_submission *sub,
			u32 op, u32 sem_off, u64 val)
{
	u32 word = sem_off / sizeof(u32);
	unsigned long deadline;
	unsigned long flags;

	if (!sub->sem_obj) {
		if (!sub->error)
			sub->error = -EINVAL;
		return;
	}
	if (sem_off % 4) {
		if (!sub->error)
			sub->error = -EINVAL;
		return;
	}

	switch (op) {
	case VGPU_OP_SIGNAL_TIMELINE:
		vgpu_bo_word(sub->sem_obj, word, true, (u32)val);
		smp_wmb();
		dev_info(&v->pdev->dev,
			 "[vgpu:sync] seq#%u SIGNAL word=%u value=%llu\n",
			 sub->seqno, word, (unsigned long long)val);
		spin_lock_irqsave(&v->lock, flags);
		v->stats_signals++;
		spin_unlock_irqrestore(&v->lock, flags);
		break;
	case VGPU_OP_WRITE_EVENT:
		vgpu_bo_word(sub->sem_obj, word, true, (u32)val);
		smp_wmb();
		dev_info(&v->pdev->dev,
			 "[vgpu:sync] seq#%u EVENT word=%u value=%llx\n",
			 sub->seqno, word, (unsigned long long)val);
		spin_lock_irqsave(&v->lock, flags);
		v->stats_events++;
		spin_unlock_irqrestore(&v->lock, flags);
		break;
	case VGPU_OP_WAIT_TIMELINE:
		deadline = jiffies + msecs_to_jiffies(VGPU_TIMELINE_WAIT_MS);
		dev_info(&v->pdev->dev,
			 "[vgpu:sync] seq#%u WAIT word=%u value=%llu\n",
			 sub->seqno, word, (unsigned long long)val);
		spin_lock_irqsave(&v->lock, flags);
		v->stats_waits++;
		spin_unlock_irqrestore(&v->lock, flags);
		while (vgpu_bo_word(sub->sem_obj, word, false, 0) < (u32)val) {
			if (time_after(jiffies, deadline)) {
				dev_info(&v->pdev->dev,
					 "[vgpu:sync] seq#%u WAIT word=%u TIMED OUT\n",
					 sub->seqno, word);
				if (!sub->error)
					sub->error = -ETIMEDOUT;
				return;
			}
			/* the engine yields; another engine may signal us */
			usleep_range(100, 300);
		}
		dev_info(&v->pdev->dev,
			 "[vgpu:sync] seq#%u WAIT word=%u satisfied\n",
			 sub->seqno, word);
		break;
	default:
		if (!sub->error)
			sub->error = -EINVAL;
	}
}

/*
 * Release what a submission's sync packets pinned (the semaphore
 * page) and free a graph's node array. Called from the finish path
 * AND from the submit error paths, where nothing is published yet —
 * hence it must not touch the lists or the stats.
 */
void vgpu_sync_release(struct vgpu *v, struct vgpu_submission *sub)
{
	u32 i;

	if (sub->sem_obj) {
		drm_gem_shmem_unpin(to_drm_gem_shmem_obj(sub->sem_obj));
		drm_gem_object_put(sub->sem_obj);
		sub->sem_obj = NULL;
	}

	if (!sub->gnodes)
		return;

	for (i = 0; i < sub->ngnodes; i++) {
		struct vgpu_gnode *gn = &sub->gnodes[i];

		if (gn->args_obj) {
			drm_gem_shmem_unpin(to_drm_gem_shmem_obj(gn->args_obj));
			drm_gem_object_put(gn->args_obj);
			gn->args_obj = NULL;
		}
		vgpu_blit_unref(v, &gn->blit);
	}
	kfree(sub->gnodes);
	kfree(sub->order);
	sub->gnodes = NULL;
	sub->order = NULL;
	sub->ngnodes = 0;

	/*
	 * A LAUNCH node borrowed the submission's launch fields (the
	 * compute engine reads them). Clear them so the generic finish
	 * path does not release the node's resources a second time.
	 */
	sub->kernel = NULL;
	sub->args_obj = NULL;
	sub->grid = sub->block = 0;
}

/* --------------------------------------------------------- graph submit */

/*
 * Topological order, Kahn's algorithm over a 16-node DAG. Nodes are
 * picked lowest-index-first, so the result is the "obvious" order for
 * the common shapes (a chain stays a chain) and independent nodes keep
 * their submit order. The driver does this at SUBMIT time, which is
 * what makes the engine's job trivial: walk order[], execute.
 *
 * A cycle (or a dependency on a node that never becomes ready) is a
 * userspace bug: reject the graph instead of hanging the engine.
 */
static int vgpu_graph_order(struct vgpu_graph_node *nodes,
			    struct vgpu_graph_edge *edges,
			    u32 nnodes, u32 nedges, u32 *order)
{
	u32 ndep_mask[VGPU_GRAPH_MAX_NODES];
	u32 scheduled = 0;
	u32 i, j;

	memset(ndep_mask, 0, sizeof(ndep_mask));

	/* the edges, spelled two ways (node dep lists / the edge array) */
	for (i = 0; i < nnodes; i++) {
		if (nodes[i].ndeps > VGPU_GRAPH_MAX_DEPS)
			return -EINVAL;
		for (j = 0; j < nodes[i].ndeps; j++) {
			u32 d = nodes[i].deps[j];

			if (d >= nnodes || d == i)
				return -EINVAL;
			ndep_mask[i] |= BIT(d);
		}
	}
	for (i = 0; i < nedges; i++) {
		u32 from = edges[i].from, to = edges[i].to;

		if (from >= nnodes || to >= nnodes || from == to)
			return -EINVAL;
		ndep_mask[to] |= BIT(from);
	}

	while (scheduled < nnodes) {
		bool progressed = false;

		for (i = 0; i < nnodes; i++) {
			if (ndep_mask[i])
				continue;
			ndep_mask[i] = ~0U;	/* visited marker */
			order[scheduled++] = i;
			progressed = true;
			for (j = 0; j < nnodes; j++)
				if (ndep_mask[j] != ~0U &&
				    (ndep_mask[j] & BIT(i)))
					ndep_mask[j] &= ~BIT(i);
			break;
		}
		if (!progressed)
			return -EINVAL;	/* cycle: nothing is ready */
	}
	return 0;
}

/*
 * Resolve one graph node: handles -> pinned objects, kernel id ->
 * struct vgpu_kernel. Same work the doorbell does for a packet, one
 * node at a time.
 */
static int vgpu_graph_resolve_node(struct drm_file *file,
				    struct vgpu_graph_node *n,
				    struct vgpu_gnode *gn)
{
	struct vgpu_file *vf = file->driver_priv;
	int ret;

	gn->type = n->type;

	switch (n->type) {
	case VGPU_PKT_COPY:
		return vgpu_blit_pin(file, &gn->blit, n->copy.src_bo,
				     n->copy.dst_bo, n->copy.off,
				     n->copy.len);
	case VGPU_PKT_LAUNCH:
		if (n->launch.block > 256 || n->launch.block % 32 ||
		    n->launch.grid < 1 || n->launch.args_off % 4)
			return -EINVAL;
		gn->kernel = vgpu_kernel_find(vf, n->launch.kid);
		if (!gn->kernel)
			return -ENOENT;
		gn->grid = n->launch.grid;
		gn->block = n->launch.block;
		gn->args_off = n->launch.args_off;
		gn->args_obj = drm_gem_object_lookup(file, n->launch.args_bo);
		if (!gn->args_obj)
			return -ENOENT;
		ret = drm_gem_shmem_pin(to_drm_gem_shmem_obj(gn->args_obj));
		if (ret) {
			drm_gem_object_put(gn->args_obj);
			gn->args_obj = NULL;
			return ret;
		}
		return 0;
	case VGPU_PKT_SIGNAL_TIMELINE:
	case VGPU_PKT_WAIT_TIMELINE:
	case VGPU_PKT_WRITE_EVENT:
		gn->sem_off = n->sem.sem_off;
		gn->value = n->sem.value;
		return 0;	/* the semaphore page is resolved below */
	case VGPU_PKT_NOP:
		return 0;
	default:
		return -EINVAL;
	}
}

/*
 * VGPU_GRAPH: submit a DAG with one doorbell.
 *
 * Shape: validate -> topologically sort -> resolve every node ->
 * publish ONE fence -> queue the engine. The engine then walks the
 * order and executes: N separate doorbells worth of work behind a
 * single syscall, with the dependencies as data instead of as
 * "submit them in this order and hope".
 */
int vgpu_graph_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct vgpu *v = to_vgpu(dev);
	struct vgpu_graph_submit *arg = data;
	struct vgpu_file *vf = file->driver_priv;
	struct vgpu_graph_node *nodes = NULL;
	struct vgpu_graph_edge *edges = NULL;
	struct vgpu_submission *sub;
	struct vgpu_fence *fence;
	struct vgpu_queue *q;
	unsigned long flags;
	u32 i, order[VGPU_GRAPH_MAX_NODES];
	int ret;

	if (!vf)
		return -EINVAL;
	if (arg->nnodes < 1 || arg->nnodes > VGPU_GRAPH_MAX_NODES ||
	    arg->nedges > VGPU_GRAPH_MAX_EDGES || !arg->nodes)
		return -EINVAL;
	if (arg->nedges && !arg->edges)
		return -EINVAL;

	nodes = kcalloc(arg->nnodes, sizeof(*nodes), GFP_KERNEL);
	edges = kcalloc(arg->nedges ? arg->nedges : 1, sizeof(*edges),
			GFP_KERNEL);
	if (!nodes || !edges) {
		ret = -ENOMEM;
		goto out;
	}
	if (copy_from_user(nodes, u64_to_user_ptr(arg->nodes),
			   arg->nnodes * sizeof(*nodes))) {
		ret = -EFAULT;
		goto out;
	}
	if (arg->nedges &&
	    copy_from_user(edges, u64_to_user_ptr(arg->edges),
			   arg->nedges * sizeof(*edges))) {
		ret = -EFAULT;
		goto out;
	}

	ret = vgpu_graph_order(nodes, edges, arg->nnodes, arg->nedges, order);
	if (ret) {
		dev_err(dev->dev,
			"[vgpu:graph] rejected: DAG not schedulable (cycle?)\n");
		goto out;
	}

	spin_lock_irqsave(&v->lock, flags);
	q = arg->qid ? vgpu_queue_find(vf, arg->qid) : vf->default_q;
	spin_unlock_irqrestore(&v->lock, flags);
	if (!q) {
		ret = -ENOENT;
		goto out;
	}

	sub = kzalloc(sizeof(*sub), GFP_KERNEL);
	if (!sub) {
		ret = -ENOMEM;
		goto out;
	}
	sub->v = v;
	sub->q = q;
	sub->engine = q->engine;
	sub->is_graph = true;
	sub->ngnodes = arg->nnodes;
	sub->gnodes = kcalloc(sub->ngnodes, sizeof(*sub->gnodes), GFP_KERNEL);
	sub->order = kmemdup(order, sizeof(order), GFP_KERNEL);
	if (!sub->gnodes || !sub->order) {
		ret = -ENOMEM;
		goto out_free_sub;
	}

	/*
	 * Resolve every node against this fd, in dependency order, so a
	 * rejected graph leaves nothing pinned behind.
	 */
	for (i = 0; i < sub->ngnodes; i++) {
		u32 n = order[i];

		ret = vgpu_graph_resolve_node(file, &nodes[n],
					      &sub->gnodes[n]);
		if (ret)
			goto out_free_sub;
		/* and the semaphore page, once per graph */
		if (vgpu_pkt_is_sync(nodes[n].type)) {
			ret = vgpu_timeline_resolve(file, sub,
						    nodes[n].sem.sem_bo,
						    nodes[n].sem.sem_off);
			if (ret)
				goto out_free_sub;
		}
	}

	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (!fence) {
		ret = -ENOMEM;
		goto out_free_sub;
	}

	/*
	 * One fence for the whole graph. No ring bytes are claimed (the
	 * nodes arrived through the ioctl, not through the queue's
	 * ring) and no scratch is needed (everything is resolved), so
	 * the accounting stays honest: ncmds == 0, ring_bytes == 0. The
	 * queue is protected from being destroyed under a running graph
	 * by vgpu_queue_busy_locked() instead.
	 */
	spin_lock_irqsave(&v->lock, flags);
	sub->seqno = v->next_seqno++;
	sub->submit_jiffies = jiffies;
	v->stats_submitted++;
	v->stats_graphs++;
	q->stats_submitted++;
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
		goto out_free_sub;
	}

	spin_lock_irqsave(&v->lock, flags);
	list_add_tail(&sub->node, &v->engine_pending[q->engine]);
	spin_unlock_irqrestore(&v->lock, flags);

	dev_info(dev->dev,
		 "[vgpu:graph] q#%u seq#%u nodes=%u edges=%u order=[%u %u %u ...]\n",
		 q->qid, sub->seqno, sub->ngnodes, arg->nedges,
		 order[0], sub->ngnodes > 1 ? order[1] : 0,
		 sub->ngnodes > 2 ? order[2] : 0);

	queue_work(v->engine_wq[q->engine], &v->engine_work[q->engine]);
	arg->seqno = sub->seqno;

	kfree(nodes);
	kfree(edges);
	return 0;

out_free_sub:
	vgpu_sync_release(v, sub);
	kfree(sub);
out:
	kfree(nodes);
	kfree(edges);
	return ret;
}

/*
 * Execute a graph: the nodes are already resolved and ordered, so the
 * engine just walks. In-order execution of a topological order means
 * every dependency is satisfied by the time its dependent runs — no
 * waits are needed inside the graph, which is exactly why the DAG is
 * free to express "these two are independent" (they simply appear in
 * either order) and "this one must follow that one".
 */
void vgpu_graph_execute(struct vgpu *v, struct vgpu_submission *sub)
{
	u32 i;

	for (i = 0; i < sub->ngnodes; i++) {
		struct vgpu_gnode *gn = &sub->gnodes[sub->order[i]];

		switch (gn->type) {
		case VGPU_PKT_NOP:
			udelay(10);
			break;
		case VGPU_PKT_COPY:
			vgpu_blit_pages(v, gn->blit.src_pages,
					gn->blit.dst_pages, gn->blit.npages);
			break;
		case VGPU_PKT_LAUNCH:
			/*
			 * vgpu_compute_run() reads the launch descriptor
			 * out of the submission; a graph node supplies it.
			 * Cleared by vgpu_sync_release().
			 */
			sub->kernel = gn->kernel;
			sub->args_obj = gn->args_obj;
			sub->args_off = gn->args_off;
			sub->grid = gn->grid;
			sub->block = gn->block;
			vgpu_compute_run(v, sub);
			break;
		case VGPU_PKT_SIGNAL_TIMELINE:
		case VGPU_PKT_WAIT_TIMELINE:
		case VGPU_PKT_WRITE_EVENT:
			vgpu_sync_exec_cmd(v, sub, vgpu_pkt_opcode(gn->type),
					   gn->sem_off, gn->value);
			break;
		default:
			if (!sub->error)
				sub->error = -EINVAL;
		}
		if (sub->error)
			break;	/* "GPU fault: stop the graph" */
	}
	dev_info(&v->pdev->dev,
		 "[vgpu:engine] seq#%u (engine %u) graph done (%u nodes)%s\n",
		 sub->seqno, sub->engine, sub->ngnodes,
		 sub->error ? " ERROR" : "");
}
