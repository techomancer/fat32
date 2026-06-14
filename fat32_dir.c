#include "fat32fs.h"

/*
 * Helper function to emit a directory entry to user space
 * Handles both 32-bit (IRIX5) and 64-bit ABIs
 *
 * Returns:
 *   0 on success
 *   EINVAL if buffer too small for this entry (caller should stop and try next time)
 *   other error codes from uiomove
 */
int
fat32_emit_dirent(struct dirent *dirent, char *d_name, uint32_t d_name_size,
                  struct uio *uiop)
{
    size_t reclen;
    int error;
    int target_abi;
    int is_irix5;

    /* Determine target ABI */
    target_abi = GETDENTS_ABI(get_current_abi(), uiop);
    is_irix5 = ABI_IS_IRIX5(target_abi);

#ifdef FAT32_DBG_DIRENT
    cmn_err(CE_NOTE, "fat32_emit_dirent: name='%s' name_size=%u ino=%u off=%u",
            d_name, d_name_size, (unsigned)dirent->d_ino, (unsigned)dirent->d_off);
    cmn_err(CE_NOTE, "fat32_emit_dirent: abi=%d is_irix5=%d",
            target_abi, is_irix5);
#endif
    if (is_irix5) {
        /* 32-bit IRIX5 ABI - use irix5_dirent structure */
        struct irix5_dirent irix5_de;

        reclen = IRIX5_DIRENTSIZE(d_name_size);
#ifdef FAT32_DBG_DIRENT
        cmn_err(CE_NOTE, "fat32_emit_dirent: using IRIX5 format, reclen=%u resid=%u",
                (unsigned)reclen, (unsigned)uiop->uio_resid);
#endif
        /* Check buffer space */
        if (reclen > uiop->uio_resid) {
#ifdef FAT32_DBG_DIRENT
            cmn_err(CE_NOTE, "fat32_emit_dirent: buffer too small for IRIX5");
#endif            
            return EINVAL;
        }

        /* Fill in irix5_dirent structure */
        irix5_de.d_ino = (app32_ulong_t)dirent->d_ino;
        irix5_de.d_off = (irix5_off_t)dirent->d_off;
        irix5_de.d_reclen = (unsigned short)reclen;

        /* Copy structure (without d_name) */
#ifdef FAT32_DBG_DIRENT
        cmn_err(CE_NOTE, "fat32_emit_dirent: copying irix5_dirent base (%u bytes)",
                (unsigned)IRIX5_DIRENTBASESIZE);
#endif
        error = uiomove(&irix5_de, IRIX5_DIRENTBASESIZE, UIO_READ, uiop);
        if (error) {
            cmn_err(CE_WARN, "fat32_emit_dirent: uiomove irix5_dirent failed: %d", error);
            return error;
        }
    } else {
        /* 64-bit ABI - use standard dirent structure */
        reclen = DIRENTSIZE(d_name_size);
#ifdef FAT32_DBG_DIRENT
        cmn_err(CE_NOTE, "fat32_emit_dirent: using 64-bit format, reclen=%u resid=%u",
                (unsigned)reclen, (unsigned)uiop->uio_resid);
#endif
        /* Check buffer space */
        if (reclen > uiop->uio_resid) {
            cmn_err(CE_NOTE, "fat32_emit_dirent: buffer too small");
            return EINVAL;
        }

        dirent->d_reclen = (unsigned short)reclen;

        /* Copy structure (without d_name) */
#ifdef FAT32_DBG_DIRENT
        cmn_err(CE_NOTE, "fat32_emit_dirent: copying dirent base (%u bytes)",
                (unsigned)DIRENTBASESIZE);
#endif
        error = uiomove(dirent, DIRENTBASESIZE, UIO_READ, uiop);
        if (error) {
            cmn_err(CE_WARN, "fat32_emit_dirent: uiomove dirent failed: %d", error);
            return error;
        }
    }

    /* Copy name with null terminator */
#ifdef FAT32_DBG_DIRENT
    cmn_err(CE_NOTE, "fat32_emit_dirent: copying name (%u bytes)", d_name_size + 1);
#endif
    error = uiomove(d_name, d_name_size + 1, UIO_READ, uiop);
    if (error) {
        cmn_err(CE_WARN, "fat32_emit_dirent: uiomove name failed: %d", error);
        return error;
    }

    /* Add padding to reach reclen */
    {
        size_t base_size = is_irix5 ? IRIX5_DIRENTBASESIZE : DIRENTBASESIZE;
        size_t copied = base_size + d_name_size + 1;
        size_t padding = reclen - copied;

#ifdef FAT32_DBG_DIRENT
        cmn_err(CE_NOTE, "fat32_emit_dirent: base=%u copied=%u padding=%u",
                (unsigned)base_size, (unsigned)copied, (unsigned)padding);
#endif
        if (padding > 0) {
            char pad[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            error = uiomove(pad, padding, UIO_READ, uiop);
            if (error) {
                cmn_err(CE_WARN, "fat32_emit_dirent: uiomove padding failed: %d", error);
                return error;
            }
        }
    }

#ifdef FAT32_DBG_DIRENT
    cmn_err(CE_NOTE, "fat32_emit_dirent: success, remaining resid=%u", (unsigned)uiop->uio_resid);
#endif
    return 0;
}

/*
 * fat32_get_dirent - Read a directory entry at given offset
 *
 * Reads directory clusters, parses FAT32 directory entries including LFN,
 * and fills in the dirent structure.
 *
 * Parameters:
 *   fv          - FAT32 vnode for directory
 *   offset      - In: byte offset into directory, Out: offset to next entry
 *   dirent      - Output dirent structure
 *   d_name      - Output buffer for filename
 *   d_name_size - Size of d_name buffer
 *   entry_fv    - If not NULL, populate this vnode with entry metadata
 *
 * Returns:
 *   0 on success, ENOENT at end of directory, error code on failure
 */
int
fat32_get_dirent(fat32fs_vnode_t *fv, uint32_t *offset, struct dirent *dirent,
                 char *d_name, uint32_t *d_name_size, fat32fs_vnode_t *entry_fv)
{
    fat32fs_info_t *fsi;
    buf_t *bp = NULL;
    uint32_t cluster_offset;
    uint32_t current_offset;
    uint32_t skip_clusters;
    uint32_t current_cluster;
    uint32_t name_size = *d_name_size;
    uint8_t *data;
    uint32_t first_cluster;
    int error = 0;
    uint32_t i;
    int has_lfn = 0;
    uint32_t entry_start_offset = 0;

    fsi = fv->fv_fsi;
    current_offset = *offset;

#ifdef FAT32_DBG_DIRENT
    cmn_err(CE_NOTE, "fat32_get_dirent: enter offset=%u cluster=%u",
            current_offset, fv->fv_cluster);
#endif
    /* Validate this is a directory */
    if (fv->fv_type != VDIR) {
        cmn_err(CE_WARN, "fat32_get_dirent: not a directory");
        return ENOTDIR;
    }

    /* Calculate which cluster and offset within cluster */
    skip_clusters = current_offset >> fsi->fsi_cluster_shift;
    cluster_offset = current_offset & fsi->fsi_cluster_mask;

    /* Seek to the starting cluster */
    error = fat32_vnode_seek_cluster(fv, skip_clusters, &current_cluster, 0);
    if (error) {
#ifdef FAT32_DBG_DIRENT
        cmn_err(CE_NOTE, "fat32_get_dirent: reached end of directory during seek");
#endif
        return ENOENT;
    }

#ifdef FAT32_DBG_DIRENT
    cmn_err(CE_NOTE, "fat32_get_dirent: seeked to cluster %u", current_cluster);
#endif

    /* Scan directory entries starting at offset */
    while (1) {
#ifdef FAT32_DBG_DIRENT
        cmn_err(CE_NOTE, "fat32_get_dirent: reading cluster %u", current_cluster);
#endif
        bp = fat32_read_cluster(fsi, current_cluster);
        if (bp->b_flags & B_ERROR) {
            cmn_err(CE_WARN, "fat32_get_dirent: failed to read cluster %u", current_cluster);
            error = bp->b_error ? bp->b_error : EIO;
            brelse(bp);
            return error;
        }

        data = (uint8_t *)bp->b_un.b_addr;

        /* Scan entries in this cluster */
        while (cluster_offset < fsi->fsi_bytes_per_cluster) {
            fat32_dirent83_t *dentry = (fat32_dirent83_t *)(data + cluster_offset);

            /* Check for end of directory */
            if (dentry->name[0] == 0x00) {
#ifdef FAT32_DBG_DIRENT
                cmn_err(CE_NOTE, "fat32_get_dirent: end of directory marker");
#endif
                brelse(bp);
                return ENOENT;
            }

            /* Skip deleted entries */
            if (dentry->name[0] == 0xE5) {
#ifdef FAT32_DBG_DIRENT
                cmn_err(CE_NOTE, "fat32_get_dirent: skipping deleted entry at offset %u",
                        current_offset);
#endif
                current_offset += FAT32_DENTRY_SIZE;
                cluster_offset += FAT32_DENTRY_SIZE;
                continue;
            }

            /* Check if this is an LFN entry */
            if (dentry->attr == FAT32_ATTR_LONG_NAME) {
                fat32_dirent_lfn_t *lfn = (fat32_dirent_lfn_t *)dentry;
                uint32_t pos;
#ifdef FAT32_DBG_DIRENT
                cmn_err(CE_NOTE, "fat32_get_dirent: found LFN entry seq=%u",
                        lfn->sequence & FAT32_LFN_SEQ_MASK);
#endif
                /* Check if this is the first LFN entry (highest sequence number) */
                if (lfn->sequence & FAT32_LFN_LAST) {
                    /* This is the start of a new LFN sequence */
                    uint32_t full_len;

                    entry_start_offset = current_offset;

                    /*
                     * Pre-terminate at the full name length.  When a name's
                     * length is an exact multiple of 13, the LFN entries hold
                     * no 0x0000 terminator (the last slot is completely full),
                     * so fat32_set_dname() never NUL-terminates and stale bytes
                     * from a previously read (longer) entry would otherwise leak
                     * in as a suffix (e.g. "Basement Jaxx" -> "Basement Jaxxsand").
                     * For names that are not a multiple of 13, the embedded
                     * 0x0000 moves the terminator earlier as characters are
                     * copied below (fat32_set_dname() shrinks name_size on NUL).
                     */
                    full_len = (uint32_t)((lfn->sequence & FAT32_LFN_SEQ_MASK)) * 13;
                    if (full_len < *d_name_size) {
                        d_name[full_len] = '\0';
                        name_size = full_len;
                    }
                }

                /* Mark that we saw an LFN */
                has_lfn = 1;

                pos = (uint32_t)((lfn->sequence & FAT32_LFN_SEQ_MASK) - 1) * 13;
                fat32_set_dname(pos +  0, RLE16(lfn->name1 +  0), d_name, &name_size);
                fat32_set_dname(pos +  1, RLE16(lfn->name1 +  2), d_name, &name_size);
                fat32_set_dname(pos +  2, RLE16(lfn->name1 +  4), d_name, &name_size);
                fat32_set_dname(pos +  3, RLE16(lfn->name1 +  6), d_name, &name_size);
                fat32_set_dname(pos +  4, RLE16(lfn->name1 +  8), d_name, &name_size);
                fat32_set_dname(pos +  5, RLE16(lfn->name2 +  0), d_name, &name_size);
                fat32_set_dname(pos +  6, RLE16(lfn->name2 +  2), d_name, &name_size);
                fat32_set_dname(pos +  7, RLE16(lfn->name2 +  4), d_name, &name_size);
                fat32_set_dname(pos +  8, RLE16(lfn->name2 +  6), d_name, &name_size);
                fat32_set_dname(pos +  9, RLE16(lfn->name2 +  8), d_name, &name_size);
                fat32_set_dname(pos + 10, RLE16(lfn->name2 + 10), d_name, &name_size);
                fat32_set_dname(pos + 11, RLE16(lfn->name3 +  0), d_name, &name_size);
                fat32_set_dname(pos + 12, RLE16(lfn->name3 +  2), d_name, &name_size);
                
                current_offset += FAT32_DENTRY_SIZE;
                cluster_offset += FAT32_DENTRY_SIZE;
                continue;
            }

            /* Skip volume ID entries */
            if (dentry->attr & FAT32_ATTR_VOLUME_ID) {
#ifdef FAT32_DBG_DIRENT
                cmn_err(CE_NOTE, "fat32_get_dirent: skipping volume ID entry");
#endif
                /* reset our LFN */
                name_size = *d_name_size;
                d_name[0] = '\0';
                has_lfn = 0;
                entry_start_offset = 0;
                current_offset += FAT32_DENTRY_SIZE;
                cluster_offset += FAT32_DENTRY_SIZE;
                continue;
            }

#ifdef FAT32_DBG_DIRENT
            /* Found a valid 8.3 entry */
            cmn_err(CE_NOTE, "fat32_get_dirent: found 8.3 entry at offset %u",
                    current_offset);
#endif
            /* If no LFN, this entry starts here */
            if (!has_lfn) {
                entry_start_offset = current_offset;
            }

            if (d_name[0] == '\0') {
                /* Extract filename from 8.3 format */
                uint32_t name_pos = 0;
                /* Copy name part (8 chars) */
                for (i = 0; i < 8 && dentry->name[i] != ' '; i++) {
                    if (name_pos < *d_name_size - 1) {
                        d_name[name_pos++] = dentry->name[i];
                    }
                }
                /* Add dot if extension exists */
                if (dentry->name[8] != ' ') {
                    if (name_pos < *d_name_size - 1) {
                        d_name[name_pos++] = '.';
                    }
                    /* Copy extension part (3 chars) */
                    for (i = 8; i < 11 && dentry->name[i] != ' '; i++) {
                        if (name_pos <*d_name_size - 1) {
                            d_name[name_pos++] = dentry->name[i];
                        }
                    }
                }
                d_name[name_pos] = '\0';
                name_size = name_pos;
            }

#ifdef FAT32_DBG_DIRENT
            cmn_err(CE_WARN, "fat32_get_dirent: filename='%s' offset=%u", d_name, entry_start_offset);
#endif
            /* Extract cluster number (high and low parts) */
            first_cluster = RLE16(dentry->first_cluster_lo) |
                           (RLE16(dentry->first_cluster_hi) << 16);

#ifdef FAT32_DBG_DIRENT
            cmn_err(CE_NOTE, "fat32_get_dirent: first_cluster=%u attr=0x%x size=%u",
                    first_cluster, dentry->attr, RLE32(dentry->file_size));
#endif
            /* Fill in dirent structure */
            dirent->d_ino = fat32_pack_ino(fv->fv_cluster, entry_start_offset,
                                           (current_offset - entry_start_offset) / FAT32_DENTRY_SIZE + 1);
            dirent->d_off = current_offset + FAT32_DENTRY_SIZE;
            dirent->d_reclen = sizeof(struct dirent);

            /* Populate vnode fields if requested */
            if (entry_fv) {
                entry_fv->fv_fsi = fv->fv_fsi;
                entry_fv->fv_cluster = first_cluster;
                entry_fv->fv_attr = dentry->attr;
                entry_fv->fv_diroff = entry_start_offset;
                entry_fv->fv_dirsz = (current_offset - entry_start_offset) / FAT32_DENTRY_SIZE + 1;
                entry_fv->fv_parent_cluster = fv->fv_cluster;
                entry_fv->fv_size = RLE32(dentry->file_size);

                /* Set vnode type based on attributes */
                if (dentry->attr & FAT32_ATTR_DIRECTORY) {
                    entry_fv->fv_type = VDIR;
                    entry_fv->fv_mode = S_IFDIR | 0755;
                } else {
                    entry_fv->fv_type = VREG;
                    if (dentry->attr & FAT32_ATTR_READ_ONLY) {
                        entry_fv->fv_mode = S_IFREG | 0444;
                    } else {
                        entry_fv->fv_mode = S_IFREG | 0644;
                    }
                }

                /* Convert FAT32 timestamps to timespec_t */
                fat32_fat_to_unix_time(RLE16(dentry->last_access_date), 0, 0, &entry_fv->fv_atime);
                fat32_fat_to_unix_time(RLE16(dentry->write_date), RLE16(dentry->write_time), 0, &entry_fv->fv_mtime);
                fat32_fat_to_unix_time(RLE16(dentry->create_date), RLE16(dentry->create_time), dentry->create_time_tenth, &entry_fv->fv_ctime);
            }

            /* Update offset to next entry */
            *offset = current_offset + FAT32_DENTRY_SIZE;
            *d_name_size = name_size;

            brelse(bp);
#ifdef FAT32_DBG_DIRENT
            cmn_err(CE_NOTE, "fat32_get_dirent: exit success, next_offset=%u", *offset);
#endif
            return 0;
        } /* end of while cluster_offset < fsi_bytes_per_cluster */

        brelse(bp);
        bp = NULL;

        /* Move to next cluster */
        error = fat32_next_cluster(fsi, current_cluster, &current_cluster, 0);
        if (error) {
#ifdef FAT32_DBG_DIRENT
            cmn_err(CE_NOTE, "fat32_get_dirent: reached end of directory");
#endif
            return ENOENT;
        }

        cluster_offset = 0;
    }

    /* Should never reach here */
}

/*
 * fat32_find_dirent - Search for a directory entry by name
 *
 * Searches the directory for an entry matching the given name (case-insensitive).
 * If found, populates the entry_fv structure with the entry metadata.
 *
 * Parameters:
 *   dir_fv   - FAT32 vnode for directory to search
 *   name     - Name to search for
 *   entry_fv - If not NULL and entry found, populate with entry metadata
 *
 * Returns:
 *   0 if found, ENOENT if not found, other error codes on failure
 */
int
fat32_find_dirent(fat32fs_vnode_t *dir_fv, const char *name, fat32fs_vnode_t *entry_fv)
{
    struct dirent dirent;
    char d_name[FAT32_MAX_NAME + 1];
    uint32_t d_name_size;
    uint32_t offset;
    int error;
    int nm_len;
    int i;

    /* Verify this is a directory */
    if (dir_fv->fv_type != VDIR) {
        return ENOTDIR;
    }

    /* Get name length */
    nm_len = 0;
    while (name[nm_len] != '\0' && nm_len < MAXNAMELEN-1) {
        nm_len++;
    }

    /* Iterate through directory entries to find matching name */
    offset = 0;
    while (1) {
        d_name_size = sizeof(d_name);
        d_name[0] = '\0';

        /* Reset entry_fv if provided (preserves lock and fsi) */
        if (entry_fv) {
            fat32_vnode_reset(entry_fv);
        }
        bzero(&dirent, sizeof(dirent));

        error = fat32_get_dirent(dir_fv, &offset, &dirent, d_name, &d_name_size, entry_fv);
        if (error == ENOENT) {
            /* End of directory, name not found */
            return ENOENT;
        } else if (error) {
            /* Other error */
            return error;
        }

        /* Case-insensitive comparison */
        if (d_name_size == nm_len) {
            int match = 1;
            for (i = 0; i < nm_len; i++) {
                if (fat32_chrcmp(name[i], d_name[i])) {
                    match = 0;
                    break;
                }
            }

            if (match) {
                /* Found matching entry */
                return 0;
            }
        }
    }
}

/*
 * fat32_update_fdentry - Update directory entry on disk
 *
 * Flushes dirty vnode metadata (currently only atime) back to the
 * directory entry on disk.
 */
int
fat32_update_fdentry(fat32fs_vnode_t *fv)
{
    fat32fs_info_t *fsi = fv->fv_fsi;
    uint32_t parent_cluster;
    uint32_t target_offset;
    uint32_t skip_clusters;
    uint32_t offset_in_cluster;
    uint32_t target_cluster;
    uint32_t sector;
    buf_t *bp;
    fat32_dirent83_t *dentry;
    uint16_t date;
    uint16_t time;
    uint8_t tenths;
    int error;

    /* If not dirty, nothing to do */
    if (!(fv->fv_flags & FV_DIRTY_MASK)) {
        return 0;
    }

    /* Root directory has no parent entry to update */
    if (fv->fv_flags & FV_ROOT) {
        fv->fv_flags &= ~FV_DIRTY_MASK;
        return 0;
    }

    /* Parent cluster is needed to find the entry */
    parent_cluster = fv->fv_parent_cluster;
    if (parent_cluster == FAT32_CLUSTER_BAD) {
        cmn_err(CE_WARN, "fat32_update_fdentry: invalid parent cluster");
        fv->fv_flags &= ~FV_DIRTY_MASK;
        return EINVAL;
    }
    if (parent_cluster == 0) {
        parent_cluster = fsi->fsi_root_cluster;
    }

    /*
     * Calculate offset of the 8.3 entry.
     * fv_diroff points to the start of the entry sequence (potentially LFNs).
     * The 8.3 entry is the last one in the sequence.
     */
    target_offset = fv->fv_diroff + (fv->fv_dirsz - 1) * FAT32_DENTRY_SIZE;

#ifdef FAT32_DBG_DIRENT
    cmn_err(CE_NOTE, "fat32_update_fdentry: location %u:%u:%u (parent:offset:count)",
            parent_cluster, fv->fv_diroff, fv->fv_dirsz);
#endif
    /* Calculate cluster index and offset within cluster */
    skip_clusters = target_offset >> fsi->fsi_cluster_shift;
    offset_in_cluster = target_offset & fsi->fsi_cluster_mask;

    /* Seek to the cluster containing the entry */
    error = fat32_seek_cluster(fsi, parent_cluster, skip_clusters, &target_cluster, 0);
    if (error) {
        cmn_err(CE_WARN, "fat32_update_fdentry: seek failed: %d", error);
        return error;
    }

    bp = fat32_read_cluster(fsi, target_cluster);
    if (bp->b_flags & B_ERROR) {
        cmn_err(CE_WARN, "fat32_update_fdentry: read failed for cluster %u", target_cluster);
        error = bp->b_error ? bp->b_error : EIO;
        brelse(bp);
        return error;
    }

    /* Update the entry */
    dentry = (fat32_dirent83_t *)((char *)bp->b_un.b_addr + offset_in_cluster);

    if (fv->fv_flags & FV_ATIME_DIRTY) {
        /* Convert atime to FAT date */
        fat32_unix_to_fat_time(&fv->fv_atime, &date, NULL, NULL);
        WLE16(date, dentry->last_access_date);
        fv->fv_flags &= ~FV_ATIME_DIRTY;
    }

    if (fv->fv_flags & FV_MTIME_DIRTY) {
        /* Convert mtime to FAT date/time */
        fat32_unix_to_fat_time(&fv->fv_mtime, &date, &time, NULL);
        WLE16(date, dentry->write_date);
        WLE16(time, dentry->write_time);
        fv->fv_flags &= ~FV_MTIME_DIRTY;
    }

    if (fv->fv_flags & FV_CTIME_DIRTY) {
        /* Convert ctime to FAT date/time */
        fat32_unix_to_fat_time(&fv->fv_ctime, &date, &time, &tenths);
        WLE16(date, dentry->create_date);
        WLE16(time, dentry->create_time);
        dentry->create_time_tenth = tenths;
        fv->fv_flags &= ~FV_CTIME_DIRTY;
    }

    if (fv->fv_flags & FV_ATTR_DIRTY) {
        dentry->attr = fv->fv_attr;
        fv->fv_flags &= ~FV_ATTR_DIRTY;
    }

    if (fv->fv_flags & FV_SIZE_DIRTY) {
#ifdef FAT32_DBG_DIRENT
        cmn_err(CE_NOTE, "fat32_update_fdentry: writing size %lld to disk entry",
                (long long)fv->fv_size);
#endif
        WLE32(fv->fv_size, dentry->file_size);
        fv->fv_flags &= ~FV_SIZE_DIRTY;
    }

    if (fv->fv_flags & FV_CLUSTER_DIRTY) {
        WLE16((uint16_t)(fv->fv_cluster >> 16), dentry->first_cluster_hi);
        WLE16((uint16_t)(fv->fv_cluster & 0xFFFF), dentry->first_cluster_lo);
        fv->fv_flags &= ~FV_CLUSTER_DIRTY;
    }

    /* Write back asynchronously */
    bdwrite(bp);

    return 0;
}

/*
 * fat32_sync_metadata - Sync dirty metadata to disk with proper locking
 *
 * This helper function acquires the necessary locks and calls fat32_update_fdentry.
 * Used from close, fsync, and inactive to ensure metadata is written to disk.
 */
int
fat32_sync_metadata(fat32fs_vnode_t *fv)
{
    fat32fs_vnode_t *parent_fv = NULL;
    int error;

#ifdef FAT32_DBG_DIRENT
    cmn_err(CE_NOTE, "fat32_sync_metadata: cluster=%u dirty=0x%x",
            fv->fv_cluster, fv->fv_flags & FV_DIRTY_MASK);
#endif
    /* Lock parent if available to protect directory entry update */
    if (fv->fv_parent_vp) {
        parent_fv = VNODE_TO_FV(fv->fv_parent_vp);
        mrupdate(&parent_fv->fv_lock);
    }

    mrupdate(&fv->fv_lock);
    error = fat32_update_fdentry(fv);
    mrunlock(&fv->fv_lock);

    if (parent_fv) {
        mrunlock(&parent_fv->fv_lock);
    }

    return error;
}

/*
 * fat32_alloc_fdentry - Allocate space for directory entries
 *
 * Scans the directory for a contiguous sequence of free slots.
 * Allocates new clusters if necessary.
 */
int
fat32_alloc_fdentry(fat32fs_vnode_t *dir_fv, uint32_t num_entries, uint32_t *offset)
{
    fat32fs_info_t *fsi = dir_fv->fv_fsi;
    uint32_t current_cluster;
    uint32_t consecutive_free = 0;
    uint32_t start_free_offset = 0;
    uint32_t current_offset = 0;
    uint32_t entry_offset;
    uint32_t needed_end;
    buf_t *bp = NULL;
    int error;
    uint32_t i;
    int found = 0;
    int scanning = 1;
    uint32_t last_cluster = 0;
    uint8_t *data;

    /* Handle empty directory (cluster 0) - needs initialization */
    if (dir_fv->fv_cluster == 0) {
        uint32_t new_cluster;
        uint32_t parent_cluster;

        error = fat32_next_cluster(fsi, 0, &new_cluster, NC_ALLOC | NC_CLEAR);
        if (error) return error;

        /* Get parent cluster for .. entry, validate it */
        parent_cluster = dir_fv->fv_parent_cluster;
        if (parent_cluster == FAT32_CLUSTER_BAD) {
            /* This shouldn't happen for a directory being initialized */
            cmn_err(CE_WARN, "fat32_alloc_fdentry: invalid parent cluster during init");
            /* Free the cluster we just allocated */
            fat32_free_clusters(fsi, new_cluster, 1);
            return EINVAL;
        }

        /* Initialize directory with . and .. entries */
        error = fat32_init_dir(fsi, new_cluster, parent_cluster);
        if (error) {
            fat32_free_clusters(fsi, new_cluster, 1);
            return error;
        }

        dir_fv->fv_cluster = new_cluster;
        dir_fv->fv_flags |= FV_CLUSTER_DIRTY;
        *offset = 64; /* Skip . and .. entries */
        return 0;
    }

    current_cluster = dir_fv->fv_cluster;

    /* Scan through directory clusters looking for free entries */
    while (1) {
        last_cluster = current_cluster;

        if (scanning) {
            bp = fat32_read_cluster(fsi, current_cluster);
            if (bp->b_flags & B_ERROR) {
                cmn_err(CE_WARN, "fat32_alloc_fdentry: read failed for cluster %u", current_cluster);
                error = bp->b_error ? bp->b_error : EIO;
                brelse(bp);
                return error;
            }

            data = (uint8_t *)bp->b_un.b_addr;
            for (entry_offset = 0; entry_offset < fsi->fsi_bytes_per_cluster; entry_offset += 32) {
                uint8_t first_char = data[entry_offset];

                if (first_char == 0x00) {
                    if (consecutive_free == 0) start_free_offset = current_offset + entry_offset;
                    scanning = 0;
                    found = 1;
                    break;
                }

                if (first_char == 0xE5) {
                    if (consecutive_free == 0) start_free_offset = current_offset + entry_offset;
                    consecutive_free++;
                    if (consecutive_free == num_entries) {
                        brelse(bp);
                        *offset = start_free_offset;
                        return 0;
                    }
                } else {
                    consecutive_free = 0;
                }
            }
            brelse(bp);
        }

        current_offset += fsi->fsi_bytes_per_cluster;

        /* Try to get next cluster */
        error = fat32_next_cluster(fsi, current_cluster, &current_cluster, 0);
        if (error) {
            /* Reached end of cluster chain */
            break;
        }
    }

    if (!found && consecutive_free == 0) start_free_offset = current_offset;

    needed_end = start_free_offset + num_entries * 32;
    if (needed_end > current_offset) {
        uint32_t bytes_needed = needed_end - current_offset;
        uint32_t clusters_needed = (bytes_needed + fsi->fsi_bytes_per_cluster - 1) >> fsi->fsi_cluster_shift;

        /* Allocate clusters one at a time */
        for (i = 0; i < clusters_needed; i++) {
            uint32_t new_cluster;
            error = fat32_next_cluster(fsi, last_cluster, &new_cluster, NC_ALLOC | NC_CLEAR);
            if (error) return error;

            last_cluster = new_cluster;
        }
    }

    *offset = start_free_offset;
    return 0;
}

/*
 * fat32_read_fdentry - Read a 32-byte directory entry at specific offset
 */
int
fat32_read_fdentry(fat32fs_info_t *fsi, uint32_t dir_cluster, uint32_t offset, void *data)
{
    uint32_t skip_clusters = offset >> fsi->fsi_cluster_shift;
    uint32_t cluster_off = offset & fsi->fsi_cluster_mask;
    uint32_t target_cluster;
    buf_t *bp;
    int error;

    /* Seek to the target cluster */
    error = fat32_seek_cluster(fsi, dir_cluster, skip_clusters, &target_cluster, 0);
    if (error) return error;

    bp = fat32_read_cluster(fsi, target_cluster);
    if (bp->b_flags & B_ERROR) {
        cmn_err(CE_WARN, "fat32_read_fdentry: read failed for cluster %u", target_cluster);
        error = bp->b_error ? bp->b_error : EIO;
        brelse(bp);
        return error;
    }

    bcopy((char *)bp->b_un.b_addr + cluster_off, data, 32);
    brelse(bp);

    return 0;
}

/*
 * fat32_write_fdentry - Write a 32-byte directory entry at specific offset
 */
int
fat32_write_fdentry(fat32fs_info_t *fsi, uint32_t dir_cluster, uint32_t offset, void *data)
{
    uint32_t skip_clusters = offset >> fsi->fsi_cluster_shift;
    uint32_t cluster_off = offset & fsi->fsi_cluster_mask;
    uint32_t target_cluster;
    buf_t *bp;
    int error;

    /* Seek to the target cluster */
    error = fat32_seek_cluster(fsi, dir_cluster, skip_clusters, &target_cluster, 0);
    if (error) return error;

    bp = fat32_read_cluster(fsi, target_cluster);
    if (bp->b_flags & B_ERROR) {
        cmn_err(CE_WARN, "fat32_write_entry: read failed for cluster %u", target_cluster);
        error = bp->b_error ? bp->b_error : EIO;
        brelse(bp);
        return error;
    }

    bcopy(data, (char *)bp->b_un.b_addr + cluster_off, 32);
    bdwrite(bp);

    return 0;
}

/*
 * fat32_write_lfn_fdentry - Write LFN entries and the final 8.3 entry
 */
static int
fat32_write_lfn_fdentry(fat32fs_info_t *fsi, uint32_t dir_cluster, uint32_t start_offset,
                        fat32_dirent83_t *d83, const char *long_name)
{
    fat32_dirent_lfn_t lfn;
    int len = (int)strlen(long_name);
    int num_lfn = (len + 12) / 13;
    int i, j;
    uint8_t checksum;
    int error;
    uint32_t current_offset;

    /* Calculate checksum of the short name */
    checksum = fat32_checksum(d83->name);

    /* Write LFN entries in reverse order */
    for (i = num_lfn; i > 0; i--) {
        int start_idx = (i - 1) * 13;
        int chars_left = len - start_idx;
        int chunk_len = (chars_left > 13) ? 13 : chars_left;
        uint8_t *name_ptr = (uint8_t *)long_name + start_idx;

        bzero(&lfn, sizeof(lfn));
        lfn.attr = FAT32_ATTR_LONG_NAME;
        lfn.type = 0;
        lfn.checksum = checksum;
        lfn.first_cluster[0] = 0;
        lfn.first_cluster[1] = 0;
        lfn.sequence = i;
        if (i == num_lfn) lfn.sequence |= FAT32_LFN_LAST;

        /* Fill name fields (UCS-2, but we just use ASCII/Latin1) */
        /* Name1 (5 chars) */
        for (j = 0; j < 5; j++) {
            if (j < chunk_len) {
                lfn.name1[j*2] = name_ptr[j];
                lfn.name1[j*2+1] = 0;
            } else if (j == chunk_len) {
                lfn.name1[j*2] = 0;
                lfn.name1[j*2+1] = 0;
            } else {
                lfn.name1[j*2] = 0xFF;
                lfn.name1[j*2+1] = 0xFF;
            }
        }

        /* Name2 (6 chars) */
        for (j = 0; j < 6; j++) {
            if (j + 5 < chunk_len) {
                lfn.name2[j*2] = name_ptr[j+5];
                lfn.name2[j*2+1] = 0;
            } else if (j + 5 == chunk_len) {
                lfn.name2[j*2] = 0;
                lfn.name2[j*2+1] = 0;
            } else {
                lfn.name2[j*2] = 0xFF;
                lfn.name2[j*2+1] = 0xFF;
            }
        }

        /* Name3 (2 chars) */
        for (j = 0; j < 2; j++) {
            if (j + 11 < chunk_len) {
                lfn.name3[j*2] = name_ptr[j+11];
                lfn.name3[j*2+1] = 0;
            } else if (j + 11 == chunk_len) {
                lfn.name3[j*2] = 0;
                lfn.name3[j*2+1] = 0;
            } else {
                lfn.name3[j*2] = 0xFF;
                lfn.name3[j*2+1] = 0xFF;
            }
        }

        /* Write LFN entry */
        /* LFN entries precede the 8.3 entry.
         * If start_offset is where the sequence starts, then:
         * Sequence 1 is at start_offset + (num_lfn - 1) * 32
         * Sequence N is at start_offset + (num_lfn - N) * 32
         * Wait, physical order: LFN(N), LFN(N-1)... LFN(1), 8.3
         * So LFN(N) is at start_offset.
         * LFN(i) is at start_offset + (num_lfn - i) * 32
         */
        current_offset = start_offset + (num_lfn - i) * FAT32_DENTRY_SIZE;
        error = fat32_write_fdentry(fsi, dir_cluster, current_offset, &lfn);
        if (error) return error;
    }

    /* Write 8.3 entry at the end */
    current_offset = start_offset + num_lfn * FAT32_DENTRY_SIZE;
    return fat32_write_fdentry(fsi, dir_cluster, current_offset, d83);
}

/*
 * fat32_init_dir - Initialize a new directory cluster with . and ..
 */
int
fat32_init_dir(fat32fs_info_t *fsi, uint32_t new_cluster, uint32_t parent_cluster)
{
    fat32_dirent83_t dot;
    timespec_t now;
    uint16_t date, time;
    uint8_t tenths;
    int error;

    /* Zero out the cluster first */
    error = fat32_clear_cluster(fsi, new_cluster);
    if (error) return error;

    /* Get time */
    nanotime(&now);
    fat32_unix_to_fat_time(&now, &date, &time, &tenths);

    /* Prepare . entry */
    bzero(&dot, sizeof(dot));
    bcopy(".          ", dot.name, 11);
    dot.attr = FAT32_ATTR_DIRECTORY;
    WLE16(date, dot.create_date);
    WLE16(time, dot.create_time);
    dot.create_time_tenth = tenths;
    WLE16(date, dot.write_date);
    WLE16(time, dot.write_time);
    WLE16(date, dot.last_access_date);
    WLE16((uint16_t)(new_cluster >> 16), dot.first_cluster_hi);
    WLE16((uint16_t)(new_cluster & 0xFFFF), dot.first_cluster_lo);

    /* Write . entry at offset 0 */
    error = fat32_write_fdentry(fsi, new_cluster, 0, &dot);
    if (error) return error;

    /* Prepare .. entry */
    bcopy("..         ", dot.name, 11);

    /* Handle root parent */
    if (parent_cluster == fsi->fsi_root_cluster) {
        parent_cluster = 0;
    }
    WLE16((uint16_t)(parent_cluster >> 16), dot.first_cluster_hi);
    WLE16((uint16_t)(parent_cluster & 0xFFFF), dot.first_cluster_lo);

    /* Write .. entry at offset 32 */
    error = fat32_write_fdentry(fsi, new_cluster, 32, &dot);
    if (error) return error;

    return 0;
}

/*
 * fat32_create_fdentry - Create a new directory entry
 */
int
fat32_create_fdentry(fat32fs_vnode_t *dir_fv, fat32fs_vnode_t *fv, char *name, int flags)
{
    fat32fs_info_t *fsi = dir_fv->fv_fsi;
    fat32_dirent83_t d83;
    uint32_t start_cluster = 0;
    int is_short;
    int error;
    int len = (int)strlen(name);
    timespec_t now;
    uint16_t date, time;
    uint8_t tenths;

#ifdef FAT32_DBG_DIRENT
    cmn_err(CE_WARN, "fat32_create_fdentry: enter name='%s' flags=0x%x", name, flags);
#endif
    /* Determine if we need LFN */
    is_short = fat32_name_is_short(name, len);

    if (is_short) {
        fv->fv_dirsz = 1;
    } else {
        fv->fv_dirsz = (len + 12) / 13 + 1;
    }
#ifdef FAT32_DBG_DIRENT
    cmn_err(CE_NOTE, "fat32_create_fdentry: is_short=%d num_entries=%u flags=0x%x", 
            is_short, fv->fv_dirsz, flags);
#endif
    /* Allocate space in directory if not reusing */
    if (!(flags & FAT32_CRE_REUSE)) {
        error = fat32_alloc_fdentry(dir_fv, fv->fv_dirsz, &fv->fv_diroff);
        if (error) {
            cmn_err(CE_WARN, "fat32_create_fdentry: alloc_dirent failed: %d", error);
            return error;
        }
#ifdef FAT32_DBG_DIRENT
        cmn_err(CE_NOTE, "fat32_create_fdentry: allocated offset=%u", fv->fv_diroff);
    } else {
        cmn_err(CE_NOTE, "fat32_create_fdentry: reusing offset=%u", fv->fv_diroff);
#endif        
    }

    /* If renaming, use existing cluster */
    if (flags & FAT32_CRE_RENAME) {
        start_cluster = fv->fv_cluster;
    }

    /* Allocate cluster for the new file/dir if needed (and not renaming) */
    else if (flags & FAT32_CRE_DIR) {
        error = fat32_next_cluster(fsi, 0, &start_cluster, NC_ALLOC | NC_CLEAR);
        if (error) {
            cmn_err(CE_WARN, "fat32_create_fdentry: alloc cluster failed: %d", error);
            return error;
        }
        error = fat32_init_dir(fsi, start_cluster, dir_fv->fv_cluster);
        if (error) {
            cmn_err(CE_WARN, "fat32_create_fdentry: init_dir failed: %d", error);
            return error;
        }
#ifdef FAT32_DBG_DIRENT
        cmn_err(CE_NOTE, "fat32_create_fdentry: allocated dir cluster=%u", start_cluster);
#endif        
    } else {
        start_cluster = 0; /* Empty file */
    }

    /* Prepare 8.3 entry */
    bzero(&d83, sizeof(d83));
    if (is_short) {
        /* Format short name directly */
        int i, j = 0;
        /* Pad with spaces */
        for (i = 0; i < 11; i++) d83.name[i] = ' ';
        
        /* Copy base */
        for (i = 0; i < len && name[i] != '.'; i++) {
            d83.name[j++] = name[i];
        }
        /* Copy ext */
        if (i < len && name[i] == '.') {
            i++;
            j = 8;
            for (; i < len; i++) {
                d83.name[j++] = name[i];
            }
        }
    } else {
        /* Generate synthetic short name */
        fat32_generate_short_name(dir_fv->fv_cluster, fv->fv_diroff, (char *)d83.name);
    }

    /* Set attributes */
    if (flags & FAT32_CRE_RENAME) {
        d83.attr = fv->fv_attr;
    } else {
        d83.attr = (flags & FAT32_CRE_DIR) ? FAT32_ATTR_DIRECTORY : 0;
        /* Check for read-only in fv_mode */
        if (!(fv->fv_mode & S_IWUSR)) {
            d83.attr |= FAT32_ATTR_READ_ONLY;
        }
    }

    /* Set timestamps */
    if (flags & FAT32_CRE_RENAME) {
        fat32_unix_to_fat_time(&fv->fv_ctime, &date, &time, &tenths);
        WLE16(date, d83.create_date);
        WLE16(time, d83.create_time);
        d83.create_time_tenth = tenths;

        fat32_unix_to_fat_time(&fv->fv_mtime, &date, &time, NULL);
        WLE16(date, d83.write_date);
        WLE16(time, d83.write_time);

        fat32_unix_to_fat_time(&fv->fv_atime, &date, NULL, NULL);
        WLE16(date, d83.last_access_date);
    } else {
        nanotime(&now);
        fat32_unix_to_fat_time(&now, &date, &time, &tenths);
        WLE16(date, d83.create_date);
        WLE16(time, d83.create_time);
        d83.create_time_tenth = tenths;
        WLE16(date, d83.write_date);
        WLE16(time, d83.write_time);
        WLE16(date, d83.last_access_date);
    }

    /* Set cluster */
    WLE16((uint16_t)(start_cluster >> 16), d83.first_cluster_hi);
    WLE16((uint16_t)(start_cluster & 0xFFFF), d83.first_cluster_lo);

    /* Write entries */
#ifdef FAT32_DBG_DIRENT
    cmn_err(CE_NOTE, "fat32_create_fdentry: writing entry (short=%d)", is_short);
#endif
    if (is_short) {
        error = fat32_write_fdentry(fsi, dir_fv->fv_cluster, fv->fv_diroff, &d83);
    } else {
        error = fat32_write_lfn_fdentry(fsi, dir_fv->fv_cluster, fv->fv_diroff, &d83, name);
    }
    if (error) {
        cmn_err(CE_WARN, "fat32_create_fdentry: write_entry failed: %d", error);
        return error;
    }

    /* Populate vnode structure if not renaming (rename preserves vnode) */
    if (!(flags & FAT32_CRE_RENAME)) {
#ifdef FAT32_DBG_DIRENT
        cmn_err(CE_NOTE, "fat32_create_fdentry: calling make_vnode cluster=%u", start_cluster);
#endif
        error = fat32_make_vnode(start_cluster, (flags & FAT32_CRE_DIR) ? VDIR : VREG, 
                                &fv->fv_vnode, fv, FV_TO_VNODE(dir_fv));
        if (error) {
            cmn_err(CE_WARN, "fat32_create_fdentry: make_vnode failed: %d", error);
#ifdef FAT32_DBG_DIRENT
        } else {
            cmn_err(CE_NOTE, "fat32_create_fdentry: success");
#endif
        }
    }
    return error;
}

#ifdef FAT32_DBG_DIRENT
/*
 * fat32_check_dir_integrity - Verify directory structure integrity
 *
 * Performs comprehensive integrity checks on a directory:
 * 1. Directories have size set to 0
 * 2. LFN entries have size set to 0
 * 3. LFN entries are properly ordered before short name entry
 * 4. LFN entries have proper checksum set
 *
 * Parameters:
 *   fv - FAT32 vnode for directory to check
 *
 * Returns:
 *   0 if directory is valid
 *   Error code indicating first integrity violation found
 */
int
fat32_check_dir_integrity(fat32fs_vnode_t *fv)
{
    fat32fs_info_t *fsi;
    buf_t *bp = NULL;
    uint32_t cluster_offset;
    uint32_t current_cluster;
    uint8_t *data;
    int error = 0;
    uint8_t expected_lfn_seq = 0;
    uint8_t expected_checksum = 0;
    uint8_t computed_checksum = 0;
    int in_lfn_sequence = 0;

    fsi = fv->fv_fsi;

    cmn_err(CE_NOTE, "fat32_check_dir_integrity: checking cluster=%u", fv->fv_cluster);

    /* Validate this is a directory */
    if (fv->fv_type != VDIR) {
        cmn_err(CE_WARN, "fat32_check_dir_integrity: not a directory");
        return ENOTDIR;
    }

    /* Handle empty directory (cluster 0) */
    if (fv->fv_cluster == 0) {
        cmn_err(CE_NOTE, "fat32_check_dir_integrity: empty directory (cluster 0), OK");
        return 0;
    }

    current_cluster = fv->fv_cluster;

    /* Scan directory entries across all clusters */
    while (1) {
        bp = fat32_read_cluster(fsi, current_cluster);
        if (bp->b_flags & B_ERROR) {
            cmn_err(CE_WARN, "fat32_check_dir_integrity: failed to read cluster %u", current_cluster);
            error = bp->b_error ? bp->b_error : EIO;
            brelse(bp);
            return error;
        }

        data = (uint8_t *)bp->b_un.b_addr;
        cluster_offset = 0;

        /* Scan entries in this cluster */
        while (cluster_offset < fsi->fsi_bytes_per_cluster) {
            fat32_dirent83_t *dentry = (fat32_dirent83_t *)(data + cluster_offset);

            /* Check for end of directory */
            if (dentry->name[0] == 0x00) {
                /* End marker found - check we're not in middle of LFN sequence */
                if (in_lfn_sequence) {
                    cmn_err(CE_WARN, "fat32_check_dir_integrity: directory end in middle of LFN sequence");
                    brelse(bp);
                    return EINVAL;
                }
                brelse(bp);
                cmn_err(CE_NOTE, "fat32_check_dir_integrity: check passed");
                return 0;
            }

            /* Skip deleted entries - they don't need validation */
            if (dentry->name[0] == 0xE5) {
                /* If we were in LFN sequence, deleted entry breaks it */
                if (in_lfn_sequence) {
                    cmn_err(CE_WARN, "fat32_check_dir_integrity: deleted entry in middle of LFN sequence cluster=%u offset=%u",
                            current_cluster, cluster_offset);
                    brelse(bp);
                    return EINVAL;
                }
                cluster_offset += FAT32_DENTRY_SIZE;
                continue;
            }

            /* Check if this is an LFN entry */
            if (dentry->attr == FAT32_ATTR_LONG_NAME) {
                fat32_dirent_lfn_t *lfn = (fat32_dirent_lfn_t *)dentry;
                uint8_t seq = lfn->sequence & FAT32_LFN_SEQ_MASK;

                cmn_err(CE_NOTE, "fat32_check_dir_integrity: LFN entry seq=%u (0x%02x) cluster=%u offset=%u",
                        seq, lfn->sequence, current_cluster, cluster_offset);

                /* LFN entries don't have a file_size field - skip size check */

                /* Check if this is the first LFN entry (highest sequence number) */
                if (lfn->sequence & FAT32_LFN_LAST) {
                    /* Start of new LFN sequence */
                    if (in_lfn_sequence) {
                        cmn_err(CE_WARN, "fat32_check_dir_integrity: new LFN sequence started before previous completed cluster=%u offset=%u",
                                current_cluster, cluster_offset);
                        brelse(bp);
                        return EINVAL;
                    }
                    in_lfn_sequence = 1;
                    expected_lfn_seq = seq;
                    expected_checksum = lfn->checksum;
                    cmn_err(CE_NOTE, "fat32_check_dir_integrity: LFN sequence start, expecting seq %u down to 1, checksum=0x%02x",
                            expected_lfn_seq, expected_checksum);
                } else {
                    /* Continuation of LFN sequence */
                    if (!in_lfn_sequence) {
                        cmn_err(CE_WARN, "fat32_check_dir_integrity: LFN entry without start marker cluster=%u offset=%u",
                                current_cluster, cluster_offset);
                        brelse(bp);
                        return EINVAL;
                    }
                }

                /* Verify sequence number is correct */
                if (seq != expected_lfn_seq) {
                    cmn_err(CE_WARN, "fat32_check_dir_integrity: LFN sequence error, expected %u got %u cluster=%u offset=%u",
                            expected_lfn_seq, seq, current_cluster, cluster_offset);
                    brelse(bp);
                    return EINVAL;
                }

                /* Verify checksum matches */
                if (lfn->checksum != expected_checksum) {
                    cmn_err(CE_WARN, "fat32_check_dir_integrity: LFN checksum mismatch, expected 0x%02x got 0x%02x cluster=%u offset=%u",
                            expected_checksum, lfn->checksum, current_cluster, cluster_offset);
                    brelse(bp);
                    return EINVAL;
                }

                /* Decrement expected sequence for next entry */
                expected_lfn_seq--;

                cluster_offset += FAT32_DENTRY_SIZE;
                continue;
            }

            /* Skip volume ID entries */
            if (dentry->attr & FAT32_ATTR_VOLUME_ID) {
                /* Volume ID breaks any LFN sequence */
                if (in_lfn_sequence) {
                    cmn_err(CE_WARN, "fat32_check_dir_integrity: volume ID entry in middle of LFN sequence cluster=%u offset=%u",
                            current_cluster, cluster_offset);
                    brelse(bp);
                    return EINVAL;
                }
                cluster_offset += FAT32_DENTRY_SIZE;
                continue;
            }

            /* This is a regular 8.3 entry */
            cmn_err(CE_NOTE, "fat32_check_dir_integrity: 8.3 entry attr=0x%02x size=%u cluster=%u offset=%u",
                    dentry->attr, RLE32(dentry->file_size), current_cluster, cluster_offset);

            /* If we were in LFN sequence, verify it completed properly */
            if (in_lfn_sequence) {
                if (expected_lfn_seq != 0) {
                    cmn_err(CE_WARN, "fat32_check_dir_integrity: LFN sequence incomplete, expected seq %u cluster=%u offset=%u",
                            expected_lfn_seq, current_cluster, cluster_offset);
                    brelse(bp);
                    return EINVAL;
                }

                /* Verify checksum of the 8.3 name matches */
                computed_checksum = fat32_checksum(dentry->name);
                if (computed_checksum != expected_checksum) {
                    cmn_err(CE_WARN, "fat32_check_dir_integrity: LFN checksum mismatch with 8.3 name, expected 0x%02x got 0x%02x cluster=%u offset=%u",
                            expected_checksum, computed_checksum, current_cluster, cluster_offset);
                    brelse(bp);
                    return EINVAL;
                }

                cmn_err(CE_NOTE, "fat32_check_dir_integrity: LFN sequence completed correctly");
                in_lfn_sequence = 0;
            }

            /* Check directory entries have size 0 */
            if (dentry->attr & FAT32_ATTR_DIRECTORY) {
                if (RLE32(dentry->file_size) != 0) {
                    cmn_err(CE_WARN, "fat32_check_dir_integrity: directory entry has non-zero size %u cluster=%u offset=%u",
                            RLE32(dentry->file_size), current_cluster, cluster_offset);
                    brelse(bp);
                    return EINVAL;
                }
            }

            cluster_offset += FAT32_DENTRY_SIZE;
        } /* end of while cluster_offset < fsi_bytes_per_cluster */

        brelse(bp);
        bp = NULL;

        /* Move to next cluster */
        error = fat32_next_cluster(fsi, current_cluster, &current_cluster, 0);
        if (error) {
            /* Reached end of cluster chain */
            /* Check we're not in middle of LFN sequence */
            if (in_lfn_sequence) {
                cmn_err(CE_WARN, "fat32_check_dir_integrity: directory ends in middle of LFN sequence");
                return EINVAL;
            }
            cmn_err(CE_NOTE, "fat32_check_dir_integrity: check passed (no end marker)");
            return 0;
        }
    }

    /* Should never reach here */
}
#endif /* FAT32_DBG_DIRENT */
