/**
 * A reader's write lease holds up no re-index of xal-server, and breaks on none
 * ==============================================================================
 *
 * Attaches to an index xal-server published in XAL_WATCHMODE_NONE, creates <file>, has the server
 * index it, opens it for writing and attaches it with xal_extents_attach_fd(). Then creates <new>,
 * marks the index dirty and waits for the server to re-index: <new> must be indexed within the
 * timeout, <file> must be in the index as one XAL_EXTENT_NOADDR extent, and the lease must be
 * held, with no break.
 * A write through the descriptor makes the next release report stale extents, and the next acquire
 * maps the file again. Detaching marks the index dirty, and the server's re-index maps <file>.
 *
 * With "dead", a child attaches <file> and is killed instead; the re-index after the parent's mark
 * must map <file>, as the child's record is ignored.
 *
 * Usage: test_shm_write_lease <shm_name> <file> <new> <timeout_ms> [dead]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libxal.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/**
 * Milliseconds on CLOCK_MONOTONIC
 */
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
 * The index's extent count of a file, or -1 when it is not indexed, as read at sequence *seq;
 * *noaddr tells whether that is one XAL_EXTENT_NOADDR extent, as for a write-leased file
 */
static int
indexed(struct xal *xal, char *path, int *seq, bool *noaddr)
{
	for (;;) {
		struct xal_inode *inode;
		int cur = xal_get_seq_lock(xal), count = -1;
		bool na = false;

		if (!(cur & 1)) {
			if (!xal_get_inode(xal, path, &inode)) {
				count = inode->content.extents.count;
				if (count == 1) {
					struct xal_extent *extent =
					    xal_extent_at(xal, inode->content.extents.extent_idx);

					na = extent && extent->flag == XAL_EXTENT_NOADDR;
				}
			}
			if (cur == xal_get_seq_lock(xal)) {
				*seq = cur;
				*noaddr = na;
				return count;
			}
		}
		usleep(100);
	}
}

/**
 * Whether the index holds a file as one XAL_EXTENT_NOADDR extent, as it does a write-leased one
 */
static bool
indexed_noaddr(struct xal *xal, char *path)
{
	bool noaddr;
	int seq;

	return indexed(xal, path, &seq, &noaddr) == 1 && noaddr;
}

/**
 * Mark the index dirty and wait for a re-index to come back clean with the file indexed and, when
 * mapped is set, mapped to addresses; returns its extent count, or -ETIMEDOUT
 */
static int
reindex(struct xal *xal, char *path, bool mapped, int timeout_ms, uint64_t *ms)
{
	uint64_t start;
	bool noaddr;
	int seq;

	indexed(xal, path, &seq, &noaddr);
	start = now_ms();
	xal_mark_dirty(xal);
	while (now_ms() - start < (uint64_t)timeout_ms) {
		int cur, count = indexed(xal, path, &cur, &noaddr);

		if (cur != seq && !xal_is_dirty(xal) && count >= 0 &&
		    (!mapped || (count > 0 && !noaddr))) {
			*ms = now_ms() - start;
			return count;
		}
		usleep(1000);
	}

	return -ETIMEDOUT;
}

static int
create_file(const char *path, size_t len)
{
	char buf[4096];
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);

	if (fd < 0) {
		return -errno;
	}
	memset(buf, 0x5a, sizeof(buf));
	for (size_t off = 0; off < len; off += sizeof(buf)) {
		if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
			close(fd);
			return -EIO;
		}
	}
	if (fsync(fd)) {
		close(fd);
		return -errno;
	}

	return close(fd) ? -errno : 0;
}

/**
 * Attach the file on a descriptor open for writing, retrying while the server's index holds the
 * file open; an inode looked up in an index rewritten meanwhile is attached again
 */
static int
attach_rw(struct xal *xal, char *path, int timeout_ms, int *fd, struct xal_inode **attached)
{
	uint64_t start = now_ms();
	int err;

	*fd = open(path, O_RDWR | O_CLOEXEC);
	if (*fd < 0) {
		return -errno;
	}
	do {
		struct xal_inode *inode;
		bool noaddr;
		int seq;

		err = -ENOENT;
		if (indexed(xal, path, &seq, &noaddr) >= 0 && !xal_get_inode(xal, path, &inode)) {
			err = xal_extents_attach_fd(xal, inode, *fd, attached);
			if (!err && seq != xal_get_seq_lock(xal)) {
				xal_extents_detach_fd(xal, *attached);
				err = -EAGAIN;
			}
		}
	} while ((err == -EAGAIN || err == -ENOENT) && now_ms() - start < (uint64_t)timeout_ms &&
		 !usleep(1000));

	return err;
}

static int
leased(struct xal *xal, char *path, char *new, int timeout_ms)
{
	struct xal_lease_stats stats;
	struct xal_inode *attached;
	struct xal_extents *extents;
	uint64_t ms;
	int fd, err;
	char buf[4096];

	err = attach_rw(xal, path, timeout_ms, &fd, &attached);
	if (err) {
		printf("FAILED: attaching %s; err(%d)\n", path, err);
		return 1;
	}

	err = create_file(new, 1);
	err = err ? err : reindex(xal, new, false, timeout_ms, &ms);
	if (err < 0) {
		printf("FAILED: %s not indexed within %d ms of the mark; err(%d)\n", new,
		       timeout_ms, err);
		return 1;
	}
	printf("OK: %s indexed %" PRIu64 " ms after the mark\n", new, ms);

	if (!indexed_noaddr(xal, path)) {
		printf("FAILED: %s is not indexed as XAL_EXTENT_NOADDR while write-leased\n", path);
		return 1;
	}
	if (fcntl(fd, F_GETLEASE) != F_WRLCK || xal_lease_stats(xal, &stats) || stats.breaks ||
	    xal_extents_recalled(xal, attached, NULL)) {
		printf("FAILED: the lease on %s was broken (%" PRIu64 " breaks)\n", path,
		       stats.breaks);
		return 1;
	}

	memset(buf, 0x77, sizeof(buf));
	err = xal_extents_acquire(xal, attached, &extents);
	if (err || !extents->count || pwrite(fd, buf, sizeof(buf), 0) != (ssize_t)sizeof(buf)) {
		printf("FAILED: acquiring %s or writing through its descriptor; err(%d)\n", path,
		       err);
		return 1;
	}
	err = xal_extents_release_range(xal, attached, 0, sizeof(buf));
	if (err != -ESTALE) {
		printf("FAILED: the release after a write through the descriptor gave %d\n", err);
		return 1;
	}
	err = xal_extents_acquire(xal, attached, &extents);
	if (err || !extents->count || xal_extents_release(xal, attached)) {
		printf("FAILED: %s was not mapped again after the write; err(%d)\n", path, err);
		return 1;
	}

	err = xal_extents_detach_fd(xal, attached);
	if (err || !xal_is_dirty(xal)) {
		printf("FAILED: the detach left the index clean; err(%d)\n", err);
		return 1;
	}
	err = reindex(xal, path, true, timeout_ms, &ms);
	if (err < 0) {
		printf("FAILED: %s not mapped within %d ms of the detach\n", path, timeout_ms);
		return 1;
	}
	printf("OK: %s mapped (%d extents) %" PRIu64 " ms after the detach\n", path, err, ms);
	close(fd);

	return 0;
}

static int
dead(struct xal *xal, char *shm_name, char *path, char *new, int timeout_ms)
{
	uint64_t ms;
	int pipefd[2], status, err;
	pid_t pid;
	char ok;

	if (pipe(pipefd)) {
		return 1;
	}
	pid = fork();
	if (!pid) {
		struct xal_inode *attached;
		struct xal *child;
		int fd;

		ok = !attach(shm_name, timeout_ms, &child) &&
		     !attach_rw(child, path, timeout_ms, &fd, &attached);
		if (write(pipefd[1], &ok, 1) != 1) {
			_exit(1);
		}
		pause();
		_exit(0);
	}
	if (read(pipefd[0], &ok, 1) != 1 || !ok) {
		printf("FAILED: the child did not attach %s\n", path);
		return 1;
	}

	err = create_file(new, 1);
	err = err ? err : reindex(xal, new, false, timeout_ms, &ms);
	if (err < 0 || !indexed_noaddr(xal, path)) {
		printf("FAILED: %s is mapped while the child holds its lease; err(%d)\n", path,
		       err);
		kill(pid, SIGKILL);
		return 1;
	}

	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	err = reindex(xal, path, true, timeout_ms, &ms);
	if (err < 0) {
		printf("FAILED: %s not mapped within %d ms of the child's death\n", path,
		       timeout_ms);
		return 1;
	}
	printf("OK: %s mapped (%d extents) %" PRIu64 " ms after a mark, its leaseholder gone\n",
	       path, err, ms);

	return 0;
}

int
main(int argc, char *argv[])
{
	struct xal *xal;
	uint64_t ms;
	int timeout_ms, rc, err;

	if (argc != 5 && !(argc == 6 && !strcmp(argv[5], "dead"))) {
		fprintf(stderr, "Usage: %s <shm_name> <file> <new> <timeout_ms> [dead]\n", argv[0]);
		return 2;
	}
	timeout_ms = atoi(argv[4]);

	err = attach(argv[1], timeout_ms, &xal);
	if (err) {
		printf("FAILED: xal_from_shm(%s); err(%d)\n", argv[1], err);
		return 1;
	}

	/* Created once attached, as an attach succeeds only after the first index */
	err = create_file(argv[2], 65536);
	err = err ? err : reindex(xal, argv[2], true, timeout_ms, &ms);
	if (err < 0) {
		printf("FAILED: %s not mapped within %d ms of the mark; err(%d)\n", argv[2],
		       timeout_ms, err);
		xal_close(xal);
		return 1;
	}

	rc = argc == 6 ? dead(xal, argv[1], argv[2], argv[3], timeout_ms)
		       : leased(xal, argv[2], argv[3], timeout_ms);
	xal_close(xal);

	return rc;
}
