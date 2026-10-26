#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <khash.h>
#include <libxal.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <xal.h>
#include <xal_be_fiemap.h>
#include <xal_be_fiemap_lease.h>
#include <xal_pool.h>

#define XAL_FIEMAP_BATCH 256

/** Offset from SIGRTMIN of the signal set with F_SETSIG on every lease fd */
#define XAL_LEASE_SIGOFF 7

/** Extents whose blocks do not hold the file's data as is, so a raw read cannot serve them */
#define XAL_FIEMAP_NOT_RAW                                                                         \
	(FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC | FIEMAP_EXTENT_ENCODED |                  \
	 FIEMAP_EXTENT_DATA_ENCRYPTED | FIEMAP_EXTENT_NOT_ALIGNED | FIEMAP_EXTENT_DATA_INLINE |    \
	 FIEMAP_EXTENT_DATA_TAIL)

static const struct timespec lease_nap = {.tv_sec = 0, .tv_nsec = 20 * 1000};

/*
 * A broken lease is reported with a signal to the file's owner, which xal sets to one thread of
 * its own with F_SETOWN_EX before taking the lease; lease_setup() keeps an owner already set. The
 * thread blocks every signal and takes these with sigtimedwait(), so no signal disposition of the
 * process changes, and the notification does not depend on the caller's threads, which may exit.
 * The one thread serves every set in the process.
 */
static struct {
	pthread_mutex_t lifecycle; ///< Serializes registering and unregistering sets
	pthread_mutex_t lock;	   ///< Guards the list of sets; taken before any ls->lock
	struct xal_lease_set *sets;
	unsigned nsets;
	pthread_t thread;
	atomic_int tid; ///< Of the thread, set once it runs
	atomic_bool stop;
	int sig;
} lease_proc = {
    .lifecycle = PTHREAD_MUTEX_INITIALIZER,
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static pthread_once_t lease_atfork_once = PTHREAD_ONCE_INIT;
/* What pthread_atfork() returned; without the handlers a forked child is not told apart */
static int lease_atfork_err;

/* Bumped in a forked child, which tells the sets it inherits apart without a getpid() call */
static unsigned lease_generation;

/*
 * A forked child inherits its parent's open files, and with them the leases, but not its thread.
 * lease_atfork_child() closes those of the sets it inherits, which then refuse to work, see
 * lease_foreign(); the child starts with no sets.
 */
static void
lease_atfork_prepare(void)
{
	pthread_mutex_lock(&lease_proc.lifecycle);
	pthread_mutex_lock(&lease_proc.lock);
	/* So that a child finds no entries[] or e->fd being changed */
	for (struct xal_lease_set *ls = lease_proc.sets; ls; ls = ls->next) {
		pthread_mutex_lock(&ls->lock);
	}
}

static void
lease_atfork_unlock_sets(void)
{
	for (struct xal_lease_set *ls = lease_proc.sets; ls; ls = ls->next) {
		pthread_mutex_unlock(&ls->lock);
	}
}

static void
lease_atfork_parent(void)
{
	lease_atfork_unlock_sets();
	pthread_mutex_unlock(&lease_proc.lock);
	pthread_mutex_unlock(&lease_proc.lifecycle);
}

static void
lease_atfork_child(void)
{
	/*
	 * The child cannot use the leases, and its copies of their descriptors would keep them past
	 * the parent's exit. Closed without F_UNLCK, which would drop them for the parent too.
	 */
	for (struct xal_lease_set *ls = lease_proc.sets; ls; ls = ls->next) {
		for (size_t i = 0; i < ls->nentries; ++i) {
			struct xal_lease_entry *e = &ls->entries[i];

			if (e->fd >= 0) {
				close(e->fd);
				e->fd = -1;
			}
		}
	}
	lease_atfork_unlock_sets();
	lease_generation++;
	lease_proc.sets = NULL;
	lease_proc.nsets = 0;
	atomic_store(&lease_proc.tid, 0);
	pthread_mutex_unlock(&lease_proc.lock);
	pthread_mutex_unlock(&lease_proc.lifecycle);
}

static void
lease_atfork_register(void)
{
	lease_atfork_err =
	    pthread_atfork(lease_atfork_prepare, lease_atfork_parent, lease_atfork_child);
}

/* A set inherited across fork(), whose leases belong to the process that took them */
static bool
lease_foreign(const struct xal_lease_set *ls)
{
	return ls->generation != lease_generation;
}

struct ext_vec {
	struct xal_extent *v;
	uint32_t n;
	uint32_t cap;
};

static int
ext_vec_push(struct ext_vec *vec, const struct fiemap_extent *fe, uint32_t blocksize)
{
	if (vec->n == vec->cap) {
		uint32_t cap = vec->cap ? vec->cap * 2 : 16;
		struct xal_extent *v = realloc(vec->v, cap * sizeof(*v));

		if (!v) {
			return -ENOMEM;
		}
		vec->v = v;
		vec->cap = cap;
	}

	vec->v[vec->n].start_offset = fe->fe_logical / blocksize;
	vec->v[vec->n].start_block = fe->fe_physical / blocksize;
	vec->v[vec->n].nblocks = fe->fe_length / blocksize;
	/* Unwritten extents are left out and those a raw read cannot serve refused before this. */
	vec->v[vec->n].flag = (fe->fe_flags & FIEMAP_EXTENT_SHARED) ? XAL_EXTENT_SHARED : 0;
	vec->n++;

	return 0;
}

/* Called with ls->lock held */
static void
lease_drop(struct xal_lease_set *ls, struct xal_lease_entry *e)
{
	khiter_t it;

	if (e->fd < 0) {
		return;
	}

	/* In a forked child the lease is the parent's, which still counts on it */
	if (!lease_foreign(ls) && fcntl(e->fd, F_SETLEASE, F_UNLCK)) {
		XAL_DEBUG("FAILED: F_SETLEASE(F_UNLCK); errno(%d)", errno);
	}

	it = kh_get(fd_to_idx, ls->fd_map, e->fd);
	if (it != kh_end(ls->fd_map)) {
		kh_del(fd_to_idx, ls->fd_map, it);
	}

	close(e->fd);
	e->fd = -1;
}

/*
 * Called with ls->lock held. Drops the lease of a REVOKING entry once it has no readers; the
 * last xal_extents_release() calls it again otherwise.
 */
static void
lease_finish_revoke(struct xal_lease_set *ls, uint32_t idx)
{
	struct xal_lease_entry *e = &ls->entries[idx];

	if (atomic_load(&e->state) != XAL_LEASE_REVOKING || atomic_load(&e->readers)) {
		return;
	}

	lease_drop(ls, e);
	xal_inode_at(ls->xal, idx)->content.extents.count = 0;
	atomic_store(&e->state, XAL_LEASE_REVOKED);
	ls->stats.revoked++;
}

/* Called with ls->lock held */
static void
lease_revoke(struct xal_lease_set *ls, uint32_t idx)
{
	int expected = XAL_LEASE_VALID;

	/* Pairs with xal_extents_acquire(): readers that see REVOKING back out. */
	if (atomic_compare_exchange_strong(&ls->entries[idx].state, &expected,
					   XAL_LEASE_REVOKING)) {
		lease_finish_revoke(ls, idx);
	}
}

/* Revoke the lease held on fd, if this set holds it */
static bool
lease_break(struct xal_lease_set *ls, int fd)
{
	khiter_t it;
	bool found;

	pthread_mutex_lock(&ls->lock);
	it = kh_get(fd_to_idx, ls->fd_map, fd);
	found = it != kh_end(ls->fd_map);
	if (found) {
		ls->stats.breaks++;
		lease_revoke(ls, kh_val(ls->fd_map, it));
	}
	pthread_mutex_unlock(&ls->lock);

	return found;
}

static void
lease_rescan(struct xal_lease_set *ls)
{
	pthread_mutex_lock(&ls->lock);
	ls->stats.rescans++;

	for (uint32_t i = 0; i < ls->nentries; ++i) {
		struct xal_lease_entry *e = &ls->entries[i];

		/* A lease being broken reports the type it is breaking to, F_UNLCK here. */
		if (e->fd >= 0 && atomic_load(&e->state) == XAL_LEASE_VALID &&
		    fcntl(e->fd, F_GETLEASE) != F_RDLCK) {
			lease_revoke(ls, i);
		}
	}

	pthread_mutex_unlock(&ls->lock);
}

/*
 * Hand fd to the set holding its lease, or with fd -1 have every set check all its leases. Holding
 * lease_proc.lock keeps the sets from being closed meanwhile; nothing under it waits for a reader.
 */
static void
lease_dispatch(int fd)
{
	struct xal_lease_set *ls;

	pthread_mutex_lock(&lease_proc.lock);
	for (ls = lease_proc.sets; ls; ls = ls->next) {
		if (fd < 0) {
			lease_rescan(ls);
		} else if (lease_break(ls, fd)) {
			break;
		}
	}
	pthread_mutex_unlock(&lease_proc.lock);
}

/*
 * Every signal is blocked in this thread from its creation. The realtime signal carries the fd of
 * a broken lease; SIGIO is what the kernel sends instead when it cannot queue that signal.
 */
static void *
lease_thread(void *arg)
{
	struct timespec timeout = {.tv_sec = 0, .tv_nsec = 100 * 1000 * 1000};
	sigset_t set;

	(void)arg;

	sigemptyset(&set);
	sigaddset(&set, lease_proc.sig);
	sigaddset(&set, SIGIO);
	atomic_store(&lease_proc.tid, gettid());

	while (!atomic_load(&lease_proc.stop)) {
		siginfo_t si;
		int sig = sigtimedwait(&set, &si, &timeout);

		/* A lease break comes as POLL_MSG; anything else on the signal is not ours */
		if (sig == lease_proc.sig && si.si_code == POLL_MSG) {
			lease_dispatch(si.si_fd);
		} else if (sig == SIGIO) {
			lease_dispatch(-1);
		}
	}

	return NULL;
}

/* Called with lease_proc.lifecycle held */
static int
lease_proc_start(void)
{
	sigset_t all, orig;
	int err;

	lease_proc.sig = SIGRTMIN + XAL_LEASE_SIGOFF;
	atomic_store(&lease_proc.tid, 0);
	atomic_store(&lease_proc.stop, false);

	sigfillset(&all);
	pthread_sigmask(SIG_SETMASK, &all, &orig);
	err = -pthread_create(&lease_proc.thread, NULL, lease_thread, NULL);
	pthread_sigmask(SIG_SETMASK, &orig, NULL);
	if (err) {
		return err;
	}

	/* Leases are taken with the thread as owner, so wait until it is known */
	while (!atomic_load(&lease_proc.tid)) {
		nanosleep(&lease_nap, NULL);
	}

	return 0;
}

/*
 * Called with lease_proc.lifecycle held. Signals sent to the thread die with it, so leases still
 * held by a set being closed go unreported until it drops them.
 */
static void
lease_proc_stop(void)
{
	atomic_store(&lease_proc.stop, true);
	/* Woken now rather than at its next timeout; a signal not from a break is ignored there */
	pthread_kill(lease_proc.thread, lease_proc.sig);
	pthread_join(lease_proc.thread, NULL);
}

static int
lease_proc_register(struct xal_lease_set *ls)
{
	int err = 0;

	pthread_once(&lease_atfork_once, lease_atfork_register);
	if (lease_atfork_err) {
		return -lease_atfork_err;
	}

	pthread_mutex_lock(&lease_proc.lifecycle);
	if (!lease_proc.nsets) {
		err = lease_proc_start();
	}
	if (!err) {
		pthread_mutex_lock(&lease_proc.lock);
		ls->next = lease_proc.sets;
		lease_proc.sets = ls;
		lease_proc.nsets++;
		pthread_mutex_unlock(&lease_proc.lock);
	}
	pthread_mutex_unlock(&lease_proc.lifecycle);

	return err;
}

static void
lease_proc_unregister(struct xal_lease_set *ls)
{
	struct xal_lease_set **pp;
	bool found = false;
	unsigned nsets;

	pthread_mutex_lock(&lease_proc.lifecycle);

	pthread_mutex_lock(&lease_proc.lock);
	for (pp = &lease_proc.sets; *pp; pp = &(*pp)->next) {
		if (*pp == ls) {
			*pp = ls->next;
			lease_proc.nsets--;
			found = true;
			break;
		}
	}
	nsets = lease_proc.nsets;
	pthread_mutex_unlock(&lease_proc.lock);

	/* A set inherited across fork() is in no list here, and the thread is not this process's */
	if (found && !nsets) {
		lease_proc_stop();
	}

	pthread_mutex_unlock(&lease_proc.lifecycle);
}

/*
 * Map with FIEMAP_FLAG_SYNC, so dirty pages are written back first and the extents within the
 * file have disk addresses. Extents past EOF, such as speculative preallocation, are left out or
 * cut at the block holding EOF. Unwritten extents are left out too: their blocks hold whatever
 * was there before, where the file reads as zeros, as in a hole. Any other extent that a raw read
 * could not serve fails the mapping.
 */
static int
map_fiemap(int fd, uint64_t size, uint32_t blocksize, struct ext_vec *vec)
{
	struct fiemap *fm;
	uint64_t start = 0;
	int err = 0;

	fm = calloc(1, sizeof(*fm) + XAL_FIEMAP_BATCH * sizeof(struct fiemap_extent));
	if (!fm) {
		return -ENOMEM;
	}

	while (start < size) {
		struct fiemap_extent *last;

		fm->fm_start = start;
		fm->fm_length = size - start;
		fm->fm_flags = FIEMAP_FLAG_SYNC;
		fm->fm_extent_count = XAL_FIEMAP_BATCH;
		fm->fm_mapped_extents = 0;

		if (ioctl(fd, FS_IOC_FIEMAP, fm)) {
			err = -errno;
			XAL_DEBUG("FAILED: FS_IOC_FIEMAP; errno(%d)", errno);
			break;
		}

		for (uint32_t i = 0; i < fm->fm_mapped_extents && !err; ++i) {
			struct fiemap_extent *fe = &fm->fm_extents[i];

			if (fe->fe_logical >= size || (fe->fe_flags & FIEMAP_EXTENT_UNWRITTEN)) {
				continue;
			}
			if (fe->fe_length > size - fe->fe_logical) {
				uint64_t len = size - fe->fe_logical;

				fe->fe_length = (len + blocksize - 1) / blocksize * blocksize;
			}
			if (fe->fe_flags & XAL_FIEMAP_NOT_RAW) {
				XAL_DEBUG("FAILED: extent at logical(%llu) has flags(0x%x)",
					  (unsigned long long)fe->fe_logical, fe->fe_flags);
				err = -EOPNOTSUPP;
				break;
			}
			err = ext_vec_push(vec, fe, blocksize);
		}
		if (err || !fm->fm_mapped_extents) {
			break;
		}

		last = &fm->fm_extents[fm->fm_mapped_extents - 1];
		if (last->fe_flags & FIEMAP_EXTENT_LAST) {
			break;
		}
		start = last->fe_logical + last->fe_length;
	}

	free(fm);

	return err;
}

/*
 * Called with the entry in MAPPING, which keeps other callers off it and readers off its extents.
 * Takes ls->lock only to publish, so a slow writeback holds up no lease break.
 */
static int
lease_and_map(struct xal_lease_set *ls, struct xal_inode *inode, uint32_t idx)
{
	struct xal_lease_entry *e = &ls->entries[idx];
	struct f_owner_ex owner = {.type = F_OWNER_TID, .pid = atomic_load(&lease_proc.tid)};
	char path[XAL_INODE_PATH_MAXLEN + 1];
	struct ext_vec vec = {0};
	uint32_t extent_idx;
	struct stat st;
	khiter_t it;
	int fd, ret, err;

	err = xal_inode_path(ls->xal, inode, path, sizeof(path));
	if (err < 0) {
		return err;
	}

	/* O_NONBLOCK: a write lease held by someone else fails the open instead of blocking it. */
	fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		return (errno == ENOENT || errno == ELOOP) ? -ESTALE : -errno;
	}

	if (fstat(fd, &st)) {
		err = -errno;
		goto failed;
	}
	/* The path may now lead elsewhere, even onto another fs with the same inode number */
	if (!S_ISREG(st.st_mode) || st.st_ino != inode->ino || st.st_dev != ls->dev) {
		err = -ESTALE;
		goto failed;
	}

	if (fcntl(fd, F_SETSIG, lease_proc.sig) || fcntl(fd, F_SETOWN_EX, &owner)) {
		err = -errno;
		goto failed;
	}
	if (fcntl(fd, F_SETLEASE, F_RDLCK)) {
		err = -errno;
		if (err == -EAGAIN) {
			pthread_mutex_lock(&ls->lock);
			ls->stats.unsealed++;
			pthread_mutex_unlock(&ls->lock);
		}
		goto failed;
	}

	/* No writer can open the file now without breaking the lease, so size and mapping hold. */
	if (fstat(fd, &st)) {
		err = -errno;
		goto failed_unlock;
	}

	err = map_fiemap(fd, st.st_size, ls->xal->sb.blocksize, &vec);
	if (err) {
		goto failed_unlock;
	}

	pthread_mutex_lock(&ls->lock);

	/* No reader is on the old extents, so their region is reused if it is large enough */
	extent_idx = inode->content.extents.extent_idx;
	if (vec.n > e->extent_cap) {
		err = xal_pool_claim_extents(&ls->xal->extents, vec.n, &extent_idx);
		if (err) {
			pthread_mutex_unlock(&ls->lock);
			goto failed_unlock;
		}
		/* Together, so that a mapping failing below leaves the cap with its region */
		inode->content.extents.extent_idx = extent_idx;
		e->extent_cap = vec.n;
	}

	it = kh_put(fd_to_idx, ls->fd_map, fd, &ret);
	if (ret < 0) {
		pthread_mutex_unlock(&ls->lock);
		err = -ENOMEM;
		goto failed_unlock;
	}
	kh_val(ls->fd_map, it) = idx;

	/*
	 * A break before the fd was in fd_map found nothing to revoke, and a lease the kernel timed
	 * out while mapping is gone; F_GETLEASE shows either as F_UNLCK. A later break finds the
	 * fd once ls->lock is released.
	 */
	if (fcntl(fd, F_GETLEASE) != F_RDLCK) {
		kh_del(fd_to_idx, ls->fd_map, it);
		pthread_mutex_unlock(&ls->lock);
		err = -EAGAIN;
		goto failed_unlock;
	}

	if (vec.n) {
		memcpy(xal_extent_at(ls->xal, extent_idx), vec.v, vec.n * sizeof(*vec.v));
	}
	inode->content.extents.extent_idx = extent_idx;
	inode->content.extents.count = vec.n;
	inode->size = st.st_size;

	e->fd = fd;
	ls->stats.mapped++;
	atomic_store(&e->state, XAL_LEASE_VALID);

	pthread_mutex_unlock(&ls->lock);

	free(vec.v);

	return 0;

failed_unlock:
	fcntl(fd, F_SETLEASE, F_UNLCK);
failed:
	free(vec.v);
	close(fd);

	return err;
}

static struct xal_lease_set *
lease_set(struct xal *xal)
{
	struct xal_be_fiemap *be = (struct xal_be_fiemap *)&xal->be;

	return be->base.type == XAL_BACKEND_FIEMAP ? be->lease : NULL;
}

int
xal_extents_acquire(struct xal *xal, struct xal_inode *inode, struct xal_extents **extents)
{
	struct xal_lease_set *ls;
	struct xal_lease_entry *e;
	uint32_t idx;

	if (!xal || !inode || !extents || !xal_inode_is_file(inode)) {
		return -EINVAL;
	}

	ls = lease_set(xal);
	if (!ls) {
		*extents = &inode->content.extents;
		return 0;
	}
	if (lease_foreign(ls)) {
		return -ECHILD;
	}

	idx = xal_inode_idx(xal, inode);
	if (idx >= ls->nentries) {
		return -EINVAL;
	}
	e = &ls->entries[idx];

	for (;;) {
		int state, err;

		/* Pairs with lease_revoke(), which sets REVOKING, then drops at 0 readers */
		atomic_fetch_add(&e->readers, 1);
		if (atomic_load(&e->state) == XAL_LEASE_VALID) {
			*extents = &inode->content.extents;
			return 0;
		}
		xal_extents_release(xal, inode);

		state = atomic_load(&e->state);
		switch (state) {
		case XAL_LEASE_VALID:
			continue;
		case XAL_LEASE_REVOKING:
			return -EAGAIN;
		case XAL_LEASE_MAPPING:
			nanosleep(&lease_nap, NULL);
			continue;
		default:
			break;
		}

		if (!atomic_compare_exchange_strong(&e->state, &state, XAL_LEASE_MAPPING)) {
			continue;
		}
		err = lease_and_map(ls, inode, idx);
		if (err == -EAGAIN && atomic_load(&e->state) == XAL_LEASE_MAPPING) {
			atomic_store(&e->state, XAL_LEASE_UNSEALED);
		} else if (err && atomic_load(&e->state) == XAL_LEASE_MAPPING) {
			atomic_store(&e->state, state);
		}
		if (err) {
			return err;
		}
	}
}

void
xal_extents_release(struct xal *xal, struct xal_inode *inode)
{
	struct xal_lease_set *ls;
	struct xal_lease_entry *e;
	unsigned readers;
	uint32_t idx;

	if (!xal || !inode) {
		return;
	}

	ls = lease_set(xal);
	if (!ls || lease_foreign(ls)) {
		return;
	}

	idx = xal_inode_idx(xal, inode);
	if (idx >= ls->nentries) {
		return;
	}
	e = &ls->entries[idx];

	readers = atomic_load(&e->readers);
	do {
		if (!readers) {
			XAL_DEBUG("FAILED: release without acquire; inode idx(%u)", idx);
			return;
		}
	} while (!atomic_compare_exchange_weak(&e->readers, &readers, readers - 1));

	if (readers == 1 && atomic_load(&e->state) == XAL_LEASE_REVOKING) {
		pthread_mutex_lock(&ls->lock);
		lease_finish_revoke(ls, idx);
		pthread_mutex_unlock(&ls->lock);
	}
}

int
xal_lease_stats(struct xal *xal, struct xal_lease_stats *stats)
{
	struct xal_lease_set *ls;

	if (!xal || !stats) {
		return -EINVAL;
	}

	ls = lease_set(xal);
	if (!ls) {
		return -EINVAL;
	}

	pthread_mutex_lock(&ls->lock);
	*stats = ls->stats;
	pthread_mutex_unlock(&ls->lock);

	return 0;
}

int
xal_be_fiemap_lease_reset(struct xal *xal, bool force)
{
	struct xal_lease_set *ls = lease_set(xal);

	if (!ls) {
		return 0;
	}

	/*
	 * lease_atfork_child() closed the descriptors of every set listed at fork(). What is still
	 * open here belongs to a set that another thread was closing then; it is closed without
	 * F_UNLCK, which would drop the parent's leases, and without ls->lock, which that thread
	 * may have held.
	 */
	if (lease_foreign(ls)) {
		if (!force) {
			return -ECHILD;
		}
		for (uint32_t i = 0; i < ls->nentries; ++i) {
			lease_drop(ls, &ls->entries[i]);
		}
		kh_clear(fd_to_idx, ls->fd_map);
		return 0;
	}

	pthread_mutex_lock(&ls->lock);
	for (uint32_t i = 0; i < ls->nentries && !force; ++i) {
		if (atomic_load(&ls->entries[i].readers)) {
			pthread_mutex_unlock(&ls->lock);
			XAL_DEBUG("FAILED: inode idx(%u) has readers", i);
			return -EBUSY;
		}
	}
	for (uint32_t i = 0; i < ls->nentries; ++i) {
		struct xal_lease_entry *e = &ls->entries[i];

		lease_drop(ls, e);
		atomic_store(&e->readers, 0);
		atomic_store(&e->state, XAL_LEASE_UNMAPPED);
		e->extent_cap = 0;
	}
	kh_clear(fd_to_idx, ls->fd_map);
	pthread_mutex_unlock(&ls->lock);

	return 0;
}

int
xal_be_fiemap_lease_prepare(struct xal *xal)
{
	struct xal_lease_set *ls = lease_set(xal);
	struct xal_lease_entry *entries;
	size_t n;

	if (!ls) {
		return 0;
	}

	n = xal->inodes.free;
	entries = calloc(n ? n : 1, sizeof(*entries));
	if (!entries) {
		return -ENOMEM;
	}
	for (size_t i = 0; i < n; ++i) {
		entries[i].fd = -1;
	}

	pthread_mutex_lock(&ls->lock);
	free(ls->entries);
	ls->entries = entries;
	ls->nentries = n;
	pthread_mutex_unlock(&ls->lock);

	return 0;
}

int
xal_be_fiemap_lease_init(struct xal *xal, const char *mountpoint)
{
	struct xal_be_fiemap *be = (struct xal_be_fiemap *)&xal->be;
	struct xal_lease_set *ls;
	struct stat st;
	int err;

	if (stat(mountpoint, &st)) {
		return -errno;
	}

	ls = calloc(1, sizeof(*ls));
	if (!ls) {
		return -ENOMEM;
	}
	ls->xal = xal;
	ls->dev = st.st_dev;
	ls->generation = lease_generation;
	pthread_mutex_init(&ls->lock, NULL);

	ls->fd_map = kh_init(fd_to_idx);
	if (!ls->fd_map) {
		err = -ENOMEM;
		goto failed;
	}

	err = lease_proc_register(ls);
	if (err) {
		goto failed;
	}

	be->lease = ls;

	XAL_DEBUG("INFO: read-lease mode; signal(%d) to tid(%d)", lease_proc.sig,
		  atomic_load(&lease_proc.tid));

	return 0;

failed:
	if (ls->fd_map) {
		kh_destroy(fd_to_idx, ls->fd_map);
	}
	pthread_mutex_destroy(&ls->lock);
	free(ls);

	return err;
}

void
xal_be_fiemap_lease_close(struct xal *xal)
{
	struct xal_be_fiemap *be = (struct xal_be_fiemap *)&xal->be;
	struct xal_lease_set *ls = be->lease;

	if (!ls) {
		return;
	}

	lease_proc_unregister(ls);
	xal_be_fiemap_lease_reset(xal, true);

	kh_destroy(fd_to_idx, ls->fd_map);
	free(ls->entries);
	pthread_mutex_destroy(&ls->lock);
	free(ls);
	be->lease = NULL;
}
