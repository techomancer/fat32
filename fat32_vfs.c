#include "fat32fs.h"
/*
 * VFS operations
 */

/*
 * fat32_read_boot - Read and parse boot sector and FSInfo
 *
 * Detects whether we're mounting a whole disk (MBR) or direct partition,
 * reads the FAT32 boot sector, and caches FSInfo data.
 * If fsi->fsi_partition_num is set (0-3), uses that specific partition.
 */
static int
fat32_read_boot(fat32fs_info_t *fsi)
{
    buf_t *bp;
    fat32_mbr_t *mbr;
    fat32_br_t *br;
    fat32_fsinfo_t *fsinfo;
    uint16_t signature;
    int error;
    int i;
    int part_to_use = -1;

    fsi->fsi_partition_offset = 0;

    /*
     * Read sector 0 to detect MBR or direct FAT32 partition
     * bread takes sector number and number of 512-byte blocks
     */
    bp = bread(fsi->fsi_dev, 0, 1);
    if (bp->b_flags & B_ERROR) {
        cmn_err(CE_WARN, "fat32_read_boot: failed to read sector 0");
        error = bp->b_error ? bp->b_error : EIO;
        brelse(bp);
        return error;
    }

    /* Check for sector signature */
    signature = RLE16((uint8_t *)bp->b_un.b_addr + 510);
    if (signature != FAT32_SECTOR_SIGNATURE) {
        cmn_err(CE_WARN, "fat32_read_boot: invalid sector signature: 0x%x", signature);
        brelse(bp);
        return EINVAL;
    }

    /*
     * Detect MBR vs FAT32 boot sector
     * MBR has partition table at offset 446
     * FAT32 boot sector has "FAT32   " at offset 82
     */
    br = (fat32_br_t *)bp->b_un.b_addr;

    /* Check if this looks like a FAT32 boot sector */
    if (br->fs_type[0] == 'F' && br->fs_type[1] == 'A' &&
        br->fs_type[2] == 'T' && br->fs_type[3] == '3') {
        /* Direct FAT32 partition mount - keep the buffer */
        cmn_err(CE_NOTE, "fat32_read_boot: detected direct FAT32 partition");
    } else {
        /* This is an MBR, search for FAT32 partition (type 0x0C or 0x0B) */
        cmn_err(CE_NOTE, "fat32_read_boot: detected MBR, searching for FAT32 partition");
        mbr = (fat32_mbr_t *)bp->b_un.b_addr;

        /* If specific partition requested, use it */
        if (fsi->fsi_partition_num >= 0 && fsi->fsi_partition_num < 4) {
            part_to_use = fsi->fsi_partition_num;
            cmn_err(CE_NOTE, "fat32_read_boot: using partition %d as requested", part_to_use);
        } else {
            /* Auto-detect first FAT32 partition */
            for (i = 0; i < 4; i++) {
                if (mbr->partition[i].system_id == 0x0C ||
                    mbr->partition[i].system_id == 0x0B) {
                    part_to_use = i;
                    cmn_err(CE_NOTE, "fat32_read_boot: auto-detected FAT32 partition %d", i);
                    break;
                }
            }
        }

        if (part_to_use < 0) {
            cmn_err(CE_WARN, "fat32_read_boot: no FAT32 partition found in MBR");
            brelse(bp);
            return EINVAL;
        }

        /* Verify the partition has a FAT32 system ID */
        if (mbr->partition[part_to_use].system_id != 0x0C &&
            mbr->partition[part_to_use].system_id != 0x0B) {
            cmn_err(CE_WARN, "fat32_read_boot: partition %d is not FAT32 (type 0x%02x)",
                    part_to_use, mbr->partition[part_to_use].system_id);
            brelse(bp);
            return EINVAL;
        }

        fsi->fsi_partition_offset = RLE32(mbr->partition[part_to_use].relative_sector);
        cmn_err(CE_NOTE, "fat32_read_boot: using partition %d at sector %u",
                part_to_use, fsi->fsi_partition_offset);

        /* Release MBR buffer, we need to read the actual boot sector */
        brelse(bp);

        /* Read FAT32 boot sector from the partition */
        bp = bread(fsi->fsi_dev, fsi->fsi_partition_offset, 1);
        if (bp->b_flags & B_ERROR) {
            cmn_err(CE_WARN, "fat32_read_boot: failed to read boot sector at %u",
                    fsi->fsi_partition_offset);
            error = bp->b_error ? bp->b_error : EIO;
            brelse(bp);
            return error;
        }

        br = (fat32_br_t *)bp->b_un.b_addr;
    }

    /* Validate boot sector */
    signature = RLE16(br->signature);
    if (signature != FAT32_SECTOR_SIGNATURE) {
        cmn_err(CE_WARN, "fat32_read_boot: invalid boot sector signature: 0x%x",
                signature);
        brelse(bp);
        return EINVAL;
    }

    if (br->boot_signature != FAT32_BOOT_SIGNATURE) {
        cmn_err(CE_WARN, "fat32_read_boot: invalid boot signature: 0x%x",
                br->boot_signature);
    }

    /* Extract boot sector parameters */
    fsi->fsi_bytes_per_sector = RLE16(br->bytes_per_sector);
    fsi->fsi_sectors_per_cluster = br->sectors_per_cluster;
    fsi->fsi_reserved_sectors = RLE16(br->reserved_sectors);
    fsi->fsi_num_fats = br->num_fats;
    fsi->fsi_fat_size = RLE32(br->fat_size_32);
    fsi->fsi_root_cluster = RLE32(br->root_cluster);
    fsi->fsi_total_sectors = RLE32(br->total_sectors_32);

    /* Calculate and cache cluster size */
    fsi->fsi_bytes_per_cluster = fsi->fsi_bytes_per_sector * fsi->fsi_sectors_per_cluster;

    /* Calculate shifts and masks for optimization */
    fsi->fsi_sector_shift = fat32_log2(fsi->fsi_bytes_per_sector);
    fsi->fsi_sector_mask = fsi->fsi_bytes_per_sector - 1;
    fsi->fsi_cluster_shift = fat32_log2(fsi->fsi_bytes_per_cluster);
    fsi->fsi_cluster_mask = fsi->fsi_bytes_per_cluster - 1;
    fsi->fsi_spc_shift = fat32_log2(fsi->fsi_sectors_per_cluster);

    /* Calculate data area start and cluster count */
    fsi->fsi_data_start = fsi->fsi_reserved_sectors +
                          (fsi->fsi_num_fats * fsi->fsi_fat_size);
    fsi->fsi_cluster_count = (fsi->fsi_total_sectors - fsi->fsi_data_start) /
                             fsi->fsi_sectors_per_cluster;

    cmn_err(CE_NOTE, "fat32_read_boot: bytes/sec=%u sec/clus=%u bytes/clus=%u reserved=%u",
            fsi->fsi_bytes_per_sector, fsi->fsi_sectors_per_cluster,
            fsi->fsi_bytes_per_cluster, fsi->fsi_reserved_sectors);
    cmn_err(CE_NOTE, "fat32_read_boot: fats=%u fat_size=%u root_clus=%u total_sec=%u",
            fsi->fsi_num_fats, fsi->fsi_fat_size, fsi->fsi_root_cluster,
            fsi->fsi_total_sectors);

    /* Cache important sector locations with partition offset applied */
    fsi->fsi_fsinfo_sector = fsi->fsi_partition_offset + RLE16(br->fs_info);
    fsi->fsi_fat_start = fsi->fsi_partition_offset + fsi->fsi_reserved_sectors;
    fsi->fsi_data_start += fsi->fsi_partition_offset;

    brelse(bp);

    /*
     * Read FSInfo sector
     */
    bp = bread(fsi->fsi_dev, fsi->fsi_fsinfo_sector, 1);
    if (bp->b_flags & B_ERROR) {
        cmn_err(CE_WARN, "fat32_read_boot: failed to read FSInfo sector at %u",
                fsi->fsi_fsinfo_sector);
        /* FSInfo is optional, continue without it */
        fsi->fsi_free_count = 0xFFFFFFFF;
        fsi->fsi_next_free = 0xFFFFFFFF;
        brelse(bp);
    } else {
        fsinfo = (fat32_fsinfo_t *)bp->b_un.b_addr;

        /* Validate FSInfo signatures */
        if (RLE32(fsinfo->lead_signature) != FAT32_FSINFO_LEAD_SIG ||
            RLE32(fsinfo->struct_signature) != FAT32_FSINFO_STRUCT_SIG ||
            RLE32(fsinfo->trail_signature) != FAT32_FSINFO_TRAIL_SIG) {
            cmn_err(CE_WARN, "fat32_read_boot: invalid FSInfo signatures");
            fsi->fsi_free_count = 0xFFFFFFFF;
            fsi->fsi_next_free = 0xFFFFFFFF;
        } else {
            fsi->fsi_free_count = RLE32(fsinfo->free_count);
            fsi->fsi_next_free = RLE32(fsinfo->next_free);
            cmn_err(CE_NOTE, "fat32_read_boot: free_count=%u next_free=%u",
                    fsi->fsi_free_count, fsi->fsi_next_free);
        }

        brelse(bp);
    }

    return 0;
}

/*
 * fat32_mount - Mount a FAT32 filesystem
 *
 * This function is called when a FAT32 filesystem is being mounted.
 * It allocates and initializes the filesystem info structure, reads
 * and validates the boot sector, and sets up the root vnode.
 */
static int
fat32_mount(vfs_t *vfsp, vnode_t *mvp, struct mounta *uap, char *attrs, cred_t *cr)
{
    fat32fs_info_t *fsi;
    vnode_t *devvp;
    vnode_t *rootvp;
    dev_t dev;
    int error;
    char *opts;
    char *p, *opt;
    int partition_num = -1;
    uid_t uid = 0;
    gid_t gid = 0;

    cmn_err(CE_NOTE, "fat32_mount: mounting device %s", uap->spec);

    /*
     * Parse mount options from uap->dataptr
     * Options are comma-separated: part=N,uid=N,gid=N
     */
    if (uap->datalen > 0 && uap->dataptr) {
        opts = kmem_alloc(uap->datalen + 1, KM_SLEEP);
        if (copyin(uap->dataptr, opts, (int)uap->datalen)) {
            kmem_free(opts, uap->datalen + 1);
            return EFAULT;
        }
        opts[uap->datalen] = '\0';

        cmn_err(CE_NOTE, "fat32_mount: parsing options: %s", opts);

        /* Parse options manually (strsep not available in kernel) */
        p = opts;
        while (p && *p) {
            /* Find next comma or end of string */
            opt = p;
            while (*p && *p != ',') {
                p++;
            }

            /* Null-terminate this option if we found a comma */
            if (*p == ',') {
                *p = '\0';
                p++;
            }

            /* Skip empty options */
            if (*opt == '\0') {
                continue;
            }

            /* Parse the option */
            if (strncmp(opt, "part=", 5) == 0) {
                partition_num = atoi(opt + 5);
                if (partition_num < 0 || partition_num > 3) {
                    cmn_err(CE_WARN, "fat32_mount: invalid partition number %d", partition_num);
                    kmem_free(opts, uap->datalen + 1);
                    return EINVAL;
                }
                cmn_err(CE_NOTE, "fat32_mount: partition=%d", partition_num);
            } else if (strncmp(opt, "uid=", 4) == 0) {
                uid = (uid_t)atoi(opt + 4);
                cmn_err(CE_NOTE, "fat32_mount: uid=%d", uid);
            } else if (strncmp(opt, "gid=", 4) == 0) {
                gid = (gid_t)atoi(opt + 4);
                cmn_err(CE_NOTE, "fat32_mount: gid=%d", gid);
            }
        }

        kmem_free(opts, uap->datalen + 1);
    }

    /*
     * Get the device vnode for the block device being mounted
     */
    error = lookupname(uap->spec, UIO_USERSPACE, FOLLOW, NULL, &devvp, NULL);
    if (error) {
        cmn_err(CE_WARN, "fat32_mount: lookupname failed: %d", error);
        return error;
    }

    /*
     * Verify it's a block device
     */
    if (devvp->v_type != VBLK) {
        cmn_err(CE_WARN, "fat32_mount: not a block device");
        FAT32_VN_RELE(devvp);
        return ENOTBLK;
    }

    dev = devvp->v_rdev;

    /*
     * Check if device is already mounted
     */
    if (vfs_devsearch(dev, fat32fstype)) {
        cmn_err(CE_WARN, "fat32_mount: device already mounted");
        FAT32_VN_RELE(devvp);
        return EBUSY;
    }

    /*
     * Allocate filesystem info structure
     */
    fsi = (fat32fs_info_t *)kmem_zalloc(sizeof(fat32fs_info_t), KM_SLEEP);
    if (!fsi) {
        cmn_err(CE_WARN, "fat32_mount: kmem_zalloc failed");
        FAT32_VN_RELE(devvp);
        return ENOMEM;
    }

    /*
     * Initialize filesystem info structure
     */
    fsi->fsi_vfsp = vfsp;
    fsi->fsi_devvp = devvp;
    fsi->fsi_dev = dev;
    fsi->fsi_partition_num = partition_num;
    fsi->fsi_uid = uid;
    fsi->fsi_gid = gid;
    mrinit(&fsi->fsi_lock, "fat32fs");

    /*
     * Read and validate boot sector, detect MBR vs direct partition mount
     */
    error = fat32_read_boot(fsi);
    if (error) {
        mrfree(&fsi->fsi_lock);
        kmem_free(fsi, sizeof(fat32fs_info_t));
        FAT32_VN_RELE(devvp);
        return error;
    }

    /*
     * Initialize behavior descriptor and attach to VFS
     */
    bhv_desc_init(&fsi->fsi_bhv, fsi, vfsp, &fat32vfsops);
    bhv_insert_initial(VFS_BHVHEAD(vfsp), &fsi->fsi_bhv);

    /*
     * Set VFS parameters
     */
    vfsp->vfs_bcount = 0;
    vfsp->vfs_bsize = fsi->fsi_bytes_per_cluster;
    vfsp->vfs_fstype = fat32fstype;
    vfsp->vfs_dev = dev;
    vfsp->vfs_flag |= VFS_NOTRUNC | VFS_LOCAL;
    vfsp->vfs_fsid.val[0] = dev;
    vfsp->vfs_fsid.val[1] = fat32fstype;

    /*
     * Create and cache the root vnode (no parent for root)
     */
    {
        fat32fs_vnode_t *root_fv;

        /* Allocate fv structure for root */
        root_fv = fat32_vnode_new(fsi);
        if (!root_fv) {
            cmn_err(CE_WARN, "fat32_mount: failed to allocate root vnode structure");
            mrfree(&fsi->fsi_lock);
            kmem_free(fsi, sizeof(fat32fs_info_t));
            FAT32_VN_RELE(devvp);
            return ENOMEM;
        }

        error = fat32_make_vnode(fsi->fsi_root_cluster, VDIR, &fsi->fsi_rootvp, root_fv, NULL);
        if (error) {
            cmn_err(CE_WARN, "fat32_mount: failed to create root vnode: %d", error);
            fat32_vnode_delete(root_fv);
            mrfree(&fsi->fsi_lock);
            kmem_free(fsi, sizeof(fat32fs_info_t));
            FAT32_VN_RELE(devvp);
            return error;
        }
    }

    /* Mark the root vnode with VROOT flag */
    VN_FLAGSET(fsi->fsi_rootvp, VROOT);

    /* Hold extra reference to root vnode - released at unmount */
    VN_HOLD(fsi->fsi_rootvp);

    /*
     * Note: We do NOT call vfs_add() here - that's done by cmount() for us
     * Only mountroot needs to call vfs_add()
     */

    cmn_err(CE_NOTE, "fat32_mount: mount successful, dev 0x%x rootvp=%p",
            dev, fsi->fsi_rootvp);
    return 0;
}

static int
fat32_rootinit(vfs_t *vfsp)
{
#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_rootinit: enter");
    cmn_err(CE_NOTE, "fat32_rootinit: exit ENOSYS");
#endif
    return ENOSYS;
}

static int
fat32_mntupdate(bhv_desc_t *bdp, vnode_t *mvp, struct mounta *uap, cred_t *cr)
{
#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_mntupdate: enter");
    cmn_err(CE_NOTE, "fat32_mntupdate: exit ENOSYS");
#endif
    return ENOSYS;
}

/*
 * fat32_unmount - Unmount a FAT32 filesystem
 *
 * This function releases all resources associated with the mounted filesystem.
 */
static int
fat32_unmount(bhv_desc_t *bdp, int flags, cred_t *cr)
{
    fat32fs_info_t *fsi;
    vfs_t *vfsp;
    vnode_t *devvp;

    fsi = BHV_TO_FSI(bdp);
    vfsp = fsi->fsi_vfsp;
    devvp = fsi->fsi_devvp;

    cmn_err(CE_NOTE, "fat32_unmount: unmounting dev 0x%x", fsi->fsi_dev);

    /*
     * TODO: Flush any cached data
     * TODO: Free FAT cache
     */

    /*
     * Release root vnode
     */
    if (fsi->fsi_rootvp) {
        FAT32_VN_RELE(fsi->fsi_rootvp);
    }

    /*
     * Remove from VFS list
     */
    vfs_remove(vfsp);

    /*
     * Clean up filesystem info structure
     */
    mrfree(&fsi->fsi_lock);

    /*
     * Release device vnode
     */
    if (devvp) {
        FAT32_VN_RELE(devvp);
    }

    /*
     * Remove behavior descriptor from VFS
     */
    bhv_remove(VFS_BHVHEAD(vfsp), bdp);

    /*
     * Free filesystem info structure
     */
    kmem_free(fsi, sizeof(fat32fs_info_t));

    cmn_err(CE_NOTE, "fat32_unmount: unmount successful");
    return 0;
}

/*
 * fat32_root - Get the root vnode of the filesystem
 *
 * Returns a reference to the cached root directory vnode.
 * Increments the reference count each time it's called.
 */
static int
fat32_root(bhv_desc_t *bdp, vnode_t **vpp)
{
    fat32fs_info_t *fsi;
    vnode_t *vp;

    fsi = BHV_TO_FSI(bdp);

#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_root: enter, returning cached root vnode");
#endif
    /*
     * Return cached root vnode with incremented reference count
     * The root vnode was created and cached during mount
     */
    vp = fsi->fsi_rootvp;
    VN_HOLD(vp);
    *vpp = vp;

#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_root: exit success, vp=%p", vp);
#endif
    return 0;
}

static int
fat32_statvfs(bhv_desc_t *bdp, struct statvfs *sp, vnode_t *vp)
{
#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_statvfs: enter");
    cmn_err(CE_NOTE, "fat32_statvfs: exit ENOSYS");
#endif
    return ENOSYS;
}

static int
fat32_sync(bhv_desc_t *bdp, int flags, cred_t *cr)
{
    fat32fs_info_t *fsi;

#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_sync: enter flags=%d", flags);
#endif
    fsi = BHV_TO_FSI(bdp);

    /* Flush all dirty buffers for this device */
#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_sync: flushing device buffers");
#endif
    bflush(fsi->fsi_dev);

#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_sync: exit success");
#endif
    return 0;
}

static int
fat32_vget(bhv_desc_t *bdp, vnode_t **vpp, fid_t *fidp)
{
#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_vget: enter");
    cmn_err(CE_NOTE, "fat32_vget: exit ENOSYS");
#endif
    return ENOSYS;
}

static int
fat32_mountroot(bhv_desc_t *bdp, whymountroot_t why)
{
#ifdef FAT32_DBG_OTHER
    cmn_err(CE_NOTE, "fat32_mountroot: enter why=%d", why);
    cmn_err(CE_NOTE, "fat32_mountroot: exit ENOSYS");
#endif
    return ENOSYS;
}

vfsops_t fat32vfsops = {
    BHV_IDENTITY_INIT_POSITION(VFS_POSITION_BASE),
    fat32_mount,
    fat32_rootinit,
    fat32_mntupdate,
    fs_dounmount,
    fat32_unmount,
    fat32_root,
    fat32_statvfs,
    fat32_sync,
    fat32_vget,
    fat32_mountroot,
    fs_realvfsops,
    fs_import,
    fs_quotactl,
    (int (*)())fs_noerr,
    VFSOPS_MAGIC,
    0,
    0
};
