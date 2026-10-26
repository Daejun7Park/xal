#ifndef XAL_BE_FIEMAP_H
#define XAL_BE_FIEMAP_H

struct xal_reflink;
struct xal_lease_set;

/**
 * Basename prefix of the per-session reflink shadow directory: <mnt>/.xal_snapshot.<pid>
 *
 * Shared with the inotify watcher, which drops events naming a shadow directory so that the
 * snapshot does not report itself as a change to the filesystem it snapshotted.
 */
#define XAL_SNAPSHOT_PREFIX ".xal_snapshot."

struct xal_be_fiemap {
	struct xal_backend_base base;
	char *mountpoint;      ///< Path to mountpoint of dev
	char *subtree;         ///< Optional absolute path at/under mountpoint to scope the index to; NULL = whole mount
	struct xal_inotify *inotify;
	void *path_inode_map;  ///< Map of paths to inodes

	struct xal_reflink *reflink; ///< Reflink-snapshot state; non-NULL in XAL_WATCHMODE_REFLINK_SNAPSHOT

	struct xal_lease_set *lease;  ///< Read-lease state; non-NULL in XAL_WATCHMODE_READ_LEASE
};
XAL_STATIC_ASSERT(sizeof(struct xal_be_fiemap) == XAL_BACKEND_SIZE, "Incorrect size");

/** Longest the index, or a reader's close, waits for another reader holding the records' lock */
#define XAL_FIEMAP_LEASE_LOCK_MS 100

/**
 * Take the shared state's lease_lock, waiting at most timeout_ms when it is not negative; the
 * xal_state_write_lease*() calls need it held. -EAGAIN when the wait timed out.
 */
int
xal_state_lease_lock(struct xal *xal, int timeout_ms);

void
xal_state_lease_unlock(struct xal *xal);

/**
 * Whether a reader holds a write lease on the file with this inode number, for an index that then
 * skips it; marks its records XAL_SHM_WRITE_LEASE_SKIPPED
 */
bool
xal_state_write_leased(struct xal *xal, uint64_t ino);

/**
 * Record a write lease through this handle; returns the record's slot, or -ENOSPC when
 * XAL_SHM_WRITE_LEASES are recorded
 */
int
xal_state_write_lease_add(struct xal *xal, uint64_t ino);

/**
 * Drop the record in a slot xal_state_write_lease_add() returned; true when an index skipped the
 * file while it was recorded
 */
bool
xal_state_write_lease_del(struct xal *xal, int slot);

void
xal_be_fiemap_close(struct xal *xal);

int
xal_be_fiemap_open(struct xal **xal, char *mountpoint, struct xal_opts *opts);

int
xal_be_fiemap_get_inode(struct xal *xal, char *path, struct xal_inode **inode);

#endif /* XAL_BE_FIEMAP_H */
