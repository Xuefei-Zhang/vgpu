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

int main(int argc, char **argv)
{
	int m = 1, i;

	if (argc > 1 && !strcmp(argv[1], "-m"))
		m = (argc > 2) ? atoi(argv[2]) : 4;

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
