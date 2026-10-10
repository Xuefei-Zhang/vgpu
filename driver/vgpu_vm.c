// SPDX-License-Identifier: GPL-2.0
/*
 * vgpu - phase C: GPU virtual memory.
 *
 * Three things live here, and together they are the whole memory model
 * of a real GPGPU card (docs/GPGPU-EVOLUTION-PLAN.md §4, phase C):
 *
 *  1. VMID isolation. Every fd is a GPU context and gets its own
 *     address space with its own page tables. Two processes can both
 *     ask for VA 0x10000 and bind DIFFERENT buffers there: the tag a
 *     memory access carries is the VMID, and the driver walks the
 *     tables that tag selects. An address nobody bound is a fault, not
 *     "whatever was there before".
 *
 *  2. The page-table walk. Two levels, both kernel pages (geometry in
 *     vgpu.h). The driver performs the walk because we have no MMU —
 *     this is what a GPU's GCMC, or an SMMU on one's behalf, does in
 *     hardware. The second level is allocated lazily per 4M of VA, so a
 *     context that mapped one page still costs one page, and a
 *     multi-page buffer works for the same reason it works on a CPU:
 *     the tables are what make the addresses contiguous when the pages
 *     are not.
 *
 *  3. The VRAM budget with LRU eviction. "VRAM" is a fiction here: the
 *     backing pages are ordinary shmem, so "eviction to host memory"
 *     is a no-op move — what eviction really does is unwind the page
 *     table entries, which is the whole of what residency means to the
 *     device. A later access faults and the pages are mapped straight
 *     back (demand paging), exactly how CPU swap pulls a page back in.
 *     TTM does this; UVM does it at a finer granularity.
 *
 * LAYERING NOTE (why this file is shaped the way it is):
 * everything that touches the page tables or the LRU takes v->vm_lock,
 * a mutex — not a spinlock, unlike v->lock. The walk allocates a PTE
 * page, and "BUG: Invalid wait Context" from sleeping inside a
 * spinlock is a bug this repo has already paid for once. What keeps
 * that honest is that nothing in the ENGINE ever touches these tables:
 * the engine obeys hardirq-like rules and runs on pages its doorbell
 * already resolved, so the only contexts here are ioctl process
 * contexts, where sleeping is fine.
 */

#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <drm/drm_device.h>
#include <drm/drm_gem_shmem_helper.h>

#include "vgpu.h"

static void vgpu_vm_free(struct vgpu *v, struct vgpu_vm *vm);

/* --------------------------------------------------------- page tables */

/*
 * Level 2 for a VA: the PTE page, creating it if the VA is new to us.
 * The caller holds vm_lock, so the "read then fill" is atomic.
 */
static u32 *vgpu_vm_pte_page(struct vgpu_vm *vm, u64 va, bool create)
{
	u32 *pdes = page_address(vm->pde);
	u32 pde = pdes[VGPU_VM_PDE_IDX(va)];
	struct page *page;

	if (pde & VGPU_PDE_VALID)
		return page_address(pfn_to_page(pde & VGPU_PDE_PFN_MASK));
	if (!create)
		return NULL;

	page = alloc_page(GFP_KERNEL);	/* vm_lock is a mutex: allowed */
	if (!page)
		return ERR_PTR(-ENOMEM);
	memset(page_address(page), 0, PAGE_SIZE);
	pdes[VGPU_VM_PDE_IDX(va)] = page_to_pfn(page) | VGPU_PDE_VALID;
	return page_address(page);
}

/*
 * Install the PTEs for [va, va + npages): our stand-in for the SMMU
 * programming that publishes a device translation.
 */
static int vgpu_vm_map_pages(struct vgpu_vm *vm, u64 va, u32 npages,
			     struct page **pages)
{
	u32 *ptes;
	u32 i;

	for (i = 0; i < npages; i++) {
		u64 a = va + (u64)i * PAGE_SIZE;

		ptes = vgpu_vm_pte_page(vm, a, true);
		if (IS_ERR(ptes))
			return PTR_ERR(ptes);
		ptes[VGPU_VM_PTE_IDX(a)] = page_to_pfn(pages[i]) |
					   VGPU_PTE_VALID;
	}
	return 0;
}

/* Invalidate the PTEs of a range. The pages themselves stay alive. */
static void vgpu_vm_unmap_range(struct vgpu_vm *vm, u64 start, u64 size)
{
	u64 va;

	for (va = start; va < start + size; va += PAGE_SIZE) {
		u32 *ptes = vgpu_vm_pte_page(vm, va, false);

		if (!IS_ERR_OR_NULL(ptes))
			ptes[VGPU_VM_PTE_IDX(va)] &= ~VGPU_PTE_VALID;
	}
}

/* ---------------------------------------------------- the VRAM manager */

/*
 * Map a range into this VM's tables and make it the most recently used
 * entry: the LRU tail is MRU, so whoever calls this last has the best
 * claim to stay resident.
 *
 * @obj is the object to map (== r->obj for the bind/demand paths), and
 * it is passed in rather than read from the range so the caller can map
 * BEFORE it publishes r->obj: an install that fails part-way through
 * must leave the range unbound, not bound-with-half-a-page-table.
 * That is why the failure path unwinds the whole range — a half
 * installed table would leave valid PTEs behind that no accounting
 * knows about, and the next unbind of this range would leave them
 * pointing at a freed BO.
 */
static int vgpu_vm_map_range(struct vgpu *v, struct vgpu_vm *vm,
			     struct vgpu_vm_range *r, struct drm_gem_object *obj)
{
	struct drm_gem_shmem_object *sh = to_drm_gem_shmem_obj(obj);

	if (vgpu_vm_map_pages(vm, r->start, r->size >> PAGE_SHIFT, sh->pages)) {
		vgpu_vm_unmap_range(vm, r->start, r->size);
		return -ENOMEM;
	}
	r->mapped = true;
	list_add_tail(&r->lru_node, &v->vram_lru);
	vm->resident += r->size;
	v->vram_resident += r->size;
	return 0;
}

/*
 * Evict until the device is back inside its budget, starting from the
 * least recently used end. @keep survives: evicting the mapping you
 * are serving right now would be silly, and TTM has the same "don't
 * evict the validating reservation" rule.
 *
 * Eviction is cheap and does not move a byte: it unwinds the PTEs. The
 * BO is still pinned and its shmem pages are still right there — that
 * is precisely the "host memory" the pages get evicted TO.
 */
static void vgpu_vm_evict_to_budget(struct vgpu *v, struct vgpu_vm_range *keep)
{
	struct vgpu_vm_range *r, *tmp;

	list_for_each_entry_safe(r, tmp, &v->vram_lru, lru_node) {
		if (v->vram_resident <= v->vram_budget)
			break;
		if (r == keep)
			continue;
		/*
		 * r->vm is the mapping's owner — the PTEs we unwind live
		 * in that context's level-1 page, NOT in the evicting
		 * one's (a range can be found by walking any VM's list
		 * only through its own vm pointer, and container_of on a
		 * shared list would give the wrong VM).
		 */
		vgpu_vm_unmap_range(r->vm, r->start, r->size);
		r->vm->resident -= r->size;
		r->mapped = false;
		list_del_init(&r->lru_node);
		v->vram_resident -= r->size;
		v->stats_evicted++;
		pr_debug("[vgpu:vm] evicted va 0x%llx (%llu B) to host "
			 "(resident %llu / %llu)\n",
			 r->start, r->size, v->vram_resident, v->vram_budget);
	}
}

void vgpu_vm_evict_all(struct vgpu *v)
{
	mutex_lock(&v->vm_lock);
	vgpu_vm_evict_to_budget(v, NULL);
	mutex_unlock(&v->vm_lock);
}

/*
 * Module unload safety net: an fd still open at rmmod would keep object
 * references against a device that is going away. Real drivers have the
 * same "close everything" step in their unload path.
 */
void vgpu_vm_destroy_all(struct vgpu *v)
{
	struct vgpu_vm *vm, *tmp;

	mutex_lock(&v->vm_lock);
	list_for_each_entry_safe(vm, tmp, &v->vm_list, node) {
		list_del(&vm->node);
		vm->file->vm = NULL;
		mutex_unlock(&v->vm_lock);
		vgpu_vm_free(v, vm);
		mutex_lock(&v->vm_lock);
	}
	mutex_unlock(&v->vm_lock);
}

/* ------------------------------------------------------- VA allocation */

/*
 * First-fit over the holes left between this VM's ranges. A real driver
 * runs drm_mm (colour tracking, splitting, alignment); ours is a list,
 * which is the honest size of the problem here and keeps "two contexts
 * asked for the same address" readable.
 */
/*
 * File-local: the only caller is the reserve path below, and keeping it
 * static means the header does not grow a one-user API (the same reason
 * vgpu_vm_pte_page and vgpu_vm_map_range stay local).
 */
static u64 vgpu_vm_alloc_range(struct vgpu_vm *vm, u64 size, u64 align)
{
	struct vgpu_vm_range *r;
	u64 va;

	if (!size || size > VGPU_VM_VA_END - VGPU_VM_VA_BASE)
		return 0;
	size = ALIGN(size, PAGE_SIZE);
	if (align < PAGE_SIZE)
		align = PAGE_SIZE;
	if (align & (align - 1))	/* not a power of two */
		return 0;

	va = VGPU_VM_VA_BASE;
	list_for_each_entry(r, &vm->ranges, node) {
		u64 cand = ALIGN(va, align);

		if (cand + size <= r->start)	/* the hole fits */
			return cand;
		va = r->start + r->size;
	}
	va = ALIGN(va, align);
	if (va < VGPU_VM_VA_BASE || va + size > VGPU_VM_VA_END)
		return 0;
	return va;
}

static struct vgpu_vm_range *vgpu_vm_range_find(struct vgpu_vm *vm, u64 va)
{
	struct vgpu_vm_range *r;

	list_for_each_entry(r, &vm->ranges, node)
		if (r->start <= va && va < r->start + r->size)
			return r;
	return NULL;
}

/*
 * VM_ALLOC: reserve a VA range. The reservation is real — a range
 * entry with no buffer attached — so a later bind has somewhere to go
 * and the allocator does not hand the same hole out twice.
 */
static int vgpu_vm_reserve(struct vgpu_vm *vm, u64 size, u64 align,
			   u64 *va_out)
{
	struct vgpu_vm_range *r, *prev, *iter;
	u64 va;

	va = vgpu_vm_alloc_range(vm, size, align);
	if (!va)
		return -ENOSPC;

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;
	INIT_LIST_HEAD(&r->node);
	INIT_LIST_HEAD(&r->lru_node);
	r->start = va;
	r->size = ALIGN(size, PAGE_SIZE);
	r->vm = vm;

	/* the range that precedes @va, or NULL if it belongs at the head */
	prev = NULL;
	list_for_each_entry(iter, &vm->ranges, node) {
		if (iter->start > va)
			break;
		prev = iter;
	}

	/*
	 * Insert the new range right after @prev, keeping the list in
	 * address order — the first-fit search above relies on it.
	 *
	 * list_add(), not list_add_tail(): list_add_tail() takes a LIST
	 * HEAD, so list_add_tail(new, &prev->node) splices the new entry
	 * in immediately BEFORE prev. With that, the list stopped being
	 * sorted, the allocator then handed the same VA out twice, and two
	 * buffers ended up described by one set of PTEs — a reservation
	 * overlapping its neighbour is the one thing a VM must never do.
	 */
	list_add(&r->node, prev ? &prev->node : &vm->ranges);

	vm->nranges++;
	*va_out = va;
	return 0;
}

int vgpu_vm_alloc(struct vgpu_file *vf, u64 size, u64 align, u64 *va_out)
{
	struct vgpu *v = vf->vf_dev;
	struct vgpu_vm *vm;
	int ret;

	vm = vgpu_vm_get(vf);
	if (IS_ERR(vm))
		return PTR_ERR(vm);

	mutex_lock(&v->vm_lock);
	ret = vgpu_vm_reserve(vm, size, align, va_out);
	mutex_unlock(&v->vm_lock);
	return ret;
}

u32 vgpu_vm_vmid(struct vgpu_file *vf)
{
	u32 vmid;

	mutex_lock(&vf->vf_dev->vm_lock);
	vmid = vf->vm ? vf->vm->vmid : 0;
	mutex_unlock(&vf->vf_dev->vm_lock);
	return vmid;
}

/* -------------------------------------------------------- VM lifecycle */

static struct vgpu_vm *vgpu_vm_create(struct vgpu *v, struct vgpu_file *vf)
{
	struct vgpu_vm *vm;
	int vmid;

	vm = kzalloc(sizeof(*vm), GFP_KERNEL);
	if (!vm)
		return ERR_PTR(-ENOMEM);

	/*
	 * The VMID: what a real memory access is tagged with so the MMU
	 * knows which tables to walk. One per context, recycled on
	 * destroy — the same life cycle as an amdkfd/kfd process VMID.
	 */
	vmid = ida_alloc_range(&v->vm_ida, VGPU_VMID_MIN, VGPU_VMID_MAX,
			       GFP_KERNEL);
	if (vmid < 0) {
		kfree(vm);
		return ERR_PTR(vmid);
	}

	vm->pde = alloc_page(GFP_KERNEL);
	if (!vm->pde) {
		ida_free(&v->vm_ida, vmid);
		kfree(vm);
		return ERR_PTR(-ENOMEM);
	}
	memset(page_address(vm->pde), 0, PAGE_SIZE);

	INIT_LIST_HEAD(&vm->ranges);
	vm->v = v;
	vm->file = vf;
	vm->vmid = vmid;
	return vm;
}

/*
 * Get (lazily creating) this fd's address space. Ioctl process context
 * only. 
 */
struct vgpu_vm *vgpu_vm_get(struct vgpu_file *vf)
{
	struct vgpu *v = vf->vf_dev;
	struct vgpu_vm *vm;

	mutex_lock(&v->vm_lock);
	if (vf->vm) {
		mutex_unlock(&v->vm_lock);
		return vf->vm;
	}
	vm = vgpu_vm_create(v, vf);
	if (IS_ERR(vm)) {
		mutex_unlock(&v->vm_lock);
		return vm;
	}
	list_add_tail(&vm->node, &v->vm_list);
	vf->vm = vm;
	mutex_unlock(&v->vm_lock);

	dev_info(&v->pdev->dev, "[vgpu:vm] new context: vmid %u\n", vm->vmid);
	return vm;
}

static void vgpu_vm_free(struct vgpu *v, struct vgpu_vm *vm)
{
	struct vgpu_vm_range *r, *tmp;
	u32 *pdes;
	int i;

	/*
	 * Reverse the binds: unwind every resident range, drop the object
	 * references the VM took, free both levels of the page table. The
	 * resident accounting has to be exact, or the device leaks
	 * "VRAM" until the module is reloaded.
	 */
	list_for_each_entry_safe(r, tmp, &vm->ranges, node) {
		if (r->obj) {
			if (r->mapped) {
				vgpu_vm_unmap_range(vm, r->start, r->size);
				list_del_init(&r->lru_node);
				vm->resident -= r->size;
				v->vram_resident -= r->size;
				r->mapped = false;
			}
			/* unpin first: the reference below may be the last */
			drm_gem_shmem_unpin(to_drm_gem_shmem_obj(r->obj));
			drm_gem_object_put(r->obj);
		}
		list_del(&r->node);
		kfree(r);
	}

	pdes = page_address(vm->pde);
	for (i = 0; i < VGPU_VM_PDE_ENTRIES; i++) {
		u32 pde = pdes[i];

		if (pde & VGPU_PDE_VALID)
			__free_page(pfn_to_page(pde & VGPU_PDE_PFN_MASK));
	}
	__free_page(vm->pde);
	ida_free(&v->vm_ida, vm->vmid);
	kfree(vm);
}

/*
 * Tear the fd's address space down. Unpinning and putting happen
 * outside vm_lock: both allocate/free pages.
 */
void vgpu_vm_destroy(struct vgpu_file *vf)
{
	struct vgpu *v = vf->vf_dev;
	struct vgpu_vm *vm;

	mutex_lock(&v->vm_lock);
	vm = vf->vm;
	if (!vm) {
		mutex_unlock(&v->vm_lock);
		return;
	}
	list_del(&vm->node);
	vf->vm = NULL;
	mutex_unlock(&v->vm_lock);

	vgpu_vm_free(v, vm);
}

/* ------------------------------------------------------------ bind/unbind */

/*
 * The queue rings are the driver's own objects. They are handed out as
 * GEM handles so the fd can mmap them (an fd may only mmap a fake offset
 * for which it holds a handle), but they are not pool memory the device
 * may address: binding one would let a VA copy scribble on a live
 * command ring. A real driver makes the same distinction (its own
 * private objects never enter the user's address space).
 */
static bool vgpu_vm_bind_private(struct vgpu_file *vf, struct drm_gem_object *obj)
{
	struct vgpu_queue *q;
	unsigned long flags;
	bool found = false;

	spin_lock_irqsave(&vf->vf_dev->lock, flags);
	list_for_each_entry(q, &vf->queues, node) {
		if (&q->ring_bo->base == obj) {
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&vf->vf_dev->lock, flags);
	return found;
}

int vgpu_vm_bind(struct vgpu_file *vf, u64 va, u32 handle, u32 flags)
{
	struct vgpu *v = vf->vf_dev;
	struct drm_gem_shmem_object *sh;
	struct vgpu_vm_range *r;
	u32 vmid;
	int ret;

	if (va & ~PAGE_MASK)
		return -EINVAL;
	if (flags & ~VGPU_VM_BIND_MANAGED)
		return -EINVAL;

	/*
	 * Resolve + pin BEFORE taking vm_lock: both allocate pages
	 * (GFP_KERNEL, and the WC path runs set_pages_array ->
	 * smp_call_function). The lock is taken only once we hold
	 * everything we may need.
	 *
	 * The lookup's reference becomes the range's own — the handle
	 * table keeps its own, so there is nothing extra to take (and
	 * unbind gives it back with exactly one put).
	 */
	sh = to_drm_gem_shmem_obj(drm_gem_object_lookup(vf->drm_file, handle));
	if (!sh)
		return -ENOENT;
	if (vgpu_vm_bind_private(vf, &sh->base)) {
		drm_gem_object_put(&sh->base);
		return -EINVAL;
	}
	if (drm_gem_shmem_pin(sh)) {
		drm_gem_object_put(&sh->base);
		return -ENOMEM;
	}

	mutex_lock(&v->vm_lock);
	if (!vf->vm) {
		ret = -ENOENT;
		goto out_unlock;
	}
	r = vgpu_vm_range_find(vf->vm, va);
	if (!r || r->start != va || r->obj || sh->base.size < r->size) {
		ret = -EINVAL;
		goto out_unlock;
	}

	/*
	 * Map first, while the range is still unbound: if the table
	 * install fails the caller gets -ENOMEM with the range (and its
	 * reservation) untouched.
	 */
	ret = vgpu_vm_map_range(v, vf->vm, r, &sh->base);
	if (ret)
		goto out_unlock;

	r->obj = &sh->base;
	r->handle = handle;
	v->stats_bound++;
	vgpu_vm_evict_to_budget(v, r);
	vmid = vf->vm->vmid;
	mutex_unlock(&v->vm_lock);

	dev_info(&v->pdev->dev,
		 "[vgpu:vm] vmid %u: bound va 0x%llx + %llu B <- bo#%u (%s)\n",
		 vmid, r->start, r->size, handle,
		 r->mapped ? "resident" : "evicted, over VRAM budget");
	return 0;

out_unlock:
	mutex_unlock(&v->vm_lock);
	drm_gem_shmem_unpin(sh);
	drm_gem_object_put(&sh->base);
	return ret;
}

void vgpu_vm_unbind(struct vgpu_file *vf, u64 va)
{
	struct vgpu *v = vf->vf_dev;
	struct drm_gem_shmem_object *sh;
	struct vgpu_vm_range *r;
	u32 vmid;

	if (va & ~PAGE_MASK)
		return;

	mutex_lock(&v->vm_lock);
	r = vf->vm ? vgpu_vm_range_find(vf->vm, va) : NULL;
	if (!r || !r->obj) {
		mutex_unlock(&v->vm_lock);
		return;
	}
	if (r->mapped) {
		vgpu_vm_unmap_range(vf->vm, r->start, r->size);
		list_del_init(&r->lru_node);
		vf->vm->resident -= r->size;
		v->vram_resident -= r->size;
		r->mapped = false;
	}
	sh = to_drm_gem_shmem_obj(r->obj);
	r->obj = NULL;
	r->handle = 0;
	vmid = vf->vm->vmid;
	mutex_unlock(&v->vm_lock);

	/* unpin/put may free pages: outside vm_lock */
	drm_gem_shmem_unpin(sh);
	drm_gem_object_put(&sh->base);

	dev_info(&v->pdev->dev, "[vgpu:vm] vmid %u: unbound va 0x%llx\n",
		 vmid, va);
}

/* ---------------------------------------------------------- the walk */

/*
 * Resolve npages of GPU VA in this fd. On success the caller owns a
 * reference on the buffer and one on every page — the same "pin and
 * reference at submit" rule the BLIT path follows, and the reason a
 * concurrent unbind or eviction cannot pull the pages out from under
 * the engine.
 *
 * This is the phase's lesson in one function:
 *
 *     va -> PDE index -> PTE page -> PTE index -> data page
 *
 * A missing level-1 entry means the address is outside anything this
 * VM ever touched: a hard fault (-ENOENT), the same class of thing an
 * SMMU context fault is. A cleared PTE means the page was evicted to
 * the host earlier: we map the range back (demand paging — the device
 * faulted, the driver serviced it) and the caller walks into the
 * freshly valid PTE. The fault counters make that observable, because
 * otherwise nobody could tell a working migration from a lucky
 * accident.
 */
int vgpu_vm_resolve(struct vgpu_file *vf, u64 va, u32 npages,
		    struct drm_gem_object **objp, struct page **pages)
{
	struct vgpu *v = vf->vf_dev;
	struct vgpu_vm_range *r;
	u32 i;
	int ret = 0;

	mutex_lock(&v->vm_lock);
	r = vf->vm ? vgpu_vm_range_find(vf->vm, va) : NULL;
	if (!r || !r->obj ||
	    va + (u64)npages * PAGE_SIZE > r->start + r->size) {
		mutex_unlock(&v->vm_lock);
		return -ENOENT;
	}

	if (!r->mapped) {
		/*
		 * The device faulted on a VA it had bound: service it.
		 * The BO never moved (its pages are host memory, which
		 * is where "evicted to host" lands), so a service is a
		 * PTE install and nothing else.
		 */
		ret = vgpu_vm_map_range(v, vf->vm, r, r->obj);
		if (ret) {
			mutex_unlock(&v->vm_lock);
			return ret;
		}
		v->stats_vm_faults++;
		v->stats_remap_pages += r->size >> PAGE_SHIFT;
		dev_info(&v->pdev->dev,
			 "[vgpu:vm] vmid %u: fault va 0x%llx -> demand mapped "
			 "%llu B back into VRAM\n",
			 vf->vm->vmid, va, r->size);
	}

	for (i = 0; i < npages; i++) {
		u64 a = va + (u64)i * PAGE_SIZE;
		u32 *ptes = vgpu_vm_pte_page(vf->vm, a, false);
		u32 pte;

		if (IS_ERR_OR_NULL(ptes)) {
			ret = -ENOENT;
			goto out_pages;
		}
		pte = ptes[VGPU_VM_PTE_IDX(a)];
		if (!(pte & VGPU_PTE_VALID)) {
			ret = -ENOENT;
			goto out_pages;
		}
		pages[i] = pfn_to_page(pte & VGPU_PTE_PFN_MASK);
		get_page(pages[i]);
	}

	/*
	 * Keep this range hot — the access we are serving is the best
	 * possible evidence it should survive the next budget decision —
	 * and re-run the budget, because the demand map we just did may
	 * have put the device over.
	 */
	list_move_tail(&r->lru_node, &v->vram_lru);
	vgpu_vm_evict_to_budget(v, r);

	drm_gem_object_get(r->obj);
	*objp = r->obj;
	mutex_unlock(&v->vm_lock);
	return 0;

out_pages:
	while (i--)
		put_page(pages[i]);
	mutex_unlock(&v->vm_lock);
	return ret;
}

void vgpu_vm_release(struct drm_gem_object *obj, struct page **pages,
		     u32 npages)
{
	u32 i;

	for (i = 0; i < npages; i++)
		put_page(pages[i]);
	kfree(pages);
	if (obj)
		drm_gem_object_put(obj);
}

/*
 * Undo a (possibly partial) VA resolution, on the submit error path.
 * Only the side that actually resolved is undone: a submission that
 * failed on the destination must not walk an uninitialised page array.
 */
void vgpu_vm_release_sub(struct vgpu_submission *sub)
{
	if (sub->vm_src_obj)
		vgpu_vm_release(sub->vm_src_obj, sub->vm_src_pages,
				sub->vm_npages);
	else
		kfree(sub->vm_src_pages);
	sub->vm_src_obj = NULL;
	sub->vm_src_pages = NULL;

	if (sub->vm_dst_obj)
		vgpu_vm_release(sub->vm_dst_obj, sub->vm_dst_pages,
				sub->vm_npages);
	else
		kfree(sub->vm_dst_pages);
	sub->vm_dst_obj = NULL;
	sub->vm_dst_pages = NULL;
}

/* ------------------------------------------------------------ debugfs */

/*
 * The "vm" view. This is the nvidia-smi-shaped half of phase C: per
 * context, per VA range, and the device's budget/eviction picture.
 * Also writable: shrinking the budget at runtime applies it
 * immediately, which is how the guest test drives eviction without
 * needing a module parameter.
 *
 * The layout is deliberately greppable — the per-range state word is
 * one of reserved/mapped/evicted, and the device line carries the two
 * counters the acceptance criteria name.
 */
int vgpu_vm_show(struct seq_file *s, void *data)
{
	/*
	 * The pointer is in seq_file::private, NOT in the show callback's
	 * @data: single_open()'s start() hands show() SEQ_START_TOKEN
	 * ((void*)1) as the iterator cookie, so @data is a dummy. Reading
	 * the device from @data dereferences 1 and takes the box down (it
	 * did — this is why every other show function in this driver uses
	 * s->private).
	 */
	struct vgpu *v = s->private;
	struct vgpu_vm *vm;
	u32 nvm = 0;

	(void)data;

	mutex_lock(&v->vm_lock);
	list_for_each_entry(vm, &v->vm_list, node) {
		struct vgpu_vm_range *r;

		seq_printf(s, "vmid %u: %u range(s), %llu B resident\n",
			   vm->vmid, vm->nranges, vm->resident);
		list_for_each_entry(r, &vm->ranges, node)
			seq_printf(s, "  va 0x%llx-0x%llx %6lluK  %-9s  bo#%u\n",
				   r->start, r->start + r->size,
				   r->size / 1024,
				   !r->obj ? "reserved" :
				   r->mapped ? "mapped" : "evicted",
				   r->handle);
		nvm++;
	}
	seq_printf(s, "contexts %u  bound %llu range(s)\n", nvm, v->stats_bound);
	seq_printf(s, "device: budget %llu B  resident %llu B  "
		   "evicted %llu  demand-faults %llu  remapped %llu pages\n",
		   v->vram_budget, v->vram_resident, v->stats_evicted,
		   v->stats_vm_faults, v->stats_remap_pages);
	mutex_unlock(&v->vm_lock);
	return 0;
}
