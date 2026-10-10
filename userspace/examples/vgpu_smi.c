/*
 * vgpu_smi - observability for the vgpu driver ("nvidia-smi lite").
 *
 * One command, one screen: what the device is doing, what each live
 * stream is doing, what the compute engine has executed, and what is
 * still in flight. Everything comes from the driver's own debugfs
 * files - the same source of truth the kernel prints:
 *
 *   /sys/kernel/debug/vgpu/stats     device counters + per-engine scratch
 *   /sys/kernel/debug/vgpu/queues    every live queue, any fd
 *   /sys/kernel/debug/vgpu/compute   kernel / block / warp / coalescing
 *   /sys/kernel/debug/vgpu/pending   submissions waiting or executing
 *   /sys/kernel/debug/vgpu/regs      the virtual BAR0 register file
 *
 * The contract: the tool invents no numbers. Every value it prints is
 * either a verbatim debugfs counter or an arithmetic combination of
 * them, and the "raw counters" block at the bottom of the report
 * restates each parsed key as "key=value" so the report can be diffed
 * against a plain `cat` of the same files. An observability tool whose
 * numbers disagree with the kernel's would be worse than none - that
 * is the whole point of this phase.
 *
 * Usage:
 *   vgpu_smi            print the report (pure observer: no ioctl, no
 *                       side effect on the device)
 *   vgpu_smi --demo     create three short-lived streams (one per
 *                       engine), submit one packet batch on each, and
 *                       report while they are still open. A teaching
 *                       aid: it does add the submissions it reports
 *                       (+3 submitted / +3 completed on an idle
 *                       device), and closes the streams before exiting.
 *   vgpu_smi --help     usage
 *
 * Exit status: 0 when a report was printed (that includes the "driver
 * not loaded" report), 1 on usage errors, 2 on internal failures.
 *
 * Environment:
 *   VGPU_SMI_DEBUGFS   debugfs directory to read (default
 *                      /sys/kernel/debug/vgpu); point it at recorded
 *                      driver output to exercise the parser.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "vgpu_drm.h"

#define SMI_DIR_DEFAULT	"/sys/kernel/debug/vgpu"
#define SMI_ENGINES	3
#define SMI_MAX_QUEUES	64
#define SMI_MAX_PENDING	64
#define SMI_MAX_PROCS	32

static const char *const smi_engines[SMI_ENGINES] = { "gfx", "copy", "compute" };
static const char *const smi_prios[3] = { "low", "normal", "high" };

/* ------------------------------------------------------------- snapshot */

struct smi_queue {
	uint32_t qid;
	uint32_t prio;
	uint32_t ring_used, ring_size;
	uint64_t sub, done, err;
	char engine[16];
};

struct smi_pend {
	bool inflight;		/* false: engine_pending, true: in_flight */
	uint32_t seqno, ncmds;
	int err;
	char engine[16];
};

struct smi_snap {
	bool have_stats, have_queues, have_compute, have_pending, have_regs;
	int nfiles;

	/* stats */
	uint64_t submitted, completed, errors;
	uint32_t scratch_used[SMI_ENGINES], scratch_size[SMI_ENGINES];
	bool scratch_seen[SMI_ENGINES];
	uint32_t backlog[SMI_ENGINES];
	bool backlog_seen[SMI_ENGINES];

	/* queues */
	int nqueues;
	struct smi_queue queues[SMI_MAX_QUEUES];

	/* compute */
	uint64_t cs_kernels, cs_blocks, cs_warps, cs_threads, cs_waves;
	uint64_t cs_instrs, cs_mem_ops, cs_bcast, cs_coal, cs_strided, cs_lat;
	bool cs_per_thread_seen;
	uint32_t cs_per_thread_int, cs_per_thread_dec;

	/* pending */
	bool pending_none;
	int npend;
	struct smi_pend pend[SMI_MAX_PENDING];

	/* regs */
	bool regs_seen;
	uint32_t r_status, r_seqno, r_error, r_user;
};

struct smi_proc {
	int pid;
	int fd;
	char comm[16];
	char node[32];
};

/* the live state --demo creates, destroyed after the report is printed */
struct smi_demo {
	int fd;
	uint32_t qid[SMI_ENGINES];
};

/* ------------------------------------------------------- file plumbing */

static char *smi_read_file(const char *path)
{
	int fd;
	char *buf;
	size_t len = 0, cap = 512;
	ssize_t n;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return NULL;
	buf = malloc(cap);
	if (!buf) {
		close(fd);
		return NULL;
	}
	for (;;) {
		if (len + 64 + 1 > cap) {
			char *nb = realloc(buf, cap * 2);

			if (!nb) {
				free(buf);
				close(fd);
				return NULL;
			}
			buf = nb;
			cap *= 2;
		}
		n = read(fd, buf + len, cap - len - 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			free(buf);
			close(fd);
			return NULL;
		}
		if (n == 0)
			break;
		len += (size_t)n;
	}
	close(fd);
	buf[len] = '\0';
	return buf;
}

static char *smi_next_line(char **cur)
{
	char *p = *cur, *nl;

	if (!p || !*p)
		return NULL;
	nl = strchr(p, '\n');
	if (nl) {
		*nl = '\0';
		*cur = nl + 1;
	} else {
		*cur = p + strlen(p);
	}
	return p;
}

/* "gfx     " (the driver pads engine names with %-8s) -> "gfx"; -1 if
 * the name is not an engine we know */
static int smi_engine_idx(const char *name)
{
	char clean[16];
	size_t i, n = 0;

	while (*name == ' ' || *name == '\t')
		name++;
	for (i = 0; name[i] && n + 1 < sizeof(clean); i++)
		if (name[i] != ' ' && name[i] != '\t')
			clean[n++] = name[i];
	clean[n] = '\0';
	for (i = 0; i < SMI_ENGINES; i++)
		if (!strcmp(clean, smi_engines[i]))
			return (int)i;
	return -1;
}

/* -------------------------------------------------------------- parsers */

static void smi_parse_stats(char *buf, struct smi_snap *s)
{
	char *line;
	unsigned long long v;
	uint32_t a, b;
	char name[64];

	s->have_stats = true;
	while ((line = smi_next_line(&buf))) {
		int e;

		if (sscanf(line, "submitted %llu", &v) == 1)
			s->submitted = v;
		else if (sscanf(line, "completed %llu", &v) == 1)
			s->completed = v;
		else if (sscanf(line, "errors %llu", &v) == 1)
			s->errors = v;
		else if (sscanf(line, "scratch[%63[^]]] %u / %u bytes",
				name, &a, &b) == 3) {
			e = smi_engine_idx(name);
			if (e >= 0) {
				s->scratch_used[e] = a;
				s->scratch_size[e] = b;
				s->scratch_seen[e] = true;
			}
		} else if (sscanf(line, "engine[%63[^]]] backlog %u",
				  name, &a) == 2) {
			e = smi_engine_idx(name);
			if (e >= 0) {
				s->backlog[e] = a;
				s->backlog_seen[e] = true;
			}
		}
	}
}

static void smi_parse_queues(char *buf, struct smi_snap *s)
{
	char *line;
	uint32_t qid, prio, used, size;
	unsigned long long sub, done, err;
	char eng[64];

	s->have_queues = true;
	while ((line = smi_next_line(&buf))) {
		struct smi_queue *q;

		if (s->nqueues >= SMI_MAX_QUEUES)
			break;
		if (sscanf(line,
			   "q#%u engine=%63s prio=%u ring %u/%u B sub=%llu done=%llu err=%llu",
			   &qid, eng, &prio, &used, &size, &sub, &done, &err) != 8)
			continue;
		q = &s->queues[s->nqueues++];
		q->qid = qid;
		q->prio = prio;
		q->ring_used = used;
		q->ring_size = size;
		q->sub = sub;
		q->done = done;
		q->err = err;
		snprintf(q->engine, sizeof(q->engine), "%.15s", eng);
	}
}

static void smi_parse_compute(char *buf, struct smi_snap *s)
{
	char *line;
	unsigned long long v, tot;
	uint32_t i1, i2;

	s->have_compute = true;
	while ((line = smi_next_line(&buf))) {
		if (sscanf(line, "kernels %llu", &v) == 1)
			s->cs_kernels = v;
		else if (sscanf(line, "blocks %llu", &v) == 1)
			s->cs_blocks = v;
		else if (sscanf(line, "warps %llu", &v) == 1)
			s->cs_warps = v;
		else if (sscanf(line, "threads %llu", &v) == 1)
			s->cs_threads = v;
		else if (sscanf(line, "waves %llu", &v) == 1)
			s->cs_waves = v;
		else if (sscanf(line, "instrs %llu (%u.%u per thread)",
				&v, &i1, &i2) == 3) {
			s->cs_instrs = v;
			s->cs_per_thread_seen = true;
			s->cs_per_thread_int = i1;
			s->cs_per_thread_dec = i2;
		} else if (sscanf(line, "mem_ops %llu", &v) == 1)
			s->cs_mem_ops = v;
		else if (sscanf(line, "broadcast warps %llu / %llu", &v, &tot) == 2)
			s->cs_bcast = v;
		else if (sscanf(line, "coalesced warps %llu / %llu", &v, &tot) == 2)
			s->cs_coal = v;
		else if (sscanf(line, "strided warps %llu / %llu", &v, &tot) == 2)
			s->cs_strided = v;
		else if (sscanf(line, "modelled mem latency %llu ns", &v) == 1)
			s->cs_lat = v;
	}
}

static void smi_parse_pending(char *buf, struct smi_snap *s)
{
	char *line;
	uint32_t seq, ncmds;
	int err = 0;
	char eng[64];

	s->have_pending = true;
	while ((line = smi_next_line(&buf))) {
		struct smi_pend *p;

		if (!strncmp(line, "(none)", 6)) {
			s->pending_none = true;
			continue;
		}
		if (s->npend >= SMI_MAX_PENDING)
			break;
		p = &s->pend[s->npend];
		memset(p, 0, sizeof(*p));
		if (sscanf(line, "pending seq#%u ncmds=%u engine=%63s",
			   &seq, &ncmds, eng) == 3) {
			p->inflight = false;
			snprintf(p->engine, sizeof(p->engine), "%.15s", eng);
		} else if (sscanf(line, "in-flight seq#%u ncmds=%u err=%d",
				  &seq, &ncmds, &err) == 3) {
			p->inflight = true;
			p->err = err;
		} else {
			continue;
		}
		p->seqno = seq;
		p->ncmds = ncmds;
		s->npend++;
	}
}

static void smi_parse_regs(char *buf, struct smi_snap *s)
{
	char *line;
	int hex;
	unsigned int v;

	s->have_regs = true;
	s->regs_seen = true;
	while ((line = smi_next_line(&buf))) {
		if (sscanf(line, "status = %i", &hex) == 1)
			s->r_status = (uint32_t)hex;
		else if (sscanf(line, "seqno = %u", &v) == 1)
			s->r_seqno = v;
		else if (sscanf(line, "error = %u", &v) == 1)
			s->r_error = v;
		else if (sscanf(line, "user = %i", &hex) == 1)
			s->r_user = (uint32_t)hex;
	}
}

static void smi_snapshot(const char *dir, struct smi_snap *s)
{
	char path[512];
	char *buf;

	memset(s, 0, sizeof(*s));

	snprintf(path, sizeof(path), "%s/stats", dir);
	if ((buf = smi_read_file(path))) {
		smi_parse_stats(buf, s);
		free(buf);
		s->nfiles++;
	}
	snprintf(path, sizeof(path), "%s/queues", dir);
	if ((buf = smi_read_file(path))) {
		smi_parse_queues(buf, s);
		free(buf);
		s->nfiles++;
	}
	snprintf(path, sizeof(path), "%s/compute", dir);
	if ((buf = smi_read_file(path))) {
		smi_parse_compute(buf, s);
		free(buf);
		s->nfiles++;
	}
	snprintf(path, sizeof(path), "%s/pending", dir);
	if ((buf = smi_read_file(path))) {
		smi_parse_pending(buf, s);
		free(buf);
		s->nfiles++;
	}
	snprintf(path, sizeof(path), "%s/regs", dir);
	if ((buf = smi_read_file(path))) {
		smi_parse_regs(buf, s);
		free(buf);
		s->nfiles++;
	}
}

/* ------------------------------------------------------ process scanner */

/*
 * Who holds the device open? Queues are per-fd state, so this is as
 * close as /proc gets to nvidia-smi's process table: the node each
 * process has open. Per-context accounting of queues would need a
 * driver-side feature (phase I's per-context billing); /proc cannot
 * see inside an fd. Processes that exit mid-scan simply vanish.
 */
static int smi_scan_processes(struct smi_proc *out, int max)
{
	DIR *d;
	struct dirent *e;
	int n = 0;

	d = opendir("/proc");
	if (!d)
		return 0;
	while ((e = readdir(d)) && n < max) {
		char fddir[512], commpath[600], comm[64], link[512];
		DIR *fds;
		struct dirent *fe;
		int cfd;
		ssize_t r;

		if (e->d_name[0] < '0' || e->d_name[0] > '9')
			continue;
		comm[0] = '\0';
		snprintf(commpath, sizeof(commpath), "/proc/%s/comm",
			 e->d_name);
		cfd = open(commpath, O_RDONLY);
		if (cfd >= 0) {
			r = read(cfd, comm, sizeof(comm) - 1);
			close(cfd);
			if (r > 0) {
				char *nl;

				comm[r] = '\0';
				nl = strchr(comm, '\n');
				if (nl)
					*nl = '\0';
			} else {
				comm[0] = '\0';
			}
		}
		snprintf(fddir, sizeof(fddir), "/proc/%s/fd", e->d_name);
		fds = opendir(fddir);
		if (!fds)
			continue;
		while ((fe = readdir(fds)) && n < max) {
			char fdpath[1024];

			if (fe->d_name[0] == '.')
				continue;
			snprintf(fdpath, sizeof(fdpath), "%s/%s", fddir,
				 fe->d_name);
			r = readlink(fdpath, link, sizeof(link) - 1);
			if (r <= 0)
				continue;
			link[r] = '\0';
			if (strncmp(link, "/dev/dri/", 9))
				continue;
			out[n].pid = atoi(e->d_name);
			out[n].fd = atoi(fe->d_name);
			snprintf(out[n].node, sizeof(out[n].node), "%.31s", link);
			snprintf(out[n].comm, sizeof(out[n].comm), "%.15s",
				 comm[0] ? comm : "?");
			n++;
		}
		closedir(fds);
	}
	closedir(d);
	return n;
}

/* ------------------------------------------------------------- printing */

static void smi_head(const char *title)
{
	int i, n = 74 - (int)strlen(title);

	printf("== %s ", title);
	for (i = 0; i < n; i++)
		putchar('-');
	putchar('\n');
}

static uint64_t smi_inflight(const struct smi_snap *s)
{
	/* every accepted submission is finished exactly once (error or
	 * not), so this derived number must equal the pending list */
	if (s->submitted < s->completed + s->errors)
		return 0;
	return s->submitted - s->completed - s->errors;
}

static void smi_print(const struct smi_snap *s, const char *dir,
		      const struct smi_proc *procs, int nprocs)
{
	int i;
	time_t t = time(NULL);

	printf("vgpu-smi-lite: %s", ctime(&t));	/* ctime() ends in \n */
	printf("source: %s (%d/5 debugfs files readable)\n", dir, s->nfiles);

	smi_head("device");
	if (s->have_stats) {
		printf("submissions   submitted %llu   completed %llu   "
		       "errors %llu   in flight %llu\n",
		       (unsigned long long)s->submitted,
		       (unsigned long long)s->completed,
		       (unsigned long long)s->errors,
		       (unsigned long long)smi_inflight(s));
		printf("              (in flight is derived: submitted - "
		       "completed - errors)\n");
	} else {
		printf("submissions   (debugfs 'stats' not readable)\n");
	}
	for (i = 0; i < SMI_ENGINES; i++) {
		if (i == 0)
			printf("engines       ");
		else
			printf("              ");
		if (s->scratch_seen[i])
			printf("%-8s scratch %5u / %-5u B   ",
			       smi_engines[i], s->scratch_used[i],
			       s->scratch_size[i]);
		else
			printf("%-8s scratch %14s   ", smi_engines[i], "n/a");
		if (s->backlog_seen[i])
			printf("backlog %u\n", s->backlog[i]);
		else
			printf("backlog n/a\n");
	}
	if (s->regs_seen)
		printf("registers     status 0x%08x   seqno %u   error %u   "
		       "user 0x%08x\n", s->r_status, s->r_seqno, s->r_error,
		       s->r_user);
	else
		printf("registers     (debugfs 'regs' not readable)\n");

	smi_head("processes (holding a /dev/dri/* node)");
	if (nprocs <= 0) {
		printf("(none right now: no process has a vgpu device node open)\n");
	} else {
		printf("%-7s %-16s %-22s %s\n", "PID", "COMM", "DEVICE", "FD");
		for (i = 0; i < nprocs; i++)
			printf("%-7d %-16s %-22s %d\n", procs[i].pid,
			       procs[i].comm, procs[i].node, procs[i].fd);
	}

	smi_head("queues (live streams; a queue lives inside an fd)");
	if (!s->have_queues) {
		printf("(debugfs 'queues' not readable)\n");
	} else if (s->nqueues == 0) {
		printf("(no live queues: every fd that created one has closed)\n");
	} else {
		printf("%-5s %-8s %-9s %-14s %-10s %-6s %s\n",
		       "QID", "ENGINE", "PRIO", "RING USED/SIZE",
		       "SUBMITTED", "DONE", "ERRORS");
		for (i = 0; i < s->nqueues; i++) {
			const struct smi_queue *q = &s->queues[i];
			char ring[32];

			snprintf(ring, sizeof(ring), "%u/%u B", q->ring_used,
				 q->ring_size);
			printf("%-5u %-8s %-9s %-14s %-10llu %-6llu %llu\n",
			       q->qid, q->engine,
			       q->prio < 3 ? smi_prios[q->prio] : "?",
			       ring,
			       (unsigned long long)q->sub,
			       (unsigned long long)q->done,
			       (unsigned long long)q->err);
		}
		printf("(submitted / done / errors count doorbell batches, "
		       "not packets)\n");
	}

	smi_head("compute (cumulative since insmod)");
	if (!s->have_compute) {
		printf("(debugfs 'compute' not readable)\n");
	} else if (!s->cs_kernels) {
		printf("(no kernel launched yet)\n");
	} else {
		uint64_t warp_accesses, single, tot;
		uint32_t pct10, avg10;

		printf("kernels %llu    blocks %llu    warps %llu    "
		       "threads %llu    waves %llu\n",
		       (unsigned long long)s->cs_kernels,
		       (unsigned long long)s->cs_blocks,
		       (unsigned long long)s->cs_warps,
		       (unsigned long long)s->cs_threads,
		       (unsigned long long)s->cs_waves);
		printf("instructions   %llu total   ",
		       (unsigned long long)s->cs_instrs);
		if (s->cs_threads) {
			uint32_t my_int = (uint32_t)(s->cs_instrs / s->cs_threads);
			uint32_t my_dec = (uint32_t)((s->cs_instrs * 10 / s->cs_threads) % 10);

			printf("%u.%u per thread", my_int, my_dec);
			if (s->cs_per_thread_seen &&
			    (my_int != s->cs_per_thread_int ||
			     my_dec != s->cs_per_thread_dec))
				printf("   MISMATCH: the kernel file says %u.%u",
				       s->cs_per_thread_int,
				       s->cs_per_thread_dec);
			printf("\n");
		} else {
			printf("n/a\n");
		}
		printf("memory         mem_ops %llu (per-thread accesses)\n",
		       (unsigned long long)s->cs_mem_ops);
		warp_accesses = s->cs_bcast + s->cs_coal + s->cs_strided;
		single = s->cs_bcast + s->cs_coal;
		printf("               warp accesses %llu: broadcast %llu, "
		       "coalesced %llu, strided %llu\n",
		       (unsigned long long)warp_accesses,
		       (unsigned long long)s->cs_bcast,
		       (unsigned long long)s->cs_coal,
		       (unsigned long long)s->cs_strided);
		tot = warp_accesses;
		if (tot) {
			pct10 = (uint32_t)(single * 1000 / tot);
			printf("               single-transaction accesses %llu/%llu"
			       " (%u.%u%%)\n", (unsigned long long)single,
			       (unsigned long long)tot, pct10 / 10, pct10 % 10);
		}
		if (s->cs_mem_ops) {
			avg10 = (uint32_t)(s->cs_lat * 10 / s->cs_mem_ops);
			printf("latency        modelled memory latency %llu ns "
			       "(%u.%u ns per mem_op)\n",
			       (unsigned long long)s->cs_lat,
			       avg10 / 10, avg10 % 10);
		} else {
			printf("latency        modelled memory latency %llu ns\n",
			       (unsigned long long)s->cs_lat);
		}
	}

	smi_head("in flight");
	if (!s->have_pending) {
		printf("(debugfs 'pending' not readable)\n");
	} else if (s->npend == 0) {
		printf("(idle: nothing pending, nothing in flight)\n");
		if (s->have_stats && smi_inflight(s))
			printf("(note: 'stats' implies %llu in flight but the "
			       "list is empty - a submission completed between "
			       "the two reads)\n",
			       (unsigned long long)smi_inflight(s));
	} else {
		for (i = 0; i < s->npend; i++) {
			const struct smi_pend *p = &s->pend[i];

			if (p->inflight)
				printf("in-flight seq#%u ncmds=%u err=%d\n",
				       p->seqno, p->ncmds, p->err);
			else
				printf("pending   seq#%u ncmds=%u engine=%s\n",
				       p->seqno, p->ncmds, p->engine);
		}
		if (s->have_stats && smi_inflight(s) != (uint64_t)s->npend)
			printf("(note: 'stats' implies %llu in flight vs %d "
			       "listed - the counters moved between the two "
			       "file reads)\n",
			       (unsigned long long)smi_inflight(s), s->npend);
	}

	/*
	 * The cross-validation payload: every parsed key, one per line,
	 * "key=value", so it can be diffed against a plain `cat` of the
	 * same debugfs files.
	 */
	smi_head("raw counters (what the tool parsed from debugfs)");
	printf("raw files present: %s%s%s%s%s (%d/5)\n",
	       s->have_stats ? "stats " : "",
	       s->have_queues ? "queues " : "",
	       s->have_compute ? "compute " : "",
	       s->have_pending ? "pending " : "",
	       s->have_regs ? "regs" : "", s->nfiles);
	if (s->have_stats) {
		printf("raw stats submitted=%llu\n",
		       (unsigned long long)s->submitted);
		printf("raw stats completed=%llu\n",
		       (unsigned long long)s->completed);
		printf("raw stats errors=%llu\n",
		       (unsigned long long)s->errors);
		for (i = 0; i < SMI_ENGINES; i++) {
			if (!s->scratch_seen[i])
				continue;
			printf("raw stats scratch[%s]=%u/%u\n", smi_engines[i],
			       s->scratch_used[i], s->scratch_size[i]);
		}
		for (i = 0; i < SMI_ENGINES; i++) {
			if (!s->backlog_seen[i])
				continue;
			printf("raw stats engine[%s].backlog=%u\n",
			       smi_engines[i], s->backlog[i]);
		}
	}
	if (s->have_queues) {
		printf("raw queues count=%d\n", s->nqueues);
		for (i = 0; i < s->nqueues; i++) {
			const struct smi_queue *q = &s->queues[i];

			printf("raw queues q#%u engine=%s prio=%u ring=%u/%u sub=%llu done=%llu err=%llu\n",
			       q->qid, q->engine, q->prio, q->ring_used,
			       q->ring_size, (unsigned long long)q->sub,
			       (unsigned long long)q->done,
			       (unsigned long long)q->err);
		}
	}
	if (s->have_compute) {
		printf("raw compute kernels=%llu\n",
		       (unsigned long long)s->cs_kernels);
		printf("raw compute blocks=%llu\n",
		       (unsigned long long)s->cs_blocks);
		printf("raw compute warps=%llu\n",
		       (unsigned long long)s->cs_warps);
		printf("raw compute threads=%llu\n",
		       (unsigned long long)s->cs_threads);
		printf("raw compute waves=%llu\n",
		       (unsigned long long)s->cs_waves);
		printf("raw compute instrs=%llu\n",
		       (unsigned long long)s->cs_instrs);
		printf("raw compute mem_ops=%llu\n",
		       (unsigned long long)s->cs_mem_ops);
		printf("raw compute warps_broadcast=%llu\n",
		       (unsigned long long)s->cs_bcast);
		printf("raw compute warps_coalesced=%llu\n",
		       (unsigned long long)s->cs_coal);
		printf("raw compute warps_strided=%llu\n",
		       (unsigned long long)s->cs_strided);
		printf("raw compute modelled_mem_latency_ns=%llu\n",
		       (unsigned long long)s->cs_lat);
	}
	if (s->have_pending) {
		printf("raw pending count=%d\n", s->npend);
		for (i = 0; i < s->npend; i++) {
			const struct smi_pend *p = &s->pend[i];

			if (p->inflight)
				printf("raw pending f=%u ncmds=%u err=%d\n",
				       p->seqno, p->ncmds, p->err);
			else
				printf("raw pending q=%u ncmds=%u engine=%s\n",
				       p->seqno, p->ncmds, p->engine);
		}
	}
	if (s->regs_seen) {
		printf("raw regs status=0x%08x\n", s->r_status);
		printf("raw regs seqno=%u\n", s->r_seqno);
		printf("raw regs error=%u\n", s->r_error);
		printf("raw regs user=0x%08x\n", s->r_user);
	}
}

/* ----------------------------------------------------------------- demo */

/*
 * --demo: make the live-stream view real. One queue per engine, one
 * doorbell batch each, every fence waited for; the caller reports
 * while the queues are still open, then closes the fd (postclose
 * drains and retires them - it flushes the bottom-half workqueue
 * first, so there is no "queue still busy" race with QUEUE_DESTROY).
 *
 * This intentionally perturbs the counters it then reports: an idle
 * device gains +3 submitted / +3 completed. That is why it is opt-in
 * and why the lines above the report say exactly what it did.
 */
static int smi_demo(struct smi_demo *d)
{
	static const struct {
		uint32_t engine;
		uint32_t prio;
	} spec[SMI_ENGINES] = {
		{ VGPU_ENGINE_GFX, 1 },
		{ VGPU_ENGINE_COPY, 0 },
		{ VGPU_ENGINE_COMPUTE, 2 },
	};
	int i, nok = 0;

	d->fd = -1;
	for (i = 0; i < SMI_ENGINES; i++)
		d->qid[i] = 0;

	d->fd = open("/dev/dri/renderD128", O_RDWR);
	if (d->fd < 0)
		d->fd = open("/dev/dri/card0", O_RDWR);
	if (d->fd < 0) {
		printf("note: --demo cannot create streams: no device node "
		       "(%s)\n", strerror(errno));
		return -1;
	}

	for (i = 0; i < SMI_ENGINES; i++) {
		struct vgpu_queue_create qc;
		struct vgpu_packet *pk;
		struct vgpu_doorbell db;
		struct vgpu_wait w;
		uint8_t *ring;

		memset(&qc, 0, sizeof(qc));
		qc.engine = spec[i].engine;
		qc.prio = spec[i].prio;
		qc.nslots = 16;
		if (ioctl(d->fd, DRM_IOCTL_VGPU_QUEUE_CREATE, &qc) < 0) {
			printf("note: --demo queue_create(%s) failed: %s\n",
			       smi_engines[i], strerror(errno));
			continue;
		}
		d->qid[i] = qc.qid;
		ring = mmap(NULL, qc.ring_size, PROT_READ | PROT_WRITE,
			    MAP_SHARED, d->fd, (off_t)qc.mmap_offset);
		if (ring == MAP_FAILED) {
			printf("note: --demo ring mmap failed: %s\n",
			       strerror(errno));
			continue;
		}
		/* one doorbell batch per stream: NOP + a register write
		 * (the DMA engine would reject the write, so it gets two
		 * NOPs instead - its command class is BLIT/NOP only) */
		pk = (struct vgpu_packet *)ring;
		memset(pk, 0, 3 * sizeof(*pk));
		pk[0].type = VGPU_PKT_NOP;
		if (spec[i].engine == VGPU_ENGINE_COPY) {
			pk[1].type = VGPU_PKT_NOP;
		} else {
			pk[1].type = VGPU_PKT_WRITE_REG;
			pk[1].reg.reg_off = 12;		/* the user register */
			pk[1].reg.reg_val = 0xfeed0001u;
		}
		memset(&db, 0, sizeof(db));
		db.qid = qc.qid;
		db.put = 2 * (uint32_t)sizeof(*pk);
		if (ioctl(d->fd, DRM_IOCTL_VGPU_DOORBELL, &db) < 0) {
			printf("note: --demo doorbell(q#%u) failed: %s\n",
			       qc.qid, strerror(errno));
			continue;
		}
		memset(&w, 0, sizeof(w));
		w.seqno = db.seqno;
		w.timeout_ms = 5000;
		if (ioctl(d->fd, DRM_IOCTL_VGPU_WAIT, &w) < 0 || w.status != 0) {
			printf("note: --demo wait(seq#%u) status=%d\n",
			       db.seqno, (int)w.status);
			continue;
		}
		printf("demo: q#%u (%s, prio %u) submitted one 2-packet batch "
		       "-> seq#%u\n", qc.qid, smi_engines[i], spec[i].prio,
		       db.seqno);
		nok++;
	}
	return nok ? 0 : -1;
}

/* ----------------------------------------------------------------- main */

static void smi_usage(void)
{
	printf("usage: vgpu_smi [--demo] [--help]\n"
	       "  (no args)  print the device report from debugfs; no ioctls,\n"
	       "             no side effects\n"
	       "  --demo     create three short-lived streams (one per engine),\n"
	       "             submit one packet batch on each and report while\n"
	       "             they are live; adds the submissions it reports,\n"
	       "             closes the streams before exiting\n"
	       "  --help     this text\n"
	       "environment:\n"
	       "  VGPU_SMI_DEBUGFS  debugfs directory to read "
	       "(default %s)\n"
	       "exit status: 0 a report was printed (including \"driver not\n"
	       "loaded\"), 1 usage error, 2 internal error\n",
	       SMI_DIR_DEFAULT);
}

int main(int argc, char **argv)
{
	const char *dir = getenv("VGPU_SMI_DEBUGFS");
	struct smi_snap snap;
	struct smi_proc procs[SMI_MAX_PROCS];
	struct smi_demo demo;
	int nprocs, i, want_demo = 0, demo_ok = -1;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			smi_usage();
			return 0;
		}
		if (!strcmp(argv[i], "--demo")) {
			want_demo = 1;
			continue;
		}
		fprintf(stderr, "vgpu_smi: unknown argument '%s'\n", argv[i]);
		smi_usage();
		return 1;
	}

	if (!dir || !*dir)
		dir = SMI_DIR_DEFAULT;

	if (access(dir, F_OK) != 0) {
		printf("vgpu-smi-lite: vgpu: not loaded\n");
		printf("source: %s does not exist\n", dir);
		if (access("/sys/kernel/debug", F_OK) != 0)
			printf("hint: debugfs is not mounted (mount -t debugfs "
			       "none /sys/kernel/debug)\n");
		else
			printf("hint: the module is not inserted "
			       "(insmod /root/vgpu.ko)\n");
		return 0;
	}

	if (want_demo)
		demo_ok = smi_demo(&demo);

	smi_snapshot(dir, &snap);
	nprocs = smi_scan_processes(procs, SMI_MAX_PROCS);
	smi_print(&snap, dir, procs, nprocs);

	if (want_demo && demo_ok == 0) {
		close(demo.fd);		/* postclose retires the streams */
		printf("demo: streams closed (queues above were live while "
		       "the report was taken)\n");
	}
	return 0;
}
