#include "fat32fs.h"

/*
 * fat32_truncate - Truncate or extend a file/directory
 *
 * Adjusts the cluster chain to match the requested size.
 * For truncation: frees clusters beyond the new size.
 * For extension: allocates additional clusters as needed.
 *
 * Parameters:
 *   fv   - FAT32 vnode to truncate/extend
 *   size - New size in bytes
 *
 * Returns:
 *   0 on success, error code on failure
 *
 * Notes:
 *   - For directories, fv_size must remain 0 (not updated)
 *   - For directories, new clusters are cleared (NC_CLEAR)
 *   - Size 0 means delete all clusters
 */
int
fat32_truncate(fat32fs_vnode_t *fv, off_t size)
{
    fat32fs_info_t *fsi = fv->fv_fsi;
    uint32_t needed_clusters;
    uint32_t cluster;
    uint32_t next_cluster;
    unsigned int flags;
    int error;

#ifdef FAT32_DBG_CLUSTERS
    cmn_err(CE_NOTE, "fat32_truncate: cluster=%u old_size=%lld new_size=%lld type=%d",
            fv->fv_cluster, fv->fv_size, size, fv->fv_type);
#endif

    /* Calculate how many clusters we need */
    if (size == 0) {
        needed_clusters = 0;
    } else {
        needed_clusters = (uint32_t)((size + fsi->fsi_bytes_per_cluster - 1) >> fsi->fsi_cluster_shift);
    }

    /* Special case: truncate to zero */
    if (needed_clusters == 0) {
        if (fv->fv_cluster >= FAT32_CLUSTER_MIN) {
            /* Free entire cluster chain */
            error = fat32_free_clusters(fsi, fv->fv_cluster, 1);
            if (error) {
                return error;
            }
            fv->fv_cluster = 0;
        }

        /* Invalidate entire cluster cache */
        mutex_lock(&fv->fv_cache_lock, PZERO);
        bzero(fv->fv_cls_cache, sizeof(fv->fv_cls_cache));
        mutex_unlock(&fv->fv_cache_lock);

        /* Update file size to 0 (also correct for directories) and mark dirty */
        fv->fv_size = 0;
        fv->fv_flags |= FV_SIZE_DIRTY | FV_CLUSTER_DIRTY | FV_MODIFIED;

        return 0;
    }

    /* Determine flags for cluster allocation */
    flags = NC_ALLOC;
    if (fv->fv_type == VDIR) {
        flags |= NC_CLEAR;  /* Clear new clusters for directories */
    }

    /* If file has no clusters yet, allocate the first one */
    if (fv->fv_cluster == 0) {
#ifdef FAT32_DBG_CLUSTERS
        cmn_err(CE_NOTE, "fat32_truncate: allocating first cluster");
#endif
        error = fat32_next_cluster(fsi, 0, &cluster, flags);
        if (error) {
            cmn_err(CE_WARN, "fat32_truncate: failed to allocate first cluster: %d", error);
            return error;
        }
        fv->fv_cluster = cluster;
        fv->fv_flags |= FV_CLUSTER_DIRTY | FV_MODIFIED;
#ifdef FAT32_DBG_CLUSTERS
        cmn_err(CE_NOTE, "fat32_truncate: first cluster allocated: %u", cluster);
#endif
    }

    /* Seek to the last cluster we need, allocating as needed */
#ifdef FAT32_DBG_CLUSTERS
    cmn_err(CE_NOTE, "fat32_truncate: seeking to cluster %u (need %u clusters)",
            needed_clusters - 1, needed_clusters);
#endif

    error = fat32_vnode_seek_cluster(fv, needed_clusters - 1, &cluster, flags);
    if (error) {
        cmn_err(CE_WARN, "fat32_truncate: failed to seek to cluster %u: %d", needed_clusters - 1, error);
        return error;
    }

#ifdef FAT32_DBG_CLUSTERS
    cmn_err(CE_NOTE, "fat32_truncate: last needed cluster is %u, checking for extras", cluster);
#endif

    /* Now 'cluster' points to the last cluster we need */
    /* Check if there are more clusters beyond this - if so, free them */
    error = fat32_next_cluster(fsi, cluster, &next_cluster, 0);
    if (error && error != ENOSPC) {
        cmn_err(CE_WARN, "fat32_truncate: failed to read next cluster after %u: %d", cluster, error);
        return error;
    }

#ifdef FAT32_DBG_CLUSTERS
    cmn_err(CE_NOTE, "fat32_truncate: after last cluster: error=%d next=%u", error, next_cluster);
#endif

    if (error == 0 && next_cluster >= FAT32_CLUSTER_MIN && next_cluster < FAT32_CLUSTER_EOC_MIN) {
        /* There are clusters beyond what we need - free them */
#ifdef FAT32_DBG_CLUSTERS
        cmn_err(CE_NOTE, "fat32_truncate: freeing extra clusters starting from %u", cluster);
#endif
        error = fat32_free_clusters(fsi, cluster, 0);
        if (error) {
            cmn_err(CE_WARN, "fat32_truncate: failed to free extra clusters: %d", error);
            return error;
        }

        /* Invalidate cache entries beyond truncation point */
        mutex_lock(&fv->fv_cache_lock, PZERO);
        {
            int i;
            for (i = 0; i < FAT32_CLS_CACHE_SIZE; i++) {
                if (fv->fv_cls_cache[i].cluster != 0 &&
                    fv->fv_cls_cache[i].index >= needed_clusters) {
                    fv->fv_cls_cache[i].cluster = 0;
                }
            }
        }
        mutex_unlock(&fv->fv_cache_lock);
    }

    /* Update file size (not for directories) */
    fv->fv_size = (fv->fv_type == VDIR) ? 0 : size;
    fv->fv_flags |= FV_SIZE_DIRTY | FV_MODIFIED;

#ifdef FAT32_DBG_CLUSTERS
    cmn_err(CE_NOTE, "fat32_truncate: done cluster=%u size=%lld",
            fv->fv_cluster, fv->fv_type == VDIR ? 0LL : fv->fv_size);
#endif

    return 0;
}

/*
 * fat32_make_vnode - Create and initialize a FAT32 vnode
 *
 * Creates a new vnode for the given cluster number.
 * Cluster number is used directly as the inode number.
 *
 * Parameters:
 *   fsi       - Filesystem info
 *   cluster   - Starting cluster number
 *   type      - Vnode type (VREG, VDIR, etc.)
 *   vpp       - Output vnode pointer
 *   fv        - Pre-allocated fat32fs_vnode_t structure (required, must not be NULL)
 *   parent_vp - Parent vnode pointer (NULL for root)
 *               If non-NULL, a reference will be held on parent
 */
int
fat32_make_vnode(uint32_t cluster, vtype_t type, vnode_t **vpp, fat32fs_vnode_t *fv, vnode_t *parent_vp)
{
    vnode_t *vp;
    fat32fs_info_t *fsi;

#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_make_vnode: enter cluster=%u type=%d fv=%p parent_vp=%p",
            cluster, type, fv, parent_vp);
#endif
    /* Validate that fv is provided */
    if (!fv) {
        cmn_err(CE_WARN, "fat32_make_vnode: fv is NULL, caller must provide fv structure");
        return EINVAL;
    }

    fsi = fv->fv_fsi;

    /* Validate provided fv */
    fat32_vnode_validate(fv, __func__, __LINE__);
#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_make_vnode: using provided fv=%p", fv);
#endif
    /* Allocate system vnode */
    vp = vn_alloc(fsi->fsi_vfsp, type, 0);
    if (!vp) {
        cmn_err(CE_WARN, "fat32_make_vnode: vn_alloc failed");
        cmn_err(CE_NOTE, "fat32_make_vnode: exit ENOMEM (vn_alloc)");
        return ENOMEM;
    }
#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_make_vnode: vn_alloc returned vp=%p", vp);
#endif
    /* Initialize FAT32 vnode */
    /* Note: magic, size, and fv_fsi already initialized by fat32fs_vnode_new */
    fv->fv_vnode = vp;
    fv->fv_cluster = cluster;
    fv->fv_type = type;
    mrinit(&fv->fv_lock, "fat32vnode");

    /* Store parent vnode and hold reference */
    if (parent_vp) {
        fat32fs_vnode_t *parent_fv = VNODE_TO_FV(parent_vp);
        VN_HOLD(parent_vp);
        fv->fv_parent_vp = parent_vp;
        fv->fv_parent_cluster = parent_fv->fv_cluster;
#ifdef FAT32_DBG_OTHER
        cmn_err(CE_NOTE, "fat32_make_vnode: held reference to parent_vp=%p cluster=%u",
                parent_vp, parent_fv->fv_cluster);
#endif
    } else {
        fv->fv_parent_vp = NULL;
        fv->fv_parent_cluster = 0;
    }

    /* Mark root directory by comparing cluster number */
    if (cluster == fsi->fsi_root_cluster && type == VDIR) {
        fv->fv_flags |= FV_ROOT;
        /* Root directory gets special permissions */
        fv->fv_mode = S_IFDIR | 0755;
    } else {
        fv->fv_flags = 0;
        if (fv->fv_mode == 0) {
            fv->fv_mode = (type == VDIR) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
        }
    }

    /* Set ownership from mount options */
    fv->fv_uid = fv->fv_fsi->fsi_uid;
    fv->fv_gid = fv->fv_fsi->fsi_gid;

    /* Initialize behavior descriptor and attach to vnode */
    bhv_desc_init(&fv->fv_bhv, fv, vp, &fat32vnodeops);
    vn_bhv_insert_initial(VN_BHV_HEAD(vp), &fv->fv_bhv);

    /* Set VINACTIVE_TEARDOWN flag - required when returning VN_INACTIVE_NOCACHE */
    VN_FLAGSET(vp, VINACTIVE_TEARDOWN);

    *vpp = vp;
#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_make_vnode: exit success, vp=%p", vp);
#endif
    return 0;
}

/*
 * fat32_touch - Update vnode timestamps
 *
 * Updates one or more timestamps on a vnode based on the provided flags.
 * Uses the FV_*_DIRTY flags to determine which timestamps to update.
 *
 * Parameters:
 *   fv    - FAT32 vnode to update
 *   flags - Bitmask of FV_ATIME_DIRTY, FV_MTIME_DIRTY, or a special
 *           value 0 to update ctime/mtime/atime (for creation)
 *
 * Notes:
 *   - If flags is 0, sets all three times (ctime, mtime, atime) to current time
 *   - Otherwise, updates only the times specified by the flags
 *   - Always marks the vnode as modified
 */
void
fat32_touch(fat32fs_vnode_t *fv, uint32_t flags)
{
    timespec_t now;

    nanotime(&now);

    /* Special case: flags == 0 means creation time (set all three) */
    if (flags == 0) {
        fv->fv_ctime = now;
        fv->fv_mtime = now;
        fv->fv_atime = now;
        fv->fv_flags |= FV_MTIME_DIRTY | FV_ATIME_DIRTY | FV_MODIFIED;
        return;
    }

    /* Update specified timestamps */
    if (flags & FV_ATIME_DIRTY) {
        fv->fv_atime = now;
    }

    if (flags & FV_MTIME_DIRTY) {
        fv->fv_mtime = now;
    }

    /* Always mark as modified with the specified flags */
    fv->fv_flags |= (flags & (FV_ATIME_DIRTY | FV_MTIME_DIRTY)) | FV_MODIFIED;
}

/*
 * fat32_faccess - Check access permissions
 */
int
fat32_faccess(fat32fs_vnode_t *fv, int mode, cred_t *cr)
{
    mode_t fmode = fv->fv_mode;
    uid_t uid = fv->fv_uid;
    gid_t gid = fv->fv_gid;
    int denied_mode = 0;

    /* Check for read-only filesystem */
    if ((mode & VWRITE) && (fv->fv_fsi->fsi_vfsp->vfs_flag & VFS_RDONLY)) {
        return EROFS;
    }

    /* Check for read-only attribute on file */
    if ((mode & VWRITE) && (fv->fv_attr & FAT32_ATTR_READ_ONLY)) {
        return EACCES;
    }

    /* If root, grant access */
    if (cr->cr_uid == 0) {
        return 0;
    }

    /* Check owner permissions */
    if (cr->cr_uid == uid) {
        if ((mode & VREAD) && !(fmode & S_IRUSR)) denied_mode |= VREAD;
        if ((mode & VWRITE) && !(fmode & S_IWUSR)) denied_mode |= VWRITE;
        if ((mode & VEXEC) && !(fmode & S_IXUSR)) denied_mode |= VEXEC;
    }
    /* Check group permissions */
    else if (groupmember(gid, cr)) {
        if ((mode & VREAD) && !(fmode & S_IRGRP)) denied_mode |= VREAD;
        if ((mode & VWRITE) && !(fmode & S_IWGRP)) denied_mode |= VWRITE;
        if ((mode & VEXEC) && !(fmode & S_IXGRP)) denied_mode |= VEXEC;
    }
    /* Check other permissions */
    else {
        if ((mode & VREAD) && !(fmode & S_IROTH)) denied_mode |= VREAD;
        if ((mode & VWRITE) && !(fmode & S_IWOTH)) denied_mode |= VWRITE;
        if ((mode & VEXEC) && !(fmode & S_IXOTH)) denied_mode |= VEXEC;
    }

    if (denied_mode) {
        return EACCES;
    }

    return 0;
}
