/**
 * A reader of an xal-server index in XAL_WATCHMODE_READ_LEASE keeps its write lease across a
 * re-index
 * ============================================================================================
 *
 * Attaches to the index, opens the given file for writing and attaches it, which takes a write
 * lease, then marks the index dirty and waits for the server to re-index. The lease must still be
 * held afterwards, unbroken, and the file's extents must still come from xal_extents_acquire().
 * The region must give no extents itself: xal_get_extents() fails with -ENOTSUP and an acquire of
 * the region's own inode with -ENOLCK.
 *
 * After the detach, a second file opened read-only is attached in the slot the first left, and
 * left attached at xal_close(), which must drop its lease and leave the descriptor open.
 *
 * Given another watch mode, the region must be in that mode and refuse the attach with -EINVAL.
 *
 * Usage: test_lease_server <shm_name> <path> <timeout_ms> <watch_mode> <path2>
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libxal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xal.h>

static uint64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/**
 * Attach, waiting while the primary is still publishing
 */
static int
attach(const char *shm_name, int timeout_ms, struct xal **xal)
{
	uint64_t start = now_ms();
	int err;

	for (;;) {
		err = xal_from_shm(shm_name, xal);
		if ((err != -EAGAIN && err != -ESTALE) ||
		    now_ms() - start >= (uint64_t)timeout_ms) {
			return err;
		}
		usleep(1000);
	}
}

/**
 * Acquire the attached file's extents, check that there are some with an address, and release
 */
static int
check_extents(struct xal *xal, struct xal_inode *inode)
{
	struct xal_extents *extents;
	int err;

	err = xal_extents_acquire(xal, inode, &extents);
	if (err) {
		printf("FAILED: xal_extents_acquire(); err(%d)\n", err);
		return 1;
	}
	if (!extents->count) {
		printf("FAILED: the attached file has no extents\n");
		xal_extents_release(xal, inode);
		return 1;
	}
	for (uint32_t i = 0; i < extents->count; i++) {
		struct xal_extent *extent = xal_extent_at(xal, extents->extent_idx + i);

		if (!extent || (extent->flag & XAL_EXTENT_NOADDR)) {
			printf("FAILED: extent(%u) of the attached file is missing or has no "
			       "address\n",
			       i);
			xal_extents_release(xal, inode);
			return 1;
		}
	}
	err = xal_extents_release(xal, inode);
	if (err) {
		printf("FAILED: xal_extents_release(); err(%d)\n", err);
		return 1;
	}
	printf("extents(%u) from extent_idx(0x%x)\n", extents->count, extents->extent_idx);

	return 0;
}

/**
 * Mark the index dirty and wait for the server to index it again, clean and at a later sequence
 */
static int
reindex(struct xal *xal, int timeout_ms)
{
	uint64_t start = now_ms(), took;
	int seq = xal_get_seq_lock(xal);

	xal_mark_dirty(xal);
	for (;;) {
		int now = xal_get_seq_lock(xal);

		if (!(now & 1) && now != seq && !xal_is_dirty(xal)) {
			break;
		}
		if (now_ms() - start >= (uint64_t)timeout_ms) {
			printf("FAILED: no re-index within %d ms\n", timeout_ms);
			return 1;
		}
		usleep(1000);
	}
	took = now_ms() - start;
	printf("re-indexed in %llu ms\n", (unsigned long long)took);

	return 0;
}

int
main(int argc, char *argv[])
{
	struct xal_inode *region_inode, *attached = NULL, *first;
	struct xal_lease_stats stats = {0};
	struct xal_extents *extents;
	struct xal *xal;
	int timeout_ms, mode, lease, fd = -1, fd2 = -1, rc = 1, err;

	if (argc != 6) {
		fprintf(stderr, "Usage: %s <shm_name> <path> <timeout_ms> <watch_mode> <path2>\n",
			argv[0]);
		return 2;
	}
	timeout_ms = atoi(argv[3]);
	mode = atoi(argv[4]);

	err = attach(argv[1], timeout_ms, &xal);
	if (err) {
		printf("FAILED: xal_from_shm(%s); err(%d)\n", argv[1], err);
		return 1;
	}
	printf("region watchmode(%d)\n", xal->state->watch_mode);
	if ((int)xal->state->watch_mode != mode) {
		printf("FAILED: the region is not in watchmode(%d)\n", mode);
		goto out;
	}

	fd = open(argv[2], O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("FAILED: open(%s); errno(%d)\n", argv[2], errno);
		goto out;
	}
	if (mode != XAL_WATCHMODE_READ_LEASE) {
		err = xal_extents_attach_fd(xal, NULL, fd, &attached);
		printf("attach err(%d)\n", err);
		if (err != -EINVAL) {
			printf("FAILED: attach on a region in watchmode(%d) was not refused\n",
			       mode);
			goto out;
		}
		rc = 0;
		goto out;
	}

	err = xal_get_extents(xal, argv[2], &extents);
	if (err != -ENOTSUP) {
		printf("FAILED: xal_get_extents() gave err(%d), expected -ENOTSUP\n", err);
		goto out;
	}
	err = xal_get_inode(xal, argv[2], &region_inode);
	if (err) {
		printf("FAILED: xal_get_inode(%s); err(%d)\n", argv[2], err);
		goto out;
	}
	err = xal_extents_acquire(xal, region_inode, &extents);
	if (err != -ENOLCK) {
		printf("FAILED: acquire of the region's inode gave err(%d), expected -ENOLCK\n",
		       err);
		goto out;
	}

	err = xal_extents_attach_fd(xal, NULL, fd, &attached);
	if (err) {
		printf("FAILED: xal_extents_attach_fd(); err(%d)\n", err);
		goto out;
	}
	if (check_extents(xal, attached)) {
		goto out;
	}

	if (reindex(xal, timeout_ms)) {
		goto out;
	}

	lease = fcntl(fd, F_GETLEASE);
	xal_lease_stats(xal, &stats);
	printf("lease(%s) breaks(%llu)\n", lease == F_WRLCK ? "F_WRLCK" : "not F_WRLCK",
	       (unsigned long long)stats.breaks);
	if (lease != F_WRLCK || stats.breaks) {
		printf("FAILED: the re-index broke the reader's write lease\n");
		goto out;
	}
	if (check_extents(xal, attached)) {
		goto out;
	}
	first = attached;
	err = xal_extents_detach_fd(xal, attached);
	if (err) {
		printf("FAILED: xal_extents_detach_fd(); err(%d)\n", err);
		goto out;
	}
	attached = NULL;

	fd2 = open(argv[5], O_RDONLY | O_CLOEXEC);
	if (fd2 < 0) {
		printf("FAILED: open(%s); errno(%d)\n", argv[5], errno);
		goto out;
	}
	err = xal_extents_attach_fd(xal, NULL, fd2, &attached);
	if (err) {
		printf("FAILED: xal_extents_attach_fd() of the second file; err(%d)\n", err);
		goto out;
	}
	if (attached != first) {
		printf("FAILED: the second file did not take the slot the first left\n");
		goto out;
	}
	if (check_extents(xal, attached)) {
		goto out;
	}
	xal_close(xal);
	xal = NULL;
	attached = NULL;
	lease = fcntl(fd2, F_GETLEASE);
	printf("closed attached: lease(%s) fd_open(%d)\n", lease == F_UNLCK ? "F_UNLCK" : "held",
	       fcntl(fd2, F_GETFD) >= 0);
	if (lease != F_UNLCK || fcntl(fd2, F_GETFD) < 0) {
		printf("FAILED: xal_close() left the lease, or closed the descriptor\n");
		goto out;
	}
	rc = 0;

out:
	if (attached) {
		xal_extents_detach_fd(xal, attached);
	}
	if (fd >= 0) {
		close(fd);
	}
	if (fd2 >= 0) {
		close(fd2);
	}
	if (xal) {
		xal_close(xal);
	}

	return rc;
}
