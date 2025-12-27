/*
 * IRIX FAT32 fs driver Copyright (c) 2025 Dominik Behr
 */
#ifndef __FAT32FS_H
#define __FAT32FS_H

/*
 * Workaround for IRIX 6.5.22 behavior.h weirdness:
 * The BHV_PREPARE section has duplicate macro definitions causing
 * redefinition warnings. We undefine it before including system headers
 * to use the simpler non-CELL path.
 */
#ifdef BHV_PREPARE
#undef BHV_PREPARE
#endif

#include <sys/types.h>
#include <sys/cmn_err.h>
#include <sys/vfs.h>
#include <sys/vnode.h>
#include <sys/cred.h>
#include <sys/statvfs.h>
#include <sys/buf.h>
#include <sys/kmem.h>
#include <sys/ddi.h>
#include <sys/mload.h>
#include <sys/errno.h>
#include <sys/fs_subr.h>
#include <sys/uio.h>
#include <sys/pathname.h>
#include <sys/flock.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sema.h>
#include <sys/dirent.h>
#include <sys/file.h>
#include <sys/kabi.h>

#include "fat32.h"

#pragma set woff 3201
#pragma set woff 1174
#pragma set woff 1204
#pragma set woff 1552

#define FAT32_DBG
#define noFAT32_DBG_CLUSTERS
#define noFAT32_DBG_DIRENT
#define noFAT32_DBG_RW
#define noFAT32_DBG_OTHER

/*
 * FAT32 inode number constants
 * We use cluster numbers directly as inode numbers
 * Root directory is identified by comparing cluster with fsi_root_cluster
 */

/*
 * FAT32 filesystem mount information
 * One instance per mounted filesystem
 */
typedef struct fat32fs_info {
    vfs_t           *fsi_vfsp;      /* Pointer to VFS structure */
    bhv_desc_t      fsi_bhv;        /* Behavior descriptor */
    mrlock_t        fsi_lock;       /* Filesystem multi-reader lock */
    vnode_t         *fsi_devvp;     /* Device vnode */
    dev_t           fsi_dev;        /* Device number */
    vnode_t         *fsi_rootvp;    /* Cached root vnode */

    /* Partition information */
    uint32_t        fsi_partition_offset;   /* Partition start sector (0 if direct mount) */
    int             fsi_partition_num;      /* Partition number (0-3, -1 = auto) */

    /* Mount options */
    uid_t           fsi_uid;                /* Owner uid for all files (default 0) */
    gid_t           fsi_gid;                /* Owner gid for all files (default 0) */

    /* Cached sector locations (all have partition offset applied) */
    uint32_t        fsi_fsinfo_sector;      /* FSInfo sector (absolute sector) */
    uint32_t        fsi_fat_start;          /* First FAT sector (absolute sector) */

    /* FAT32 filesystem parameters from boot sector */
    uint32_t        fsi_bytes_per_sector;
    uint32_t        fsi_sectors_per_cluster;
    uint32_t        fsi_bytes_per_cluster;  /* Cached: bytes_per_sector * sectors_per_cluster */
    uint32_t        fsi_reserved_sectors;

    /* Optimization fields */
    uint32_t        fsi_sector_shift;       /* log2(bytes_per_sector) */
    uint32_t        fsi_sector_mask;        /* bytes_per_sector - 1 */
    uint32_t        fsi_cluster_shift;      /* log2(bytes_per_cluster) */
    uint32_t        fsi_cluster_mask;       /* bytes_per_cluster - 1 */
    uint32_t        fsi_spc_shift;          /* log2(sectors_per_cluster) */

    uint32_t        fsi_num_fats;
    uint32_t        fsi_fat_size;
    uint32_t        fsi_root_cluster;   /* Root cluster from boot record */
    uint32_t        fsi_total_sectors;
    uint32_t        fsi_data_start;     /* First data sector */
    uint32_t        fsi_cluster_count;  /* Total clusters */

    /* FSInfo cache */
    uint32_t        fsi_free_count;     /* Free cluster count from FSInfo */
    uint32_t        fsi_next_free;      /* Next free cluster hint from FSInfo */
} fat32fs_info_t;

/*
 * Cluster cache entry for LRU cluster chain lookups
 */
#define FAT32_CLS_CACHE_SIZE 16

typedef struct fat32_cls_cache_entry {
    uint32_t        index;      /* Cluster index in chain */
    uint32_t        cluster;    /* Cluster number (0 = unused entry) */
} fat32_cls_cache_entry_t;

/*
 * FAT32 vnode (inode) information
 * One instance per active file/directory
 */
typedef struct fat32fs_vnode {
    /* Debug marker - must be first */
    char            fv_magic[4];      /* Magic marker "FT32" */
    uint32_t        fv_structsize;    /* Structure size (0 if deleted) */

    vnode_t         *fv_vnode;      /* Pointer to system vnode */
    bhv_desc_t      fv_bhv;         /* Behavior descriptor */
    fat32fs_info_t  *fv_fsi;        /* Pointer to filesystem info */

    /* Inode identification - cluster number IS the inode number */
    uint32_t        fv_cluster;     /* Starting cluster number */

    /* File/directory attributes */
    vtype_t         fv_type;        /* Vnode type (VREG, VDIR, etc.) */
    mode_t          fv_mode;        /* File mode and permissions */
    uid_t           fv_uid;         /* Owner user ID */
    gid_t           fv_gid;         /* Owner group ID */
    off_t           fv_size;        /* File size in bytes */

    /* Timestamps (converted from FAT format) */
    timespec_t      fv_atime;       /* Last access time */
    timespec_t      fv_mtime;       /* Last modification time */
    timespec_t      fv_ctime;       /* Creation time */

    /* FAT32-specific metadata */
    uint8_t         fv_attr;        /* FAT32 attributes byte */
    uint32_t        fv_diroff;      /* Start offset of directory entry */
    uint32_t        fv_dirsz;       /* Number of directory entries */
    uint32_t        fv_parent_cluster; /* Parent directory cluster */

    /* Parent chain - holds reference to parent vnode */
    vnode_t         *fv_parent_vp;  /* Parent vnode (holds reference) */

    /* Synchronization */
    mrlock_t        fv_lock;        /* Vnode multi-reader lock */
    mutex_t         fv_cache_lock;  /* Cluster cache lock */

    /* Cluster chain cache (LRU) */
    fat32_cls_cache_entry_t fv_cls_cache[FAT32_CLS_CACHE_SIZE];

    /* Flags */
    uint32_t        fv_flags;       /* Vnode flags (see below) */
} fat32fs_vnode_t;

/*
 * fat32fs_vnode flags
 */
#define FV_MODIFIED     0x0001      /* Vnode has been modified */
#define FV_ROOT         0x0002      /* This is the root directory */
#define FV_ATIME_DIRTY  0x0004      /* Access time has changed */
#define FV_MTIME_DIRTY  0x0008      /* Modification time has changed */
#define FV_SIZE_DIRTY   0x0010      /* File size has changed */
#define FV_CLUSTER_DIRTY 0x0020     /* Start cluster has changed */
#define FV_CTIME_DIRTY  0x0040      /* Creation time has changed */
#define FV_ATTR_DIRTY   0x0080      /* Attributes have changed */
#define FV_DIRTY_MASK   (FV_ATIME_DIRTY | FV_MTIME_DIRTY | FV_SIZE_DIRTY | FV_CLUSTER_DIRTY | FV_CTIME_DIRTY | FV_ATTR_DIRTY)

/*
 * Flags for fat32_next_cluster()
 */
#define NC_ALLOC        0x0001      /* Allocate new cluster if at EOC */
#define NC_CLEAR        0x0002      /* Clear newly allocated cluster to zeros */

/*
 * Flags for fat32_create_fdentry
 */
#define FAT32_CRE_DIR     0x01      /* Create a directory */
#define FAT32_CRE_RENAME  0x02      /* Rename existing entry (preserve cluster) */
#define FAT32_CRE_REUSE   0x04      /* Reuse existing directory slot */

/*
 * Macros to convert between structures
 */

/* Filesystem info conversions */
#define BHV_TO_FSI(bdp)  \
    ((fat32fs_info_t *)BHV_PDATA(bdp))

#define VFS_TO_FSI(vfsp) \
    BHV_TO_FSI((vfsp)->vfs_fbhv)

/* Vnode conversions */
#ifdef FAT32_DBG
/* Debug version with validation */
fat32fs_vnode_t *fat32_bhv_to_fv_checked(bhv_desc_t *bdp, const char *func, int line);
#define BHV_TO_FV(bdp) \
    fat32_bhv_to_fv_checked((bdp), __func__, __LINE__)
#else
/* Release version - direct cast */
#define BHV_TO_FV(bdp) \
    ((fat32fs_vnode_t *)BHV_PDATA(bdp))
#endif

#define VNODE_TO_FV(vp) \
    BHV_TO_FV(VNODE_TO_FIRST_BHV(vp))

#define FV_TO_VNODE(fv) \
    ((fv)->fv_vnode)

/* Vnode allocation/deallocation helpers */
void fat32_vnode_init(fat32fs_vnode_t *fv, fat32fs_info_t *fsi);
fat32fs_vnode_t *fat32_vnode_new(fat32fs_info_t *fsi);
void fat32_vnode_reset(fat32fs_vnode_t *fv);
void fat32_vnode_delete(fat32fs_vnode_t *fv);
int fat32_vnode_validate(fat32fs_vnode_t *fv, const char *func, int line);

extern vfsops_t fat32vfsops;
extern vnodeops_t fat32vnodeops;
extern int fat32fstype;

/*
 * FAT32 cluster chain operations
 */
int fat32_rmw_cluster(fat32fs_info_t *fsi, uint32_t cluster, uint32_t *old_value, uint32_t *new_value);
int fat32_next_cluster(fat32fs_info_t *fsi, uint32_t current, uint32_t *next, unsigned int flags);
int fat32_seek_cluster(fat32fs_info_t *fsi, uint32_t start, uint32_t skip_clusters,
                       uint32_t *result, unsigned int flags);
int fat32_vnode_seek_cluster(fat32fs_vnode_t *fv, uint32_t skip_clusters,
                              uint32_t *result, unsigned int flags);
void fat32_vnode_update_cache(fat32fs_vnode_t *fv, uint32_t index, uint32_t cluster);
int fat32_free_clusters(fat32fs_info_t *fsi, uint32_t start_cluster, int free_start);

buf_t *fat32_read_cluster(fat32fs_info_t *fsi, uint32_t cluster);
uint32_t fat32_cluster_to_sector(fat32fs_info_t *fsi, uint32_t cluster);
int fat32_make_vnode(uint32_t cluster, vtype_t type, vnode_t **vpp, fat32fs_vnode_t *fv, vnode_t *parent_vp);

int fat32_clear_cluster(fat32fs_info_t *fsi, uint32_t cluster);
int fat32_init_dir(fat32fs_info_t *fsi, uint32_t new_cluster, uint32_t parent_cluster);
int fat32_faccess(fat32fs_vnode_t *fv, int mode, cred_t *cr);
void fat32_touch(fat32fs_vnode_t *fv, uint32_t flags);
int fat32_truncate(fat32fs_vnode_t *fv, off_t size);

/*
 * Time conversion
 */
void fat32_unix_to_fat_time(timespec_t *ts, uint16_t *date, uint16_t *time, uint8_t *tenths);
void fat32_fat_to_unix_time(uint16_t date, uint16_t time, uint8_t tenths, timespec_t *ts);

/*
 * String utilities for FAT32 filename handling
 */
static __inline char fat32_toupper(char c) {
    return (c >= 'a' && c <= 'z') ? (c - 32) : c;
}

static __inline int fat32_chrcmp(char c1, char c2) {
    return fat32_toupper(c1) != fat32_toupper(c2);
}
void fat32_set_dname(uint32_t pos, unsigned short s, char *dname, uint32_t *d_name_size);

/*
 * FAT32 directory operations
 */
int
fat32_get_dirent(fat32fs_vnode_t *fv, uint32_t *offset, struct dirent *dirent,
                 char *d_name, uint32_t *d_name_size, fat32fs_vnode_t *entry_fv);

int fat32_find_dirent(fat32fs_vnode_t *dir_fv, const char *name, fat32fs_vnode_t *entry_fv);
int fat32_update_fdentry(fat32fs_vnode_t *fv);
int fat32_alloc_fdentry(fat32fs_vnode_t *dir_fv, uint32_t num_entries, uint32_t *offset);
int fat32_name_is_short(const char *name, int name_len);
int fat32_create_fdentry(fat32fs_vnode_t *dir_fv, fat32fs_vnode_t *fv, char *name, int flags);
ino_t fat32_pack_ino(uint32_t dir_cluster, uint32_t offset, uint32_t num_slots);
int fat32_dirempty(fat32fs_vnode_t *fv);
int fat32_emit_dirent(struct dirent *dirent, char *d_name, uint32_t d_name_size, struct uio *uiop);
int fat32_sync_metadata(fat32fs_vnode_t *fv);
int fat32_read_fdentry(fat32fs_info_t *fsi, uint32_t dir_cluster, uint32_t offset, void *data);
int fat32_write_fdentry(fat32fs_info_t *fsi, uint32_t dir_cluster, uint32_t offset, void *data);

#ifdef FAT32_DBG_DIRENT
int fat32_check_dir_integrity(fat32fs_vnode_t *fv);
#endif

/*
 * Utilities
 */
uint32_t fat32_log2(uint32_t n);
uint8_t fat32_checksum(const uint8_t *name);
void fat32_generate_short_name(uint32_t cluster, uint32_t offset, char *short_name);

/*
 * Debug macro for VN_RELE to catch v_count issues
 */
#ifdef FAT32_DBG_RELE
#define FAT32_VN_RELE(vp) \
    do { \
        if ((vp) && (vp)->v_count <= 1) { \
            printf("FAT32_DBG: VN_RELE at %s:%d - vp=%p v_count=%d (WARNING: will go to zero or negative!)\n", \
                   __FILE__, __LINE__, (vp), (vp)->v_count); \
        } \
        VN_RELE(vp); \
    } while (0)
#else
#define FAT32_VN_RELE(vp) VN_RELE(vp)
#endif

#endif /* __FAT32FS_H */
