#include "fat32fs.h"

/*
 * Vnode operations stubs - all return ENOSYS (not implemented)
 */

static int
fat32_open(bhv_desc_t *bdp, vnode_t **vpp, mode_t mode, cred_t *cr)
{
    fat32fs_vnode_t *fv;
    vnode_t *vp;

    fv = BHV_TO_FV(bdp);
    vp = FV_TO_VNODE(fv);

#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_open: enter vp=%p mode=0x%x type=%d", vp, mode, fv->fv_type);
#endif
    /* Check if trying to open a directory for writing */
    if (fv->fv_type == VDIR && (mode & (FWRITE | FTRUNC))) {
        cmn_err(CE_NOTE, "fat32_open: cannot open directory for write");
        return EISDIR;
    }

#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_open: exit success");
#endif
    return 0;
}

static int
fat32_close(bhv_desc_t *bdp, int flag, lastclose_t lastclose, cred_t *cr)
{
    fat32fs_vnode_t *fv;
    fat32fs_vnode_t *parent_fv = NULL;

    fv = BHV_TO_FV(bdp);

#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_close: enter flag=%d lastclose=%d cluster=%u dirty=0x%x",
            flag, lastclose, fv->fv_cluster, fv->fv_flags & FV_DIRTY_MASK);
#endif
    /* On last close, flush dirty metadata to disk */
    if (lastclose == L_TRUE && (fv->fv_flags & FV_DIRTY_MASK)) {
#ifdef FAT32_DBG_OTHER
        cmn_err(CE_NOTE, "fat32_close: last close with dirty metadata, flushing");
#endif
        fat32_sync_metadata(fv);
    }

#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_close: exit success");
#endif
    return 0;
}

static int
fat32_read(bhv_desc_t *bdp, struct uio *uiop, int ioflag, cred_t *cr, struct flid *fl)
{
    fat32fs_vnode_t *fv;
    fat32fs_info_t *fsi;
    off_t file_offset;
    off_t file_size;
    ssize_t remaining;
    uint32_t skip_clusters;
    uint32_t offset_in_cluster;
    uint32_t current_cluster;
    int error = 0;

    fv = BHV_TO_FV(bdp);
    fsi = fv->fv_fsi;
#ifdef FAT32_DBG_RW
    cmn_err(CE_NOTE, "fat32_read: enter offset=%lld resid=%d size=%lld",
            (long long)uiop->uio_offset, uiop->uio_resid, (long long)fv->fv_size);
#endif
    /* Verify this is a regular file (no lock needed - type never changes) */
    if (fv->fv_type != VREG) {
        cmn_err(CE_WARN, "fat32_read: not a regular file");
        return EISDIR;
    }

    /* Take update lock to modify atime */
    mrupdate(&fv->fv_lock);

    /* Update access time */
    fat32_touch(fv, FV_ATIME_DIRTY);

    file_offset = uiop->uio_offset;
    file_size = fv->fv_size;

    /* Demote to read lock for the actual read operation */
    mrdemote(&fv->fv_lock);

    /* Validate offset and size */
    if (file_offset < 0) {
        cmn_err(CE_WARN, "fat32_read: invalid offset %lld", (long long)file_offset);
        mraccunlock(&fv->fv_lock);
        return EINVAL;
    }

    /* Discard reads beyond EOF */
    if (file_offset >= file_size) {
        cmn_err(CE_NOTE, "fat32_read: offset beyond EOF, nothing to read");
        mraccunlock(&fv->fv_lock);
        return 0;
    }

    /* Clip read size to file size */
    remaining = uiop->uio_resid;
    if (file_offset + remaining > file_size) {
        remaining = file_size - file_offset;
        cmn_err(CE_NOTE, "fat32_read: clipping read to %d bytes", (int)remaining);
    }

    if (remaining == 0) {
        mraccunlock(&fv->fv_lock);
        return 0;
    }

    /* Calculate starting cluster and offset within cluster */
    skip_clusters = (uint32_t)(file_offset >> fsi->fsi_cluster_shift);
    offset_in_cluster = (uint32_t)(file_offset & fsi->fsi_cluster_mask);

    /* Seek to starting cluster */
    error = fat32_vnode_seek_cluster(fv, skip_clusters, &current_cluster, 0);
    if (error) {
        cmn_err(CE_WARN, "fat32_read: seek failed: %d", error);
        mraccunlock(&fv->fv_lock);
        return error;
    }

#ifdef FAT32_DBG_RW
    cmn_err(CE_NOTE, "fat32_read: starting at cluster %u, offset %u",
            current_cluster, offset_in_cluster);
#endif
    /* Read loop - process one cluster at a time */
    while (remaining > 0) {
        uint32_t bytes_to_copy;
        buf_t *bp;

#ifdef FAT32_DBG_RW
        cmn_err(CE_NOTE, "fat32_read: reading cluster %u", current_cluster);
#endif
        bp = fat32_read_cluster(fsi, current_cluster);
        if (bp->b_flags & B_ERROR) {
            cmn_err(CE_WARN, "fat32_read: bread failed for cluster %u", current_cluster);
            error = bp->b_error ? bp->b_error : EIO;
            brelse(bp);
            mraccunlock(&fv->fv_lock);
            return error;
        }

        /* Calculate how much to copy */
        bytes_to_copy = fsi->fsi_bytes_per_cluster - offset_in_cluster;
        if (bytes_to_copy > remaining) {
            bytes_to_copy = (uint32_t)remaining;
        }

#ifdef FAT32_DBG_RW
        cmn_err(CE_NOTE, "fat32_read: copying %u bytes", bytes_to_copy);
#endif
        /* Copy to user space */
        error = uiomove((char *)bp->b_un.b_addr + offset_in_cluster, bytes_to_copy, UIO_READ, uiop);
        brelse(bp);

        if (error) {
            cmn_err(CE_WARN, "fat32_read: uiomove failed: %d", error);
            mraccunlock(&fv->fv_lock);
            return error;
        }

        remaining -= bytes_to_copy;
        offset_in_cluster = 0;  /* No offset after first read */

        /* Move to next cluster if more data remains */
        if (remaining > 0) {
            skip_clusters++;
            error = fat32_next_cluster(fsi, current_cluster, &current_cluster, 0);
            if (error) {
                cmn_err(CE_WARN, "fat32_read: unexpected end of file");
                mraccunlock(&fv->fv_lock);
                return EIO;
            }
        }
    } /* end of read loop */

    /* Update cache with final read position */
    fat32_vnode_update_cache(fv, skip_clusters, current_cluster);

    mraccunlock(&fv->fv_lock);
#ifdef FAT32_DBG_RW
    cmn_err(CE_NOTE, "fat32_read: exit success, new offset=%lld",
            (long long)uiop->uio_offset);
#endif
    return 0;
}

static int
fat32_write(bhv_desc_t *bdp, struct uio *uiop, int ioflag, cred_t *cr, struct flid *fl)
{
    fat32fs_vnode_t *fv;
    fat32fs_info_t *fsi;
    off_t file_offset;
    ssize_t remaining;
    uint32_t skip_clusters;
    uint32_t offset_in_cluster;
    uint32_t current_cluster;
    int error = 0;

    fv = BHV_TO_FV(bdp);
    fsi = fv->fv_fsi;

#ifdef FAT32_DBG_RW
    cmn_err(CE_NOTE, "fat32_write: enter offset=%lld resid=%d size=%lld",
            (long long)uiop->uio_offset, uiop->uio_resid, (long long)fv->fv_size);
#endif

    /* Verify this is a regular file (no lock needed - type never changes) */
    if (fv->fv_type != VREG) {
        cmn_err(CE_WARN, "fat32_write: not a regular file");
        return EISDIR;
    }

    /* Take write lock */
    mrupdate(&fv->fv_lock);

    /* Update modification time */
    fat32_touch(fv, FV_MTIME_DIRTY);

    file_offset = uiop->uio_offset;
    remaining = uiop->uio_resid;

    if (remaining == 0) {
        mrunlock(&fv->fv_lock);
        return 0;
    }

    /* Handle append mode */
    if (ioflag & IO_APPEND) {
        file_offset = fv->fv_size;
        uiop->uio_offset = file_offset;
    }

    /* Handle empty file allocation */
    if (fv->fv_cluster == 0) {
#ifdef FAT32_DBG_RW
        cmn_err(CE_NOTE, "fat32_write: empty file, allocating first cluster");
#endif
        error = fat32_next_cluster(fsi, 0, &fv->fv_cluster, NC_ALLOC);
        if (error) {
            mrunlock(&fv->fv_lock);
            return error;
        }
        fv->fv_flags |= FV_CLUSTER_DIRTY;
#ifdef FAT32_DBG_RW
        cmn_err(CE_NOTE, "fat32_write: empty file, allocated cluster=%u", fv->fv_cluster);
#endif
    }

    /* Calculate starting cluster and offset within cluster */
    skip_clusters = (uint32_t)(file_offset >> fsi->fsi_cluster_shift);
    offset_in_cluster = (uint32_t)(file_offset & fsi->fsi_cluster_mask);

#ifdef FAT32_DBG_RW
    cmn_err(CE_WARN, "fat32_write: seeking to cluster offset %u", skip_clusters);
#endif
    /* Seek to starting cluster, allocating if necessary */
    error = fat32_vnode_seek_cluster(fv, skip_clusters, &current_cluster, NC_ALLOC);
    if (error) {
        cmn_err(CE_WARN, "fat32_write: seek failed: %d", error);
        mrunlock(&fv->fv_lock);
        return error;
    }
#ifdef FAT32_DBG_RW
    cmn_err(CE_NOTE, "fat32_write: seek done, current_cluster=%u", current_cluster);
#endif

    /* Write loop - process one cluster at a time */
    while (remaining > 0) {
        uint32_t bytes_to_copy;
        buf_t *bp;

#ifdef FAT32_DBG_RW
        cmn_err(CE_NOTE, "fat32_write: processing cluster %u", current_cluster);
#endif
        bp = fat32_read_cluster(fsi, current_cluster);
        if (bp->b_flags & B_ERROR) {
            cmn_err(CE_WARN, "fat32_write: bread failed for cluster %u", current_cluster);
            error = bp->b_error ? bp->b_error : EIO;
            brelse(bp);
            mrunlock(&fv->fv_lock);
            return error;
        }

        bytes_to_copy = fsi->fsi_bytes_per_cluster - offset_in_cluster;
        if (bytes_to_copy > remaining) bytes_to_copy = (uint32_t)remaining;

#ifdef FAT32_DBG_RW
        cmn_err(CE_NOTE, "fat32_write: uiomove bytes=%u", bytes_to_copy);
#endif
        error = uiomove((char *)bp->b_un.b_addr + offset_in_cluster, bytes_to_copy, UIO_WRITE, uiop);
        if (error) {
            brelse(bp);
            mrunlock(&fv->fv_lock);
            return error;
        }

        bdwrite(bp);
#ifdef FAT32_DBG_RW
        cmn_err(CE_NOTE, "fat32_write: write done for cluster %u", current_cluster);
#endif

        /* Update file size after every write to handle partial writes */
        if (uiop->uio_offset > fv->fv_size) {
            off_t old_size = fv->fv_size;
            fv->fv_size = uiop->uio_offset;
            fv->fv_flags |= FV_SIZE_DIRTY;
            cmn_err(CE_NOTE, "fat32_write: file size updated from %lld to %lld",
                    (long long)old_size, (long long)fv->fv_size);
        }

        remaining -= bytes_to_copy;
        offset_in_cluster = 0;

        /* Move to next cluster if more data remains, allocating if needed */
        if (remaining > 0) {
            skip_clusters++;
            error = fat32_next_cluster(fsi, current_cluster, &current_cluster, NC_ALLOC);
            if (error) {
                cmn_err(CE_WARN, "fat32_write: failed to get/allocate next cluster: %d", error);
                mrunlock(&fv->fv_lock);
                return error;
            }
        }
    }

    /* Update cache with final write position */
    fat32_vnode_update_cache(fv, skip_clusters, current_cluster);

    mrunlock(&fv->fv_lock);
#ifdef FAT32_DBG_RW
    cmn_err(CE_NOTE, "fat32_write: exit success");
#endif
    return 0;
}

static int
fat32_ioctl(bhv_desc_t *bdp, int cmd, void *arg, int flag, cred_t *cr, int *rvalp, struct vopbd *vopbdp)
{
    cmn_err(CE_NOTE, "fat32_ioctl: enter cmd=%d", cmd);
    cmn_err(CE_NOTE, "fat32_ioctl: exit ENOSYS");
    return ENOSYS;
}

static int
fat32_getattr(bhv_desc_t *bdp, struct vattr *vap, int flags, cred_t *cr)
{
    fat32fs_vnode_t *fv;
    fat32fs_info_t *fsi;
#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_getattr: enter flags=%d", flags);
#endif
    fv = BHV_TO_FV(bdp);
    fsi = fv->fv_fsi;

    /* Populate vattr structure from our vnode */
    vap->va_type = fv->fv_type;
    vap->va_mode = fv->fv_mode;
    /* Return vnode uid/gid (initialized from fsi, modifiable by setattr) */
    vap->va_uid = fv->fv_uid;
    vap->va_gid = fv->fv_gid;
    vap->va_fsid = fsi->fsi_dev;
    vap->va_nodeid = (ino_t)fv->fv_cluster;  /* Use cluster as inode number */
    vap->va_nlink = (fv->fv_type == VDIR) ? 2 : 1;  /* Directories: 2, files: 1 */

    /* Set size */
    if (fv->fv_type == VDIR) {
        /* Directories: calculate size by walking cluster chain */
        uint32_t current_cluster = fv->fv_cluster;
        uint32_t num_clusters = 0;
        int error;

        /* Count clusters in chain */
        while (current_cluster >= FAT32_CLUSTER_MIN &&
               current_cluster < FAT32_CLUSTER_EOC_MIN) {
            num_clusters++;
            error = fat32_next_cluster(fsi, current_cluster, &current_cluster, 0);
            if (error) {
                break;  /* Stop on error, use what we have */
            }
        }

        if (num_clusters == 0) {
            num_clusters = 1;  /* At least one cluster */
        }

        vap->va_size = num_clusters * fsi->fsi_bytes_per_cluster;
        vap->va_nblocks = num_clusters;
#ifdef FAT32_DBG_OTHER
        cmn_err(CE_NOTE, "fat32_getattr: directory size=%lld blocks=%u",
                (long long)vap->va_size, num_clusters);
#endif                
    } else {
        vap->va_size = fv->fv_size;
        vap->va_nblocks = (fv->fv_size + fsi->fsi_bytes_per_cluster - 1) >> fsi->fsi_cluster_shift;
#ifdef FAT32_DBG_OTHER
        cmn_err(CE_NOTE, "fat32_getattr: file size=%lld blocks=%lld",
                (long long)vap->va_size, (long long)vap->va_nblocks);
#endif
    }

    vap->va_atime = fv->fv_atime;
    vap->va_mtime = fv->fv_mtime;
    vap->va_ctime = fv->fv_ctime;
    vap->va_rdev = 0;  /* Not a device file */
    vap->va_blksize = fsi->fsi_bytes_per_cluster;

    /* Extended attributes - not used by FAT32 */
    vap->va_vcode = 0;
    vap->va_xflags = 0;
    vap->va_extsize = 0;
    vap->va_nextents = 0;
    vap->va_anextents = 0;
    vap->va_projid = 0;
    vap->va_gencount = 0;

    /* Set mask to indicate which fields are valid */
    vap->va_mask = AT_TYPE | AT_MODE | AT_UID | AT_GID | AT_FSID | AT_NODEID |
                   AT_NLINK | AT_SIZE | AT_ATIME | AT_MTIME | AT_CTIME |
                   AT_RDEV | AT_BLKSIZE | AT_NBLOCKS;

#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_getattr: exit success type=%d mode=0%o size=%lld",
            vap->va_type, vap->va_mode, (long long)vap->va_size);
#endif
    return 0;
}

static int
fat32_setattr(bhv_desc_t *bdp, struct vattr *vap, int flags, cred_t *cr)
{
    fat32fs_vnode_t *fv = BHV_TO_FV(bdp);
    int error = 0;
    int mask = vap->va_mask;
    int sync_needed = 0;

#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_setattr: enter flags=%d", flags);
#endif
    /* Check for read-only filesystem */
    if (fv->fv_fsi->fsi_vfsp->vfs_flag & VFS_RDONLY) {
        return EROFS;
    }

    mrupdate(&fv->fv_lock);

    /* Handle size change */
    if (mask & AT_SIZE) {
        if (fv->fv_type == VDIR) {
            error = EISDIR;
            goto out;
        }
        error = fat32_truncate(fv, vap->va_size);
        if (error) goto out;
    }

    /* Handle mode change (Read-Only attribute) */
    if (mask & AT_MODE) {
        fv->fv_mode = vap->va_mode;
        if (vap->va_mode & S_IWUSR) {
            if (fv->fv_attr & FAT32_ATTR_READ_ONLY) {
                fv->fv_attr &= ~FAT32_ATTR_READ_ONLY;
                fv->fv_flags |= FV_ATTR_DIRTY;
            }
        } else {
            if (!(fv->fv_attr & FAT32_ATTR_READ_ONLY)) {
                fv->fv_attr |= FAT32_ATTR_READ_ONLY;
                fv->fv_flags |= FV_ATTR_DIRTY;
            }
        }
    }

    /* Handle ownership (in-memory only) */
    if (mask & AT_UID) fv->fv_uid = vap->va_uid;
    if (mask & AT_GID) fv->fv_gid = vap->va_gid;

    /* Handle timestamps */
    if (mask & AT_ATIME) {
        fv->fv_atime = vap->va_atime;
        fv->fv_flags |= FV_ATIME_DIRTY;
    }
    if (mask & AT_MTIME) {
        fv->fv_mtime = vap->va_mtime;
        fv->fv_flags |= FV_MTIME_DIRTY;
    }
    if (mask & AT_CTIME) {
        fv->fv_ctime = vap->va_ctime;
        fv->fv_flags |= FV_CTIME_DIRTY;
    }

    /* Sync metadata if needed */
    if (fv->fv_flags & FV_DIRTY_MASK) {
        sync_needed = 1;
    }

out:
    mrunlock(&fv->fv_lock);

    if (!error && sync_needed) {
        error = fat32_sync_metadata(fv);
    }

    return error;
}

static int
fat32_access(bhv_desc_t *bdp, int mode, cred_t *cr)
{
    fat32fs_vnode_t *fv = BHV_TO_FV(bdp);

#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_access: enter mode=%d attr=0x%x", mode, fv->fv_attr);
#endif
    return fat32_faccess(fv, mode, cr);
}

static int
fat32_lookup(bhv_desc_t *bdp, char *nm, vnode_t **vpp, struct pathname *pnp, int flags, vnode_t *rdir, cred_t *cr)
{
    fat32fs_vnode_t *dir_fv;
    fat32fs_vnode_t *fv;
    fat32fs_info_t *fsi;
    vnode_t *vp;
    int error;

#ifdef FAT32_DBG_DIRENT
    cmn_err(CE_NOTE, "fat32_lookup: enter nm='%s' flags=%` d", nm ? nm : "(null)", flags);
#endif
    if (!nm || !vpp) {
        cmn_err(CE_WARN, "fat32_lookup: invalid parameters");
        return EINVAL;
    }

    dir_fv = BHV_TO_FV(bdp);
    fsi = dir_fv->fv_fsi;

    /* Verify this is a directory */
    if (dir_fv->fv_type != VDIR) {
        cmn_err(CE_WARN, "fat32_lookup: not a directory");
        return ENOTDIR;
    }

    /* Handle "." lookup */
    if (nm[0] == '.' && nm[1] == '\0') {
        vp = FV_TO_VNODE(dir_fv);
        VN_HOLD(vp);
        *vpp = vp;
#ifdef FAT32_DBG_DIRENT
        cmn_err(CE_NOTE, "fat32_lookup: exit success (current dir)");
#endif
        return 0;
    }

    /* Handle ".." lookup */
    if (nm[0] == '.' && nm[1] == '.' && nm[2] == '\0') {
        /* For root directory, return 0 and leave vpp unmodified (like EFS) */
        if (dir_fv->fv_flags & FV_ROOT) {
#ifdef FAT32_DBG_DIRENT
            cmn_err(CE_NOTE, "fat32_lookup: exit success (root parent, vpp unmodified)");
#endif            
            return 0;
        }

        /* For non-root, return reference to parent from parent chain */
        if (!dir_fv->fv_parent_vp) {
            /* No parent vnode available, return error */
            cmn_err(CE_WARN, "fat32_lookup: no parent vnode in chain");
            return ENOENT;
        }

        /* Return reference to parent vnode */
        vp = dir_fv->fv_parent_vp;
        VN_HOLD(vp);
        *vpp = vp;
#ifdef FAT32_DBG_DIRENT
        cmn_err(CE_NOTE, "fat32_lookup: exit success (parent dir from chain, vp=%p)", vp);
#endif
        return 0;
    }

    /* Allocate temporary vnode structure to hold entry metadata */
    fv = fat32_vnode_new(fsi);
    if (!fv) {
        cmn_err(CE_WARN, "fat32_lookup: fat32_vnode_new failed");
        return ENOMEM;
    }

    /* Search for the directory entry */
    mraccess(&dir_fv->fv_lock);
    error = fat32_find_dirent(dir_fv, nm, fv);
    mraccunlock(&dir_fv->fv_lock);

    if (error) {
        /* Not found or other error */
        fat32_vnode_delete(fv);
#ifdef FAT32_DBG_DIRENT
        if (error == ENOENT) {
            cmn_err(CE_NOTE, "fat32_lookup: exit ENOENT (not found)");
        } else {
            cmn_err(CE_WARN, "fat32_lookup: fat32_find_dirent failed: %d", error);
        }
#endif
        return error;
    }

    /* Found matching entry - create vnode using the pre-filled fv structure */
#ifdef FAT32_DBG_DIRENT
    cmn_err(CE_NOTE, "fat32_lookup: found match cluster=%u", fv->fv_cluster);
#endif

    /* Pass current directory vnode as parent to maintain parent chain */
    error = fat32_make_vnode(fv->fv_cluster, fv->fv_type, &vp, fv, FV_TO_VNODE(dir_fv));
    if (error) {
        fat32_vnode_delete(fv);
        cmn_err(CE_WARN, "fat32_lookup: failed to create vnode: %d", error);
        return error;
    }

    /* fat32_make_vnode now uses our fv structure directly, no need to copy */
    /* The fv structure is now owned by the vnode, don't free it */
#ifdef FAT32_DBG_DIRENT
    cmn_err(CE_NOTE, "fat32_lookup: created vnode location %u:%u:%u (parent:offset:count)",
            fv->fv_parent_cluster, fv->fv_diroff, fv->fv_dirsz);
#endif
    *vpp = vp;
#ifdef FAT32_DBG_DIRENT
    cmn_err(CE_NOTE, "fat32_lookup: exit success vp=%p", vp);
#endif
    return 0;
}

static int
fat32_create(bhv_desc_t *bdp, char *name, struct vattr *vap, int flags, int mode, vnode_t **vpp, cred_t *cr)
{
    fat32fs_vnode_t *dir_fv = BHV_TO_FV(bdp);
    fat32fs_vnode_t *fv;
    int error;

#ifdef FAT32_DBG_DIRENT
    cmn_err(CE_NOTE, "fat32_create: enter name='%s' flags=0x%x", name ? name : "(null)", flags);
#endif
    if (!name || !vpp)
        return EINVAL;

    /* Verify this is a directory */
    if (dir_fv->fv_type != VDIR)
        return ENOTDIR;

    /* Check access permissions on parent directory */
    error = fat32_faccess(dir_fv, VWRITE | VEXEC, cr);
    if (error) {
        return error;
    }

    /* Allocate vnode structure */
    fv = fat32_vnode_new(dir_fv->fv_fsi);
    if (!fv)
        return ENOMEM;

    /* Lock directory for update */
    mrupdate(&dir_fv->fv_lock);

    /* Check if file already exists */
    error = fat32_find_dirent(dir_fv, name, fv);

    if (error == 0) {
        /* Entry exists - check if it's a directory */
        if (fv->fv_type == VDIR) {
            /* Allow opening directory for read-only, deny for write */
            if (mode & (VWRITE)) {
#ifdef FAT32_DBG_DIRENT
                cmn_err(CE_NOTE, "fat32_create: cannot open directory for write");
#endif
                mrunlock(&dir_fv->fv_lock);
                fat32_vnode_delete(fv);
                return EISDIR;
            }

            /* Directory opened read-only - return vnode for readdir */
#ifdef FAT32_DBG_DIRENT
            cmn_err(CE_NOTE, "fat32_create: opening existing directory read-only");
#endif
            error = fat32_make_vnode(fv->fv_cluster, fv->fv_type, vpp, fv, FV_TO_VNODE(dir_fv));
            mrunlock(&dir_fv->fv_lock);
            if (error) {
                fat32_vnode_delete(fv);
                return error;
            }
            return 0;
        }

        /* File exists */
        if (flags & VEXCL) {
            /* Exclusive create - file must not exist */
#ifdef FAT32_DBG_DIRENT
            cmn_err(CE_NOTE, "fat32_create: file exists and VEXCL flag set");
#endif
            mrunlock(&dir_fv->fv_lock);
            fat32_vnode_delete(fv);
            return EEXIST;
        }

        /* File exists but VEXCL not set - check access */
        error = fat32_faccess(fv, mode, cr);
        if (error) {
#ifdef FAT32_DBG_DIRENT
            cmn_err(CE_NOTE, "fat32_create: access denied to existing file (mode=%d attr=0x%x)",
                    mode, fv->fv_attr);
#endif
            mrunlock(&dir_fv->fv_lock);
            fat32_vnode_delete(fv);
            return error;
        }

        /* Truncate if size specified */
        if (vap->va_mask & AT_SIZE) {
#ifdef FAT32_DBG_DIRENT
            cmn_err(CE_NOTE, "fat32_create: file exists, truncating to %lld", (long long)vap->va_size);
#endif
            error = fat32_truncate(fv, vap->va_size);
            if (error) {
                mrunlock(&dir_fv->fv_lock);
                fat32_vnode_delete(fv);
                return error;
            }
        }

        /* Set creation and modification times */
        fat32_touch(fv, 0);  /* 0 = set all times (ctime, mtime, atime) */

        /* Update directory mtime */
        fat32_touch(dir_fv, FV_MTIME_DIRTY);

        /* Create vnode from modified entry */
        error = fat32_make_vnode(fv->fv_cluster, fv->fv_type, vpp, fv, FV_TO_VNODE(dir_fv));
        mrunlock(&dir_fv->fv_lock);
        if (error) {
            fat32_vnode_delete(fv);
            return error;
        }
        return 0;
    }

    if (error != ENOENT) {
        /* Unexpected error during search */
        mrunlock(&dir_fv->fv_lock);
        fat32_vnode_delete(fv);
        return error;
    }

    /* File doesn't exist - create it */
    fv->fv_mode = vap->va_mode;
    error = fat32_create_fdentry(dir_fv, fv, name, 0); /* 0 flags = regular file */

    if (!error) {
        /* Set creation times for new file */
        fat32_touch(fv, 0);  /* 0 = set all times (ctime, mtime, atime) */

        /* Update directory mtime */
        fat32_touch(dir_fv, FV_MTIME_DIRTY);

#ifdef FAT32_DBG_DIRENT
        /* Check directory integrity after creation */
        fat32_check_dir_integrity(dir_fv);
#endif
    }

    mrunlock(&dir_fv->fv_lock);

    if (error) {
        fat32_vnode_delete(fv);
        return error;
    }

#ifdef FAT32_DBG_DIRENT
    cmn_err(CE_NOTE, "fat32_create: created file '%s' location %u:%u:%u (parent:offset:count)",
            name, fv->fv_parent_cluster, fv->fv_diroff, fv->fv_dirsz);
#endif
    *vpp = fv->fv_vnode;
#ifdef FAT32_DBG_DIRENT
    cmn_err(CE_NOTE, "fat32_create: exit vp=%p", *vpp);
#endif
    return 0;
}

/*
 * fat32_delete_fdentry_chain - Mark a sequence of directory entries as deleted
 */
static int
fat32_delete_fdentry_chain(fat32fs_info_t *fsi, uint32_t dir_cluster, uint32_t offset, uint32_t count)
{
    uint32_t skip_clusters = offset >> fsi->fsi_cluster_shift;
    uint32_t cluster_off = offset & fsi->fsi_cluster_mask;
    uint32_t current_cluster;
    buf_t *bp;
    int error;
    uint32_t i;

    /* Seek to the starting cluster */
    error = fat32_seek_cluster(fsi, dir_cluster, skip_clusters, &current_cluster, 0);
    if (error) return error;

    bp = fat32_read_cluster(fsi, current_cluster);
    if (bp->b_flags & B_ERROR) {
        error = bp->b_error ? bp->b_error : EIO;
        brelse(bp);
        return error;
    }

    for (i = 0; i < count; i++) {
        /* Check if we need to move to next cluster */
        if (cluster_off >= fsi->fsi_bytes_per_cluster) {
            bdwrite(bp); /* Write out current cluster */

            error = fat32_next_cluster(fsi, current_cluster, &current_cluster, 0);
            if (error) return error;

            bp = fat32_read_cluster(fsi, current_cluster);
            if (bp->b_flags & B_ERROR) {
                error = bp->b_error ? bp->b_error : EIO;
                brelse(bp);
                return error;
            }
            cluster_off = 0;
        }

        /* Mark entry as deleted */
        *(uint8_t *)((char *)bp->b_un.b_addr + cluster_off) = FAT32_LFN_DELETED;
        cluster_off += FAT32_DENTRY_SIZE;
    }

    bdwrite(bp);
    return 0;
}

/*
 * fat32_remove_common - Common logic for remove and rmdir
 */
static int
fat32_remove_common(fat32fs_vnode_t *dir_fv, char *name, int is_dir, vnode_t *cdir, cred_t *cr)
{
    fat32fs_vnode_t *fv;
    fat32fs_info_t *fsi = dir_fv->fv_fsi;
    int error;
    uint32_t dir_cluster;
    uint32_t entry_offset;

    /* Check for . and .. */
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
        return EINVAL;
    }

    /* Check access permissions on parent directory */
    error = fat32_faccess(dir_fv, VWRITE | VEXEC, cr);
    if (error) {
        return error;
    }

    /* Allocate temp vnode for lookup */
    fv = fat32_vnode_new(fsi);
    if (!fv) {
        return ENOMEM;
    }

    mrupdate(&dir_fv->fv_lock);

    /* Find the entry */
    error = fat32_find_dirent(dir_fv, name, fv);
    if (error) {
        mrunlock(&dir_fv->fv_lock);
        fat32_vnode_delete(fv);
        return error;
    }

    /* Check type match */
    if (is_dir) {
        if (fv->fv_type != VDIR) {
            mrunlock(&dir_fv->fv_lock);
            fat32_vnode_delete(fv);
            return ENOTDIR;
        }
        
        /* Check if trying to remove current directory */
        if (cdir) {
            fat32fs_vnode_t *cdir_fv = VNODE_TO_FV(cdir);
            if (fv->fv_cluster == cdir_fv->fv_cluster) {
                mrunlock(&dir_fv->fv_lock);
                fat32_vnode_delete(fv);
                return EINVAL;
            }
        }

        /* Check if empty */
        error = fat32_dirempty(fv);
        if (error) {
            mrunlock(&dir_fv->fv_lock);
            fat32_vnode_delete(fv);
            return error; /* ENOTEMPTY or other error */
        }
    } else {
        if (fv->fv_type == VDIR) {
            mrunlock(&dir_fv->fv_lock);
            fat32_vnode_delete(fv);
            return EISDIR;
        }
    }

    /*
     * Mark entry as deleted.
     * We need to mark the 8.3 entry and any preceding LFN entries.
     */
    dir_cluster = fv->fv_parent_cluster;
    entry_offset = fv->fv_diroff;

    /* Validate parent cluster */
    if (dir_cluster == FAT32_CLUSTER_BAD) {
        cmn_err(CE_WARN, "fat32_remove_common: invalid parent cluster");
        return EINVAL;
    }

    error = fat32_delete_fdentry_chain(fsi, dir_cluster, entry_offset, fv->fv_dirsz);
    if (error) {
        cmn_err(CE_WARN, "fat32_remove_common: failed to delete entries at offset %u", entry_offset);
    }

    /* Free clusters */
    if (fv->fv_cluster != 0) {
        error = fat32_free_clusters(fsi, fv->fv_cluster, 1);
        if (error) {
            cmn_err(CE_WARN, "fat32_remove_common: failed to free clusters starting at %u", fv->fv_cluster);
        }
    }

    /* Update parent directory mtime */
    fat32_touch(dir_fv, FV_MTIME_DIRTY);

#ifdef FAT32_DBG_DIRENT
    /* Check directory integrity after deletion */
    fat32_check_dir_integrity(dir_fv);
#endif

    mrunlock(&dir_fv->fv_lock);
    fat32_vnode_delete(fv);

    return 0;
}

static int
fat32_remove(bhv_desc_t *bdp, char *nm, cred_t *cr)
{
    fat32fs_vnode_t *dir_fv = BHV_TO_FV(bdp);
    return fat32_remove_common(dir_fv, nm, 0 /* file */, NULL, cr);
}

static int
fat32_link(bhv_desc_t *bdp, vnode_t *tdvp, char *tnm, cred_t *cr)
{
#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_link: enter tnm='%s'", tnm ? tnm : "(null)");
    cmn_err(CE_NOTE, "fat32_link: exit ENOSYS");
#endif    
    return ENOSYS;
}

static int
fat32_rename(bhv_desc_t *bdp, char *snm, vnode_t *tdvp, char *tnm, struct pathname *tpnp, cred_t *cr)
{
    fat32fs_vnode_t *sdvp = BHV_TO_FV(bdp);
    fat32fs_vnode_t *tdvp_fv = VNODE_TO_FV(tdvp);
    fat32fs_vnode_t *fv = NULL;
    fat32fs_vnode_t *target_fv = NULL;
    int error = 0;
    int same_dir = (sdvp->fv_cluster == tdvp_fv->fv_cluster);
    int new_dirsz;
    int is_short;
    int len;

    /* Check if names are identical in same directory */
    if (same_dir && strcmp(snm, tnm) == 0) {
        return 0;
    }

    mrupdate(&sdvp->fv_lock);
    if (!same_dir) {
        mrupdate(&tdvp_fv->fv_lock);
    }

    /* Lookup source */
    fv = fat32_vnode_new(sdvp->fv_fsi);
    error = fat32_find_dirent(sdvp, snm, fv);
    if (error) {
        goto out;
    }

    /* Lookup target */
    target_fv = fat32_vnode_new(sdvp->fv_fsi);
    error = fat32_find_dirent(tdvp_fv, tnm, target_fv);
    if (error == 0) {
        /* Target exists - remove it */
        if (target_fv->fv_type == VDIR) {
             if (target_fv->fv_type != fv->fv_type) { error = EISDIR; goto out; }
             if (!fat32_dirempty(target_fv)) { error = EEXIST; goto out; }
        } else {
             if (fv->fv_type == VDIR) { error = ENOTDIR; goto out; }
        }
        
        error = fat32_remove_common(tdvp_fv, tnm, (target_fv->fv_type == VDIR), NULL, cr);
        if (error) goto out;
    } else if (error != ENOENT) {
        goto out;
    }
    error = 0;

    /* Calculate new size needed */
    len = (int)strlen(tnm);
    is_short = fat32_name_is_short(tnm, len);
    new_dirsz = is_short ? 1 : (len + 12) / 13 + 1;

    if (same_dir && new_dirsz <= fv->fv_dirsz) {
        /* Reuse existing slots */
        uint32_t old_dirsz = fv->fv_dirsz;
        uint32_t old_diroff = fv->fv_diroff;
        int flags = FAT32_CRE_RENAME | FAT32_CRE_REUSE;
        if (fv->fv_type == VDIR) flags |= FAT32_CRE_DIR;
        
        error = fat32_create_fdentry(sdvp, fv, tnm, flags);
        if (error) goto out;
        
        /* Delete extra entries if we shrank */
        if (new_dirsz < old_dirsz) {
             /* The new entries occupy [old_diroff, old_diroff + new_dirsz*32) */
             /* We need to delete [old_diroff + new_dirsz*32, old_diroff + old_dirsz*32) */
             uint32_t delete_start = old_diroff + new_dirsz * FAT32_DENTRY_SIZE;
             uint32_t delete_count = old_dirsz - new_dirsz;
             fat32_delete_fdentry_chain(sdvp->fv_fsi, sdvp->fv_cluster, delete_start, delete_count);
        }
    } else {
        /* Move or Grow - Allocate new slots */
        int flags = FAT32_CRE_RENAME;
        if (fv->fv_type == VDIR) flags |= FAT32_CRE_DIR;

        /* Delete old entries */
        error = fat32_delete_fdentry_chain(sdvp->fv_fsi, sdvp->fv_cluster, fv->fv_diroff, fv->fv_dirsz);
        if (error) goto out;
        
        /* Create new entries */
        error = fat32_create_fdentry(tdvp_fv, fv, tnm, flags);
        if (error) goto out;
    }

    /* If directory moved, update ".." */
    if (fv->fv_type == VDIR && !same_dir) {
        fat32_dirent83_t dotdot;
        uint32_t parent_clus = tdvp_fv->fv_cluster;

        /* Validate the directory has a valid cluster */
        if (fv->fv_cluster < FAT32_CLUSTER_MIN || fv->fv_cluster == FAT32_CLUSTER_BAD) {
            cmn_err(CE_WARN, "fat32_rename: directory has invalid cluster %u", fv->fv_cluster);
            error = 0; /* Non-fatal, continue with rename */
        } else {
            if (parent_clus == sdvp->fv_fsi->fsi_root_cluster) parent_clus = 0;

            /* ".." is at offset 32 of the directory cluster */
            error = fat32_read_fdentry(sdvp->fv_fsi, fv->fv_cluster, 32, &dotdot);
            if (!error) {
                 WLE16((uint16_t)(parent_clus >> 16), dotdot.first_cluster_hi);
                 WLE16((uint16_t)(parent_clus & 0xFFFF), dotdot.first_cluster_lo);
                 fat32_write_fdentry(sdvp->fv_fsi, fv->fv_cluster, 32, &dotdot);
            }
            error = 0; /* Ignore error reading/writing .. for rename success? */
        }
    }
    
    /* Update timestamps */
    fat32_touch(sdvp, FV_MTIME_DIRTY);
    if (!same_dir) fat32_touch(tdvp_fv, FV_MTIME_DIRTY);

#ifdef FAT32_DBG_DIRENT
    /* Check directory integrity after rename */
    fat32_check_dir_integrity(sdvp);
    if (!same_dir) fat32_check_dir_integrity(tdvp_fv);
#endif

out:
    if (target_fv) fat32_vnode_delete(target_fv);
    if (fv) fat32_vnode_delete(fv);
    
    if (!same_dir) mrunlock(&tdvp_fv->fv_lock);
    mrunlock(&sdvp->fv_lock);
    
    return error;
}

static int
fat32_mkdir(bhv_desc_t *bdp, char *dirname, struct vattr *vap, vnode_t **vpp, cred_t *cr)
{
    fat32fs_vnode_t *dir_fv = BHV_TO_FV(bdp);
    fat32fs_vnode_t *fv;
    int error;

#ifdef FAT32_DBG_DIRENT
    cmn_err(CE_NOTE, "fat32_mkdir: enter dirname='%s'", dirname ? dirname : "(null)");
#endif
    if (!dirname || !vpp)
        return EINVAL;

    /* Verify this is a directory */
    if (dir_fv->fv_type != VDIR)
        return ENOTDIR;

    /* Check access permissions on parent directory */
    error = fat32_faccess(dir_fv, VWRITE | VEXEC, cr);
    if (error) {
        return error;
    }

    /* Allocate vnode structure */
    fv = fat32_vnode_new(dir_fv->fv_fsi);
    if (!fv)
        return ENOMEM;

    /* Lock directory for update */
    mrupdate(&dir_fv->fv_lock);

    /* Check if directory already exists */
    error = fat32_find_dirent(dir_fv, dirname, fv);

    if (error == 0) {
        /* Entry exists - check if it's a file */
        if (fv->fv_type != VDIR) {
            /* Can't create directory - file with same name exists */
#ifdef FAT32_DBG_DIRENT
            cmn_err(CE_NOTE, "fat32_mkdir: file with same name exists");
#endif
            mrunlock(&dir_fv->fv_lock);
            fat32_vnode_delete(fv);
            return ENOTDIR;
        }

        /* Directory already exists */
#ifdef FAT32_DBG_DIRENT
        cmn_err(CE_NOTE, "fat32_mkdir: directory already exists");
#endif
        mrunlock(&dir_fv->fv_lock);
        fat32_vnode_delete(fv);
        return EEXIST;
    }

    if (error != ENOENT) {
        /* Unexpected error during search */
        mrunlock(&dir_fv->fv_lock);
        fat32_vnode_delete(fv);
        return error;
    }

    /* Directory doesn't exist - create it */
    fv->fv_mode = vap->va_mode;
    error = fat32_create_fdentry(dir_fv, fv, dirname, FAT32_CRE_DIR);

    if (!error) {
        /* Set creation times for new directory */
        fat32_touch(fv, 0);  /* 0 = set all times (ctime, mtime, atime) */

        /* Update parent directory mtime */
        fat32_touch(dir_fv, FV_MTIME_DIRTY);

#ifdef FAT32_DBG_DIRENT
        /* Check directory integrity after creation */
        fat32_check_dir_integrity(dir_fv);
#endif
    }

    mrunlock(&dir_fv->fv_lock);

    if (error) {
        fat32_vnode_delete(fv);
        return error;
    }

#ifdef FAT32_DBG_DIRENT
    cmn_err(CE_NOTE, "fat32_mkdir: created directory '%s' location %u:%u:%u (parent:offset:count)",
            dirname, fv->fv_parent_cluster, fv->fv_diroff, fv->fv_dirsz);
#endif
    *vpp = fv->fv_vnode;
#ifdef FAT32_DBG_DIRENT
    cmn_err(CE_NOTE, "fat32_mkdir: exit vp=%p", *vpp);
#endif
    return 0;
}

/*
 * fat32_dirempty - Check if a directory is empty
 *
 * Returns 0 if directory is empty (contains only . and .. or nothing),
 * error code (particularly ENOTEMPTY) otherwise.
 */
int
fat32_dirempty(fat32fs_vnode_t *fv)
{
    fat32fs_info_t *fsi = fv->fv_fsi;
    uint32_t current_cluster = fv->fv_cluster;
    buf_t *bp;
    uint8_t *data;
    uint32_t offset;
    int error;

    /* Verify this is a directory */
    if (fv->fv_type != VDIR) {
        return ENOTDIR;
    }

    /* Handle empty/unallocated directory */
    if (current_cluster == 0) {
        return 0;
    }

    while (1) {
        bp = fat32_read_cluster(fsi, current_cluster);
        if (bp->b_flags & B_ERROR) {
            brelse(bp);
            return ENOTEMPTY; /* Error reading, assume not empty to be safe */
        }

        data = (uint8_t *)bp->b_un.b_addr;

        for (offset = 0; offset < fsi->fsi_bytes_per_cluster; offset += 32) {
            fat32_dirent83_t *dentry = (fat32_dirent83_t *)(data + offset);

            /* Check for end of directory */
            if (dentry->name[0] == 0x00) {
                brelse(bp);
                return 0;
            }

            /* Check for deleted entry */
            if (dentry->name[0] == FAT32_LFN_DELETED) {
                continue;
            }

            /* Check for volume ID - skip it */
            if (dentry->attr & FAT32_ATTR_VOLUME_ID) {
                continue;
            }

            /* Check for LFN entry - if we see one, it's a file/dir */
            if (dentry->attr == FAT32_ATTR_LONG_NAME) {
                brelse(bp);
                return ENOTEMPTY;
            }

            /* Check for . and .. */
            if (dentry->name[0] == '.') {
                /* . and .. are allowed */
                continue;
            }

            /* Found a valid entry that is not . or .. */
            brelse(bp);
            return ENOTEMPTY;
        }

        brelse(bp);

        /* Move to next cluster */
        error = fat32_next_cluster(fsi, current_cluster, &current_cluster, 0);
        if (error) {
            /* End of chain or error */
            break;
        }
    }
    return 0;
}

static int
fat32_rmdir(bhv_desc_t *bdp, char *nm, vnode_t *cdir, cred_t *cr)
{
    fat32fs_vnode_t *dir_fv = BHV_TO_FV(bdp);
    return fat32_remove_common(dir_fv, nm, 1 /* dir */, cdir, cr);
}

static int
fat32_readdir(bhv_desc_t *bdp, struct uio *uiop, cred_t *cr, int *eofp)
{
    fat32fs_vnode_t *dir_fv;
    fat32fs_vnode_t *parent_fv;
    struct dirent dirent;
    char d_name[FAT32_MAX_NAME + 1];
    uint32_t d_name_size;
    uint32_t offset;
    uint32_t disk_offset;
    int error;
    size_t reclen;

#ifdef FAT32_DBG_DIRENT
    cmn_err(CE_NOTE, "fat32_readdir: enter offset=%lld resid=%d",
            (long long)uiop->uio_offset, uiop->uio_resid);
#endif
    dir_fv = BHV_TO_FV(bdp);

    /* Verify this is a directory */
    if (dir_fv->fv_type != VDIR) {
        cmn_err(CE_WARN, "fat32_readdir: not a directory");
        return ENOTDIR;
    }

    /* Initialize EOF flag */
    if (eofp) {
        *eofp = 0;
    }

    mraccess(&dir_fv->fv_lock);

    /* Read directory entries until buffer is full or end of directory */
    offset = (uint32_t)uiop->uio_offset;

    while (uiop->uio_resid > 0) {
        /* Synthesize "." entry at offset 0 for root directory only */
        if (offset == 0 && (dir_fv->fv_flags & FV_ROOT)) {
            d_name[0] = '.';
            d_name[1] = '\0';
            d_name_size = 1;

            dirent.d_ino = (ino_t)0;
            dirent.d_off = 1;  /* Next offset will be ".." */

#ifdef FAT32_DBG_DIRENT
            cmn_err(CE_NOTE, "fat32_readdir: returning synthetic '.' ino=%u",
                    (unsigned)dirent.d_ino);
#endif
            error = fat32_emit_dirent(&dirent, d_name, d_name_size, uiop);
            if (error == EINVAL) {
#ifdef FAT32_DBG_DIRENT
                cmn_err(CE_NOTE, "fat32_readdir: buffer full at '.' entry");
#endif
                break;
            }
            if (error) {
                cmn_err(CE_WARN, "fat32_readdir: emit '.' failed: %d", error);
                break;
            }

            offset = 1;
            continue;
        }

        /* Synthesize ".." entry at offset 1 for root directory only */
        if (offset == 1 && (dir_fv->fv_flags & FV_ROOT)) {
            d_name[0] = '.';
            d_name[1] = '.';
            d_name[2] = '\0';
            d_name_size = 2;

            /* For root, ".." points to itself */
            dirent.d_ino = (ino_t)0;
            dirent.d_off = 2;  /* Next offset (will be rounded to 0 for disk read) */

#ifdef FAT32_DBG_DIRENT
            cmn_err(CE_NOTE, "fat32_readdir: returning synthetic '..' ino=%u",
                    (unsigned)dirent.d_ino);
#endif
            error = fat32_emit_dirent(&dirent, d_name, d_name_size, uiop);
            if (error == EINVAL) {
#ifdef FAT32_DBG_DIRENT
                cmn_err(CE_NOTE, "fat32_readdir: buffer full at '..' entry");
#endif
                break;
            }
            if (error) {
                cmn_err(CE_WARN, "fat32_readdir: emit '..' failed: %d", error);
                break;
            }

            offset = 2;
            continue;
        }

        /*
         * For real entries from disk:
         * - Root directory: if offset is less than FAT32_DENTRY_SIZE, treat as 0
         *   (since we synthesized . and .. at offsets 0 and 1)
         * - Subdirectories: use offset directly (. and .. are real disk entries)
         */
        if (dir_fv->fv_flags & FV_ROOT) {
            disk_offset = (offset < FAT32_DENTRY_SIZE) ? 0 : offset;
        } else {
            disk_offset = offset;
        }

        d_name_size = sizeof(d_name);
        d_name[0] = '\0';

        /* Get next directory entry from disk - don't need vnode metadata for readdir */
        error = fat32_get_dirent(dir_fv, &disk_offset, &dirent, d_name, &d_name_size, NULL);
        if (error == ENOENT) {
            /* End of directory */
#ifdef FAT32_DBG_DIRENT
            cmn_err(CE_NOTE, "fat32_readdir: reached end of directory");
#endif
            if (eofp) {
                *eofp = 1;
            }
            error = 0;
            break;
        } else if (error) {
            cmn_err(CE_WARN, "fat32_readdir: fat32_get_dirent failed: %d", error);
            break;
        }

#ifdef FAT32_DBG_DIRENT
        cmn_err(CE_NOTE, "fat32_readdir: returning entry '%s' ino=%u off=%u",
                d_name, (unsigned)dirent.d_ino, (unsigned)dirent.d_off);
#endif
        /* Emit entry to user space */
        error = fat32_emit_dirent(&dirent, d_name, d_name_size, uiop);
        if (error == EINVAL) {
#ifdef FAT32_DBG_DIRENT
            cmn_err(CE_NOTE, "fat32_readdir: buffer full, stopping");
#endif
            /* Don't advance offset - this entry will be read on next call */
            break;
        }
        if (error) {
            cmn_err(CE_WARN, "fat32_readdir: emit failed: %d", error);
            break;
        }

        /* Update offset to the value returned by getdirent */
        offset = disk_offset;
    }
    mraccunlock(&dir_fv->fv_lock);

    /* Update offset for next call */
    uiop->uio_offset = offset;

#ifdef FAT32_DBG_DIRENT
    cmn_err(CE_NOTE, "fat32_readdir: exit error=%d offset=%lld eof=%d",
            error, (long long)uiop->uio_offset, eofp ? *eofp : -1);
#endif
    return error;
}

static int
fat32_symlink(bhv_desc_t *bdp, char *linkname, struct vattr *vap, char *target, cred_t *cr)
{
#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_symlink: enter linkname='%s' target='%s'",
            linkname ? linkname : "(null)", target ? target : "(null)");
    cmn_err(CE_NOTE, "fat32_symlink: exit ENOSYS");
#endif
    return ENOSYS;
}

static int
fat32_readlink(bhv_desc_t *bdp, struct uio *uiop, cred_t *cr)
{
#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_readlink: enter");
    cmn_err(CE_NOTE, "fat32_readlink: exit ENOSYS");
#endif
    return ENOSYS;
}

static int
fat32_fsync(bhv_desc_t *bdp, int flag, cred_t *cr, off_t start, off_t stop)
{
    fat32fs_vnode_t *fv = BHV_TO_FV(bdp);

#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_fsync: enter flag=%d start=%lld stop=%lld",
            flag, (long long)start, (long long)stop);
#endif
    if (fv->fv_flags & FV_DIRTY_MASK) {
        fat32_sync_metadata(fv);
    }
#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_fsync: exit success");
#endif
    return 0;
}

static int
fat32_inactive(bhv_desc_t *bdp, cred_t *cr)
{
    fat32fs_vnode_t *fv;
    fat32fs_vnode_t *parent_fv = NULL;
    vnode_t *vp;

    fv = BHV_TO_FV(bdp);
    vp = FV_TO_VNODE(fv);

#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_inactive: enter vp=%p cluster=%u v_count=%d",
            vp, fv->fv_cluster, vp->v_count);
#endif
    /* Check if vnode is still referenced */
    if (vp->v_count > 0) {
        cmn_err(CE_NOTE, "fat32_inactive: vnode still has references, keeping cached");
        return VN_INACTIVE_CACHE;
    }

    /* Vnode is no longer referenced, tear it down */
#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_inactive: tearing down vnode");
#endif
    /* Flush any dirty metadata */
    if (fv->fv_flags & FV_DIRTY_MASK) {
        fat32_sync_metadata(fv);
    }

    /* Remove behavior descriptor */
    vn_bhv_remove(VN_BHV_HEAD(vp), bdp);

    /* Free the FAT32 vnode structure (releases parent_vp, frees mrlock) */
    fat32_vnode_delete(fv);

    /*
     * Note: Do NOT call vn_free(vp) here!
     * Per IRIX vnode.h documentation: "Do not call vn_free from within
     * VOP_INACTIVE; just remove the behaviors and vn_rele will do the
     * right thing."
     * The VFS layer will handle vnode deallocation when appropriate.
     */

#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_inactive: exit, vnode torn down");
#endif
    return VN_INACTIVE_NOCACHE;
}

static int
fat32_fid(bhv_desc_t *bdp, struct fid **fidpp)
{
#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_fid: enter");
    cmn_err(CE_NOTE, "fat32_fid: exit ENOSYS");
#endif
    return ENOSYS;
}

static int
fat32_fid2(bhv_desc_t *bdp, struct fid *fidp)
{
#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_fid2: enter");
    cmn_err(CE_NOTE, "fat32_fid2: exit ENOSYS");
#endif
    return ENOSYS;
}

static void
fat32_rwlock(bhv_desc_t *bdp, vrwlock_t locktype)
{
#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_rwlock: enter locktype=%d", locktype);
    cmn_err(CE_NOTE, "fat32_rwlock: exit");
#endif
}

static void
fat32_rwunlock(bhv_desc_t *bdp, vrwlock_t locktype)
{
#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_rwunlock: enter locktype=%d", locktype);
    cmn_err(CE_NOTE, "fat32_rwunlock: exit");
#endif
}

static int
fat32_seek(bhv_desc_t *bdp, off_t ooff, off_t *noffp)
{
    if (*noffp < 0)
        return EINVAL;
    return 0;
}

static int
fat32_cmp(bhv_desc_t *bdp, vnode_t *vp2)
{
    fat32fs_vnode_t *fv1;
    fat32fs_vnode_t *fv2;

    fv1 = BHV_TO_FV(bdp);
    fv2 = VNODE_TO_FV(vp2);

    /* Check for root directory special case */
    if (fv1->fv_flags & FV_ROOT) {
        return (fv2->fv_flags & FV_ROOT) ? 1 : 0;
    }
    if (fv2->fv_flags & FV_ROOT) {
        return 0;
    }

    /* Compare packed inode numbers derived from directory entry location */
    return (fat32_pack_ino(fv1->fv_parent_cluster, fv1->fv_diroff, fv1->fv_dirsz) ==
            fat32_pack_ino(fv2->fv_parent_cluster, fv2->fv_diroff, fv2->fv_dirsz));
}

static int
fat32_frlock(bhv_desc_t *bdp, int cmd, struct flock *bfp, int flag, off_t offset, vrwlock_t vrwlock, cred_t *cr)
{
    return ENOSYS;
}

static int
fat32_realvp(bhv_desc_t *bdp, vnode_t **vpp)
{
    return ENOSYS;
}

static int
fat32_bmap(bhv_desc_t *bdp, off_t offset, ssize_t count, int rw, cred_t *cr, struct bmapval *bmv, int *nbmaps)
{
    return ENOSYS;
}

static void
fat32_strategy(bhv_desc_t *bdp, struct buf *bp)
{
}

static int
fat32_map(bhv_desc_t *bdp, off_t off, size_t len, mprot_t prot, u_int flags, cred_t *cr, vnode_t **nvp)
{
    return ENOSYS;
}

static int
fat32_addmap(bhv_desc_t *bdp, vaddmap_t op, struct __vhandl_s *vt, pgno_t *pgno, off_t off, size_t len, mprot_t prot, cred_t *cr)
{
    return ENOSYS;
}

static int
fat32_delmap(bhv_desc_t *bdp, struct __vhandl_s *vt, size_t len, cred_t *cr)
{
    return ENOSYS;
}

static int
fat32_poll(bhv_desc_t *bdp, short events, int anyyet, short *reventsp, struct pollhead **phpp, unsigned int *genp)
{
    return ENOSYS;
}

static int
fat32_dump(bhv_desc_t *bdp, caddr_t addr, daddr_t bn, u_int count)
{
    return ENOSYS;
}

static int
fat32_pathconf(bhv_desc_t *bdp, int cmd, long *valp, cred_t *cr)
{
    return ENOSYS;
}

static int
fat32_allocstore(bhv_desc_t *bdp, off_t off, size_t len, cred_t *cr)
{
    return ENOSYS;
}

static int
fat32_fcntl(bhv_desc_t *bdp, int cmd, void *arg, int flags, off_t offset, cred_t *cr, union rval *rvp)
{
    return ENOSYS;
}

static int
fat32_reclaim(bhv_desc_t *bdp, int flag)
{
#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_reclaim: enter flag=%d", flag);
    cmn_err(CE_NOTE, "fat32_reclaim: exit ENOSYS");
#endif
    return ENOSYS;
}

static int
fat32_attr_get(bhv_desc_t *bdp, char *name, char *value, int *valuelenp, int flags, cred_t *cr)
{
    return ENOSYS;
}

static int
fat32_attr_set(bhv_desc_t *bdp, char *name, char *value, int valuelen, int flags, cred_t *cr)
{
    return ENOSYS;
}

static int
fat32_attr_remove(bhv_desc_t *bdp, char *name, int flags, cred_t *cr)
{
    return ENOSYS;
}

static int
fat32_attr_list(bhv_desc_t *bdp, char *buffer, int bufsize, int flags, struct attrlist_cursor_kern *cursor, cred_t *cr)
{
    return ENOSYS;
}

static int
fat32_cover(bhv_desc_t *bdp, struct mounta *uap, char *attrs, cred_t *cr)
{
    return ENOSYS;
}

static void
fat32_link_removed(bhv_desc_t *bdp, vnode_t *dvp, int linkzero)
{
}

static void
fat32_vnode_change(bhv_desc_t *bdp, vchange_t cmd, __psint_t val)
{
}

static void
fat32_tosspages(bhv_desc_t *bdp, off_t first, off_t last, int fiopt)
{
}

static void
fat32_flushinval_pages(bhv_desc_t *bdp, off_t first, off_t last, int fiopt)
{
}

static int
fat32_flush_pages(bhv_desc_t *bdp, off_t first, off_t last, uint64_t flags, int fiopt)
{
    return ENOSYS;
}

static void
fat32_invalfree_pages(bhv_desc_t *bdp, off_t off)
{
}

static void
fat32_pages_sethole(bhv_desc_t *bdp, struct pfdat *pfd, int cnt, int doremap, off_t remapoffset)
{
}

static int
fat32_commit(bhv_desc_t *bdp, struct buf *bp)
{
    return ENOSYS;
}

static int
fat32_readbuf(bhv_desc_t *bdp, off_t offset, ssize_t count, int flags, cred_t *cr, struct flid *fl, struct buf **bpp, int *pboff, int *pblen)
{
    return ENOSYS;
}

static int
fat32_strgetmsg(bhv_desc_t *bdp, struct strbuf *mctl, struct strbuf *mdata, unsigned char *prip, int *flagsp, int fmode, union rval *rvp)
{
    return ENOSYS;
}

static int
fat32_strputmsg(bhv_desc_t *bdp, struct strbuf *mctl, struct strbuf *mdata, unsigned char pri, int flag, int fmode)
{
    return ENOSYS;
}

vnodeops_t fat32vnodeops = {
    BHV_IDENTITY_INIT_POSITION(VNODE_POSITION_BASE),
    fat32_open,
    fat32_close,
    fat32_read,
    fat32_write,
    fat32_ioctl,
    fs_setfl,
    fat32_getattr,
    fat32_setattr,
    fat32_access,
    fat32_lookup,
    fat32_create,
    fat32_remove,
    fat32_link,
    fat32_rename,
    fat32_mkdir,
    fat32_rmdir,
    fat32_readdir,
    fat32_symlink,
    fat32_readlink,
    fat32_fsync,
    fat32_inactive,
    fat32_fid,
    fat32_fid2,
    fat32_rwlock,
    fat32_rwunlock,
    fat32_seek,
    fat32_cmp,
    fat32_frlock,
    fat32_realvp,
    fat32_bmap,
    fat32_strategy,
    fat32_map,
    fat32_addmap,
    fat32_delmap,
    fat32_poll,
    fat32_dump,
    fat32_pathconf,
    fat32_allocstore,
    fat32_fcntl,
    fat32_reclaim,
    fat32_attr_get,
    fat32_attr_set,
    fat32_attr_remove,
    fat32_attr_list,
    fat32_cover,
    fat32_link_removed,
    fat32_vnode_change,
    fat32_tosspages,
    fat32_flushinval_pages,
    fat32_flush_pages,
    fat32_invalfree_pages,
    fat32_pages_sethole,
    fat32_commit,
    fat32_readbuf,
    fat32_strgetmsg,
    fat32_strputmsg
};
