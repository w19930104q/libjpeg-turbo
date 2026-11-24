/*
 * albumbatch.c — Atomic single-call crop + orientation batch processor.
 *
 * The old album pipeline ran orientation and cropping as two separate
 * jpegtran invocations through a cache file.  If the second step failed,
 * the half-transformed cache image was left behind and later mistaken for
 * a finished result.
 *
 * This tool performs the crop and the geometric transform together in a
 * single tj3Transform() call (TJXOPT_CROP is combined with one TJXOP in the
 * same tjtransform structure), and it never writes the destination in place:
 *
 *   1. The complete output JPEG is generated in memory.
 *   2. It is written to a temporary file in the same directory as the
 *      destination.
 *   3. The temporary file is fsync()'d and atomically renamed over the
 *      destination (rename(2) is atomic on POSIX).
 *
 * Thus a failure at any point before the rename leaves the previous
 * destination byte-for-byte untouched.  Re-running the same command
 * reproduces the same bytes, because the transform is a pure function of
 * the source and the entropy-coding parameters are fully specified.
 *
 * --fail-point is a test hook that aborts at a chosen stage before the
 * atomic rename, simulating the historical mid-batch failure.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "turbojpeg.h"

/* Failure-injection points (test only). */
enum fail_point {
	FAIL_NONE = 0,   /* complete the full pipeline */
	FAIL_TRANSFORM,  /* fail before tj3Transform() runs */
	FAIL_WRITE       /* write the temp file, then fail before the rename */
};

struct job {
	const char *src;
	const char *dst;
};

static const char *progname;

static void usage(void)
{
	fprintf(stderr,
		"Usage: %s [options] -op {none|hflip|vflip|transpose|transverse|\n"
		"          rot90|rot180|rot270} [-crop WxH+X+Y] [-trim] [-perfect]\n"
		"          -o OUTPUT INPUT [-o OUTPUT INPUT ...]\n\n"
		"Crop and orientation are applied in ONE lossless transform call per\n"
		"image, and each destination is replaced atomically (temp file +\n"
		"fsync + rename).\n\n"
		"Options (test hooks):\n"
		"  --fail-point transform|write  abort before the named stage\n",
		progname);
	exit(1);
}

static int parse_op(const char *s, int *op)
{
	if (!strcmp(s, "none")) *op = TJXOP_NONE;
	else if (!strcmp(s, "hflip")) *op = TJXOP_HFLIP;
	else if (!strcmp(s, "vflip")) *op = TJXOP_VFLIP;
	else if (!strcmp(s, "transpose")) *op = TJXOP_TRANSPOSE;
	else if (!strcmp(s, "transverse")) *op = TJXOP_TRANSVERSE;
	else if (!strcmp(s, "rot90")) *op = TJXOP_ROT90;
	else if (!strcmp(s, "rot180")) *op = TJXOP_ROT180;
	else if (!strcmp(s, "rot270")) *op = TJXOP_ROT270;
	else return -1;
	return 0;
}

static int parse_crop(const char *s, tjregion *r)
{
	char sep;

	if (sscanf(s, "%d%c%d+%d+%d", &r->w, &sep, &r->h, &r->x, &r->y) != 5 ||
	    (sep != 'x' && sep != 'X') || r->w < 1 || r->h < 1 || r->x < 0 ||
	    r->y < 0)
		return -1;
	return 0;
}

/* Read an entire file into a tj3Alloc()'d buffer. */
static unsigned char *read_file(const char *path, size_t *size_out)
{
	FILE *f = NULL;
	unsigned char *buf = NULL;
	long size;

	if ((f = fopen(path, "rb")) == NULL) {
		fprintf(stderr, "%s: opening %s: %s\n", progname, path,
			strerror(errno));
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) < 0 || (size = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET) < 0) {
		fprintf(stderr, "%s: sizing %s: %s\n", progname, path,
			strerror(errno));
		goto fail;
	}
	if (size == 0) {
		fprintf(stderr, "%s: %s is empty\n", progname, path);
		goto fail;
	}
	if ((buf = tj3Alloc((size_t)size)) == NULL) {
		fprintf(stderr, "%s: allocating %ld bytes: %s\n", progname,
			size, strerror(errno));
		goto fail;
	}
	if (fread(buf, (size_t)size, 1, f) < 1) {
		fprintf(stderr, "%s: reading %s: %s\n", progname, path,
			strerror(errno));
		tj3Free(buf);
		buf = NULL;
		goto fail;
	}
	fclose(f);
	*size_out = (size_t)size;
	return buf;

fail:
	fclose(f);
	return NULL;
}

/*
 * Atomically publish data to dst: write a unique temp file in the same
 * directory, fsync it, then rename(2) it over dst.  Returns 0 on success.
 * On any failure the temp file is removed and dst is left untouched.
 */
static int atomic_publish(const char *dst, const unsigned char *data,
                          size_t size)
{
	char *tmp = NULL;
	size_t dlen;
	int fd = -1;
	int rc = -1;

	dlen = strlen(dst);
	tmp = malloc(dlen + 32);
	if (!tmp) {
		fprintf(stderr, "%s: %s\n", progname, strerror(ENOMEM));
		return -1;
	}
	snprintf(tmp, dlen + 32, "%s.tmp.%ld", dst, (long)getpid());

	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0) {
		fprintf(stderr, "%s: creating temp %s: %s\n", progname, tmp,
			strerror(errno));
		goto out;
	}
	{
		size_t off = 0;
		while (off < size) {
			ssize_t n = write(fd, data + off, size - off);
			if (n < 0) {
				if (errno == EINTR) continue;
				fprintf(stderr, "%s: writing %s: %s\n",
					progname, tmp, strerror(errno));
				goto out;
			}
			off += (size_t)n;
		}
	}
	if (fsync(fd) < 0) {
		fprintf(stderr, "%s: fsync %s: %s\n", progname, tmp,
			strerror(errno));
		goto out;
	}
	if (close(fd) < 0) {
		fprintf(stderr, "%s: close %s: %s\n", progname, tmp,
			strerror(errno));
		fd = -1;
		goto out;
	}
	fd = -1;
	if (rename(tmp, dst) < 0) {
		fprintf(stderr, "%s: renaming onto %s: %s\n", progname, dst,
			strerror(errno));
		goto out;
	}
	/* Best-effort directory fsync so the rename survives a crash. */
	{
		const char *slash = strrchr(dst, '/');
		if (slash) {
			char dir[4096];
			int dfd;
			size_t dl = (size_t)(slash - dst);
			if (dl >= sizeof(dir)) dl = sizeof(dir) - 1;
			memcpy(dir, dst, dl);
			dir[dl] = '\0';
			dfd = open(dir, O_RDONLY | O_DIRECTORY);
			if (dfd >= 0) {
				fsync(dfd);
				close(dfd);
			}
		}
	}
	rc = 0;
	tmp[0] = '\0'; /* consumed by rename */

out:
	if (fd >= 0) close(fd);
	if (tmp && tmp[0]) unlink(tmp);
	free(tmp);
	return rc;
}

/*
 * Run one crop+orientation job.  The crop region and the geometric op live
 * in the same tjtransform, so tj3Transform() completes both in one call;
 * there is no intermediate "half-rotated" cache image anywhere.
 */
static int process_one(tjhandle inst, const struct job *job,
                       const tjtransform *base_xform, enum fail_point fp)
{
	tjtransform xform = *base_xform;
	unsigned char *src_buf = NULL, *dst_buf = NULL;
	size_t src_size = 0, dst_size = 0;
	int rc = -1;

	if ((src_buf = read_file(job->src, &src_size)) == NULL)
		return -1;

	if (tj3DecompressHeader(inst, src_buf, src_size) < 0) {
		fprintf(stderr, "%s: header %s: %s\n", progname, job->src,
			tj3GetErrorStr(inst));
		goto out;
	}

	if (fp == FAIL_TRANSFORM) {
		fprintf(stderr,
			"%s: injected failure before transform of %s "
			"(destination untouched)\n", progname, job->src);
		goto out;
	}

	if (tj3Transform(inst, src_buf, src_size, 1, &dst_buf, &dst_size,
	                 &xform) < 0) {
		fprintf(stderr, "%s: transform %s: %s\n", progname, job->src,
			tj3GetErrorStr(inst));
		goto out;
	}

	if (fp == FAIL_WRITE) {
		fprintf(stderr,
			"%s: injected failure after transform but before "
			"publishing %s (destination untouched)\n", progname,
			job->dst);
		goto out;
	}

	if (atomic_publish(job->dst, dst_buf, dst_size) < 0)
		goto out;

	printf("%s -> %s (%zu bytes)\n", job->src, job->dst, dst_size);
	rc = 0;

out:
	tj3Free(src_buf);
	tj3Free(dst_buf);
	return rc;
}

int main(int argc, char **argv)
{
	tjhandle inst = NULL;
	tjtransform xform;
	enum fail_point fp = FAIL_NONE;
	struct job *jobs = NULL;
	size_t njobs = 0, cap = 0;
	int i, rc = 1;
	int subsamp;

	progname = argv[0];
	memset(&xform, 0, sizeof(xform));
	xform.op = TJXOP_NONE;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-op") && i < argc - 1) {
			if (parse_op(argv[++i], &xform.op) < 0) usage();
		} else if (!strcmp(argv[i], "-crop") && i < argc - 1) {
			if (parse_crop(argv[++i], &xform.r) < 0) usage();
			xform.options |= TJXOPT_CROP;
		} else if (!strcmp(argv[i], "-trim")) {
			xform.options |= TJXOPT_TRIM;
		} else if (!strcmp(argv[i], "-perfect")) {
			xform.options |= TJXOPT_PERFECT;
		} else if (!strcmp(argv[i], "--fail-point") && i < argc - 1) {
			const char *s = argv[++i];
			if (!strcmp(s, "transform")) fp = FAIL_TRANSFORM;
			else if (!strcmp(s, "write")) fp = FAIL_WRITE;
			else usage();
		} else if (!strcmp(argv[i], "-o") && i < argc - 2) {
			if (njobs == cap) {
				cap = cap ? cap * 2 : 8;
				jobs = realloc(jobs, cap * sizeof(*jobs));
				if (!jobs) {
					fprintf(stderr, "%s: out of memory\n",
						progname);
					return 1;
				}
			}
			jobs[njobs].dst = argv[++i];
			jobs[njobs].src = argv[++i];
			njobs++;
		} else {
			usage();
		}
	}
	if (njobs == 0) usage();

	if ((inst = tj3Init(TJINIT_TRANSFORM)) == NULL) {
		fprintf(stderr, "%s: tj3Init failed\n", progname);
		goto done;
	}
	/* Fully specified entropy-coding parameters => deterministic output. */
	if (tj3Set(inst, TJPARAM_PROGRESSIVE, 0) < 0 ||
	    tj3Set(inst, TJPARAM_ARITHMETIC, 0) < 0 ||
	    tj3Set(inst, TJPARAM_OPTIMIZE, 0) < 0 ||
	    tj3Set(inst, TJPARAM_SAVEMARKERS, 0) < 0) {
		fprintf(stderr, "%s: %s\n", progname, tj3GetErrorStr(inst));
		goto done;
	}

	/*
	 * Crop origins must be iMCU-aligned.  Subsampling is a per-source
	 * property, so each job below probes its own header and adjusts
	 * xform.r before running.
	 */

	rc = 0;
	for (i = 0; i < (int)njobs; i++) {
		tjtransform job_xform = xform;

		if (xform.options & TJXOPT_CROP) {
			tjhandle probe = tj3Init(TJINIT_TRANSFORM);
			unsigned char *pb;
			size_t ps;

			if (!probe || (pb = read_file(jobs[i].src, &ps)) == NULL) {
				if (probe) tj3Destroy(probe);
				rc = 1;
				continue;
			}
			if (tj3DecompressHeader(probe, pb, ps) < 0) {
				fprintf(stderr, "%s: %s\n", progname,
					tj3GetErrorStr(probe));
				tj3Free(pb);
				tj3Destroy(probe);
				rc = 1;
				continue;
			}
			subsamp = tj3Get(probe, TJPARAM_SUBSAMP);
			tj3Free(pb);
			tj3Destroy(probe);
			if (subsamp < 0) {
				fprintf(stderr,
					"%s: unknown subsampling in %s\n",
					progname, jobs[i].src);
				rc = 1;
				continue;
			}
			{
				int xa = job_xform.r.x % tjMCUWidth[subsamp];
				int ya = job_xform.r.y % tjMCUHeight[subsamp];
				job_xform.r.x -= xa;
				job_xform.r.w += xa;
				job_xform.r.y -= ya;
				job_xform.r.h += ya;
			}
		}

		if (process_one(inst, &jobs[i], &job_xform, fp) < 0)
			rc = 1;
	}

done:
	if (inst) tj3Destroy(inst);
	free(jobs);
	return rc;
}
