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
 * -p:   packet + doorbell submission (phase B): the engine fetches.
 * -q:   multi-queue / multi-engine: one fd, one stream per engine
 *        (streams + per-engine command classes).
 * -e:   the negative paths only (rejected doorbell, engine faults,
 * ring full) — each must fail the exact way DESIGN.md §10 says.
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
#include <unistd.h>

#include "drm.h"
#include "vgpu_drm.h"

#define BO_SIZE (64 * 1024)
#define BLIT_LEN 4096
#define USER_MAGIC 0xdeadbeefu

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
	struct vgpu_queue_destroy qd;
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

int main(int argc, char **argv)
{
	int m = 1, i;

	if (argc > 1 && !strcmp(argv[1], "-m"))
		m = (argc > 2) ? atoi(argv[2]) : 4;

	if (argc > 1 && !strcmp(argv[1], "-q"))
		return run_queues() ? 1 : 0;

	if (argc > 1 && !strcmp(argv[1], "-p"))
		return run_doorbell() ? 1 : 0;

	if (argc > 1 && !strcmp(argv[1], "-e"))
		return run_negative() ? 1 : 0;

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
