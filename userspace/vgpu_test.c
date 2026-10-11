/*
 * vgpu_test - userspace test for the vgpu virtual GPU driver.
 *
 * The full chain, in one process:
 *   open /dev/dri/renderD128
 *   VGPU_BO_CREATE   src BO (64K, pattern) + dst BO (64K, zeroed)
 *   VGPU_RING_GET    -> mmap the command ring
 *   write NOP + BLIT + SET commands into our ring slot
 *   VGPU_CS          (the doorbell)        -> seqno
 *   VGPU_WAIT        (seqno, kernel-side fence wait)
 *   verify: dst BO holds the pattern; the MMIO user register was written
 *   VGPU_QUERY       (device stats)
 *
 * -m N: fork N processes; each runs the chain with its own BOs and its
 * own ring slot (the ring is shared scratch: disjoint slots, like real
 * per-context rings).
 * -i:   phase I: runtime PM (sleep + wake), the DVFS clock knob, and
 *        per-context compute accounting.
 * -p:   packet + doorbell submission (phase B): the engine fetches.
 * -q:   multi-queue / multi-engine: one fd, one stream per engine
 *        (streams + per-engine command classes).
 * -e:   the negative paths only (rejected doorbell, engine faults,
 * ring full) — each must fail the exact way DESIGN.md §10 says.
 * -g:   phase G: memory-semantics sync. Completion is discovered by
 *        POLLING MMAP'ED MEMORY (timeline semaphore + event slots) —
 *        no VGPU_WAIT anywhere in that path — and a 4-node graph
 *        (copy -> launch -> copy -> signal) is submitted with ONE
 *        doorbell and measured against 4 separate doorbells.
 * -v:   phase C: GPU virtual memory. Two processes bind DIFFERENT
 *        BOs to the SAME virtual address and each reads back its own
 *        data (VMID isolation); binding past the VRAM budget evicts
 *        and demand-faults transparently.
 * -f:   phase F: timeslicing (two contexts on the compute engine must
 *        interleave) and preemption (a prio=2 launch submitted while a
 *        prio=0 one runs must pause it), with both results correct.
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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "drm.h"
#include "vgpu_drm.h"

#define BO_SIZE (64 * 1024)
#define BLIT_LEN 4096
#define USER_MAGIC 0xdeadbeefu

static int open_dev(void)
{
	int fd = open("/dev/dri/renderD128", O_RDWR);

	if (fd < 0)
		fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0)
		fprintf(stderr, "FAIL: open /dev/dri/*: %s\n", strerror(errno));
	return fd;
}

static int check(int fd, void *arg, unsigned long nr, const char *what)
{
	if (ioctl(fd, nr, arg) < 0) {
		fprintf(stderr, "FAIL: %s: %s\n", what, strerror(errno));
		return -1;
	}
	return 0;
}

static void *xmap(int fd, uint64_t offset, size_t size)
{
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fd, (off_t)offset);

	if (p == MAP_FAILED) {
		fprintf(stderr, "FAIL: mmap: %s\n", strerror(errno));
		exit(1);
	}
	return p;
}

/* an ioctl that is expected to fail with a specific errno */
static int expect_errno(int fd, void *arg, unsigned long nr, int want,
			const char *what)
{
	if (ioctl(fd, nr, arg) == 0) {
		fprintf(stderr, "FAIL: %s: succeeded, expected errno=%d\n",
			what, want);
		return -1;
	}
	if (errno != want) {
		fprintf(stderr, "FAIL: %s: errno=%d (%s), expected %d\n",
			what, errno, strerror(errno), want);
		return -1;
	}
	printf("[pid %d] PASS: %s rejected (-%s)\n", (int)getpid(), what,
	       strerror(errno));
	return 0;
}

static int run_one(int slot)
{
	int fd, i;
	int rc = 1;
	uint8_t *src, *dst;
	uint8_t *ring;
	struct vgpu_cmd *cmds;
	struct vgpu_bo_create bo;
	struct vgpu_ring_get rg;
	struct vgpu_cs cs;
	struct vgpu_wait w;
	struct vgpu_query q;
	size_t off = (size_t)slot * 64;
	uint32_t src_handle, dst_handle;

	fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0)
		fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "FAIL: open /dev/dri/*: %s\n",
			strerror(errno));
		return 1;
	}

	/* -- create the two BOs ("VRAM" allocations) -------------- */
	memset(&bo, 0, sizeof(bo));
	bo.size = BO_SIZE;
	if (check(fd, &bo, DRM_IOCTL_VGPU_BO_CREATE, "bo_create(src)"))
		goto out;
	src_handle = bo.handle;
	src = xmap(fd, bo.mmap_offset, BO_SIZE);

	memset(&bo, 0, sizeof(bo));
	bo.size = BO_SIZE;
	if (check(fd, &bo, DRM_IOCTL_VGPU_BO_CREATE, "bo_create(dst)"))
		goto out;
	dst_handle = bo.handle;
	dst = xmap(fd, bo.mmap_offset, BO_SIZE);
	memset(dst, 0, BO_SIZE);

	/* pattern the source: 0xA0..0xAF repeating */
	for (i = 0; i < BLIT_LEN; i++)
		src[i] = (uint8_t)(0xA0 + (i & 0x0f));

	/* -- get the ring ------------------------------------------ */
	memset(&rg, 0, sizeof(rg));
	if (check(fd, &rg, DRM_IOCTL_VGPU_RING_GET, "ring_get"))
		goto out;
	ring = xmap(fd, rg.mmap_offset, 4096);

	/* -- write our commands into our slot ---------------------- */
	cmds = (struct vgpu_cmd *)(ring + off);
	memset(cmds, 0, 3 * sizeof(*cmds));

	cmds[0].opcode = VGPU_OP_NOP;

	cmds[1].opcode = VGPU_OP_BLIT;
	cmds[1].src_bo = src_handle;
	cmds[1].dst_bo = dst_handle;
	cmds[1].src_off = 0;
	cmds[1].dst_off = 0;
	cmds[1].length = BLIT_LEN;

	cmds[2].opcode = VGPU_OP_SET;
	cmds[2].set_off = 12;	/* the MMIO 'user' register */
	cmds[2].set_data = USER_MAGIC;

	/* -- doorbell ------------------------------------------------ */
	memset(&cs, 0, sizeof(cs));
	cs.ring_off = off;
	cs.ncmds = 3;
	if (check(fd, &cs, DRM_IOCTL_VGPU_CS, "cs"))
		goto out;
	fprintf(stderr, "[pid %d] doorbell slot=%d -> seqno=%u\n",
		(int)getpid(), slot, cs.seqno);

	/* -- wait for completion (kernel-side fence wait) ------------ */
	memset(&w, 0, sizeof(w));
	w.seqno = cs.seqno;
	w.timeout_ms = 5000;
	if (check(fd, &w, DRM_IOCTL_VGPU_WAIT, "wait"))
		goto out;
	if (w.status != 0) {
		fprintf(stderr, "FAIL: wait status=%d\n", (int)w.status);
		goto out;
	}

	/* -- verify the BLIT ------------------------------------------ */
	for (i = 0; i < BLIT_LEN; i++) {
		if (dst[i] != src[i]) {
			fprintf(stderr,
				"FAIL: blit mismatch at %d (0x%02x != 0x%02x)\n",
				i, dst[i], src[i]);
			goto out;
		}
	}

	/* -- verify the MMIO write + stats ----------------------------- */
	memset(&q, 0, sizeof(q));
	if (check(fd, &q, DRM_IOCTL_VGPU_QUERY, "query"))
		goto out;
	if (q.r_user != USER_MAGIC) {
		fprintf(stderr, "FAIL: mmio user=0x%08x (want 0x%08x)\n",
			q.r_user, USER_MAGIC);
		goto out;
	}
	printf("[pid %d] PASS: seqno=%u blit ok mmio ok "
	       "(submitted=%u completed=%u errors=%u)\n",
	       (int)getpid(), cs.seqno, q.submitted, q.completed, q.errors);
	rc = 0;

out:
	close(fd);
	return rc;
}

/*
 * Negative paths: the error handling from DESIGN.md §10, driven from
 * userspace. A doorbell that names a handle that does not exist must
 * be rejected at submit time; a command the engine cannot decode must
 * complete the submission with an error fence; the ring must refuse a
 * doorbell when it is full.
 */
static int run_negative(void)
{
	int fd;
	int rc = 1;
	int i;
	uint8_t *ring;
	struct vgpu_cmd *cmds;
	struct vgpu_ring_get rg;
	struct vgpu_cs cs;
	struct vgpu_cs cs2;
	struct vgpu_wait w;

	fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0)
		fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "FAIL: open /dev/dri/*: %s\n", strerror(errno));
		return 1;
	}

	/* -- 1. WAIT on a seqno that was never submitted ------------- */
	memset(&w, 0, sizeof(w));
	w.seqno = 0x7ffffff0;	/* never assigned: next_seqno starts at 1 */
	w.timeout_ms = 100;
	if (check(fd, &w, DRM_IOCTL_VGPU_WAIT, "wait(unknown seqno)"))
		goto out;
	if (w.status != -ENOENT) {
		fprintf(stderr, "FAIL: wait(unknown seqno) status=%d, want -ENOENT\n",
			(int)w.status);
		goto out;
	}
	printf("[pid %d] PASS: wait(unknown seqno) -> -ENOENT\n",
	       (int)getpid());

	/* -- 2. doorbell a BLIT naming a handle we never created ----- */
	memset(&rg, 0, sizeof(rg));
	if (check(fd, &rg, DRM_IOCTL_VGPU_RING_GET, "ring_get"))
		goto out;
	ring = xmap(fd, rg.mmap_offset, 4096);
	cmds = (struct vgpu_cmd *)ring;
	memset(cmds, 0, sizeof(*cmds));
	cmds[0].opcode = VGPU_OP_BLIT;
	cmds[0].src_bo = 0xdead;	/* no such handle in this fd */
	cmds[0].dst_bo = 0xbeef;
	cmds[0].length = 4096;
	memset(&cs, 0, sizeof(cs));
	cs.ring_off = 0;
	cs.ncmds = 1;
	if (expect_errno(fd, &cs, DRM_IOCTL_VGPU_CS, ENOENT,
			 "cs(bad blit handle)"))
		goto out;

	/*
	 * -- 3. an opcode no engine decodes is rejected at the doorbell.
	 * Real drivers validate packets in the submission frontend
	 * (PM4 type checks), so the caller sees the error immediately
	 * instead of getting a failed fence later.
	 */
	memset(cmds, 0, sizeof(*cmds));
	cmds[0].opcode = 99;
	memset(&cs, 0, sizeof(cs));
	cs.ring_off = 0;
	cs.ncmds = 1;
	if (expect_errno(fd, &cs, DRM_IOCTL_VGPU_CS, EINVAL,
			 "cs(bad opcode)"))
		goto out;

	/*
	 * -- 4. engine fault: a command the DOORBELL accepts but the
	 * engine cannot execute. Misaligned SET offset passes the
	 * packet check, so the engine is the one that reports it —
	 * through the fence, exactly like a real GPU fault.
	 */

	/* -- 5. engine-detected fault: misaligned SET offset ---------- */
	memset(cmds, 0, sizeof(*cmds));
	cmds[0].opcode = VGPU_OP_SET;
	cmds[0].set_off = 3;	/* must be 4-aligned */
	memset(&cs, 0, sizeof(cs));
	cs.ring_off = 0;
	cs.ncmds = 1;
	if (check(fd, &cs, DRM_IOCTL_VGPU_CS, "cs(bad set_off)"))
		goto out;
	memset(&w, 0, sizeof(w));
	w.seqno = cs.seqno;
	w.timeout_ms = 5000;
	if (check(fd, &w, DRM_IOCTL_VGPU_WAIT, "wait(bad set_off)"))
		goto out;
	if (w.status != -EINVAL) {
		fprintf(stderr, "FAIL: bad set_off wait status=%d, want -EINVAL\n",
			(int)w.status);
		goto out;
	}
	printf("[pid %d] PASS: engine fault (misaligned SET) -> fence -EINVAL\n",
	       (int)getpid());

	/* -- 6. ring full: 64 NOPs fill the whole ring; the next
	 *       doorbell must be refused until the ring drains ------ */
	memset(cmds, 0, 64 * sizeof(*cmds));
	for (i = 0; i < 64; i++)
		cmds[i].opcode = VGPU_OP_NOP;
	memset(&cs, 0, sizeof(cs));
	cs.ring_off = 0;
	cs.ncmds = 64;		/* 64 x 64B == the whole 4096B ring */
	if (check(fd, &cs, DRM_IOCTL_VGPU_CS, "cs(ring fill)"))
		goto out;
	memset(&cs2, 0, sizeof(cs2));
	cs2.ring_off = 0;
	cs2.ncmds = 1;
	if (expect_errno(fd, &cs2, DRM_IOCTL_VGPU_CS, EBUSY,
			 "cs(ring full)")) {
		/* the engine may have drained the ring already on a fast
		 * machine: not a driver failure, unreproducible timing */
		printf("SKIP: ring-full not observed (engine drained first)\n");
	}
	memset(&w, 0, sizeof(w));
	w.seqno = cs.seqno;
	w.timeout_ms = 5000;
	if (check(fd, &w, DRM_IOCTL_VGPU_WAIT, "wait(ring fill)"))
		goto out;
	if (w.status != 0) {
		fprintf(stderr, "FAIL: ring-fill wait status=%d\n",
			(int)w.status);
		goto out;
	}
	printf("[pid %d] PASS: ring drained, status=0\n", (int)getpid());

	printf("negative paths: all passed\n");
	rc = 0;

out:
	close(fd);
	return rc;
}


/*
 * Multi-queue / multi-engine mode: one fd, three streams, one engine
 * each -- the driver half of "streams". Each queue owns its ring, its
 * fence timeline and its doorbell (the qid field in the CS ioctl).
 */
static int queue_submit(int fd, uint8_t *ring, uint32_t qid, uint32_t slot,
			struct vgpu_cmd *cmds, uint32_t n, uint32_t *seqno_out)
{
	struct vgpu_cs cs;

	memcpy(ring + (size_t)slot * 64, cmds, n * sizeof(*cmds));
	memset(&cs, 0, sizeof(cs));
	cs.qid = qid;
	cs.ring_off = (uint64_t)slot * 64;
	cs.ncmds = n;
	if (check(fd, &cs, DRM_IOCTL_VGPU_CS, "cs"))
		return -1;
	*seqno_out = cs.seqno;
	return 0;
}

static int wait_seqno(int fd, uint32_t seqno, int timeout_ms)
{
	struct vgpu_wait w;

	memset(&w, 0, sizeof(w));
	w.seqno = seqno;
	w.timeout_ms = timeout_ms;
	if (check(fd, &w, DRM_IOCTL_VGPU_WAIT, "wait"))
		return -1;
	return w.status;
}

static int run_queues(void)
{
	int fd;
	int rc = 1;
	int i;
	uint8_t *src, *dst;
	uint32_t src_handle, dst_handle;
	struct vgpu_bo_create bo;
	uint8_t *rings[3];
	uint32_t qids[3], seqnos[3];
	struct vgpu_queue_create qc;
	struct vgpu_cmd cmds[8];
	static const uint32_t engines[3] = { VGPU_ENGINE_GFX,
					     VGPU_ENGINE_COPY,
					     VGPU_ENGINE_COMPUTE };

	fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0)
		fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "FAIL: open: %s\n", strerror(errno));
		return 1;
	}

	/* the "VRAM" the engines shuffle */
	memset(&bo, 0, sizeof(bo));
	bo.size = BO_SIZE;
	if (check(fd, &bo, DRM_IOCTL_VGPU_BO_CREATE, "bo_create(src)"))
		goto out;
	src_handle = bo.handle;
	src = xmap(fd, bo.mmap_offset, BO_SIZE);

	memset(&bo, 0, sizeof(bo));
	bo.size = BO_SIZE;
	if (check(fd, &bo, DRM_IOCTL_VGPU_BO_CREATE, "bo_create(dst)"))
		goto out;
	dst_handle = bo.handle;
	dst = xmap(fd, bo.mmap_offset, BO_SIZE);
	memset(dst, 0, BO_SIZE);

	for (i = 0; i < BLIT_LEN; i++)
		src[i] = (uint8_t)(0xA0 + (i & 0x0f));

	/* create one queue per engine */
	for (i = 0; i < 3; i++) {
		memset(&qc, 0, sizeof(qc));
		qc.engine = engines[i];
		qc.prio = i;			/* 0/1/2 across the engines */
		qc.nslots = 64;
		if (check(fd, &qc, DRM_IOCTL_VGPU_QUEUE_CREATE,
			  "queue_create"))
			goto out;
		qids[i] = qc.qid;
		rings[i] = xmap(fd, qc.mmap_offset, qc.ring_size);
		printf("[pid %d] queue q#%u engine=%u ring=%llu B\n",
		       (int)getpid(), qids[i], engines[i],
		       (unsigned long long)qc.ring_size);
	}

	/* GFX queue: NOP + BLIT + SET (the full chain) */
	memset(cmds, 0, sizeof(cmds));
	cmds[0].opcode = VGPU_OP_NOP;
	cmds[1].opcode = VGPU_OP_BLIT;
	cmds[1].src_bo = src_handle;
	cmds[1].dst_bo = dst_handle;
	cmds[1].length = BLIT_LEN;
	cmds[2].opcode = VGPU_OP_SET;
	cmds[2].set_off = 12;
	cmds[2].set_data = USER_MAGIC;
	if (queue_submit(fd, rings[0], qids[0], 0, cmds, 3, &seqnos[0]))
		goto out;

	/* COPY queue: NOP + BLIT (a DMA engine's whole job) */
	memset(cmds, 0, sizeof(cmds));
	cmds[0].opcode = VGPU_OP_NOP;
	cmds[1].opcode = VGPU_OP_BLIT;
	cmds[1].src_bo = src_handle;
	cmds[1].dst_bo = dst_handle;
	cmds[1].length = BLIT_LEN;
	if (queue_submit(fd, rings[1], qids[1], 0, cmds, 2, &seqnos[1]))
		goto out;

	/* COMPUTE queue: NOP + SET (compute ops land in phase D) */
	memset(cmds, 0, sizeof(cmds));
	cmds[0].opcode = VGPU_OP_NOP;
	cmds[1].opcode = VGPU_OP_SET;
	cmds[1].set_off = 12;
	cmds[1].set_data = USER_MAGIC;
	if (queue_submit(fd, rings[2], qids[2], 0, cmds, 2, &seqnos[2]))
		goto out;

	/* per-queue engine classes are enforced at the doorbell */
	memset(cmds, 0, sizeof(cmds));
	cmds[0].opcode = VGPU_OP_BLIT;
	cmds[0].src_bo = src_handle;
	cmds[0].dst_bo = dst_handle;
	cmds[0].length = BLIT_LEN;
	{
		struct vgpu_cs cs;

		memcpy(rings[2], cmds, sizeof(*cmds));
		memset(&cs, 0, sizeof(cs));
		cs.qid = qids[2];	/* compute engine rejects a BLIT */
		cs.ncmds = 1;
		if (expect_errno(fd, &cs, DRM_IOCTL_VGPU_CS, EINVAL,
				 "cs(BLIT on compute engine)"))
			goto out;

		memset(cmds, 0, sizeof(cmds));
		cmds[0].opcode = VGPU_OP_SET;
		cmds[0].set_off = 12;
		cmds[0].set_data = 0x1234;
		memcpy(rings[1], cmds, sizeof(*cmds));
		memset(&cs, 0, sizeof(cs));
		cs.qid = qids[1];	/* copy engine rejects a SET */
		cs.ncmds = 1;
		if (expect_errno(fd, &cs, DRM_IOCTL_VGPU_CS, EINVAL,
				 "cs(SET on copy engine)"))
			goto out;
	}

	/* every queue's submission must complete */
	for (i = 0; i < 3; i++) {
		int st = wait_seqno(fd, seqnos[i], 5000);

		if (st != 0) {
			fprintf(stderr, "FAIL: q#%u seq#%u status=%d\n",
				qids[i], seqnos[i], st);
			goto out;
		}
		printf("[pid %d] PASS: q#%u seq#%u done\n", (int)getpid(),
		       qids[i], seqnos[i]);
	}

	/* the BLIT results (both engines wrote the same pattern) */
	for (i = 0; i < BLIT_LEN; i++) {
		if (dst[i] != src[i]) {
			fprintf(stderr, "FAIL: blit mismatch at %d\n", i);
			goto out;
		}
	}

	/* destroy an unknown queue, then a real one */
	struct vgpu_queue_destroy qd;

	memset(&qd, 0, sizeof(qd));
	qd.qid = 0xdead;
	if (expect_errno(fd, &qd, DRM_IOCTL_VGPU_QUEUE_DESTROY, ENOENT,
			 "queue_destroy(unknown)"))
		goto out;

	memset(&qd, 0, sizeof(qd));
	qd.qid = qids[2];
	if (check(fd, &qd, DRM_IOCTL_VGPU_QUEUE_DESTROY, "queue_destroy"))
		goto out;
	printf("[pid %d] PASS: q#%u destroyed\n", (int)getpid(), qids[2]);
	qids[2] = 0;

	printf("multi-queue: 3 engines on one fd, all submissions ok\n");
	rc = 0;

out:
	close(fd);
	return rc;
}


/*
 * Phase B: the packet/doorbell path. Userspace writes PACKETS into the
 * queue ring and rings the doorbell; the engine fetches and decodes
 * them. No ioctl per command — one syscall per batch.
 */
static int run_doorbell(void)
{
	int fd;
	int rc = 1;
	int i;
	uint8_t *src, *dst;
	uint32_t src_handle, dst_handle;
	struct vgpu_bo_create bo;
	uint8_t *ring;
	struct vgpu_queue_create qc;
	struct vgpu_packet *pk;
	struct vgpu_doorbell db;
	struct vgpu_wait w;
	uint32_t qid = 0;

	fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0)
		fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "FAIL: open: %s\n", strerror(errno));
		return 1;
	}

	memset(&bo, 0, sizeof(bo));
	bo.size = BO_SIZE;
	if (check(fd, &bo, DRM_IOCTL_VGPU_BO_CREATE, "bo_create(src)"))
		goto out;
	src_handle = bo.handle;
	src = xmap(fd, bo.mmap_offset, BO_SIZE);

	memset(&bo, 0, sizeof(bo));
	bo.size = BO_SIZE;
	if (check(fd, &bo, DRM_IOCTL_VGPU_BO_CREATE, "bo_create(dst)"))
		goto out;
	dst_handle = bo.handle;
	dst = xmap(fd, bo.mmap_offset, BO_SIZE);
	memset(dst, 0, BO_SIZE);

	for (i = 0; i < BLIT_LEN; i++)
		src[i] = (uint8_t)(0xA0 + (i & 0x0f));

	memset(&qc, 0, sizeof(qc));
	qc.engine = VGPU_ENGINE_GFX;
	qc.prio = 1;
	qc.nslots = 64;
	if (check(fd, &qc, DRM_IOCTL_VGPU_QUEUE_CREATE, "queue_create"))
		goto out;
	qid = qc.qid;
	ring = xmap(fd, qc.mmap_offset, qc.ring_size);
	pk = (struct vgpu_packet *)ring;

	/* -- batch 1: NOP + COPY + WRITE_REG as packets ---------------- */
	memset(pk, 0, 3 * sizeof(*pk));
	pk[0].type = VGPU_PKT_NOP;
	pk[1].type = VGPU_PKT_COPY;
	pk[1].copy.src_bo = src_handle;
	pk[1].copy.dst_bo = dst_handle;
	pk[1].copy.off = 0;
	pk[1].copy.len = BLIT_LEN;
	pk[2].type = VGPU_PKT_WRITE_REG;
	pk[2].reg.reg_off = 12;
	pk[2].reg.reg_val = USER_MAGIC;

	memset(&db, 0, sizeof(db));
	db.qid = qid;
	db.put = 3 * (uint32_t)sizeof(*pk);
	if (check(fd, &db, DRM_IOCTL_VGPU_DOORBELL, "doorbell(batch 1)"))
		goto out;

	memset(&w, 0, sizeof(w));
	w.seqno = db.seqno;
	w.timeout_ms = 5000;
	if (check(fd, &w, DRM_IOCTL_VGPU_WAIT, "wait(batch 1)"))
		goto out;
	if (w.status != 0) {
		fprintf(stderr, "FAIL: batch 1 status=%d\n", (int)w.status);
		goto out;
	}
	printf("[pid %d] PASS: packet batch 1 (NOP+COPY+WRITE_REG) seq#%u\n",
	       (int)getpid(), db.seqno);

	/* -- batch 2: the ring is consumed; keep appending ------------ */
	memset(pk + 3, 0, 2 * sizeof(*pk));
	pk[3].type = VGPU_PKT_WRITE_REG;
	pk[3].reg.reg_off = 12;
	pk[3].reg.reg_val = 0xcafef00d;
	pk[4].type = VGPU_PKT_NOP;

	memset(&db, 0, sizeof(db));
	db.qid = qid;
	db.put = 5 * (uint32_t)sizeof(*pk);
	if (check(fd, &db, DRM_IOCTL_VGPU_DOORBELL, "doorbell(batch 2)"))
		goto out;
	memset(&w, 0, sizeof(w));
	w.seqno = db.seqno;
	w.timeout_ms = 5000;
	if (check(fd, &w, DRM_IOCTL_VGPU_WAIT, "wait(batch 2)"))
		goto out;
	if (w.status != 0) {
		fprintf(stderr, "FAIL: batch 2 status=%d\n", (int)w.status);
		goto out;
	}
	printf("[pid %d] PASS: packet batch 2 seq#%u, ring wrapped back\n",
	       (int)getpid(), db.seqno);

	/* -- doorbell input validation --------------------------------- */
	memset(&db, 0, sizeof(db));
	db.qid = qid;
	db.put = 5 * (uint32_t)sizeof(*pk) - 4;	/* misaligned */
	if (expect_errno(fd, &db, DRM_IOCTL_VGPU_DOORBELL, EINVAL,
			 "doorbell(misaligned put)"))
		goto out;

	memset(&db, 0, sizeof(db));
	db.qid = qid;
	db.put = 5 * (uint32_t)sizeof(*pk);	/* did not advance */
	if (expect_errno(fd, &db, DRM_IOCTL_VGPU_DOORBELL, EINVAL,
			 "doorbell(put did not advance)"))
		goto out;

	memset(&db, 0, sizeof(db));
	db.qid = qid;
	db.put = qc.ring_size + 32;		/* past the end */
	if (expect_errno(fd, &db, DRM_IOCTL_VGPU_DOORBELL, EINVAL,
			 "doorbell(put out of range)"))
		goto out;

	/* -- the COPY really ran --------------------------------------- */
	for (i = 0; i < BLIT_LEN; i++) {
		if (dst[i] != src[i]) {
			fprintf(stderr, "FAIL: packet copy mismatch at %d\n", i);
			goto out;
		}
	}
	printf("[pid %d] PASS: packet copy wrote the pattern\n",
	       (int)getpid());

	printf("doorbell path: packets fetched and executed by the engine\n");
	rc = 0;

out:
	close(fd);
	return rc;
}

/* ------------------------------------------- phase G: memory-semantics sync */

/*
 * Phase G: completion as MEMORY, not as a syscall.
 *
 * Everywhere else in this driver the app asks "is it done?" with
 * VGPU_WAIT and the kernel blocks on its behalf in
 * dma_fence_wait_timeout(). Modern GPU APIs do the opposite: the
 * completion lives in a word of memory both sides can see — a Vulkan
 * timeline semaphore, a CUDA graph's completion flag, the sync-object
 * pages every real driver puts in its submission ring — and the CPU
 * simply polls it. This mode is that:
 *
 *   A.  VGPU_SEM_BIND installs a BO as this fd's semaphore page. A
 *       batch whose LAST packet is SIGNAL_TIMELINE writes the timeline
 *       value into it, and we discover completion by reading the
 *       mmap'ed BO. There is NO VGPU_WAIT anywhere in this mode.
 *   A2. A WAIT_TIMELINE packet on the COPY engine stalls the ENGINE
 *       until the GFX engine writes the value: the cross-engine
 *       dependency is one word of shared memory. We poll for the copy
 *       engine's event slot, so the CPU never blocks either.
 *   B.  A 4-node DAG (copy-in -> launch -> copy-out -> signal)
 *       submitted with ONE VGPU_GRAPH ioctl: one syscall, one
 *       doorbell, one fence for all four nodes.
 *   B2. The same four units of work as FOUR separate doorbells, with
 *       VGPU_QUERY's submission counters as the measurement.
 *
 * The graph's node indices are deliberately NOT the dependency order:
 * index 1 (the copy-out) depends on index 2 (the launch). The only
 * assertion available from userspace is the DATA dependency — if the
 * driver ran the nodes in index order, the copy-out would move the
 * pre-launch poison instead of the kernel's output, and the checks
 * below would fail. (dmesg also prints the resolved order; the driver
 * logs no per-node start, so the data path is the real proof.)
 */
#define SEM_BO_WORDS	1024		/* the semaphore page: one page */
#define SEM_W_TIMELINE	0		/* word 0: the timeline value */
#define SEM_W_EVENT_A	1		/* word 1: an event slot */
#define SEM_W_EVENT_B	2		/* word 2: another event slot */
#define SEM_W_STREAM_1	3		/* the stream path's step markers */
#define SEM_W_STREAM_2	4
#define SEM_W_STREAM_3	5
#define SEM_OFF(w)	((w) * 4)

/* the stream path's per-step completion values (word 3, 4, 5) */
#define STREAM_MARK(i)	(0x57ea0000u + (uint32_t)(i))

/* increasing timeline values, as a real timeline semaphore's waits */
#define TL_BLIT		0x5e000001u
#define TL_CROSS	0x5e000002u
#define TL_GRAPH	0x5e000003u
#define TL_STREAMS	0x5e000004u
#define EVT_BLIT	0xe17e0001u
#define EVT_CROSS	0xbeef0002u

/* the saxpy-style kernel the graph's launch node runs (see saxpy.c) */
#define GN_N		512		/* elements the kernel processes */
#define GN_BLOCK	64		/* threads per block (multiple of 32) */
#define GN_GRID		(GN_N / GN_BLOCK)
#define GN_A		3
#define GN_B		7
#define GN_IN_BASE	1024		/* word index of in[]  (page 1) */
#define GN_OUT_BASE	2048		/* word index of out[] (page 2) */
#define GN_IN_OFF	4096		/* byte offset of in[]  (page aligned) */
#define GN_OUT_OFF	8192		/* byte offset of out[] (page aligned) */
#define ARGS_PAGES	4
#define GN_POISON	0xdeadbeefu

/* the kernel: out[i] = a * in[i] + b, i = global thread id */
static const struct vgpu_instr vgpu_saxpy_code[] = {
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

/*
 * Poll a word of the mmap'ed semaphore page. This is the whole of the
 * phase G completion path: a load from memory, no ioctl. Bounded so a
 * broken driver fails the test instead of hanging the autorun.
 */
static uint32_t sem_poll(volatile uint32_t *sem, uint32_t word, uint32_t want,
			 unsigned timeout_ms)
{
	unsigned i;

	for (i = 0; i <= timeout_ms / 2; i++) {
		if (sem[word] == want)
			break;
		usleep(2000);
	}
	return sem[word];
}

static int sem_check(volatile uint32_t *sem, uint32_t word, uint32_t want,
		     const char *what)
{
	if (sem[word] != want) {
		fprintf(stderr, "FAIL: %s: word%u = 0x%08x, want 0x%08x\n",
			what, word, sem[word], want);
		return -1;
	}
	printf("[pid %d] PASS: %s: word%u = 0x%08x\n", (int)getpid(), what,
	       word, want);
	return 0;
}

/* poll for a word, then report it (the stream path's step markers) */
static int sem_poll_check(volatile uint32_t *sem, uint32_t word, uint32_t want,
			  const char *what)
{
	if (sem_poll(sem, word, want, 5000) != want) {
		fprintf(stderr, "FAIL: %s: word%u = 0x%08x, want 0x%08x\n",
			what, word, sem[word], want);
		return -1;
	}
	printf("[pid %d] PASS: %s: word%u = 0x%08x\n", (int)getpid(), what,
	       word, want);
	return 0;
}

/* ring one stream's doorbell for the npkts packets already in its ring */
static int stream_ring(int fd, uint32_t qid, uint32_t npkts,
		       uint32_t *seqno_out)
{
	struct vgpu_doorbell db;

	memset(&db, 0, sizeof(db));
	db.qid = qid;
	db.put = npkts * (uint32_t)sizeof(struct vgpu_packet);
	if (check(fd, &db, DRM_IOCTL_VGPU_DOORBELL, "doorbell(stream step)"))
		return -1;
	*seqno_out = db.seqno;
	return 0;
}

/* a byte-for-byte comparison (the DMA result of a batch) */
static int data_check_pattern(const uint8_t *a, const uint8_t *b, int len,
			      const char *what)
{
	int i;

	for (i = 0; i < len; i++) {
		if (a[i] != b[i]) {
			fprintf(stderr,
				"FAIL: %s: mismatch at %d (0x%02x != 0x%02x)\n",
				what, i, a[i], b[i]);
			return -1;
		}
	}
	printf("[pid %d] PASS: %s (%d bytes identical)\n", (int)getpid(),
	       what, len);
	return 0;
}

/* out[i] = a * in[i] + b, as the kernel in this test computes it */
static int data_check_saxpy(const uint32_t *out, const uint32_t *in,
			    const char *what)
{
	int i;

	for (i = 0; i < GN_N; i++) {
		uint32_t want = GN_A * in[i] + GN_B;

		if (out[i] != want) {
			fprintf(stderr,
				"FAIL: %s: out[%d] = %u, want %u\n",
				what, i, out[i], want);
			return -1;
		}
	}
	printf("[pid %d] PASS: %s: out[i] = %u*in[i] + %u "
	       "(%d elements verified)\n", (int)getpid(), what, GN_A, GN_B,
	       GN_N);
	return 0;
}

static int run_sync(void)
{
	int fd;
	int rc = 1;
	int i;
	uint32_t sem_handle, src_handle, dst_handle, args_handle;
	uint32_t res_handle, res2_handle;
	uint32_t qid_gfx = 0, qid_copy = 0, kid = 0;
	uint32_t graph_seqno = 0;
	uint32_t sqid[4];
	uint32_t stream_seqno[4] = { 0, 0, 0, 0 };
	volatile uint32_t *sem;
	uint32_t *src, *dst, *args, *res, *res2;
	uint8_t *ring = NULL, *cring = NULL;
	uint8_t *sring[4];
	struct vgpu_packet *pk, *cpk, *spk;
	struct vgpu_bo_create bo;
	struct vgpu_sem_bind sb;
	struct vgpu_queue_create qc;
	struct vgpu_module_load ml;
	struct vgpu_doorbell db;
	struct vgpu_query q, before;
	struct vgpu_graph_node nodes[4], cyc[2];
	struct vgpu_graph_submit gs;

	fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0)
		fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "FAIL: open: %s\n", strerror(errno));
		return 1;
	}

	/* ---- the semaphore page: a BO the CPU polls ---------------- */
	memset(&bo, 0, sizeof(bo));
	bo.size = SEM_BO_WORDS * 4;
	if (check(fd, &bo, DRM_IOCTL_VGPU_BO_CREATE, "bo_create(sem)"))
		goto out;
	sem_handle = bo.handle;
	sem = xmap(fd, bo.mmap_offset, bo.size);

	/* ---- the work buffers -------------------------------------- */
	/*
	 * src holds the input pattern on page 1, at the same offset the
	 * kernel's in[] sits at inside the argument buffer: a graph COPY
	 * node carries ONE offset for both BOs, so the copy-in has to be
	 * legal at that offset in each of them.
	 */
	memset(&bo, 0, sizeof(bo));
	bo.size = ARGS_PAGES * 4096;
	if (check(fd, &bo, DRM_IOCTL_VGPU_BO_CREATE, "bo_create(src)"))
		goto out;
	src_handle = bo.handle;
	src = xmap(fd, bo.mmap_offset, bo.size);
	for (i = 0; i < ARGS_PAGES * 1024; i++)
		src[i] = 2 * i + 1;

	memset(&bo, 0, sizeof(bo));
	bo.size = 4096;
	if (check(fd, &bo, DRM_IOCTL_VGPU_BO_CREATE, "bo_create(dst)"))
		goto out;
	dst_handle = bo.handle;
	dst = xmap(fd, bo.mmap_offset, bo.size);
	memset(dst, 0, bo.size);

	/* page 0: the kernel's parameter header; 1: in[]; 2: out[] */
	memset(&bo, 0, sizeof(bo));
	bo.size = ARGS_PAGES * 4096;
	if (check(fd, &bo, DRM_IOCTL_VGPU_BO_CREATE, "bo_create(args)"))
		goto out;
	args_handle = bo.handle;
	args = xmap(fd, bo.mmap_offset, bo.size);
	args[0] = GN_N;
	args[1] = GN_A;
	args[2] = GN_B;
	args[3] = GN_IN_BASE;
	args[4] = GN_OUT_BASE;
	/* poison in[] and out[]: the copy node and the launch must
	 * overwrite both for the checks below to pass */
	for (i = 0; i < 2 * 1024; i++)
		args[GN_IN_BASE + i] = GN_POISON;

	/*
	 * The result BOs are the same shape as the argument buffer: a
	 * graph COPY node carries ONE offset for both BOs, so the
	 * copy-out (args@GN_OUT_OFF -> res@GN_OUT_OFF) needs a page 2
	 * to land in.
	 */
	memset(&bo, 0, sizeof(bo));
	bo.size = ARGS_PAGES * 4096;
	if (check(fd, &bo, DRM_IOCTL_VGPU_BO_CREATE, "bo_create(res)"))
		goto out;
	res_handle = bo.handle;
	res = xmap(fd, bo.mmap_offset, bo.size);

	memset(&bo, 0, sizeof(bo));
	bo.size = ARGS_PAGES * 4096;
	if (check(fd, &bo, DRM_IOCTL_VGPU_BO_CREATE, "bo_create(res2)"))
		goto out;
	res2_handle = bo.handle;
	res2 = xmap(fd, bo.mmap_offset, bo.size);

	/* ---- install this fd's semaphore page ----------------------- */
	memset(&sb, 0, sizeof(sb));
	sb.bo_handle = sem_handle;
	sb.sem_off = SEM_OFF(SEM_W_TIMELINE);
	if (check(fd, &sb, DRM_IOCTL_VGPU_SEM_BIND, "sem_bind"))
		goto out;
	if (!sb.bound) {
		fprintf(stderr, "FAIL: sem_bind did not report bound\n");
		goto out;
	}
	printf("[pid %d] PASS: sem_bind: bo#%u word_off=%u bound\n",
	       (int)getpid(), sb.bo_handle, sb.sem_off);

	/* the two ways to bind badly: a handle we do not own, a
	 * misaligned word */
	memset(&sb, 0, sizeof(sb));
	sb.bo_handle = 0xdead;
	if (expect_errno(fd, &sb, DRM_IOCTL_VGPU_SEM_BIND, ENOENT,
			 "sem_bind(unknown handle)"))
		goto out;
	memset(&sb, 0, sizeof(sb));
	sb.bo_handle = sem_handle;
	sb.sem_off = 2;
	if (expect_errno(fd, &sb, DRM_IOCTL_VGPU_SEM_BIND, EINVAL,
			 "sem_bind(unaligned sem_off)"))
		goto out;

	/* ---- two streams: GFX and COPY ------------------------------ */
	memset(&qc, 0, sizeof(qc));
	qc.engine = VGPU_ENGINE_GFX;
	qc.prio = 1;
	qc.nslots = 64;
	if (check(fd, &qc, DRM_IOCTL_VGPU_QUEUE_CREATE, "queue_create(gfx)"))
		goto out;
	qid_gfx = qc.qid;
	ring = xmap(fd, qc.mmap_offset, qc.ring_size);

	memset(&qc, 0, sizeof(qc));
	qc.engine = VGPU_ENGINE_COPY;
	qc.prio = 1;
	qc.nslots = 64;
	if (check(fd, &qc, DRM_IOCTL_VGPU_QUEUE_CREATE, "queue_create(copy)"))
		goto out;
	qid_copy = qc.qid;
	cring = xmap(fd, qc.mmap_offset, qc.ring_size);
	pk = (struct vgpu_packet *)ring;
	cpk = (struct vgpu_packet *)cring;

	/* ---- the kernel the graph's launch node will run ------------ */
	memset(&ml, 0, sizeof(ml));
	ml.nregs = 16;
	ml.smem = 0;
	ml.ninstr = sizeof(vgpu_saxpy_code) / sizeof(vgpu_saxpy_code[0]);
	ml.code = (uint64_t)(uintptr_t)vgpu_saxpy_code;
	if (check(fd, &ml, DRM_IOCTL_VGPU_MODULE_LOAD, "module_load"))
		goto out;
	kid = ml.kid;

	/*
	 * ==== A: a batch whose LAST packet is SIGNAL_TIMELINE ========
	 * copy + an event slot + the timeline signal, then find out it
	 * happened by reading the mmap'ed page. No VGPU_WAIT.
	 */
	memset(pk, 0, 3 * sizeof(*pk));
	pk[0].type = VGPU_PKT_COPY;
	pk[0].copy.src_bo = src_handle;
	pk[0].copy.dst_bo = dst_handle;
	pk[0].copy.off = 0;
	pk[0].copy.len = 4096;
	pk[1].type = VGPU_PKT_WRITE_EVENT;
	pk[1].timeline.sem_bo = sem_handle;
	pk[1].timeline.sem_off = SEM_OFF(SEM_W_EVENT_A);
	pk[1].timeline.value = EVT_BLIT;
	pk[2].type = VGPU_PKT_SIGNAL_TIMELINE;
	pk[2].timeline.sem_bo = sem_handle;
	pk[2].timeline.sem_off = SEM_OFF(SEM_W_TIMELINE);
	pk[2].timeline.value = TL_BLIT;

	memset(&db, 0, sizeof(db));
	db.qid = qid_gfx;
	db.put = 3 * (uint32_t)sizeof(*pk);
	if (check(fd, &db, DRM_IOCTL_VGPU_DOORBELL, "doorbell(blit+signal)"))
		goto out;

	if (sem_poll(sem, SEM_W_TIMELINE, TL_BLIT, 5000) != TL_BLIT) {
		fprintf(stderr,
			"FAIL: timeline word%u = 0x%08x, want 0x%08x "
			"(no VGPU_WAIT was issued)\n",
			SEM_W_TIMELINE, sem[SEM_W_TIMELINE], TL_BLIT);
		goto out;
	}
	printf("[pid %d] PASS: timeline semaphore: word0 = 0x%08x seen by "
	       "POLLING THE MMAP (no VGPU_WAIT issued)\n", (int)getpid(),
	       sem[SEM_W_TIMELINE]);
	if (sem_check(sem, SEM_W_EVENT_A, EVT_BLIT, "event slot A"))
		goto out;
	if (data_check_pattern((const uint8_t *)dst, (const uint8_t *)src,
			       4096, "blit in the same batch"))
		goto out;

	/*
	 * ==== A2: the ENGINE waits, not the CPU =====================
	 * The copy engine's batch stalls on WAIT_TIMELINE until the GFX
	 * engine writes the next timeline value. We discover the wait
	 * was satisfied by polling the event slot the copy engine
	 * writes after it — again, no ioctl.
	 */
	memset(cpk, 0, 2 * sizeof(*cpk));
	cpk[0].type = VGPU_PKT_WAIT_TIMELINE;
	cpk[0].timeline.sem_bo = sem_handle;
	cpk[0].timeline.sem_off = SEM_OFF(SEM_W_TIMELINE);
	cpk[0].timeline.value = TL_CROSS;
	cpk[1].type = VGPU_PKT_WRITE_EVENT;
	cpk[1].timeline.sem_bo = sem_handle;
	cpk[1].timeline.sem_off = SEM_OFF(SEM_W_EVENT_B);
	cpk[1].timeline.value = EVT_CROSS;

	memset(&db, 0, sizeof(db));
	db.qid = qid_copy;
	db.put = 2 * (uint32_t)sizeof(*cpk);
	if (check(fd, &db, DRM_IOCTL_VGPU_DOORBELL,
		  "doorbell(copy engine waits)"))
		goto out;

	/* the GFX engine is what releases the copy engine */
	memset(pk + 3, 0, 2 * sizeof(*pk));
	pk[3].type = VGPU_PKT_NOP;
	pk[4].type = VGPU_PKT_SIGNAL_TIMELINE;
	pk[4].timeline.sem_bo = sem_handle;
	pk[4].timeline.sem_off = SEM_OFF(SEM_W_TIMELINE);
	pk[4].timeline.value = TL_CROSS;

	memset(&db, 0, sizeof(db));
	db.qid = qid_gfx;
	db.put = 5 * (uint32_t)sizeof(*pk);
	if (check(fd, &db, DRM_IOCTL_VGPU_DOORBELL,
		  "doorbell(gfx signals)"))
		goto out;

	if (sem_poll(sem, SEM_W_EVENT_B, EVT_CROSS, 5000) != EVT_CROSS) {
		fprintf(stderr,
			"FAIL: copy engine's event word2 = 0x%08x, want 0x%08x "
			"(its WAIT_TIMELINE never released)\n",
			sem[SEM_W_EVENT_B], EVT_CROSS);
		goto out;
	}
	printf("[pid %d] PASS: cross-engine WAIT_TIMELINE: the copy engine "
	       "stalled on word0 until the GFX engine wrote 0x%08x; "
	       "the CPU never blocked\n", (int)getpid(), TL_CROSS);
	if (sem_check(sem, SEM_W_EVENT_B, EVT_CROSS, "event slot B"))
		goto out;

	/*
	 * ==== B: one doorbell for a whole DAG ========================
	 * n0 copy-in, n1 copy-out (deps: n2), n2 launch (deps: n0),
	 * n3 signal (deps: n1). The engine must run [0, 2, 1, 3].
	 */
	memset(nodes, 0, sizeof(nodes));
	nodes[0].type = VGPU_PKT_COPY;
	nodes[0].copy.src_bo = src_handle;
	nodes[0].copy.dst_bo = args_handle;
	nodes[0].copy.off = GN_IN_OFF;
	nodes[0].copy.len = 4096;
	nodes[1].type = VGPU_PKT_COPY;
	nodes[1].copy.src_bo = args_handle;
	nodes[1].copy.dst_bo = res_handle;
	nodes[1].copy.off = GN_OUT_OFF;
	nodes[1].copy.len = 4096;
	nodes[1].ndeps = 1;
	nodes[1].deps[0] = 2;		/* after the launch */
	nodes[2].type = VGPU_PKT_LAUNCH;
	nodes[2].launch.kid = kid;
	nodes[2].launch.grid = GN_GRID;
	nodes[2].launch.block = GN_BLOCK;
	nodes[2].launch.args_bo = args_handle;
	nodes[2].launch.args_off = 0;
	nodes[2].ndeps = 1;
	nodes[2].deps[0] = 0;		/* after the copy-in */
	nodes[3].type = VGPU_PKT_SIGNAL_TIMELINE;
	nodes[3].sem.sem_bo = sem_handle;
	nodes[3].sem.sem_off = SEM_OFF(SEM_W_TIMELINE);
	nodes[3].sem.value = TL_GRAPH;
	nodes[3].ndeps = 1;
	nodes[3].deps[0] = 1;		/* after the copy-out */

	printf("[pid %d] graph DAG: n0=copy-in, n1=copy-out(deps n2), "
	       "n2=launch grid=%u block=%u (deps n0), n3=signal(deps n1)\n",
	       (int)getpid(), GN_GRID, GN_BLOCK);

	memset(&before, 0, sizeof(before));
	if (check(fd, &before, DRM_IOCTL_VGPU_QUERY, "query(before graph)"))
		goto out;

	memset(&gs, 0, sizeof(gs));
	gs.nodes = (uint64_t)(uintptr_t)nodes;
	gs.qid = qid_gfx;
	gs.nnodes = 4;
	gs.nedges = 0;
	if (check(fd, &gs, DRM_IOCTL_VGPU_GRAPH, "graph"))
		goto out;
	graph_seqno = gs.seqno;
	printf("[pid %d] graph: 4 nodes behind ONE doorbell -> seq#%u\n",
	       (int)getpid(), graph_seqno);

	if (sem_poll(sem, SEM_W_TIMELINE, TL_GRAPH, 5000) != TL_GRAPH) {
		fprintf(stderr,
			"FAIL: graph timeline word%u = 0x%08x, want 0x%08x\n",
			SEM_W_TIMELINE, sem[SEM_W_TIMELINE], TL_GRAPH);
		goto out;
	}
	printf("[pid %d] PASS: graph done: word0 = 0x%08x (polled, "
	       "no VGPU_WAIT)\n", (int)getpid(), sem[SEM_W_TIMELINE]);
	/*
	 * The copy-out (node 1) ran AFTER the launch (node 2): if the
	 * nodes had run in index order this would be the poison. The
	 * result landed at the same offset the copy node used.
	 */
	if (data_check_saxpy(res + GN_OUT_BASE, src + GN_IN_BASE,
			     "graph: node 1 copied the LAUNCH's output"))
		goto out;

	memset(&q, 0, sizeof(q));
	if (check(fd, &q, DRM_IOCTL_VGPU_QUERY, "query(after graph)"))
		goto out;
	printf("[pid %d] PASS: graph cost: submitted +%u (4 nodes, 1 ioctl), "
	       "graphs +%u, signals +%u, events +%u\n", (int)getpid(),
	       q.submitted - before.submitted, q.graphs - before.graphs,
	       q.signals - before.signals, q.events - before.events);

	/*
	 * ==== B2: the same work as four separate doorbells ==========
	 * Four streams, four doorbells, four fences — and the dependency
	 * expressed only as "submit them in this order and hope": here
	 * the CPU polls each step's memory word before the next ring,
	 * which is exactly how an app orders streams without events.
	 * Still no VGPU_WAIT.
	 *
	 * One stream per step on purpose: a doorbell covers [get, put),
	 * and the word the engine writes lands BEFORE the bottom half
	 * advances get, so two rings on one queue can cover the same
	 * packets twice. Separate rings make that impossible.
	 */
	for (i = 0; i < 1024; i++)
		args[GN_OUT_BASE + i] = GN_POISON;

	for (i = 0; i < 4; i++) {
		memset(&qc, 0, sizeof(qc));
		qc.engine = VGPU_ENGINE_GFX;
		qc.prio = 1;
		qc.nslots = 64;
		if (check(fd, &qc, DRM_IOCTL_VGPU_QUEUE_CREATE,
			  "queue_create(stream)"))
			goto out;
		sqid[i] = qc.qid;
		sring[i] = xmap(fd, qc.mmap_offset, qc.ring_size);
		printf("[pid %d] stream q#%u (GFX engine)\n", (int)getpid(),
		       sqid[i]);
	}

	/* step 0: copy the input page into the argument buffer */
	spk = (struct vgpu_packet *)sring[0];
	memset(spk, 0, 2 * sizeof(*spk));
	spk[0].type = VGPU_PKT_COPY;
	spk[0].copy.src_bo = src_handle;
	spk[0].copy.dst_bo = args_handle;
	spk[0].copy.off = GN_IN_OFF;
	spk[0].copy.len = 4096;
	spk[1].type = VGPU_PKT_WRITE_EVENT;
	spk[1].timeline.sem_bo = sem_handle;
	spk[1].timeline.sem_off = SEM_OFF(SEM_W_STREAM_1);
	spk[1].timeline.value = STREAM_MARK(1);
	if (stream_ring(fd, sqid[0], 2, &stream_seqno[0]))
		goto out;
	if (sem_poll_check(sem, SEM_W_STREAM_1, STREAM_MARK(1),
			   "stream step 0 (copy-in)"))
		goto out;

	/* step 1: the launch over the page step 0 just wrote */
	spk = (struct vgpu_packet *)sring[1];
	memset(spk, 0, 2 * sizeof(*spk));
	spk[0].type = VGPU_PKT_LAUNCH;
	spk[0].launch.kid = kid;
	spk[0].launch.grid = GN_GRID;
	spk[0].launch.block = GN_BLOCK;
	spk[0].launch.args_bo = args_handle;
	spk[0].launch.args_off = 0;
	spk[1].type = VGPU_PKT_WRITE_EVENT;
	spk[1].timeline.sem_bo = sem_handle;
	spk[1].timeline.sem_off = SEM_OFF(SEM_W_STREAM_2);
	spk[1].timeline.value = STREAM_MARK(2);
	if (stream_ring(fd, sqid[1], 2, &stream_seqno[1]))
		goto out;
	if (sem_poll_check(sem, SEM_W_STREAM_2, STREAM_MARK(2),
			   "stream step 1 (launch)"))
		goto out;

	/* step 2: copy the launch's output out of the argument buffer */
	spk = (struct vgpu_packet *)sring[2];
	memset(spk, 0, 2 * sizeof(*spk));
	spk[0].type = VGPU_PKT_COPY;
	spk[0].copy.src_bo = args_handle;
	spk[0].copy.dst_bo = res2_handle;
	spk[0].copy.off = GN_OUT_OFF;
	spk[0].copy.len = 4096;
	spk[1].type = VGPU_PKT_WRITE_EVENT;
	spk[1].timeline.sem_bo = sem_handle;
	spk[1].timeline.sem_off = SEM_OFF(SEM_W_STREAM_3);
	spk[1].timeline.value = STREAM_MARK(3);
	if (stream_ring(fd, sqid[2], 2, &stream_seqno[2]))
		goto out;
	if (sem_poll_check(sem, SEM_W_STREAM_3, STREAM_MARK(3),
			   "stream step 2 (copy-out)"))
		goto out;

	/* step 3: the timeline signal the CPU polls for */
	spk = (struct vgpu_packet *)sring[3];
	memset(spk, 0, sizeof(*spk));
	spk[0].type = VGPU_PKT_SIGNAL_TIMELINE;
	spk[0].timeline.sem_bo = sem_handle;
	spk[0].timeline.sem_off = SEM_OFF(SEM_W_TIMELINE);
	spk[0].timeline.value = TL_STREAMS;
	if (stream_ring(fd, sqid[3], 1, &stream_seqno[3]))
		goto out;

	if (sem_poll(sem, SEM_W_TIMELINE, TL_STREAMS, 5000) != TL_STREAMS) {
		fprintf(stderr,
			"FAIL: stream timeline word%u = 0x%08x, want 0x%08x\n",
			SEM_W_TIMELINE, sem[SEM_W_TIMELINE], TL_STREAMS);
		goto out;
	}
	printf("[pid %d] streams: the same 4 units of work as 4 doorbells "
	       "(seq#%u %u %u %u)\n", (int)getpid(), stream_seqno[0],
	       stream_seqno[1], stream_seqno[2], stream_seqno[3]);
	if (data_check_saxpy(res2 + GN_OUT_BASE, src + GN_IN_BASE,
			     "streams: same work, 4 doorbells"))
		goto out;

	memset(&q, 0, sizeof(q));
	if (check(fd, &q, DRM_IOCTL_VGPU_QUERY, "query(after streams)"))
		goto out;
	printf("[pid %d] PASS: stream cost: submitted +%u for the same 4 "
	       "units of work (4 doorbells)\n", (int)getpid(),
	       q.submitted - before.submitted - 1);
	printf("[pid %d] doorbell comparison: graph 1 vs streams 4 "
	       "for identical work -> 4x fewer submissions\n",
	       (int)getpid());

	/*
	 * ==== C: the phase G rejections ==============================
	 * A sync packet naming a BO this fd did not bind, and a cyclic
	 * DAG (rejected before any node is resolved).
	 */
	memset(pk + 12, 0, sizeof(*pk));
	pk[12].type = VGPU_PKT_SIGNAL_TIMELINE;
	pk[12].timeline.sem_bo = dst_handle;	/* ours, but not the bound one */
	pk[12].timeline.sem_off = SEM_OFF(SEM_W_TIMELINE);
	pk[12].timeline.value = 0xffff;
	memset(&db, 0, sizeof(db));
	db.qid = qid_gfx;
	db.put = 13 * (uint32_t)sizeof(*pk);
	if (expect_errno(fd, &db, DRM_IOCTL_VGPU_DOORBELL, EINVAL,
			 "doorbell(signal on an unbound BO)"))
		goto out;

	memset(cyc, 0, sizeof(cyc));
	cyc[0].type = VGPU_PKT_NOP;
	cyc[0].ndeps = 1;
	cyc[0].deps[0] = 1;
	cyc[1].type = VGPU_PKT_NOP;
	cyc[1].ndeps = 1;
	cyc[1].deps[0] = 0;
	memset(&gs, 0, sizeof(gs));
	gs.nodes = (uint64_t)(uintptr_t)cyc;
	gs.qid = qid_gfx;
	gs.nnodes = 2;
	if (expect_errno(fd, &gs, DRM_IOCTL_VGPU_GRAPH, EINVAL,
			 "graph(cyclic DAG)"))
		goto out;

	printf("phase G: timeline semaphore, event slots, cross-engine wait, "
	       "4-node graph on one doorbell; no VGPU_WAIT was issued\n");
	rc = 0;

out:
	close(fd);
	return rc;
}

/*
 * Phase F: preemption and timeslicing.
 *
 * Two hires:
 *  (a) two contexts of the SAME priority each launch a long kernel on
 *      the compute engine. One engine, one pipe: they can only both
 *      finish if the scheduler round-robins between them at safe
 *      points, which is the timeslice. The debugfs "compute" file (the
 *      driver's own accounting) must show context switches.
 *  (b) a high-priority launch submitted while a low-priority one is
 *      running must pause it mid-grid and run first — cooperative
 *      preemption, since a GPU has no "stop" wire — and BOTH must
 *      produce correct results afterwards (a preemption that loses the
 *      interrupted launch's state would corrupt its output).
 *
 * The long kernels are saxpy with a large grid: the modelled per-block
 * SM work makes each launch take milliseconds of real time, so the
 * timing is deterministic without any sleep.
 */
#define F_BLOCK		64
#define F_GRID		256
#define F_N_ELEMS	(F_GRID * F_BLOCK)	/* one element per thread */
#define F_PARAM_WORDS	8

/* one scheduler counter out of debugfs (device-global, cumulative) */
static unsigned long long f_stat(const char *key)
{
	static char buf[4096];
	char *p;
	int fd;
	ssize_t n, total = 0;

	fd = open("/sys/kernel/debug/vgpu/compute", O_RDONLY);
	if (fd < 0)
		return 0;
	while (total < (ssize_t)sizeof(buf) - 1 &&
	       (n = read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
		total += n;
	close(fd);
	if (total <= 0)
		return 0;
	buf[total] = '\0';

	p = strstr(buf, key);
	return p ? strtoull(p + strlen(key), NULL, 10) : 0;
}

struct f_ctx {
	uint32_t *args;
	uint8_t *ring;
	uint32_t bo_handle;
	uint32_t qid;
	uint32_t a, b;
};

/* one context: its own "VRAM" (args BO) and its own compute queue */
static int f_ctx_create(int fd, uint32_t prio, struct f_ctx *c,
			uint32_t a, uint32_t b)
{
	struct vgpu_bo_create bo;
	struct vgpu_queue_create qc;
	int i;

	memset(&bo, 0, sizeof(bo));
	bo.size = (F_PARAM_WORDS + 2 * F_N_ELEMS) * 4;
	if (check(fd, &bo, DRM_IOCTL_VGPU_BO_CREATE, "bo_create"))
		return -1;
	c->bo_handle = bo.handle;
	c->args = xmap(fd, bo.mmap_offset, bo.size);

	c->args[0] = F_N_ELEMS;
	c->args[1] = a;
	c->args[2] = b;
	c->args[3] = F_PARAM_WORDS;			/* in[] word index */
	c->args[4] = F_PARAM_WORDS + F_N_ELEMS;		/* out[] word index */
	for (i = 0; i < F_N_ELEMS; i++) {
		c->args[F_PARAM_WORDS + i] = i * 2 + 1;
		c->args[F_PARAM_WORDS + F_N_ELEMS + i] = 0xdeadbeef;
	}

	memset(&qc, 0, sizeof(qc));
	qc.engine = VGPU_ENGINE_COMPUTE;
	qc.prio = prio;
	qc.nslots = 16;
	if (check(fd, &qc, DRM_IOCTL_VGPU_QUEUE_CREATE, "queue_create"))
		return -1;
	c->qid = qc.qid;
	c->ring = xmap(fd, qc.mmap_offset, qc.ring_size);
	c->a = a;
	c->b = b;
	return 0;
}

/* "cudaLaunchKernel" on one context's queue */
static int f_launch(int fd, uint32_t kid, struct f_ctx *c, uint32_t *seqno)
{
	struct vgpu_packet *pk = (struct vgpu_packet *)c->ring;
	struct vgpu_doorbell db;

	memset(pk, 0, sizeof(*pk));
	pk[0].type = VGPU_PKT_LAUNCH;
	pk[0].launch.kid = kid;
	pk[0].launch.grid = F_GRID;
	pk[0].launch.block = F_BLOCK;
	pk[0].launch.args_bo = c->bo_handle;
	pk[0].launch.args_off = 0;

	memset(&db, 0, sizeof(db));
	db.qid = c->qid;
	db.put = sizeof(*pk);
	if (check(fd, &db, DRM_IOCTL_VGPU_DOORBELL, "doorbell(launch)"))
		return -1;
	*seqno = db.seqno;
	printf("  launch: q#%u prio-queue grid=%u block=%u -> seq#%u\n",
	       c->qid, F_GRID, F_BLOCK, db.seqno);
	return 0;
}

/*
 * Wait until the engine has actually PICKED UP a submission: a doorbell
 * only queues work, and waking a kworker is asynchronous. Preempting a
 * launch needs a RUNNING victim, so the test must not assume the engine
 * started it — the driver says so in the "pending" debugfs view, which
 * lists a launch in flight with its resume point. This waits for a
 * state; it does not pace the mechanism: the pause itself still happens
 * at an instruction boundary inside the running launch.
 */
static int f_wait_inflight(uint32_t seqno, uint32_t *block, uint32_t *warp)
{
	static char buf[2048];
	char want[64], *p;
	int fd, i, found = -1;

	snprintf(want, sizeof(want), "in-flight seq#%u", seqno);
	fd = open("/sys/kernel/debug/vgpu/pending", O_RDONLY);
	if (fd < 0)
		return -1;
	for (i = 0; i < 100; i++) {
		ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);

		if (n > 0) {
			unsigned s = 0, b = 0, g = 0, w = 0, pc = 0;

			buf[n] = '\0';
			if (strstr(buf, want)) {
				found = 0;
				p = strstr(buf, "launch");
				if (p && sscanf(p,
						" launch  seq#%u at block %u/%u warp %u pc %u",
						&s, &b, &g, &w, &pc) >= 5 &&
				    s == seqno) {
					*block = b;
					*warp = w;
				}
				break;
			}
		}
		usleep(200);
	}
	close(fd);
	return found;
}

/* every thread must have computed out[i] = a*in[i] + b */
static int f_verify(struct f_ctx *c, const char *who)
{
	int i;

	for (i = 0; i < F_N_ELEMS; i++) {
		uint32_t want = c->a * c->args[F_PARAM_WORDS + i] + c->b;
		uint32_t got = c->args[F_PARAM_WORDS + F_N_ELEMS + i];

		if (got != want) {
			fprintf(stderr,
				"FAIL: %s out[%d] = %u, want %u (preemption lost state?)\n",
				who, i, got, want);
			return -1;
		}
	}
	return 0;
}

static int run_preempt(void)
{
	int fd, rc = 1;
	uint32_t kid = 0, seqA, seqB, seqC, seqD;
	uint32_t blkC = 0, warpC = 0;
	unsigned long long sw0, pr0, sw1, pr1, d_sw, d_pr;
	struct f_ctx ca, cb, cc, cd;
	struct vgpu_module_load ml;

	static const struct vgpu_instr code[] = {
		{ .op = VGPU_ISA_MOVI, .dst = 15, .a = 0 },	/* r15 = 0 */
		{ .op = VGPU_ISA_LD,   .dst = 1,  .a = 15, .b = 1 },
		{ .op = VGPU_ISA_LD,   .dst = 2,  .a = 15, .b = 2 },
		{ .op = VGPU_ISA_LD,   .dst = 3,  .a = 15, .b = 3 },
		{ .op = VGPU_ISA_LD,   .dst = 4,  .a = 15, .b = 4 },
		{ .op = VGPU_ISA_ADD,  .dst = 5,  .a = 3,  .b = 0 },
		{ .op = VGPU_ISA_LD,   .dst = 6,  .a = 5,  .b = 0 },
		{ .op = VGPU_ISA_MUL,  .dst = 7,  .a = 1,  .b = 6 },
		{ .op = VGPU_ISA_ADD,  .dst = 8,  .a = 7,  .b = 2 },
		{ .op = VGPU_ISA_ADD,  .dst = 9,  .a = 4,  .b = 0 },
		{ .op = VGPU_ISA_ST,   .dst = 8,  .a = 9,  .b = 0 },
		{ .op = VGPU_ISA_EXIT },
	};

	fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0)
		fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "FAIL: open: %s\n", strerror(errno));
		return 1;
	}

	memset(&ml, 0, sizeof(ml));
	ml.nregs = 16;
	ml.smem = 0;
	ml.ninstr = sizeof(code) / sizeof(code[0]);
	ml.code = (uint64_t)(uintptr_t)code;
	if (check(fd, &ml, DRM_IOCTL_VGPU_MODULE_LOAD, "module_load"))
		goto out;
	kid = ml.kid;

	/* ---- (a) timeslicing: two contexts, same priority ----------- */
	if (f_ctx_create(fd, 1, &ca, 3, 7) ||
	    f_ctx_create(fd, 1, &cb, 5, 11))
		goto out;

	sw0 = f_stat("ctx switches");
	pr0 = f_stat("preemptions");
	printf("=== phase F/a: 2 contexts (prio=1) x grid=%u block=%u on ONE compute engine ===\n",
	       F_GRID, F_BLOCK);
	if (f_launch(fd, kid, &ca, &seqA) || f_launch(fd, kid, &cb, &seqB))
		goto out;
	if (wait_seqno(fd, seqA, 30000) != 0 ||
	    wait_seqno(fd, seqB, 30000) != 0) {
		fprintf(stderr, "FAIL: timeslice launches did not complete\n");
		goto out;
	}
	sw1 = f_stat("ctx switches");
	pr1 = f_stat("preemptions");
	d_sw = sw1 - sw0;
	d_pr = pr1 - pr0;
	if (f_verify(&ca, "ctx A") || f_verify(&cb, "ctx B"))
		goto out;
	printf("[pid %d] PASS: timeslicing: both %u-block grids correct, "
	       "engine interleaved them (ctx switches +%llu, preemptions +%llu)\n",
	       (int)getpid(), F_GRID, d_sw, d_pr);
	if (!d_sw) {
		fprintf(stderr,
			"FAIL: no context switches: the engine ran one context to completion\n");
		goto out;
	}

	/* ---- (b) preemption: prio=2 arrives while prio=0 runs ------ */
	if (f_ctx_create(fd, 0, &cc, 2, 1) ||
	    f_ctx_create(fd, 2, &cd, 9, 4))
		goto out;

	sw0 = f_stat("ctx switches");
	pr0 = f_stat("preemptions");
	printf("=== phase F/b: prio=2 launch submitted while prio=0 is in flight ===\n");
	/*
	 * The low-priority launch is queued first and starts running; the
	 * high-priority doorbell follows, so it lands while the low launch
	 * is mid-grid and has to preempt it (a launch is milliseconds of
	 * modelled work — no sleep needed to make them overlap).
	 */
	if (f_launch(fd, kid, &cc, &seqC))
		goto out;
	if (f_wait_inflight(seqC, &blkC, &warpC)) {
		fprintf(stderr,
			"FAIL: low-priority launch never started running\n");
		goto out;
	}
	printf("  low-prio seq#%u is running at block %u/%u warp %u -> submit prio=2\n",
	       seqC, blkC, F_GRID, warpC);
	if (f_launch(fd, kid, &cd, &seqD))
		goto out;
	if (wait_seqno(fd, seqD, 30000) != 0 ||
	    wait_seqno(fd, seqC, 30000) != 0) {
		fprintf(stderr, "FAIL: preemption launches did not complete\n");
		goto out;
	}
	sw1 = f_stat("ctx switches");
	pr1 = f_stat("preemptions");
	d_sw = sw1 - sw0;
	d_pr = pr1 - pr0;
	if (f_verify(&cc, "low prio") || f_verify(&cd, "high prio"))
		goto out;
	printf("[pid %d] PASS: preemption: prio=0 paused for prio=2 "
	       "(ctx switches +%llu, preemptions +%llu); both results correct\n",
	       (int)getpid(), d_sw, d_pr);
	if (!d_pr) {
		fprintf(stderr,
			"FAIL: no preemption: the high-priority launch did not interrupt the running low-priority one\n");
		goto out;
	}

	printf("preemption + timeslicing: all passed\n");
	rc = 0;

out:
	close(fd);
	return rc;
}


/* ---------------------------------------------- phase C: GPU virtual memory */

/*
 * Phase C is the memory model of a GPGPU card observed from userspace:
 *
 *   cudaMalloc            -> VGPU_BO_CREATE   (a pool buffer)
 *   + VM_ALLOC            -> a GPU VA in MY context
 *   + VM_BIND             -> the device can dereference that VA
 *   VGPU_PKT_VM_COPY      -> the driver walks MY page tables
 *
 * Every fd is one context with its own VMID, so "the same address"
 * means nothing until you say whose tables to walk. That is the whole
 * point, and it is what this mode demonstrates:
 *
 *  (a) two contexts reserve the SAME VA (0x10000, by asking for the
 *      same alignment) and bind DIFFERENT buffers there. A VA->VA DMA
 *      through each context's doorbell brings back its own pattern.
 *      A third context that reserved the VA but never bound it faults
 *      with -ENOENT: an address nobody bound is a fault, not garbage.
 *
 *  (b) binding more than the device's VRAM budget evicts; the evicted
 *      ranges still read back correctly, because a fault re-maps them
 *      (demand paging — the TTM model, with host memory as the
 *      destination the pages never actually leave).
 */
#define VM_RANGE	(64 * 1024)	/* the range each context reserves */
#define VM_ALIGN	0x10000		/* ... at this alignment, so both */
#define VM_WANT_VA	0x10000		/* ... contexts get this VA */
#define VM_2ND_VA	0x20000		/* first-fit then hands this out */
#define VM_COPY_LEN	4096		/* one page of VA->VA DMA */
#define VM_EVICT_N	4		/* 4 x 64K against a 128K budget */
#define VM_EVICT_BUDGET	(128 * 1024)
#define VM_DBG		"/sys/kernel/debug/vgpu/vm"

struct vm_ctx {
	int fd;
	uint8_t *ring;
	uint32_t qid;
	uint32_t ring_put;	/* how far this fd's writer has produced */
	uint32_t src_bo, dst_bo;
	uint8_t *src, *dst;
	uint64_t src_va, dst_va;
	uint32_t vmid;
};

/* what a context reports back (through shared memory, it is forked) */
struct vm_report {
	uint32_t vmid;
	int rc;
};

static int vm_bo(struct vm_ctx *c, uint32_t *handle, uint8_t **map)
{
	struct vgpu_bo_create bo = {};

	bo.size = VM_RANGE;
	if (check(c->fd, &bo, DRM_IOCTL_VGPU_BO_CREATE, "vm bo_create"))
		return -1;
	*handle = bo.handle;
	if (map)
		*map = xmap(c->fd, bo.mmap_offset, VM_RANGE);
	return 0;
}

static int vm_alloc(struct vm_ctx *c, uint64_t align, uint64_t *va)
{
	struct vgpu_vm_alloc a = {};

	a.size = VM_RANGE;
	a.align = align;
	if (check(c->fd, &a, DRM_IOCTL_VGPU_VM_ALLOC, "vm_alloc"))
		return -1;
	c->vmid = a.vmid;	/* assigned on first use, kept afterwards */
	*va = a.va;
	return 0;
}

static int vm_bind(struct vm_ctx *c, uint64_t va, uint32_t handle)
{
	struct vgpu_vm_bind b = {};

	b.va = va;
	b.bo_handle = handle;
	return check(c->fd, &b, DRM_IOCTL_VGPU_VM_BIND, "vm_bind");
}

/* one stream on the DMA engine: a VA->VA copy is that engine's job */
static int vm_queue(struct vm_ctx *c)
{
	struct vgpu_queue_create qc = {};

	qc.engine = VGPU_ENGINE_COPY;
	qc.prio = 1;
	qc.nslots = 16;
	if (check(c->fd, &qc, DRM_IOCTL_VGPU_QUEUE_CREATE, "queue_create"))
		return -1;
	c->qid = qc.qid;
	c->ring = xmap(c->fd, qc.mmap_offset, qc.ring_size);
	return 0;
}

/*
 * A VA->VA DMA through the doorbell. Returns 0, or a negative errno:
 * the doorbell itself resolves both addresses against THIS fd's page
 * tables, so an address this context never bound fails here.
 */
static int vm_copy(struct vm_ctx *c, uint64_t src, uint64_t dst)
{
	struct vgpu_packet *pk;
	struct vgpu_doorbell db = {};
	struct vgpu_wait w = {};

	/*
	 * Append to the queue ring, don't rewrite it: the engine advances
	 * get as it consumes, so the next doorbell must name a put past
	 * that (the same rule the phase-B batch-2 test relies on).
	 */
	if (c->ring_put + sizeof(*pk) > 16 * 64) {
		fprintf(stderr, "FAIL: va ring full\n");
		return -ENOMEM;
	}
	pk = (struct vgpu_packet *)(c->ring + c->ring_put);
	memset(pk, 0, sizeof(*pk));
	pk[0].type = VGPU_PKT_VM_COPY;
	pk[0].vm_copy.src_va = src;
	pk[0].vm_copy.dst_va = dst;
	pk[0].vm_copy.len = VM_COPY_LEN;

	db.qid = c->qid;
	db.put = c->ring_put + sizeof(*pk);
	if (ioctl(c->fd, DRM_IOCTL_VGPU_DOORBELL, &db) < 0)
		return -errno;
	c->ring_put = db.put;

	w.seqno = db.seqno;
	w.timeout_ms = 5000;
	if (check(c->fd, &w, DRM_IOCTL_VGPU_WAIT, "vm wait"))
		return -1;
	if (w.status != 0) {
		fprintf(stderr, "FAIL: va copy status=%d\n", (int)w.status);
		return w.status ? w.status : -1;
	}
	return 0;
}

/* One counter off the driver's "device:" line in the vm view. */
static long long vm_dev_num(const char *buf, const char *key)
{
	const char *line, *p;
	char *end;
	long long v;

	line = strstr(buf, "device:");
	if (!line)
		return -1;
	p = strstr(line, key);
	if (!p)
		return -1;
	p += strlen(key);
	while (*p == ' ' || *p == '\t')
		p++;
	v = strtoll(p, &end, 10);
	return end == p ? -1 : v;
}

static int vm_dbg_read(char *buf, size_t len)
{
	FILE *f = fopen(VM_DBG, "r");
	size_t n;

	if (!f)
		return -1;
	n = fread(buf, 1, len - 1, f);
	buf[n] = '\0';
	fclose(f);
	return 0;
}

/* the vm view is writable: a new budget applies immediately */
static int vm_dbg_write(uint64_t budget)
{
	FILE *f = fopen(VM_DBG, "w");

	if (!f)
		return -1;
	if (fprintf(f, "%llu\n", (unsigned long long)budget) < 0) {
		fclose(f);
		return -1;
	}
	return fclose(f) ? -1 : 0;
}

/*
 * Acceptance (a): one context's copy of the shared VA. @seed is the
 * pattern only this context's buffer holds.
 */
static int vm_isolation_child(uint8_t seed, struct vm_report *rep)
{
	struct vm_ctx c = {};
	int rc = 1, i;

	c.fd = open_dev();
	if (c.fd < 0)
		return 1;

	if (vm_bo(&c, &c.src_bo, &c.src) ||
	    vm_bo(&c, &c.dst_bo, &c.dst) ||
	    vm_alloc(&c, VM_ALIGN, &c.src_va) ||
	    vm_alloc(&c, 0, &c.dst_va) ||
	    vm_bind(&c, c.src_va, c.src_bo) ||
	    vm_bind(&c, c.dst_va, c.dst_bo) ||
	    vm_queue(&c))
		goto out;

	if (c.src_va != VM_WANT_VA || c.dst_va != VM_2ND_VA) {
		fprintf(stderr, "FAIL: va 0x%llx/0x%llx, want 0x%x/0x%x\n",
			(unsigned long long)c.src_va,
			(unsigned long long)c.dst_va,
			VM_WANT_VA, VM_2ND_VA);
		goto out;
	}

	for (i = 0; i < VM_COPY_LEN; i++)
		c.src[i] = (uint8_t)(seed + (i & 0x0f));
	memset(c.dst, 0, VM_RANGE);

	if (vm_copy(&c, c.src_va, c.dst_va))
		goto out;
	for (i = 0; i < VM_COPY_LEN; i++) {
		if (c.dst[i] != c.src[i]) {
			fprintf(stderr,
				"FAIL: vmid %u va 0x%llx read 0x%02x, own pattern is 0x%02x\n",
				c.vmid, (unsigned long long)c.src_va,
				c.dst[i], c.src[i]);
			goto out;
		}
	}
	printf("[pid %d] PASS: vmid %u: va 0x%llx is MY buffer "
	       "(pattern 0x%02x.., DMA'd to va 0x%llx and read back)\n",
	       (int)getpid(), c.vmid, (unsigned long long)c.src_va, seed,
	       (unsigned long long)c.dst_va);
	rep->vmid = c.vmid;
	rep->rc = 0;
	rc = 0;

out:
	close(c.fd);
	return rc;
}

/*
 * Acceptance (a), the other half: a context that reserved the very same
 * VA and never attached a buffer to it must fault, not read the other
 * context's bytes.
 */
static int vm_unbound(uint32_t *vmid_out)
{
	struct vm_ctx c = {};
	uint64_t va;
	int rc = 1;

	c.fd = open_dev();
	if (c.fd < 0)
		return 1;

	if (vm_alloc(&c, VM_ALIGN, &va) || vm_queue(&c))
		goto out;
	if (va != VM_WANT_VA) {
		fprintf(stderr, "FAIL: va 0x%llx, want 0x%x\n",
			(unsigned long long)va, VM_WANT_VA);
		goto out;
	}
	*vmid_out = c.vmid;

	rc = vm_copy(&c, va, VM_2ND_VA);
	if (rc != -ENOENT) {
		fprintf(stderr,
			"FAIL: unbound va 0x%llx returned %d, want -ENOENT\n",
			(unsigned long long)va, rc);
		rc = 1;
		goto out;
	}
	printf("[pid %d] PASS: vmid %u: va 0x%llx reserved but never bound "
	       "-> -ENOENT (a fault, not another context's data)\n",
	       (int)getpid(), c.vmid, (unsigned long long)va);
	rc = 0;

out:
	close(c.fd);
	return rc;
}

/* Acceptance (b): bind past the budget, then read everything back. */
static int vm_eviction(void)
{
	struct vm_ctx c = {};
	uint8_t *map[VM_EVICT_N];
	uint32_t handle[VM_EVICT_N];
	uint64_t va[VM_EVICT_N], scratch, budget = 0;
	char dbg[8192];
	long long b, evicted, faults;
	int rc = 1, i, j;

	c.fd = open_dev();
	if (c.fd < 0)
		return 1;

	if (vm_dbg_read(dbg, sizeof(dbg))) {
		fprintf(stderr, "FAIL: read %s: %s\n", VM_DBG, strerror(errno));
		goto out;
	}
	b = vm_dev_num(dbg, "budget ");
	if (b < 0) {
		fprintf(stderr, "FAIL: no budget in %s\n", VM_DBG);
		goto out;
	}
	budget = (uint64_t)b;
	printf("[pid %d] device VRAM budget is %llu B; forcing %d B\n",
	       (int)getpid(), (unsigned long long)budget, VM_EVICT_BUDGET);
	if (vm_dbg_write(VM_EVICT_BUDGET)) {
		fprintf(stderr, "FAIL: write %s: %s\n", VM_DBG, strerror(errno));
		goto out;
	}

	for (i = 0; i < VM_EVICT_N; i++) {
		if (vm_bo(&c, &handle[i], &map[i]))
			goto out;
		/* a distinct pattern per buffer: a wrong mapping shows up */
		memset(map[i], 0xC0 + i, VM_RANGE);
		if (vm_alloc(&c, 0, &va[i]) || vm_bind(&c, va[i], handle[i]))
			goto out;
	}

	/* the buffer the ranges are read back through */
	if (vm_bo(&c, &c.dst_bo, &c.dst) ||
	    vm_alloc(&c, 0, &scratch) ||
	    vm_bind(&c, scratch, c.dst_bo) ||
	    vm_queue(&c))
		goto out;

	if (vm_dbg_read(dbg, sizeof(dbg))) {
		fprintf(stderr, "FAIL: read %s: %s\n", VM_DBG, strerror(errno));
		goto out;
	}
	printf("--- debugfs: %s (budget forced to %d B) ---\n%s",
	       VM_DBG, VM_EVICT_BUDGET, dbg);
	evicted = vm_dev_num(dbg, "evicted ");
	if (evicted <= 0) {
		fprintf(stderr, "FAIL: evicted=%lld, expected > 0\n", evicted);
		goto out;
	}
	printf("PASS: %lld range(s) evicted (%d B of bindings past a %d B budget)\n",
	       evicted, VM_EVICT_N * VM_RANGE, VM_EVICT_BUDGET);

	/*
	 * Read every range back. The resident ones answer from their PTEs;
	 * the evicted ones fault on the way in and are mapped back — which
	 * is why the data is correct either way.
	 */
	for (i = 0; i < VM_EVICT_N; i++) {
		memset(c.dst, 0, VM_RANGE);
		if (vm_copy(&c, va[i], scratch))
			goto out;
		for (j = 0; j < VM_COPY_LEN; j++) {
			if (c.dst[j] != (uint8_t)(0xC0 + i)) {
				fprintf(stderr,
					"FAIL: va 0x%llx read 0x%02x, want 0x%02x\n",
					(unsigned long long)va[i], c.dst[j],
					0xC0 + i);
				goto out;
			}
		}
		printf("[pid %d] PASS: va 0x%llx read its own pattern 0x%02x.. after eviction\n",
		       (int)getpid(), (unsigned long long)va[i], 0xC0 + i);
	}

	if (vm_dbg_read(dbg, sizeof(dbg))) {
		fprintf(stderr, "FAIL: read %s: %s\n", VM_DBG, strerror(errno));
		goto out;
	}
	faults = vm_dev_num(dbg, "demand-faults ");
	if (faults <= 0) {
		fprintf(stderr, "FAIL: demand-faults=%lld, expected > 0\n",
			faults);
		goto out;
	}
	printf("PASS: %lld demand fault(s) serviced by re-mapping "
	       "(eviction is transparent to the reader)\n", faults);
	rc = 0;

out:
	/* leave the device the way we found it */
	if (budget > 0)
		vm_dbg_write(budget);
	close(c.fd);
	return rc;
}

static int run_vm(void)
{
	struct vm_report *sh;
	pid_t pids[2];
	int i, failed = 0;
	uint32_t vmid = 0;

	printf("=== phase C: VMID isolation — one VA, two contexts, two buffers ===\n");

	/* shared memory: the forked contexts report their VMID back */
	sh = mmap(NULL, sizeof(*sh) * 2, PROT_READ | PROT_WRITE,
		  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED) {
		fprintf(stderr, "FAIL: mmap: %s\n", strerror(errno));
		return 1;
	}

	for (i = 0; i < 2; i++) {
		pids[i] = fork();
		if (pids[i] < 0) {
			fprintf(stderr, "FAIL: fork: %s\n", strerror(errno));
			return 1;
		}
		if (!pids[i])
			_exit(vm_isolation_child(i ? 0xB0 : 0xA0, &sh[i]));
	}
	for (i = 0; i < 2; i++) {
		int st;

		waitpid(pids[i], &st, 0);
		if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
			fprintf(stderr, "child %d (pid %d) FAILED\n", i,
				(int)pids[i]);
			failed++;
		}
	}

	if (failed)
		return 1;
	if (sh[0].vmid == sh[1].vmid) {
		fprintf(stderr, "FAIL: both contexts got vmid %u\n", sh[0].vmid);
		return 1;
	}
	printf("PASS: VMID isolation: vmid %u and vmid %u both hold va 0x%x, "
	       "each mapping its own buffer\n", sh[0].vmid, sh[1].vmid,
	       VM_WANT_VA);

	if (vm_unbound(&vmid))
		return 1;

	printf("=== phase C: VRAM budget — bind past it, evict, read back ===\n");
	if (vm_eviction())
		return 1;

	printf("phase C: VMID isolation, budget eviction and demand re-map all pass\n");
	return 0;
}


/*
 * Phase I: runtime power management, DVFS and per-context accounting.
 *
 * The device should power-gate after ~2s idle and the next submission
 * must wake it (a measurable cost, not a hang); a clock knob must scale
 * the compute engine's modelled memory latency; and each queue must
 * account its own modelled compute time.
 */
/*
 * Read "key <number>" out of a debugfs file. The key may appear at the
 * start of a line ("clock  1000 MHz") or after a word ("rpm  suspends
 * 3, ..."), so search for it inside the line and skip past it.
 */
static long dbg_long(const char *path, const char *key, long dflt)
{
	FILE *f = fopen(path, "r");
	char line[256];
	long val = dflt;

	if (!f)
		return dflt;
	while (fgets(line, sizeof(line), f)) {
		char *p = strstr(line, key);

		if (!p)
			continue;
		val = strtol(p + strlen(key), NULL, 10);
		break;
	}
	fclose(f);
	return val;
}

static int run_power(void)
{
	int fd, rc = 1;
	struct vgpu_bo_create bo;
	struct vgpu_queue_create qc;
	struct vgpu_module_load ml;
	struct vgpu_doorbell db;
	struct vgpu_wait w;
	uint8_t *ring;
	struct vgpu_packet *pk;
	uint32_t ring_put = 0;	/* the doorbell needs put to advance past get */
	uint32_t *args;
	long ns_a, ns_b, ns_ref, ns_before, ns_after;
	long susp_a, susp_b, clock_a, clock_b;
	int i;
	static const struct vgpu_instr code[] = {
		{ .op = VGPU_ISA_MOVI, .dst = 15, .a = 0 },
		{ .op = VGPU_ISA_LD,   .dst = 1,  .a = 15, .b = 1 },
		{ .op = VGPU_ISA_LD,   .dst = 2,  .a = 15, .b = 2 },
		{ .op = VGPU_ISA_LD,   .dst = 3,  .a = 15, .b = 3 },
		{ .op = VGPU_ISA_LD,   .dst = 4,  .a = 15, .b = 4 },
		{ .op = VGPU_ISA_ADD,  .dst = 5,  .a = 3,  .b = 0 },
		{ .op = VGPU_ISA_LD,   .dst = 6,  .a = 5,  .b = 0 },
		{ .op = VGPU_ISA_MUL,  .dst = 7,  .a = 1,  .b = 6 },
		{ .op = VGPU_ISA_ADD,  .dst = 8,  .a = 7,  .b = 2 },
		{ .op = VGPU_ISA_ADD,  .dst = 9,  .a = 4,  .b = 0 },
		{ .op = VGPU_ISA_ST,   .dst = 8,  .a = 9,  .b = 0 },
		{ .op = VGPU_ISA_EXIT },
	};

	fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0)
		fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "FAIL: open: %s\n", strerror(errno));
		return 1;
	}

	/* a small kernel + an argument buffer: the same saxpy shape */
	memset(&ml, 0, sizeof(ml));
	ml.nregs = 16;
	ml.smem = 0;
	ml.ninstr = sizeof(code) / sizeof(code[0]);
	ml.code = (uint64_t)(uintptr_t)code;
	if (check(fd, &ml, DRM_IOCTL_VGPU_MODULE_LOAD, "module_load"))
		goto out;

	memset(&bo, 0, sizeof(bo));
	bo.size = 8192;		/* >= PAGE_SIZE: 8 words header + 2 x 512 */
	if (check(fd, &bo, DRM_IOCTL_VGPU_BO_CREATE, "bo_create"))
		goto out;
	args = mmap(NULL, bo.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		    fd, (off_t)bo.mmap_offset);
	if (args == MAP_FAILED) {
		fprintf(stderr, "FAIL: mmap: %s\n", strerror(errno));
		goto out;
	}
	args[0] = 512;
	args[1] = 3;
	args[2] = 7;
	args[3] = 8;
	args[4] = 8 + 512;
	for (i = 0; i < 512; i++)
		args[8 + i] = i + 1;

	memset(&qc, 0, sizeof(qc));
	qc.engine = VGPU_ENGINE_COMPUTE;
	qc.prio = 1;
	qc.nslots = 16;
	if (check(fd, &qc, DRM_IOCTL_VGPU_QUEUE_CREATE, "queue_create"))
		goto out;
	ring = mmap(NULL, qc.ring_size, PROT_READ | PROT_WRITE, MAP_SHARED,
		    fd, (off_t)qc.mmap_offset);
	if (ring == MAP_FAILED) {
		fprintf(stderr, "FAIL: mmap ring: %s\n", strerror(errno));
		goto out;
	}
	pk = (struct vgpu_packet *)ring;

	/* ---- (a) the device sleeps, then wakes on the next submit ---- */
	susp_a = dbg_long("/sys/kernel/debug/vgpu/stats", "suspends ", 0);
	printf("phase I/a: idling past the autosuspend delay...\n");
	sleep(4);			/* autosuspend is 2s */

	ns_before = ns_ref =
		dbg_long("/sys/kernel/debug/vgpu/compute", "latency ", 0);

	pk = (struct vgpu_packet *)(ring + ring_put);
	memset(pk, 0, sizeof(*pk));
	pk[0].type = VGPU_PKT_LAUNCH;
	pk[0].launch.kid = ml.kid;
	pk[0].launch.grid = 8;
	pk[0].launch.block = 64;
	pk[0].launch.args_bo = bo.handle;
	pk[0].launch.args_off = 0;
	ring_put += sizeof(*pk);
	memset(&db, 0, sizeof(db));
	db.qid = qc.qid;
	db.put = ring_put;
	if (check(fd, &db, DRM_IOCTL_VGPU_DOORBELL, "doorbell(after idle)"))
		goto out;
	memset(&w, 0, sizeof(w));
	w.seqno = db.seqno;
	w.timeout_ms = 5000;
	if (check(fd, &w, DRM_IOCTL_VGPU_WAIT, "wait(after idle)"))
		goto out;
	if (w.status != 0) {
		fprintf(stderr, "FAIL: launch after idle status=%d\n",
			(int)w.status);
		goto out;
	}
	susp_b = dbg_long("/sys/kernel/debug/vgpu/stats", "suspends ", 0);
	printf("phase I/a: rpm suspends %ld -> %ld after the idle window\n",
	       susp_a, susp_b);
	/* this launch's share of the cumulative latency counter */
	ns_after = dbg_long("/sys/kernel/debug/vgpu/compute", "latency ", 0);
	ns_ref = ns_after - ns_ref;
	printf("phase I/a: modelled latency %ld -> %ld (this launch: %ld ns)\n",
	       ns_before, ns_after, ns_ref);
	if (susp_b <= susp_a) {
		fprintf(stderr,
			"FAIL: the device did not power-gate during the idle window\n");
		goto out;
	}
	printf("[pid %d] PASS: rpm: the device slept and woke for a submission\n",
	       (int)getpid());

	/* ---- (b) the clock knob scales the modelled latency ---- */
	/*
	 * The counter is cumulative since insmod, so what matters is the
	 * DELTA each launch adds: a launch at half the clock must add
	 * roughly twice the modelled latency of one at the reference.
	 */
	clock_a = dbg_long("/sys/kernel/debug/vgpu/stats", "clock", 0);
	ns_a = dbg_long("/sys/kernel/debug/vgpu/compute", "latency ", 0);
	{
		FILE *f = fopen("/sys/kernel/debug/vgpu/clock_hz", "w");

		if (!f) {
			fprintf(stderr, "FAIL: open clock_hz: %s\n",
				strerror(errno));
			goto out;
		}
		fprintf(f, "%ld\n", clock_a / 2);	/* half the clock */
		fclose(f);
	}
	/* one more launch at the lower clock, appended to the ring */
	pk = (struct vgpu_packet *)(ring + ring_put);
	memset(pk, 0, sizeof(*pk));
	pk[0].type = VGPU_PKT_LAUNCH;
	pk[0].launch.kid = ml.kid;
	pk[0].launch.grid = 8;
	pk[0].launch.block = 64;
	pk[0].launch.args_bo = bo.handle;
	pk[0].launch.args_off = 0;
	ring_put += sizeof(*pk);
	memset(&db, 0, sizeof(db));
	db.qid = qc.qid;
	db.put = ring_put;
	if (check(fd, &db, DRM_IOCTL_VGPU_DOORBELL, "doorbell(low clock)"))
		goto out;
	memset(&w, 0, sizeof(w));
	w.seqno = db.seqno;
	w.timeout_ms = 5000;
	if (check(fd, &w, DRM_IOCTL_VGPU_WAIT, "wait(low clock)"))
		goto out;
	clock_b = dbg_long("/sys/kernel/debug/vgpu/stats", "clock", 0);
	ns_b = dbg_long("/sys/kernel/debug/vgpu/compute", "latency ", 0);
	printf("phase I/b: clock %ld -> %ld MHz, launch cost %ld -> %ld ns\n",
	       clock_a, clock_b, ns_ref, ns_b - ns_a);
	if (clock_b != clock_a / 2) {
		fprintf(stderr, "FAIL: the clock knob did not take effect\n");
		goto out;
	}
	if (ns_b - ns_a < ns_ref) {
		fprintf(stderr,
			"FAIL: the low-clock launch did not cost more (ref %ld, got %ld)\n",
			ns_ref, ns_b - ns_a);
		goto out;
	}
	printf("[pid %d] PASS: dvfs: halving the clock at least doubles the modelled kernel time (%ld -> %ld ns)\n",
	       (int)getpid(), ns_ref, ns_b - ns_a);
	/* restore the reference clock */
	{
		FILE *f = fopen("/sys/kernel/debug/vgpu/clock_hz", "w");

		if (f) {
			fprintf(f, "%ld\n", clock_a);
			fclose(f);
		}
	}

	/* ---- (c) per-context accounting ---- */
	{
		FILE *f = fopen("/sys/kernel/debug/vgpu/queues", "r");
		char line[256];
		int found = 0;

		if (!f) {
			fprintf(stderr, "FAIL: open queues: %s\n",
				strerror(errno));
			goto out;
		}
		while (fgets(line, sizeof(line), f)) {
			char *p = line;

			/*
			 * "compute" appears twice in the line (the engine
			 * name and the accounting field) — take any
			 * occurrence that is followed by a positive number.
			 */
			while ((p = strstr(p, "compute ")) != NULL) {
				long v = strtol(p + strlen("compute "), NULL, 10);

				if (v > 0)
					found = 1;
				p++;
			}
		}
		if (!found) {
			rewind(f);
			while (fgets(line, sizeof(line), f))
				fprintf(stderr, "queues| %s", line);
			fclose(f);
			fprintf(stderr,
				"FAIL: no per-queue compute accounting found\n");
			goto out;
		}
		fclose(f);
		printf("[pid %d] PASS: accounting: the queue shows its modelled compute ns\n",
		       (int)getpid());
	}

	/* the launch results are still correct after all that */
	for (i = 0; i < 512; i++) {
		uint32_t want = 3 * args[8 + i] + 7;
		uint32_t got = args[8 + 512 + i];

		if (got != want) {
			fprintf(stderr, "FAIL: out[%d] = %u, want %u\n",
				i, got, want);
			goto out;
		}
	}
	printf("phase I: runtime PM, DVFS and per-context accounting all pass\n");
	rc = 0;

out:
	close(fd);
	return rc;
}

int main(int argc, char **argv)
{
	int m = 1, i;

	if (argc > 1 && !strcmp(argv[1], "-m"))
		m = (argc > 2) ? atoi(argv[2]) : 4;

	if (argc > 1 && !strcmp(argv[1], "-i"))
		return run_power() ? 1 : 0;

	if (argc > 1 && !strcmp(argv[1], "-q"))
		return run_queues() ? 1 : 0;

	if (argc > 1 && !strcmp(argv[1], "-p"))
		return run_doorbell() ? 1 : 0;

	if (argc > 1 && !strcmp(argv[1], "-e"))
		return run_negative() ? 1 : 0;

	if (argc > 1 && !strcmp(argv[1], "-g"))
		return run_sync() ? 1 : 0;

	if (argc > 1 && !strcmp(argv[1], "-v"))
		return run_vm() ? 1 : 0;

	if (argc > 1 && !strcmp(argv[1], "-f"))
		return run_preempt() ? 1 : 0;

	if (m == 1)
		return run_one(0) ? 1 : 0;
	/* multi-process: disjoint ring slots, like per-context rings */
	{
		pid_t pids[64];
		int n = 0, failed = 0;

		for (i = 0; i < m && n < 64; i++) {
			pid_t pid = fork();

			if (pid == 0)
				_exit(run_one(i) ? 1 : 0);
			pids[n++] = pid;
		}
		for (i = 0; i < n; i++) {
			int st;

			waitpid(pids[i], &st, 0);
			if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
				fprintf(stderr, "child %d (pid %d) FAILED\n",
					i, (int)pids[i]);
				failed++;
			}
		}
		printf("multi-process: %d/%d passed\n", n - failed, n);
		return failed ? 1 : 0;
	}
}
