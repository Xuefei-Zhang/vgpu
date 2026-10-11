/*
 * saxpy over UNIFIED memory: the phase-E example.
 *
 * This is userspace/examples/saxpy.c with one thing changed — the
 * kernel's global memory is named by GPU VIRTUAL ADDRESS instead of by
 * GEM handle — and the point of the example is everything that change
 * buys. The mapping to what CUDA calls Unified Memory:
 *
 *   CUDA                                   | vgpu
 *   ---------------------------------------|---------------------------
 *   cudaMallocManaged(&p, bytes)           | VGPU_BO_CREATE (host shmem)
 *   cudaMallocManaged's one address space  | VM_ALLOC + VM_BIND MANAGED
 *   cudaLaunchKernel(...)                  | VGPU_PKT_LAUNCH_VA
 *   a page fault on the GPU                | the engine's LD/ST misses a
 *                                          |   PTE -> vgpu_vm_loan() ->
 *                                          |   phase C's demand re-map
 *   cudaDeviceSynchronize()                | VGPU_WAIT (kernel-side fence)
 *   read p on the CPU                      | read the BO's mmap
 *
 * The SAME ISA program runs as in saxpy.c. Addresses are word indices
 * either way; what changes is what they are an index into — a pinned
 * object the doorbell resolved once, or an address space the CPU and
 * the device share. The CPU writes in[] before the launch and reads
 * out[] after it, through the BO's mmap, while the device reaches both
 * through the page tables. If a kernel's store lands anywhere else, the
 * verification below fails — that is the "unified" property under test,
 * not a nicety.
 *
 * The flow proves demand migration by forcing it:
 *
 *   1. VM_ALLOC a range, VM_BIND it MANAGED, with the budget wide open
 *      -> the range is resident, the device can reach it by VA.
 *   2. shrink the VRAM budget to 0 through the writable debugfs view
 *      -> the range is EVICTED (its PTEs unwound; the shmem pages stay,
 *      because "evicted to host memory" is a no-op move for them).
 *   3. launch by VA. The kernel's very first LD finds no PTE, the
 *      driver migrates the range back in and the access retries. The
 *      kernel never learns this happened.
 *   4. assert the results are correct AND the demand-fault counter went
 *      up, then do the whole cycle a second time to show it is a
 *      mechanism, not a one-off.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "drm.h"
#include "vgpu_drm.h"

#define N_ELEMS		2048		/* elements per array */
#define BLOCK		64		/* threads per block (multiple of 32) */
#define PARAM_WORDS	8		/* header of the argument buffer */
#define ARGS_BYTES	(((PARAM_WORDS + 2 * N_ELEMS) * 4 + 4095) & ~4095UL)
#define VM_DBG		"/sys/kernel/debug/vgpu/vm"
#define CS_DBG		"/sys/kernel/debug/vgpu/compute"

static int check(int fd, void *arg, unsigned long nr, const char *what)
{
	if (ioctl(fd, nr, arg) < 0) {
		fprintf(stderr, "FAIL: %s: %s\n", what, strerror(errno));
		return -1;
	}
	return 0;
}

static int open_dev(void)
{
	int fd = open("/dev/dri/renderD128", O_RDWR);

	if (fd < 0)
		fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0)
		fprintf(stderr, "FAIL: open: %s\n", strerror(errno));
	return fd;
}

static void *xmap(int fd, uint64_t off, uint64_t size)
{
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		       (off_t)off);

	if (p == MAP_FAILED) {
		fprintf(stderr, "FAIL: mmap: %s\n", strerror(errno));
		return NULL;
	}
	return p;
}

/* slurp a debugfs file; returns 0 on success */
static int dbg_read(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");
	size_t n;

	if (!f)
		return -1;
	n = fread(buf, 1, len - 1, f);
	buf[n] = '\0';
	fclose(f);
	return 0;
}

/* the vm view is writable: a new budget applies at once */
static int dbg_write(const char *path, unsigned long long val)
{
	FILE *f = fopen(path, "w");

	if (!f)
		return -1;
	if (fprintf(f, "%llu\n", val) < 0) {
		fclose(f);
		return -1;
	}
	return fclose(f) ? -1 : 0;
}

/*
 * One number out of a "key N" field. The vm view prints its counters on
 * the "device:" line; the compute view is plain "key N" lines, so the
 * same helper works on both.
 */
static long long num_of(const char *buf, const char *key)
{
	const char *line, *p;
	char *end;
	long long v;

	line = strstr(buf, "device:") ? strstr(buf, "device:") : buf;
	p = strstr(line, key);
	if (!p)
		return -1;
	p += strlen(key);
	while (*p == ' ' || *p == '\t')
		p++;
	v = strtoll(p, &end, 10);
	return end == p ? -1 : v;
}

struct uvm_ctx {
	int fd;
	uint32_t qid;
	uint8_t *ring;
	uint32_t ring_put;
	uint32_t bo;			/* the shared argument buffer */
	uint32_t *args;			/* its mmap: the CPU's view of it */
	uint64_t va;			/* its GPU VA: the device's view */
	uint32_t kid;
};

/*
 * One launch. Returns 0, or -errno from the doorbell (the engine is not
 * involved in a rejection — that is the point of validating at submit
 * time) or -1 if a launch that WAS accepted came back failed.
 *
 * @expect_fail: the caller is checking that the doorbell REJECTS this
 * launch, so the errno is the expected answer rather than an error to
 * report (see the non-managed range check at the end of main).
 */
static int uvm_launch(struct uvm_ctx *c, bool expect_fail)
{
	struct vgpu_packet *pk;
	struct vgpu_doorbell db = {};
	struct vgpu_wait w = {};

	if (c->ring_put + sizeof(*pk) > 16 * 64) {
		fprintf(stderr, "FAIL: uvm ring full\n");
		return -1;
	}
	pk = (struct vgpu_packet *)(c->ring + c->ring_put);
	memset(pk, 0, sizeof(*pk));

	/*
	 * The VA-named launch: the operand that saxpy.c spells as a GEM
	 * handle is here a byte offset in this context's address space,
	 * plus how many bytes of it are addressable. Nothing else about
	 * the descriptor changed — same grid, same block, same kernel.
	 */
	pk->type = VGPU_PKT_LAUNCH_VA;
	pk->launch_va.kid = c->kid;
	pk->launch_va.grid = (N_ELEMS + BLOCK - 1) / BLOCK;
	pk->launch_va.block = BLOCK;
	pk->launch_va.base_va = c->va;
	pk->launch_va.size = ARGS_BYTES;

	db.qid = c->qid;
	db.put = c->ring_put + sizeof(*pk);
	if (ioctl(c->fd, DRM_IOCTL_VGPU_DOORBELL, &db) < 0) {
		if (!expect_fail)
			fprintf(stderr, "FAIL: uvm doorbell: %s\n",
				strerror(errno));
		return -errno;
	}
	c->ring_put = db.put;

	w.seqno = db.seqno;
	w.timeout_ms = 10000;
	if (check(c->fd, &w, DRM_IOCTL_VGPU_WAIT, "uvm wait"))
		return -1;
	if (w.status != 0) {
		fprintf(stderr, "FAIL: uvm launch status=%d\n", (int)w.status);
		return -1;
	}
	return 0;
}

/* every element the kernel should have produced */
static int verify(const uint32_t *args)
{
	int i;

	for (i = 0; i < N_ELEMS; i++) {
		uint32_t want = args[1] * args[PARAM_WORDS + i] + args[2];
		uint32_t got = args[PARAM_WORDS + N_ELEMS + i];

		if (got != want) {
			fprintf(stderr,
				"FAIL: out[%d] = %u, want %u (device wrote "
				"through the page tables, CPU read the mmap)\n",
				i, got, want);
			return -1;
		}
	}
	return 0;
}

int main(void)
{
	static const struct vgpu_instr code[] = {
		{ .op = VGPU_ISA_MOVI, .dst = 15, .a = 0 },	/* r15 = 0 */
		{ .op = VGPU_ISA_LD,   .dst = 1,  .a = 15, .b = 1 }, /* a */
		{ .op = VGPU_ISA_LD,   .dst = 2,  .a = 15, .b = 2 }, /* b */
		{ .op = VGPU_ISA_LD,   .dst = 3,  .a = 15, .b = 3 }, /* in_base */
		{ .op = VGPU_ISA_LD,   .dst = 4,  .a = 15, .b = 4 }, /* out_base */
		{ .op = VGPU_ISA_ADD,  .dst = 5,  .a = 3,  .b = 0 }, /* in + tid */
		{ .op = VGPU_ISA_LD,   .dst = 6,  .a = 5,  .b = 0 }, /* in[tid] */
		{ .op = VGPU_ISA_MUL,  .dst = 7,  .a = 1,  .b = 6 }, /* a * in[tid] */
		{ .op = VGPU_ISA_ADD,  .dst = 8,  .a = 7,  .b = 2 }, /* + b */
		{ .op = VGPU_ISA_ADD,  .dst = 9,  .a = 4,  .b = 0 }, /* out + tid */
		{ .op = VGPU_ISA_ST,   .dst = 8,  .a = 9,  .b = 0 }, /* out[tid] = */
		{ .op = VGPU_ISA_EXIT },
	};
	struct uvm_ctx c = {};
	struct vgpu_bo_create bo;
	struct vgpu_queue_create qc;
	struct vgpu_module_load ml;
	struct vgpu_vm_alloc va_alloc;
	struct vgpu_vm_bind bind;
	char dbg[8192], cs[4096];
	long long budget = 0;
	long long evicted, faults, remapped, engine_faults;
	long long ev0, fl0, rm0, ef0;
	int i, rc = 1, round;

	c.fd = open_dev();
	if (c.fd < 0)
		return 1;

	if (dbg_read(VM_DBG, dbg, sizeof(dbg))) {
		fprintf(stderr, "FAIL: read %s: %s\n", VM_DBG,
			strerror(errno));
		goto out;
	}
	budget = num_of(dbg, "budget ");
	if (budget < 0) {
		fprintf(stderr, "FAIL: no budget in %s\n", VM_DBG);
		goto out;
	}
	ev0 = num_of(dbg, "evicted ");
	fl0 = num_of(dbg, "demand-faults ");
	rm0 = num_of(dbg, "remapped ");
	if (dbg_read(CS_DBG, cs, sizeof(cs))) {
		fprintf(stderr, "FAIL: read %s: %s\n", CS_DBG,
			strerror(errno));
		goto out;
	}
	ef0 = num_of(cs, "engine vm faults");
	if (ev0 < 0 || fl0 < 0 || rm0 < 0 || ef0 < 0) {
		fprintf(stderr, "FAIL: counters missing from the vm/compute views\n");
		goto out;
	}
	printf("uvm: device budget %lld B, demand-faults %lld, "
	       "engine vm faults %lld\n",
	       budget, fl0, ef0);

	/* ---- the one buffer the CPU and the device will share -------- */
	memset(&bo, 0, sizeof(bo));
	bo.size = ARGS_BYTES;
	if (check(c.fd, &bo, DRM_IOCTL_VGPU_BO_CREATE, "uvm bo_create"))
		goto out;
	c.bo = bo.handle;
	c.args = xmap(c.fd, bo.mmap_offset, ARGS_BYTES);
	if (!c.args)
		goto out;

	/* the CPU side fills the buffer before the device ever sees it */
	c.args[0] = N_ELEMS;
	c.args[1] = 3;				/* a */
	c.args[2] = 7;				/* b */
	c.args[3] = PARAM_WORDS;		/* in[] word index */
	c.args[4] = PARAM_WORDS + N_ELEMS;	/* out[] word index */
	for (i = 0; i < N_ELEMS; i++) {
		c.args[PARAM_WORDS + i] = i * 2 + 1;		/* in[i] */
		c.args[PARAM_WORDS + N_ELEMS + i] = 0xdeadbeef;	/* poison */
	}

	/* ---- the module: identical to saxpy's ------------------------ */
	memset(&ml, 0, sizeof(ml));
	ml.nregs = 16;
	ml.smem = 0;
	ml.ninstr = sizeof(code) / sizeof(code[0]);
	ml.code = (uint64_t)(uintptr_t)code;
	if (check(c.fd, &ml, DRM_IOCTL_VGPU_MODULE_LOAD, "module_load"))
		goto out;
	c.kid = ml.kid;

	/* ---- a compute stream ---------------------------------------- */
	memset(&qc, 0, sizeof(qc));
	qc.engine = VGPU_ENGINE_COMPUTE;
	qc.prio = 1;
	qc.nslots = 16;
	if (check(c.fd, &qc, DRM_IOCTL_VGPU_QUEUE_CREATE, "queue_create"))
		goto out;
	c.qid = qc.qid;
	c.ring = xmap(c.fd, qc.mmap_offset, qc.ring_size);
	if (!c.ring)
		goto out;

	/* ---- one GPU VA for that buffer, in THIS context ------------- */
	memset(&va_alloc, 0, sizeof(va_alloc));
	va_alloc.size = ARGS_BYTES;
	if (check(c.fd, &va_alloc, DRM_IOCTL_VGPU_VM_ALLOC, "vm_alloc"))
		goto out;
	c.va = va_alloc.va;
	printf("uvm: vmid %u reserved va 0x%llx + %llu B\n",
	       va_alloc.vmid, (unsigned long long)c.va,
	       (unsigned long long)ARGS_BYTES);

	/* ... and bound MANAGED: this is the unified-memory contract */
	memset(&bind, 0, sizeof(bind));
	bind.va = c.va;
	bind.bo_handle = c.bo;
	bind.flags = VGPU_VM_BIND_MANAGED;
	if (check(c.fd, &bind, DRM_IOCTL_VGPU_VM_BIND, "vm_bind(managed)"))
		goto out;

	/*
	 * The whole demonstration, twice: evict, launch across the hole,
	 * check the results and the counters. Twice because a single
	 * migration could be luck — the same mechanism repeating on
	 * demand is the evidence.
	 */
	for (round = 1; round <= 2; round++) {
		long long d_ev, d_fl, d_rm, d_ef;

		/*
		 * Shrink the budget to nothing. The range's PTEs are
		 * unwound; the shmem pages are exactly where they were,
		 * which is what "evicted to host memory" means here.
		 */
		if (dbg_write(VM_DBG, 0)) {
			fprintf(stderr, "FAIL: write %s: %s\n", VM_DBG,
				strerror(errno));
			goto out;
		}
		if (dbg_read(VM_DBG, dbg, sizeof(dbg))) {
			fprintf(stderr, "FAIL: read %s: %s\n", VM_DBG,
				strerror(errno));
			goto out;
		}
		printf("--- debugfs vm after forcing the budget to 0 ---\n%s",
		       dbg);
		if (!strstr(dbg, "evicted")) {
			fprintf(stderr,
				"FAIL: the managed range is not listed evicted\n");
			goto out;
		}

		/*
		 * Launch by VA. Every LD/ST walks this context's page
		 * tables; the first one finds no PTE, the driver
		 * migrates the range back in, and the access retries.
		 */
		if (uvm_launch(&c, false))
			goto out;
		printf("uvm: round %d: launch over va 0x%llx retired\n",
		       round, (unsigned long long)c.va);

		if (verify(c.args)) {
			fprintf(stderr,
				"FAIL: round %d wrong after demand migration\n",
				round);
			goto out;
		}

		if (dbg_read(VM_DBG, dbg, sizeof(dbg)) ||
		    dbg_read(CS_DBG, cs, sizeof(cs))) {
			fprintf(stderr, "FAIL: re-read the debugfs views\n");
			goto out;
		}
		evicted = num_of(dbg, "evicted ");
		faults = num_of(dbg, "demand-faults ");
		remapped = num_of(dbg, "remapped ");
		engine_faults = num_of(cs, "engine vm faults");
		d_ev = evicted - ev0;
		d_fl = faults - fl0;
		d_rm = remapped - rm0;
		d_ef = engine_faults - ef0;

		if (d_ev <= 0) {
			fprintf(stderr,
				"FAIL: round %d evicted=%lld, expected > 0\n",
				round, d_ev);
			goto out;
		}
		if (d_fl <= 0 || d_rm <= 0) {
			fprintf(stderr,
				"FAIL: round %d demand-faults=%lld remapped=%lld,"
				" expected both > 0\n", round, d_fl, d_rm);
			goto out;
		}
		if (d_ef <= 0) {
			fprintf(stderr,
				"FAIL: round %d engine vm faults=%lld, "
				"expected > 0 (the fault must be the "
				"engine's, not a DMA's)\n", round, d_ef);
			goto out;
		}
		printf("PASS: round %d: %lld range(s) evicted, %lld demand "
		       "fault(s) serviced mid-kernel, %lld page(s) remapped, "
		       "%lld engine fault(s)\n",
		       round, d_ev, d_fl, d_rm, d_ef);
	}

		printf("PASS: unified memory — the device reached va 0x%llx after "
		       "its pages were evicted, transparently (%d elements "
		       "verified, CPU and device sharing the same pages)\n",
		       (unsigned long long)c.va, N_ELEMS);
		rc = 0;

	/*
	 * The flag is load-bearing, not decoration: the very same buffer
	 * bound WITHOUT VGPU_VM_BIND_MANAGED is ordinary device memory, and
	 * a VA-named launch naming it is rejected at the doorbell rather
	 * than silently run as if it were managed. An address nobody
	 * declared managed is a fault (phase C's rule), not a coincidence.
	 */
	memset(&va_alloc, 0, sizeof(va_alloc));
	va_alloc.size = ARGS_BYTES;
	if (check(c.fd, &va_alloc, DRM_IOCTL_VGPU_VM_ALLOC,
		  "vm_alloc(plain)")) {
		rc = 1;
		goto out;
	}
	memset(&bind, 0, sizeof(bind));
	bind.va = va_alloc.va;
	bind.bo_handle = c.bo;
	if (check(c.fd, &bind, DRM_IOCTL_VGPU_VM_BIND, "vm_bind(plain)")) {
		rc = 1;
		goto out;
	}
	c.va = va_alloc.va;
	if (uvm_launch(&c, true) != -ENOENT) {
		fprintf(stderr,
			"FAIL: a launch naming a non-managed range was not "
			"rejected with -ENOENT\n");
		rc = 1;
		goto out;
	}
	printf("PASS: the same buffer bound WITHOUT VGPU_VM_BIND_MANAGED "
	       "rejects a VA-named launch (-ENOENT)\n");

out:
	/* leave the device the way we found it */
	if (budget > 0)
		dbg_write(VM_DBG, budget);
	close(c.fd);
	return rc;
}
