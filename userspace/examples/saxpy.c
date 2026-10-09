/*
 * saxpy on vgpu: the "CUDA example" of this driver.
 *
 * Each step mirrors what a CUDA program does, with the mapping noted:
 *
 *   CUDA                              | vgpu
 *   ----------------------------------|---------------------------
 *   cudaMalloc(sizeof(float) * n)     | VGPU_BO_CREATE (a shmem BO)
 *   cudaMemcpy(h_in, d_in, ..., H2D)  | mmap the BO and write it
 *   __global__ void saxpy(...)        | the tiny ISA program in code[]
 *   cudaLaunchKernel(grid, block)     | a LAUNCH packet + doorbell
 *   cudaDeviceSynchronize()           | VGPU_WAIT (kernel-side fence)
 *   read h_out                        | read the mmap'ed BO back
 *
 * SAXPY: out[i] = a * in[i] + b      (i = thread id)
 *
 * Register/ISA reference: r0 = thread id, r15 = 0. The argument BO is
 * word-addressed: word 0 = N, 1 = a, 2 = b, 3 = in_base, 4 = out_base
 * (in/out_base are word indices into the same BO).
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

#define N_ELEMS		4096		/* elements per array */
#define BLOCK		64		/* threads per block (multiple of 32) */
#define PARAM_WORDS	8		/* header of the argument buffer */

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
	struct vgpu_bo_create bo;
	struct vgpu_queue_create qc;
	struct vgpu_module_load ml;
	struct vgpu_doorbell db;
	struct vgpu_wait w;
	struct vgpu_packet *pk;
	uint8_t *ring;
	uint32_t *args;
	uint32_t seqno = 0;
	int i;

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

	fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0)
		fd = open("/dev/dri/card0", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "FAIL: open: %s\n", strerror(errno));
		return 1;
	}

	/* ---- "cudaMalloc": one argument buffer holding params+in+out --- */
	memset(&bo, 0, sizeof(bo));
	bo.size = (PARAM_WORDS + 2 * N_ELEMS) * 4;
	if (check(fd, &bo, DRM_IOCTL_VGPU_BO_CREATE, "bo_create"))
		goto out;
	args = mmap(NULL, bo.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		    fd, (off_t)bo.mmap_offset);
	if (args == MAP_FAILED) {
		fprintf(stderr, "FAIL: mmap: %s\n", strerror(errno));
		goto out;
	}

	/* the parameters each thread reads */
	args[0] = N_ELEMS;
	args[1] = 3;				/* a */
	args[2] = 7;				/* b */
	args[3] = PARAM_WORDS;			/* in[] word index */
	args[4] = PARAM_WORDS + N_ELEMS;		/* out[] word index */
	for (i = 0; i < N_ELEMS; i++) {
		args[PARAM_WORDS + i] = i * 2 + 1;			/* in[i] */
		args[PARAM_WORDS + N_ELEMS + i] = 0xdeadbeef;		/* poison */
	}
	printf("saxpy: a=%u b=%u n=%u (out[i] = a*in[i] + b)\n",
	       args[1], args[2], args[0]);

	/* ---- "module load": the kernel ---------------------------------- */
	memset(&ml, 0, sizeof(ml));
	ml.nregs = 16;
	ml.smem = 0;
	ml.ninstr = sizeof(code) / sizeof(code[0]);
	ml.code = (uint64_t)(uintptr_t)code;
	if (check(fd, &ml, DRM_IOCTL_VGPU_MODULE_LOAD, "module_load"))
		goto out;

	/* ---- a compute stream ------------------------------------------- */
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

	/* ---- "cudaLaunchKernel": one LAUNCH packet --------------------- */
	memset(pk, 0, sizeof(*pk));
	pk[0].type = VGPU_PKT_LAUNCH;
	pk[0].launch.kid = ml.kid;
	pk[0].launch.grid = (N_ELEMS + BLOCK - 1) / BLOCK;
	pk[0].launch.block = BLOCK;
	pk[0].launch.args_bo = bo.handle;
	pk[0].launch.args_off = 0;

	memset(&db, 0, sizeof(db));
	db.qid = qc.qid;
	db.put = sizeof(*pk);
	if (check(fd, &db, DRM_IOCTL_VGPU_DOORBELL, "doorbell"))
		goto out;
	seqno = db.seqno;
	printf("launch: grid=%u block=%u -> seq#%u\n",
	       pk[0].launch.grid, pk[0].launch.block, seqno);

	/* ---- "cudaDeviceSynchronize" ------------------------------------ */
	memset(&w, 0, sizeof(w));
	w.seqno = seqno;
	w.timeout_ms = 5000;
	if (check(fd, &w, DRM_IOCTL_VGPU_WAIT, "wait"))
		goto out;
	if (w.status != 0) {
		fprintf(stderr, "FAIL: launch status=%d\n", (int)w.status);
		goto out;
	}

	/* ---- verify: the "device" computed out[i] = a*in[i] + b --------- */
	for (i = 0; i < N_ELEMS; i++) {
		uint32_t want = args[1] * args[PARAM_WORDS + i] + args[2];
		uint32_t got = args[PARAM_WORDS + N_ELEMS + i];

		if (got != want) {
			fprintf(stderr,
				"FAIL: out[%d] = %u, want %u\n",
				i, got, want);
			goto out;
		}
	}
	printf("PASS: saxpy correct on the compute engine "
	       "(%d elements verified)\n", N_ELEMS);
	rc = 0;

out:
	close(fd);
	return rc;
}
