// SPDX-License-Identifier: BSD-3-Clause

/**
 * A reader of an xal-server index in XAL_WATCHMODE_READ_LEASE sets its own lease_no_recheck
 * ==========================================================================================
 *
 * Attaches to the index with xal_from_shm_opts(), xal_opts.lease_no_recheck as given, opens <path>
 * read-only, attaches it and acquires its extents. It then dedupes <src>, a file of the same
 * content, into the whole of <path>, which breaks no lease, and releases. With the recheck, the
 * release must find the mapping changed and return -ESTALE; with lease_no_recheck it must miss it
 * and return 0. Prints "RELEASE err=<err>" either way.
 *
 * Usage: test_lease_reader_recheck <shm_name> <path> <src> <timeout_ms> <no_recheck>
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libxal.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/**
 * Dedupe all of src into dst, both of size bytes; returns 0 when every byte was deduped
 */
static int
dedupe(int src, int dst, uint64_t size)
{
	struct file_dedupe_range *range;
	int err;

	range = calloc(1, sizeof(*range) + sizeof(struct file_dedupe_range_info));
	if (!range) {
		return -ENOMEM;
	}
	range->src_offset = 0;
	range->src_length = size;
	range->dest_count = 1;
	range->info[0].dest_fd = dst;
	range->info[0].dest_offset = 0;

	err = ioctl(src, FIDEDUPERANGE, range) ? -errno : 0;
	if (!err && range->info[0].status) {
		err = range->info[0].status < 0 ? range->info[0].status : -EBADE;
	}
	if (!err && range->info[0].bytes_deduped != size) {
		err = -EIO;
	}
	free(range);

	return err;
}

int
main(int argc, char *argv[])
{
	struct xal_opts opts = {0};
	struct xal_inode *attached;
	struct xal_extents *extents;
	struct xal *xal = NULL;
	struct stat st;
	uint64_t start;
	int timeout_ms, fd, src, err;

	if (argc != 6) {
		printf("usage: %s <shm_name> <path> <src> <timeout_ms> <no_recheck>\n", argv[0]);
		return 1;
	}
	timeout_ms = atoi(argv[4]);
	opts.lease_no_recheck = atoi(argv[5]);

	/* Attach, waiting while the primary is still publishing */
	start = now_ms();
	for (;;) {
		err = xal_from_shm_opts(argv[1], &xal, &opts);
		if ((err != -EAGAIN && err != -ESTALE) ||
		    now_ms() - start >= (uint64_t)timeout_ms) {
			break;
		}
		usleep(1000);
	}
	if (err) {
		printf("FAILED: xal_from_shm_opts(%s); err(%d)\n", argv[1], err);
		return 1;
	}

	fd = open(argv[2], O_RDONLY | O_CLOEXEC);
	src = open(argv[3], O_RDONLY | O_CLOEXEC);
	if (fd < 0 || src < 0 || fstat(fd, &st)) {
		printf("FAILED: open(%s, %s); errno(%d)\n", argv[2], argv[3], errno);
		xal_close(xal);
		return 1;
	}

	err = xal_extents_attach_fd(xal, NULL, fd, &attached);
	if (err) {
		printf("FAILED: xal_extents_attach_fd(); err(%d)\n", err);
		xal_close(xal);
		return 1;
	}
	err = xal_extents_acquire(xal, attached, &extents);
	if (err) {
		printf("FAILED: xal_extents_acquire(); err(%d)\n", err);
		xal_close(xal);
		return 1;
	}

	err = dedupe(src, fd, (uint64_t)st.st_size);
	if (err) {
		printf("FAILED: dedupe of %s into %s; err(%d)\n", argv[3], argv[2], err);
		xal_extents_release(xal, attached);
		xal_close(xal);
		return 1;
	}

	err = xal_extents_release(xal, attached);
	printf("RELEASE err=%d\n", err);
	xal_close(xal);
	close(src);
	close(fd);

	if (err != (opts.lease_no_recheck ? 0 : -ESTALE)) {
		printf("FAILED: expected err(%d) with lease_no_recheck(%d)\n",
		       opts.lease_no_recheck ? 0 : -ESTALE, opts.lease_no_recheck);
		return 1;
	}

	return 0;
}
