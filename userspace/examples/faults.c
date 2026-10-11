/*
 * fault containment: the phase-H acceptance test.
 *
 * The property under test, in the words of the plan (docs/
 * GPGPU-EVOLUTION-PLAN.md §4, phase H): "one context's wild pointer must
 * not take down every other context's work." Contexts on one device:
 *
 *   context A  runs a kernel that dereferences past its managed range
 *              (a wild pointer). Its launch must FAIL, a STRUCTURED
 *              FAULT must be recorded naming A's VA and VMID, and A's
 *              NEXT (perfectly legal) launch — in flight at the same
 *              instant — must be DRAINED with -EIO. That is containment,
 *              scoped to A.
 *
 *   context B  runs a normal saxpy over its own managed range. It must
 *              complete CORRECTLY, and only after A's fault ran (it is
 *              queued behind A on the compute engine). This is the whole
 *              point: the device was NOT reset, so B's work survives.
 *
 *   context C  is opened FRESH, after the fault is confirmed, and its
 *              launch must succeed — the device is still fully usable.
 *
 * A final pass drives the inject_fault debugfs knob: an otherwise-legal
 * launch is forced to fault deterministically (vgem's FAKE_HANG, for
 * faults), exercising the same record + containment path.
 *
 * Everything is GPU-VA-named (phase E's VGPU_PKT_LAUNCH_VA), so the "wild
 * pointer" is real: the kernel's own computed address lands outside the
 * managed range it was handed, and it is the driver's engine-side walk
 * that catches it — exactly where a real GPU's page-table walk would.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "drm.h"
#include "vgpu_drm.h"

#define N_ELEMS		512			/* elements per saxpy array */
#define PARAM_WORDS	8			/* header: N,a,b,in_base,out_base */
#define ARGS_BYTES	(((PARAM_WORDS + 2 * N_ELEMS) * 4 + 4095) & ~4095UL)
/* the wild pointer: word index 200000 == byte offset 800000, far past a
 * ~8K managed range, so the OOR check fires on the very first LD. */
#define WILD_WORD	200000

#define FAULTS_DBG	"/sys/kernel/debug/vgpu/faults"
#define INJECT_DBG	"/sys/kernel/debug/vgpu/inject_fault"

/* the good kernel: out[i] = a * in[i] + b (i = global thread id) */
static const struct vgpu_instr saxpy_code[] = {
	{ .op = VGPU_ISA_MOVI, .dst = 15, .a = 0 },
	{ .op = VGPU_ISA_LD,   .dst = 1,  .a = 15, .b = 1 },	/* a */
	{ .op = VGPU_ISA_LD,   .dst = 2,  .a = 15, .b = 2 },	/* b */
	{ .op = VGPU_ISA_LD,   .dst = 3,  .a = 15, .b = 3 },	/* in_base */
	{ .op = VGPU_ISA_LD,   .dst = 4,  .a = 15, .b = 4 },	/* out_base */
	{ .op = VGPU_ISA_ADD,  .dst = 5,  .a = 3,  .b = 0 },	/* in + tid */
	{ .op = VGPU_ISA_LD,   .dst = 6,  .a = 5,  .b = 0 },	/* in[tid] */
	{ .op = VGPU_ISA_MUL,  .dst = 7,  .a = 1,  .b = 6 },	/* a * in[tid] */
	{ .op = VGPU_ISA_ADD,  .dst = 8,  .a = 7,  .b = 2 },	/* + b */
	{ .op = VGPU_ISA_ADD,  .dst = 9,  .a = 4,  .b = 0 },	/* out + tid */
	{ .op = VGPU_ISA_ST,   .dst = 8,  .a = 9,  .b = 0 },	/* out[tid] = */
	{ .op = VGPU_ISA_EXIT },
};

/* the bad kernel: r1 = mem[0 + WILD_WORD] — reads far outside the range */
static const struct vgpu_instr wild_code[] = {
	{ .op = VGPU_ISA_MOVI, .dst = 15, .a = 0 },
	{ .op = VGPU_ISA_LD,   .dst = 1,  .a = 15, .b = WILD_WORD },
	{ .op = VGPU_ISA_EXIT },
};

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

/*
 * One context: its own fd (== its own VMID), the shared argument buffer
 * bound as a MANAGED VA range, a saxpy kernel, and one or more compute
 * queues. Context A carries TWO queues so its faulting launch and the
 * legal "sibling" that containment must drain can be in flight at the
 * same instant — two doorbells on one queue would overlap (the second
 * would cover the first packet until it is reclaimed and be rejected as
 * "two launches"), which is the phase-G lesson about separate rings.
 */
struct fctx {
	int fd;
	uint32_t bo;		/* the shared argument buffer */
	uint32_t *args;		/* the CPU's view of it (its mmap) */
	uint64_t va;		/* the GPU VA it is bound at (managed) */
	uint32_t vmid;
	uint32_t kid;		/* the saxpy kernel id */
	uint32_t wild_kid;	/* the out-of-range kernel (only context A) */
	int nq;			/* how many compute queues this context has */
	uint32_t qid[2];	/* queue ids (compute engine) */
	uint8_t *ring[2];	/* their mmap'ed command rings */
	uint32_t ring_put[2];	/* how far each ring's writer has produced */
	uint32_t a, b;
};

static int fctx_open(struct fctx *c, uint32_t a, uint32_t b, int want_bad,
		     int nq)
{
	struct vgpu_bo_create bo;
	struct vgpu_queue_create qc;
	struct vgpu_module_load ml;
	struct vgpu_vm_alloc al;
	struct vgpu_vm_bind bd;
	int i, q;

	c->a = a;
	c->b = b;
	c->nq = nq;
	c->fd = open_dev();
	if (c->fd < 0)
		return -1;

	memset(&bo, 0, sizeof(bo));
	bo.size = ARGS_BYTES;
	if (check(c->fd, &bo, DRM_IOCTL_VGPU_BO_CREATE, "faults bo_create"))
		return -1;
	c->bo = bo.handle;
	c->args = xmap(c->fd, bo.mmap_offset, ARGS_BYTES);
	if (!c->args)
		return -1;

	c->args[0] = N_ELEMS;
	c->args[1] = a;
	c->args[2] = b;
	c->args[3] = PARAM_WORDS;			/* in[] word index */
	c->args[4] = PARAM_WORDS + N_ELEMS;		/* out[] word index */
	for (i = 0; i < N_ELEMS; i++) {
		c->args[PARAM_WORDS + i] = i * 2 + 1;		/* in[i] */
		c->args[PARAM_WORDS + N_ELEMS + i] = 0xdeadbeef;	/* poison */
	}

	memset(&ml, 0, sizeof(ml));
	ml.nregs = 16;
	ml.smem = 0;
	ml.ninstr = sizeof(saxpy_code) / sizeof(saxpy_code[0]);
	ml.code = (uint64_t)(uintptr_t)saxpy_code;
	if (check(c->fd, &ml, DRM_IOCTL_VGPU_MODULE_LOAD, "module_load(saxpy)"))
		return -1;
	c->kid = ml.kid;

	if (want_bad) {
		memset(&ml, 0, sizeof(ml));
		ml.nregs = 16;
		ml.smem = 0;
		ml.ninstr = sizeof(wild_code) / sizeof(wild_code[0]);
		ml.code = (uint64_t)(uintptr_t)wild_code;
		if (check(c->fd, &ml, DRM_IOCTL_VGPU_MODULE_LOAD,
			  "module_load(wild)"))
			return -1;
		c->wild_kid = ml.kid;
	}

	for (q = 0; q < nq; q++) {
		memset(&qc, 0, sizeof(qc));
		qc.engine = VGPU_ENGINE_COMPUTE;
		qc.prio = 1;
		qc.nslots = 16;
		if (check(c->fd, &qc, DRM_IOCTL_VGPU_QUEUE_CREATE,
			  "queue_create"))
			return -1;
		c->qid[q] = qc.qid;
		c->ring[q] = xmap(c->fd, qc.mmap_offset, qc.ring_size);
		if (!c->ring[q])
			return -1;
	}

	memset(&al, 0, sizeof(al));
	al.size = ARGS_BYTES;
	if (check(c->fd, &al, DRM_IOCTL_VGPU_VM_ALLOC, "vm_alloc"))
		return -1;
	c->va = al.va;
	c->vmid = al.vmid;

	memset(&bd, 0, sizeof(bd));
	bd.va = c->va;
	bd.bo_handle = c->bo;
	bd.flags = VGPU_VM_BIND_MANAGED;
	if (check(c->fd, &bd, DRM_IOCTL_VGPU_VM_BIND, "vm_bind(managed)"))
		return -1;
	return 0;
}

/* a VA-named launch of @kid over @va, on this context's queue @qi.
 * Returns the seqno, or -1 on ioctl error. */
static int fctx_launch(struct fctx *c, uint32_t kid, int qi,
		       uint32_t *seqno_out)
{
	struct vgpu_packet *pk;
	struct vgpu_doorbell db = {};

	if (c->ring_put[qi] + sizeof(*pk) > 16 * 64) {
		fprintf(stderr, "FAIL: ring full\n");
		return -1;
	}
	pk = (struct vgpu_packet *)(c->ring[qi] + c->ring_put[qi]);
	memset(pk, 0, sizeof(*pk));
	pk->type = VGPU_PKT_LAUNCH_VA;
	pk->launch_va.kid = kid;
	pk->launch_va.grid = (N_ELEMS + 63) / 64;
	pk->launch_va.block = 64;
	pk->launch_va.base_va = c->va;
	pk->launch_va.size = ARGS_BYTES;

	db.qid = c->qid[qi];
	db.put = c->ring_put[qi] + sizeof(*pk);
	if (ioctl(c->fd, DRM_IOCTL_VGPU_DOORBELL, &db) < 0) {
		fprintf(stderr, "FAIL: doorbell: %s\n", strerror(errno));
		return -1;
	}
	c->ring_put[qi] = db.put;
	*seqno_out = db.seqno;
	return 0;
}

/* wait for @seqno; returns the fence status (0 ok, negative errno). */
static int fctx_wait(struct fctx *c, uint32_t seqno)
{
	struct vgpu_wait w = {};

	w.seqno = seqno;
	w.timeout_ms = 10000;
	if (check(c->fd, &w, DRM_IOCTL_VGPU_WAIT, "wait"))
		return -1;
	return w.status;
}

/* every element must equal out[i] = a*in[i] + b */
static int fctx_verify(struct fctx *c, const char *who)
{
	int i;

	for (i = 0; i < N_ELEMS; i++) {
		uint32_t want = c->a * c->args[PARAM_WORDS + i] + c->b;
		uint32_t got = c->args[PARAM_WORDS + N_ELEMS + i];

		if (got != want) {
			fprintf(stderr, "FAIL: %s out[%d] = %u, want %u\n",
				who, i, got, want);
			return -1;
		}
	}
	return 0;
}

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

static int dbg_write_all(const char *path, const char *s)
{
	FILE *f = fopen(path, "w");

	if (!f)
		return -1;
	if (fputs(s, f) < 0) {
		fclose(f);
		return -1;
	}
	return fclose(f) ? -1 : 0;
}

int main(void)
{
	struct fctx a, b, c, inj;
	uint32_t seqA_bad, seqA_good, seqB, seqC, seqInj;
	char fb[8192], want[160];
	uint64_t wild_va;
	int rc = 1;

	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	memset(&c, 0, sizeof(c));
	memset(&inj, 0, sizeof(inj));

	/* start each scenario from a known-empty fault buffer */
	if (dbg_write_all(FAULTS_DBG, "clear\n")) {
		fprintf(stderr, "FAIL: clear %s: %s\n", FAULTS_DBG,
			strerror(errno));
		return 1;
	}

	printf("=== phase H: fault containment — one context's wild pointer must not take down the others ===\n");

	/*
	 * context A carries the saxpy kernel, the wild kernel, and TWO
	 * compute queues (one for the faulting launch, one for the legal
	 * sibling containment will drain). B has just its one queue.
	 */
	if (fctx_open(&a, 3, 7, 1, 2))
		return 1;
	if (fctx_open(&b, 5, 11, 0, 1))
		return 1;
	printf("context A: vmid %u va 0x%llx (q#%u + q#%u)  |  context B: vmid %u va 0x%llx\n",
	       a.vmid, (unsigned long long)a.va, a.qid[0], a.qid[1],
	       b.vmid, (unsigned long long)b.va);
	if (a.vmid == b.vmid) {
		fprintf(stderr, "FAIL: A and B share vmid %u\n", a.vmid);
		return 1;
	}
	wild_va = a.va + (unsigned long long)WILD_WORD * 4;

	/*
	 * Queue all three at once: A's wild launch, A's LEGAL launch (the
	 * same context), then B's legal launch. The compute engine runs
	 * them in order: A_wild faults (records + poisons A), A_good is
	 * drained (-EIO, contained), B runs untouched (other context).
	 */
	if (fctx_launch(&a, a.wild_kid, 0, &seqA_bad))
		return 1;
	if (fctx_launch(&a, a.kid, 1, &seqA_good))
		return 1;
	if (fctx_launch(&b, b.kid, 0, &seqB))
		return 1;
	printf("queued: A wild seq#%u (will fault), A saxpy seq#%u (same ctx, queued), B saxpy seq#%u (other ctx)\n",
	       seqA_bad, seqA_good, seqB);

	/*
	 * (c) B completes correctly. Because the engine drains in FIFO
	 * order and B is last, waiting on B guarantees A_wild already
	 * faulted and was recorded before B finished — "after A's fault".
	 */
	if (fctx_wait(&b, seqB) != 0) {
		fprintf(stderr,
			"FAIL: B's launch failed — A's fault took B down with it\n");
		return 1;
	}
	if (fctx_verify(&b, "context B")) {
		fprintf(stderr,
			"FAIL: B's result is wrong (device reset? state lost?)\n");
		return 1;
	}
	printf("PASS: (c) context B completed CORRECTLY after A faulted — the other context survives (%d elements verified)\n",
	       N_ELEMS);

	/* (b1) A's wild launch returns an error. */
	{
		int st = fctx_wait(&a, seqA_bad);

		if (st >= 0) {
			fprintf(stderr,
				"FAIL: A's wild launch returned %d, expected an error\n",
				st);
			return 1;
		}
		printf("PASS: (b) context A's wild launch failed (status=%d)\n",
		       st);
	}

	/* containment: A's NEXT, perfectly legal launch is drained -EIO */
	{
		int st = fctx_wait(&a, seqA_good);

		if (st != -EIO) {
			fprintf(stderr,
				"FAIL: A's legal launch status=%d, want -EIO (containment)\n",
				st);
			return 1;
		}
		printf("PASS: containment: A's in-flight legal launch drained with -EIO (context contained, not the device)\n");
	}

	/* (b2) the fault buffer has a STRUCTURED record naming A's VA */
	if (dbg_read(FAULTS_DBG, fb, sizeof(fb))) {
		fprintf(stderr, "FAIL: read %s: %s\n", FAULTS_DBG,
			strerror(errno));
		return 1;
	}
	printf("--- fault buffer ---\n%s", fb);
	snprintf(want, sizeof(want), "vmid %u va 0x%llx reason=out-of-range",
		 a.vmid, (unsigned long long)wild_va);
	if (!strstr(fb, want)) {
		fprintf(stderr, "FAIL: no fault record naming A: want \"%s\"\n",
			want);
		return 1;
	}
	if (!strstr(fb, "reason=contained-eio")) {
		fprintf(stderr,
			"FAIL: no -EIO containment record for A's drained sibling\n");
		return 1;
	}
	printf("PASS: (b) the fault buffer holds a structured record naming A: vmid %u, wild va 0x%llx, reason out-of-range\n",
	       a.vmid, (unsigned long long)wild_va);

	/*
	 * (d) the device is still usable: open a FRESH context now, after
	 * the fault, and its launch succeeds. No reset, no wedged engine.
	 */
	if (fctx_open(&c, 2, 1, 0, 1))
		return 1;
	if (fctx_launch(&c, c.kid, 0, &seqC))
		return 1;
	if (fctx_wait(&c, seqC) != 0) {
		fprintf(stderr,
			"FAIL: a fresh context's launch failed — the device is wedged\n");
		return 1;
	}
	if (fctx_verify(&c, "context C")) {
		fprintf(stderr, "FAIL: fresh context C's result is wrong\n");
		return 1;
	}
	printf("PASS: (d) a fresh context (vmid %u) launched and verified after the fault — the device is still usable\n",
	       c.vmid);

	/*
	 * The inject_fault knob: arm it, then submit an OTHERWISE-LEGAL
	 * launch on a fresh context. Its first LD faults deterministically
	 * through the exact same record + containment path — proof the
	 * path is testable without a program that really does dereference
	 * a wild pointer on every run (vgem's FAKE_HANG, for faults).
	 */
	if (fctx_open(&inj, 9, 4, 0, 1))
		return 1;
	if (dbg_write_all(INJECT_DBG, "1\n")) {
		fprintf(stderr, "FAIL: write %s: %s\n", INJECT_DBG,
			strerror(errno));
		return 1;
	}
	if (fctx_launch(&inj, inj.kid, 0, &seqInj))
		return 1;
	{
		int st = fctx_wait(&inj, seqInj);

		if (st >= 0) {
			fprintf(stderr,
				"FAIL: injected launch returned %d, expected an error\n",
				st);
			return 1;
		}
		printf("PASS: inject_fault: an otherwise-legal launch forced to fault (status=%d)\n",
		       st);
	}
	if (dbg_read(FAULTS_DBG, fb, sizeof(fb))) {
		fprintf(stderr, "FAIL: re-read %s\n", FAULTS_DBG);
		return 1;
	}
	if (!strstr(fb, "reason=injected")) {
		fprintf(stderr,
			"FAIL: no reason=injected record (the knob did not fire)\n");
		return 1;
	}
	printf("PASS: inject_fault recorded as reason=injected (vmid %u)\n",
	       inj.vmid);

	close(a.fd);
	close(b.fd);
	close(c.fd);
	close(inj.fd);

	printf("phase H: fault contained to the offending context; the fault buffer named it; the other contexts and a fresh context all ran correctly — the device was never reset\n");
	rc = 0;
	return rc;
}
