#ifndef XAL_H
#define XAL_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <unistd.h>
#include <xal_pool.h>

#define BUF_NBYTES 4096 * 32UL		    ///< Number of bytes in a buffer
#define CHUNK_NINO 64			    ///< Number of inodes in a chunk
#define BUF_BLOCKSIZE 4096		    ///< Number of bytes in a block
#define ODF_BLOCK_DIR_BYTES_MAX 64UL * 1024 ///< Maximum size of a directory block
#define ODF_BLOCK_FS_BYTES_MAX 64UL * 1024  ///< Maximum size of a filestem block
#define ODF_INODE_MAX_NBYTES 2048	    ///< Maximum size of an inode
#define XAL_BACKEND_SIZE 72

struct xal_backend_base {
	enum xal_backend type;
	int (*index)(struct xal *xal);
	void (*close)(struct xal *xal);
};

enum xal_procrole {
	XAL_PROCROLE_SINGLE = 0,
	XAL_PROCROLE_PRIMARY = 1,
	XAL_PROCROLE_SECONDARY = 2,
};

enum xal_state {
	XAL_STATE_CLEAN = 0,	///< The representation matches the last indexed filesystem state
	XAL_STATE_DIRTY = 1,	///< A breaking change occurred that no index has begun to observe
	XAL_STATE_INDEXING = 2, ///< xal_index() is rebuilding the representation
};

#define XAL_SHM_MAGIC 0x58414c5341ULL ///< "XALSA", XAL Shared ABI; zero until published

/**
 * Bumped whenever a secondary built at one version would misread a region published at another.
 * That covers the layout of this struct and of the pool elements, and equally what the pool
 * contents mean: changing xal_inode.name from an assembled path to a leaf name leaves every size
 * and offset intact and still requires a bump.
 */
#define XAL_SHM_VERSION 4

/**
 * Most files the readers of a region may hold a write lease on at once; see
 * xal_extents_attach_fd()
 */
#define XAL_SHM_WRITE_LEASES 4096

#define XAL_SHM_WRITE_LEASE_SKIPPED 1 ///< An index left the file unmapped while recorded

/**
 * A file a reader of the region holds a write lease on: the primary's index does not open it, as
 * any open() of the file would break the lease. Slot i is held by an OFD lock on byte i of the
 * state object, taken through the reader's descriptor of it, so a record whose lock is gone,
 * as when the reader exited, is ignored.
 */
struct xal_shm_write_lease {
	_Atomic uint64_t ino; ///< 0 for a free slot
	int32_t pid;	      ///< The reader's, as it sees it; for diagnostics only
	uint32_t flags;	      ///< XAL_SHM_WRITE_LEASE_SKIPPED
};

struct xal_shared_state {
	_Atomic uint64_t magic; ///< XAL_SHM_MAGIC, stored last, so zero means not yet published
	uint32_t version; ///< XAL_SHM_VERSION
	enum xal_backend type;
	struct xal_sb sb;
	char mountpoint[XAL_PATH_MAXLEN];
	char subtree[XAL_PATH_MAXLEN]; ///< Empty when the index covers the whole mount
	atomic_int index_state; ///< One of enum xal_state
	atomic_int seq_lock; ///< Even when stable; odd while the pools are being rewritten in place

	/* Orders the index's check of write_leases and its open() of a file against a reader's
	 * record. Robust and process-shared, as a reader may die holding it. Its layout is the C
	 * library's, so primary and readers must use the same one. */
	pthread_mutex_t lease_lock;
	_Atomic uint32_t nwrite_leases; ///< Slots of write_leases up to the last in use
	struct xal_shm_write_lease write_leases[XAL_SHM_WRITE_LEASES];
};
XAL_STATIC_ASSERT(offsetof(struct xal_shared_state, magic) == 0, "shm ABI");
XAL_STATIC_ASSERT(offsetof(struct xal_shared_state, version) == 8, "shm ABI");

/**
 * XAL
 *
 * Contains a handle to the storage device along with meta-data describing the data-layout and a
 * pool of inodes.
 *
 * @struct xal
 */
struct xal {
	struct xnvme_dev *dev;
	struct xal_pool inodes;  ///< Pool of inodes in host-native format
	struct xal_pool extents; ///< Pool of extents in host-native format
	uint32_t root_idx;       ///< Index of the root inode in the inodes pool
	struct xal_sb sb;
	uint8_t be[XAL_BACKEND_SIZE];
	atomic_int *index_state; ///< One of enum xal_state; may point to external shared memory
	atomic_int _index_state_storage; ///< Backing store for index_state when shm_name is not set
	atomic_int *seq_lock;    ///< An uneven number indicates the struct is being modified and is not safe to read; may point to external shared memory
	atomic_int _seq_lock_storage; ///< Backing store for seq_lock when shm_name is not set
	struct xal_shared_state *state; ///< Mapped shared state region; non-NULL when shm_name was set
	int state_fd; ///< Descriptor of the state object while state is mapped, for its OFD locks
	pid_t pid;    ///< The process of a handle from xal_from_shm(), which alone may attach files
	char *state_shm_name;           ///< Name of the _state shm region; set by primary only, for unlink on close
	enum xal_procrole procrole;

	/* Held for the duration of xal_index(); a second caller is refused with -EBUSY. Separate
	 * from index_state because xal_mark_dirty() clears XAL_STATE_INDEXING unconditionally, so
	 * that state cannot also be the thing that excludes an index. Not shared: a secondary
	 * cannot index. */
	atomic_bool indexing;

	/* Result of the last completed xal_index(). The watch loop cannot infer it -- a failed
	 * index and a successful one with a mark landing mid-rebuild both leave DIRTY with
	 * seq_lock advanced, and want opposite treatment. Not set by the -EBUSY path. */
	atomic_int last_index_err;

	/* A secondary's own handle for the files it attached with xal_extents_attach_fd(), whose
	 * extents xal_extent_at() finds at XAL_EXTENT_SHADOW_BASE and above; the shadow points at
	 * itself. NULL otherwise. */
	_Atomic(struct xal *) shadow;
};

#define XAL_EXTENT_SHADOW_BASE (1U << 31) ///< Extent indices from here are in xal->shadow's pool

int
search_by_traversal(struct xal *xal, struct xal_inode *root, char *path, char *basepath, struct xal_inode **inode);

int
compare_name_to_inode(const void *key, const void *elem);

void
xal_mark_index_done(struct xal *xal, int err);

#endif /* XAL_H */
