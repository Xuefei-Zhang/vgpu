/*
 * dequant_kv — an LLM-serving-shaped kernel that actually runs here.
 *
 * The pattern is the one quantized inference runs on every decode step:
 * a per-channel scale/zero-point travels with the packed weights, and
 * the kernel must dequantize and accumulate into the KV cache in ONE
 * pass (a separate dequant kernel + a separate add kernel is two extra
 * launches and two extra DRAM round-trips — exactly what fusion
 * removes). This example runs that fused shape as a single launch.
 *
 *   CUDA                                              | vgpu
 *   --------------------------------------------------|--------------------
 *   __global__ void dequant_kv(...)                   | the ~18-instruction
 *                                                     |   program in code[]
 *   <<<grid, block>>>                                 | VGPU_PKT_LAUNCH
 *   q->scale[i], q->zp[i] (per-channel)               | header words
 *   out[i] += scale * q[i] + zp                       | LD out, MUL, ADD,
 *                                                     |   ADD, ST back
 *   slot->version++  (cache bookkeeping)              | second output region
 *   cudaDeviceSynchronize()                           | VGPU_WAIT
 *
 * WHAT THIS IS NOT: the real kernel reads PACKED INT8 (two values per
 * 32-bit word). vgpu's ISA has no shift and no mask, so bit extraction
 * is not expressible; the quantized values here are stored ONE PER WORD
 * (zero-extended INT8), which is what the example's "quantized region"
 * is. The fusion SHAPE — dequant + accumulate + bookkeeping in one
 * launch — is the point, and the shape is identical.
 *
 * Registers: r0 = global thread id (preloaded by the engine), r15 = 0
 * (set by the first instruction; used as the base for header loads).
 *
 * The argument buffer layout (word indices, all in ONE BO):
 *   0 : n            number of elements
 *   1 : scale        per-channel scale
 *   2 : zero_point
 *   3 : q_base       start of the quantized input
 *   4 : out_base     start of the KV cache (accumulated in place)
 *   5 : ver_base     start of the per-slot version counters
 *   6 : one          the constant 1 (there is no MOV-immediate-add)
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

#define N_ELEMS		512		/* KV slots to dequant-accumulate */
#define BLOCK		64		/* threads per block (multiple of 32) */
#define HDR_WORDS	8

static int check(int fd, void *arg, unsigned long nr, const char *what)
{
	if (ioctl(fd, nr, arg) < 0) {
		fprintf(stderr, "FAIL: %s: %s\n", what, strerror(errno));
		return -1;
	}
	return 0;
}

int main(void)
{
	int fd, rc = 1;
	int i;
	struct vgpu_bo_create bo;
	struct vgpu_queue_create qc;
	struct vgpu_module_load ml;
	struct vgpu_doorbell db;
	struct vgpu_wait w;
	struct vgpu_packet *pk;
	uint8_t *ring;
	uint32_t *m;
	uint32_t seqno;
	uint32_t grid = (N_ELEMS + BLOCK - 1) / BLOCK;

	/*
	 * The kernel: 18 instructions, one thread per KV slot.
	 *
	 *   r1  = scale            r9  = &out[i]
	 *   r2  = zero_point       r10 = out[i]        (the cache slot)
	 *   r3  = q_base           r11 = dequantized    = scale*q + zp
	 *   r4  = out_base         r12 = out[i] + r11   (the accumulation)
	 *   r5  = ver_base         r13 = &ver[i]
	 *   r6  = &q[i]            r14 = ver[i] + 1     (the bookkeeping)
	 *   r7  = q[i]
	 */
	static const struct vgpu_instr code[] = {
		{ .op = VGPU_ISA_MOVI, .dst = 15, .a = 0 },	/* r15 = 0 */
		{ .op = VGPU_ISA_LD,   .dst = 1,  .a = 15, .b = 1 },  /* scale */
		{ .op = VGPU_ISA_LD,   .dst = 2,  .a = 15, .b = 2 },  /* zp */
		{ .op = VGPU_ISA_LD,   .dst = 3,  .a = 15, .b = 3 },  /* q_base */
		{ .op = VGPU_ISA_LD,   .dst = 4,  .a = 15, .b = 4 },  /* out_base */
		{ .op = VGPU_ISA_LD,   .dst = 5,  .a = 15, .b = 5 },  /* ver_base */
		{ .op = VGPU_ISA_LD,   .dst = 6,  .a = 15, .b = 6 },  /* the 1 */
		{ .op = VGPU_ISA_ADD,  .dst = 7,  .a = 3,  .b = 0 },  /* &q[i] */
		{ .op = VGPU_ISA_LD,   .dst = 8,  .a = 7,  .b = 0 },  /* q[i] */
		{ .op = VGPU_ISA_MUL,  .dst = 9,  .a = 1,  .b = 8 },  /* scale*q */
		{ .op = VGPU_ISA_ADD,  .dst = 10, .a = 9,  .b = 2 },  /* + zp */
		{ .op = VGPU_ISA_ADD,  .dst = 11, .a = 4,  .b = 0 },  /* &out[i] */
		{ .op = VGPU_ISA_LD,   .dst = 12, .a = 11, .b = 0 },  /* out[i] */
		{ .op = VGPU_ISA_ADD,  .dst = 13, .a = 12, .b = 10 }, /* accumulate */
		{ .op = VGPU_ISA_ST,   .dst = 13, .a = 11, .b = 0 },  /* store */
		{ .op = VGPU_ISA_ADD,  .dst = 14, .a = 5,  .b = 0 },  /* &ver[i] */
		{ .op = VGPU_ISA_LD,   .dst = 12, .a = 14, .b = 0 },  /* ver[i] */
		{ .op = VGPU_ISA_ADD,  .dst = 13, .a = 12, .b = 6 },  /* + 1 */
		{ .op = VGPU_ISA_ST,   .dst = 13, .a = 14, .b = 0 },  /* store */
		{ .op = VGPU_ISA_EXIT },
	};

	fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0)
		fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "FAIL: open: %s\n", strerror(errno));
		return 1;
	}

	/* ---- the kernel (the "module load") -------------------------- */
	memset(&ml, 0, sizeof(ml));
	ml.nregs = 16;
	ml.smem = 0;
	ml.ninstr = sizeof(code) / sizeof(code[0]);
	ml.code = (uint64_t)(uintptr_t)code;
	if (check(fd, &ml, DRM_IOCTL_VGPU_MODULE_LOAD, "module_load"))
		goto out;
	printf("dequant_kv: %u instructions, grid=%u block=%u (%d slots)\n",
	       ml.ninstr, grid, BLOCK, N_ELEMS);

	/* ---- one argument buffer: header + q + cache + versions ------ */
	memset(&bo, 0, sizeof(bo));
	bo.size = 16384;	/* 4 pages: room for 3 regions of 512 words */
	if (check(fd, &bo, DRM_IOCTL_VGPU_BO_CREATE, "bo_create"))
		goto out;
	m = mmap(NULL, bo.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		 fd, (off_t)bo.mmap_offset);
	if (m == MAP_FAILED) {
		fprintf(stderr, "FAIL: mmap: %s\n", strerror(errno));
		goto out;
	}
	memset(m, 0, bo.size);

	m[0] = N_ELEMS;
	m[1] = 3;			/* scale */
	m[2] = 7;			/* zero_point */
	m[3] = HDR_WORDS;		/* q_base */
	m[4] = HDR_WORDS + N_ELEMS;	/* out_base (the KV cache) */
	m[5] = HDR_WORDS + 2 * N_ELEMS;	/* ver_base */
	m[6] = 1;			/* the constant 1 */
	for (i = 0; i < N_ELEMS; i++) {
		m[HDR_WORDS + i] = i + 1;		/* the "INT8" value */
		m[HDR_WORDS + 2 * N_ELEMS + i] = 100;	/* version starts at 100 */
	}
	/* preload the cache with 1000 per slot, so the accumulation is visible */
	for (i = 0; i < N_ELEMS; i++)
		m[HDR_WORDS + N_ELEMS + i] = 1000;

	/* ---- a compute stream ---------------------------------------- */
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

	/* ---- ONE launch: dequant + accumulate + bookkeeping ---------- */
	memset(pk, 0, sizeof(*pk));
	pk[0].type = VGPU_PKT_LAUNCH;
	pk[0].launch.kid = ml.kid;
	pk[0].launch.grid = grid;
	pk[0].launch.block = BLOCK;
	pk[0].launch.args_bo = bo.handle;
	pk[0].launch.args_off = 0;

	memset(&db, 0, sizeof(db));
	db.qid = qc.qid;
	db.put = sizeof(*pk);
	if (check(fd, &db, DRM_IOCTL_VGPU_DOORBELL, "doorbell"))
		goto out;
	seqno = db.seqno;
	printf("dequant_kv: doorbell -> seq#%u\n", seqno);

	memset(&w, 0, sizeof(w));
	w.seqno = seqno;
	w.timeout_ms = 5000;
	if (check(fd, &w, DRM_IOCTL_VGPU_WAIT, "wait"))
		goto out;
	if (w.status != 0) {
		fprintf(stderr, "FAIL: wait status=%d\n", (int)w.status);
		goto out;
	}

	/* ---- verify: every slot accumulated its dequantized value ---- */
	for (i = 0; i < N_ELEMS; i++) {
		uint32_t q = m[HDR_WORDS + i];
		uint32_t want = 1000 + m[1] * q + m[2];
		uint32_t got = m[HDR_WORDS + N_ELEMS + i];

		if (got != want) {
			fprintf(stderr,
				"FAIL: slot %d: cache=%u want=%u (q=%u)\n",
				i, got, want, q);
			goto out;
		}
		if (m[HDR_WORDS + 2 * N_ELEMS + i] != 101) {
			fprintf(stderr, "FAIL: slot %d version=%u want 101\n",
				i, m[HDR_WORDS + 2 * N_ELEMS + i]);
			goto out;
		}
	}
	printf("PASS: fused dequant+accumulate+bookkeeping: %d slots x "
	       "scale=%u zp=%u, one launch, all correct (versions 100 -> 101)\n",
	       N_ELEMS, m[1], m[2]);
	printf("PASS: 3 memory regions touched per slot (q, cache, version) "
	       "in a single kernel — the fusion a real serving stack wants\n");
	rc = 0;

out:
	close(fd);
	return rc;
}
