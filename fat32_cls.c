#include "fat32fs.h"

/*
 * fat32_cluster_to_sector - Convert cluster number to absolute sector
 */
uint32_t
fat32_cluster_to_sector(fat32fs_info_t *fsi, uint32_t cluster)
{
    return fsi->fsi_data_start +
           ((cluster - FAT32_CLUSTER_MIN) << fsi->fsi_spc_shift);
}

/*
 * fat32_read_cluster - Read a single cluster into a buffer
 *
 * Returns a locked buffer. The caller must check for B_ERROR.
 */
buf_t *
fat32_read_cluster(fat32fs_info_t *fsi, uint32_t cluster)
{
    uint32_t sector;
    sector = fat32_cluster_to_sector(fsi, cluster);
    return bread(fsi->fsi_dev, sector, fsi->fsi_sectors_per_cluster);
}

/*
 * fat32_rmw_cluster - Read and optionally write a FAT entry
 *
 * Reads the FAT entry for the given cluster and optionally writes a new value.
 * If new_value is NULL, only reads the entry (read-only operation).
 * If new_value is not NULL, writes the new value to all FAT copies.
 *
 * Parameters:
 *   fsi       - Filesystem info
 *   cluster   - Cluster number to read/modify
 *   old_value - Output: current value of the FAT entry (can be NULL if not needed)
 *   new_value - Input: if not NULL, write this value to all FAT copies
 *
 * Returns:
 *   0 on success, error code on failure
 */
int
fat32_rmw_cluster(fat32fs_info_t *fsi, uint32_t cluster, uint32_t *old_value, uint32_t *new_value)
{
    buf_t *bp;
    uint32_t fat_offset, fat_sector, entry_offset;
    uint32_t value;
    uint32_t fat_num;
    int error = 0;

    /* Validate cluster range */
    if (cluster < FAT32_CLUSTER_MIN || cluster > fsi->fsi_cluster_count + 1) {
        cmn_err(CE_WARN, "fat32_rmw_cluster: invalid cluster %u", cluster);
        return EINVAL;
    }

    /* Calculate FAT entry location */
    fat_offset = cluster * 4;
    fat_sector = fsi->fsi_fat_start + (fat_offset >> fsi->fsi_sector_shift);
    entry_offset = fat_offset & fsi->fsi_sector_mask;

    /* Read FAT sector */
    bp = bread(fsi->fsi_dev, fat_sector, 1);
    if (bp->b_flags & B_ERROR) {
        cmn_err(CE_WARN, "fat32_rmw_cluster: failed to read FAT sector %u", fat_sector);
        error = bp->b_error ? bp->b_error : EIO;
        brelse(bp);
        return error;
    }

    /* Read current value */
    value = RLE32((uint8_t *)bp->b_un.b_addr + entry_offset);
    value &= FAT32_CLUSTER_MASK;

    if (old_value) {
        *old_value = value;
    }

    /* If new_value is NULL, this is a read-only operation */
    if (!new_value) {
        brelse(bp);
        return 0;
    }

    /* Write new value to all FAT copies */
    for (fat_num = 0; fat_num < fsi->fsi_num_fats; fat_num++, fat_sector += fsi->fsi_fat_size) {
        if (fat_num) // we already read it for fat0
            bp = bread(fsi->fsi_dev, fat_sector, 1);
        if (bp->b_flags & B_ERROR) {
            cmn_err(CE_WARN, "fat32_rmw_cluster: failed to read FAT sector %u", fat_sector);
            error = bp->b_error ? bp->b_error : EIO;
            brelse(bp);
            return error;
        }

        /* Write new value */
        WLE32(*new_value, (uint8_t *)bp->b_un.b_addr + entry_offset);
        bdwrite(bp);
    }

    return 0;
}

/*
 * fat32_allocate_cluster - Allocate a new free cluster and optionally link it
 *
 * Searches for a free cluster starting from fsi_next_free, verifying it's
 * actually free. If the cluster is not free, continues searching with wrap-around.
 * Marks the allocated cluster with EOC and optionally links it to 'parent'.
 *
 * Parameters:
 *   fsi    - Filesystem info
 *   parent - Parent cluster to link to (0 means no linking)
 *   result - Output: newly allocated cluster number
 *   flags  - NC_CLEAR to zero out newly allocated cluster
 *
 * Returns:
 *   0 on success, ENOSPC if no free clusters found, other errors
 *
 * NOTE: Caller must hold fsi_lock in update mode
 */
static int
fat32_allocate_cluster(fat32fs_info_t *fsi, uint32_t parent, uint32_t *result, unsigned int flags)
{
    buf_t *bp;
    uint32_t new_cluster;
    uint32_t start_search;
    uint32_t cluster_value;
    uint32_t new_val;
    uint32_t free_count;
    uint32_t max_cluster;
    uint32_t clusters_searched = 0;
    int error = 0;

    /* Read free count and next free from cached FSInfo */
    free_count = fsi->fsi_free_count;
    start_search = fsi->fsi_next_free;

    /* Validate we have free clusters */
    if (free_count == 0 || free_count == 0xFFFFFFFF) {
        cmn_err(CE_WARN, "fat32_allocate_cluster: no free clusters available");
        return ENOSPC;
    }

    /* Validate and clamp start_search */
    max_cluster = fsi->fsi_cluster_count + 1;
    if (start_search < FAT32_CLUSTER_MIN || start_search > max_cluster) {
        start_search = FAT32_CLUSTER_MIN;
    }

    new_cluster = start_search;

#ifdef FAT32_DBG_CLUSTERS
    cmn_err(CE_NOTE, "fat32_allocate_cluster: searching from cluster %u, max=%u",
            new_cluster, max_cluster);
#endif

    /* Search for a free cluster with wrap-around */
    while (clusters_searched < fsi->fsi_cluster_count) {
        /* Read the FAT entry to verify it's free */
        error = fat32_rmw_cluster(fsi, new_cluster, &cluster_value, NULL);
        if (error) {
            cmn_err(CE_WARN, "fat32_allocate_cluster: failed to read cluster %u: %d",
                    new_cluster, error);
            return error;
        }

#ifdef FAT32_DBG_CLUSTERS
        cmn_err(CE_NOTE, "fat32_allocate_cluster: cluster %u = 0x%x",
                new_cluster, cluster_value);
#endif

        /* Found a free cluster */
        if (cluster_value == FAT32_CLUSTER_FREE) {
            break;
        }

        /* Try next cluster */
        new_cluster++;
        clusters_searched++;

        /* Wrap around if we hit the end */
        if (new_cluster > max_cluster) {
#ifdef FAT32_DBG_CLUSTERS
            cmn_err(CE_NOTE, "fat32_allocate_cluster: wrapping around to cluster %u",
                    FAT32_CLUSTER_MIN);
#endif
            new_cluster = FAT32_CLUSTER_MIN;
        }

        /* Check if we wrapped around to where we started */
        if (new_cluster == start_search && clusters_searched > 0) {
            cmn_err(CE_WARN, "fat32_allocate_cluster: no free clusters found after full search");
            return ENOSPC;
        }
    }

    /* Check if we exhausted the search */
    if (clusters_searched >= fsi->fsi_cluster_count) {
        cmn_err(CE_WARN, "fat32_allocate_cluster: no free clusters found");
        return ENOSPC;
    }

#ifdef FAT32_DBG_CLUSTERS
    cmn_err(CE_NOTE, "fat32_allocate_cluster: found free cluster %u after %u searches",
            new_cluster, clusters_searched);
#endif

    /* Mark the cluster with EOC */
    new_val = FAT32_CLUSTER_EOC;
    error = fat32_rmw_cluster(fsi, new_cluster, NULL, &new_val);
    if (error) {
        cmn_err(CE_WARN, "fat32_allocate_cluster: failed to mark cluster %u with EOC: %d",
                new_cluster, error);
        return error;
    }

    /* If parent != 0, link parent -> new_cluster */
    if (parent != 0) {
        new_val = new_cluster;
        error = fat32_rmw_cluster(fsi, parent, NULL, &new_val);
        if (error) {
            cmn_err(CE_WARN, "fat32_allocate_cluster: failed to link parent %u to %u: %d",
                    parent, new_cluster, error);
            /* Try to free the cluster we just allocated */
            new_val = FAT32_CLUSTER_FREE;
            fat32_rmw_cluster(fsi, new_cluster, NULL, &new_val);
            return error;
        }
    }

    /* Update FSInfo sector */
    bp = bread(fsi->fsi_dev, fsi->fsi_fsinfo_sector, 1);
    if (bp->b_flags & B_ERROR) {
        cmn_err(CE_WARN, "fat32_allocate_cluster: failed to read FSInfo sector");
        error = bp->b_error ? bp->b_error : EIO;
        brelse(bp);
        return error;
    }

    {
        fat32_fsinfo_t *fsinfo = (fat32_fsinfo_t *)bp->b_un.b_addr;
        uint32_t new_free_count = free_count - 1;
        uint32_t new_next_free = new_cluster + 1;

        /* Wrap next_free if needed */
        if (new_next_free > max_cluster) {
            new_next_free = FAT32_CLUSTER_MIN;
        }

        WLE32(new_free_count, fsinfo->free_count);
        WLE32(new_next_free, fsinfo->next_free);
        bdwrite(bp);

        /* Update cached values */
        fsi->fsi_free_count = new_free_count;
        fsi->fsi_next_free = new_next_free;
    }

    /* Clear cluster to zeros if requested */
    if (flags & NC_CLEAR) {
        error = fat32_clear_cluster(fsi, new_cluster);
        if (error) {
            cmn_err(CE_WARN, "fat32_allocate_cluster: failed to clear cluster %u", new_cluster);
            /* Non-fatal, cluster is already allocated */
        }
    }

    *result = new_cluster;

#ifdef FAT32_DBG_CLUSTERS
    cmn_err(CE_NOTE, "fat32_allocate_cluster: allocated cluster %u (clear=%d)",
            new_cluster, (flags & NC_CLEAR) ? 1 : 0);
#endif

    return 0;
}

/*
 * fat32_next_cluster - Get next cluster in chain, optionally allocating
 *
 * Reads the FAT entry for 'current' and returns the next cluster.
 * If current is at EOC and NC_ALLOC is set, allocates a new cluster
 * and links it to current.
 *
 * Parameters:
 *   fsi     - Filesystem info
 *   current - Current cluster number (0 means allocate new unlinked cluster)
 *   next    - Output: next cluster number
 *   flags   - NC_ALLOC to allocate when reaching EOC
 *             NC_CLEAR to zero out newly allocated cluster
 *
 * Returns:
 *   0 on success, ENOSPC if at EOC and no allocation requested, other errors
 */
int
fat32_next_cluster(fat32fs_info_t *fsi, uint32_t current, uint32_t *next, unsigned int flags)
{
    buf_t *bp;
    uint32_t next_cluster;
    uint32_t new_val;
    int error = 0;

#ifdef FAT32_DBG_CLUSTERS
    cmn_err(CE_NOTE, "fat32_next_cluster: current=%u flags=0x%x", current, flags);
#endif

    /* Handle current == 0: allocate new unlinked cluster */
    if (current == 0) {
        if (!(flags & NC_ALLOC)) {
            return EINVAL;
        }
        /* Allocate new cluster without linking to parent */
        goto allocate_new;
    }

    /* Validate cluster range */
    if (current < FAT32_CLUSTER_MIN || current > fsi->fsi_cluster_count + 1) {
        cmn_err(CE_WARN, "fat32_next_cluster: invalid current cluster %u", current);
        return EINVAL;
    }

    /* Read FAT entry for current cluster */
    mraccess(&fsi->fsi_lock);
    error = fat32_rmw_cluster(fsi, current, &next_cluster, NULL);
    mraccunlock(&fsi->fsi_lock);

    if (error) {
        return error;
    }

#ifdef FAT32_DBG_CLUSTERS
    cmn_err(CE_NOTE, "fat32_next_cluster: FAT[%u]=%u", current, next_cluster);
#endif

    /* Check for end of chain */
    if (next_cluster >= FAT32_CLUSTER_EOC_MIN) {
        if (!(flags & NC_ALLOC)) {
            *next = next_cluster;
            return ENOSPC;
        }
        goto allocate_new;
    }

    /* Check for bad cluster */
    if (next_cluster == FAT32_CLUSTER_BAD) {
        cmn_err(CE_WARN, "fat32_next_cluster: bad cluster %u", current);
        return EIO;
    }

    /* Check for free cluster (shouldn't happen in a chain) */
    if (next_cluster == FAT32_CLUSTER_FREE) {
        cmn_err(CE_WARN, "fat32_next_cluster: free cluster in chain at %u", current);
        return EIO;
    }

    /* Validate next cluster range */
    if (next_cluster < FAT32_CLUSTER_MIN || next_cluster > fsi->fsi_cluster_count + 1) {
        cmn_err(CE_WARN, "fat32_next_cluster: invalid next cluster %u", next_cluster);
        return EIO;
    }

    *next = next_cluster;
    return 0;

allocate_new:
    /* Allocate a new cluster and optionally link to current */
    mrupdate(&fsi->fsi_lock);
    error = fat32_allocate_cluster(fsi, current, next, flags);
    mrunlock(&fsi->fsi_lock);
    return error;
}

/*
 * fat32_free_clusters - Free a cluster chain starting from a given cluster
 *
 * Walks the FAT chain starting from start_cluster and marks all clusters as free.
 * If free_start is set, the start_cluster itself is also marked as free.
 * If free_start is not set, the start_cluster is marked with EOC (truncation).
 *
 * Parameters:
 *   fsi           - Filesystem info
 *   start_cluster - Starting cluster of the chain to free
 *   free_start    - If non-zero, mark start_cluster as free too
 *                   If zero, mark start_cluster with EOC (truncate chain)
 *
 * Returns:
 *   0 on success, error code on failure
 */
int
fat32_free_clusters(fat32fs_info_t *fsi, uint32_t start_cluster, int free_start)
{
    buf_t *bp;
    uint32_t current_cluster;
    uint32_t next_cluster;
    uint32_t freed_count = 0;
    uint32_t new_val;
    int error = 0;

#ifdef FAT32_DBG_CLUSTERS
    cmn_err(CE_NOTE, "fat32_free_clusters: start=%u free_start=%d",
            start_cluster, free_start);
#endif

    /* Validate start cluster */
    if (start_cluster < FAT32_CLUSTER_MIN ||
        start_cluster > fsi->fsi_cluster_count + 1) {
        cmn_err(CE_WARN, "fat32_free_clusters: invalid start cluster %u",
                start_cluster);
        return EINVAL;
    }

    mrupdate(&fsi->fsi_lock);

    current_cluster = start_cluster;

    /* If we're not freeing the start cluster:
     * 1. Read next cluster from start_cluster
     * 2. Write EOC to start_cluster
     * 3. Continue freeing from next_cluster
     */
    if (!free_start) {
#ifdef FAT32_DBG_CLUSTERS
        cmn_err(CE_NOTE, "fat32_free_clusters: truncating at cluster %u", start_cluster);
#endif
        /* Read the next cluster and write EOC to start_cluster */
        new_val = FAT32_CLUSTER_EOC;
        error = fat32_rmw_cluster(fsi, start_cluster, &next_cluster, &new_val);
        if (error) {
            cmn_err(CE_WARN, "fat32_free_clusters: failed to read/mark EOC at cluster %u: %d",
                    start_cluster, error);
            mrunlock(&fsi->fsi_lock);
            return error;
        }

#ifdef FAT32_DBG_CLUSTERS
        cmn_err(CE_NOTE, "fat32_free_clusters: marked cluster %u with EOC, next=%u",
                start_cluster, next_cluster);
#endif

        /* If already at EOC, we're done */
        if (next_cluster >= FAT32_CLUSTER_EOC_MIN) {
            mrunlock(&fsi->fsi_lock);
#ifdef FAT32_DBG_CLUSTERS
            cmn_err(CE_NOTE, "fat32_free_clusters: chain already ended, freed 0 clusters");
#endif
            return 0;
        }

        current_cluster = next_cluster;
    } else {
#ifdef FAT32_DBG_CLUSTERS
        cmn_err(CE_NOTE, "fat32_free_clusters: freeing entire chain starting at %u", start_cluster);
#endif
    }

    /* Walk the chain and free all clusters */
#ifdef FAT32_DBG_CLUSTERS
    cmn_err(CE_NOTE, "fat32_free_clusters: entering free loop, current=%u", current_cluster);
#endif

    while (current_cluster >= FAT32_CLUSTER_MIN &&
           current_cluster < FAT32_CLUSTER_EOC_MIN) {

#ifdef FAT32_DBG_CLUSTERS
        cmn_err(CE_NOTE, "fat32_free_clusters: freeing cluster %u", current_cluster);
#endif

        /* Read next cluster and mark current as free */
        new_val = FAT32_CLUSTER_FREE;
        error = fat32_rmw_cluster(fsi, current_cluster, &next_cluster, &new_val);
        if (error) {
            cmn_err(CE_WARN, "fat32_free_clusters: failed to free cluster %u: %d",
                    current_cluster, error);
            mrunlock(&fsi->fsi_lock);
            return error;
        }

        freed_count++;

#ifdef FAT32_DBG_CLUSTERS
        cmn_err(CE_NOTE, "fat32_free_clusters: freed cluster %u, next=%u",
                current_cluster, next_cluster);
#endif

        /* Move to next cluster */
        current_cluster = next_cluster;
    }

#ifdef FAT32_DBG_CLUSTERS
    cmn_err(CE_NOTE, "fat32_free_clusters: exited loop, current=%u freed_count=%u",
            current_cluster, freed_count);
#endif

    /* Update FSInfo sector with new free count */
    if (freed_count > 0) {
        bp = bread(fsi->fsi_dev, fsi->fsi_fsinfo_sector, 1);
        if (bp->b_flags & B_ERROR) {
            cmn_err(CE_WARN, "fat32_free_clusters: failed to read FSInfo sector");
            /* Non-fatal - continue without updating FSInfo */
            brelse(bp);
        } else {
            fat32_fsinfo_t *fsinfo = (fat32_fsinfo_t *)bp->b_un.b_addr;
            uint32_t new_free_count;

            /* Update free count if it's valid */
            if (fsi->fsi_free_count != 0xFFFFFFFF) {
                new_free_count = fsi->fsi_free_count + freed_count;
                WLE32(new_free_count, fsinfo->free_count);
                bdwrite(bp);

                /* Update cached value */
                fsi->fsi_free_count = new_free_count;
            } else {
                brelse(bp);
            }
        }
    }

    mrunlock(&fsi->fsi_lock);

#ifdef FAT32_DBG_CLUSTERS
    cmn_err(CE_NOTE, "fat32_free_clusters: freed %u clusters", freed_count);
#endif

    return 0;
}

/*
 * fat32_seek_cluster - Seek to a cluster at given offset in cluster chain
 *
 * Skips the first 'skip_clusters' in the chain, returning the cluster number
 * at that position.
 *
 * Parameters:
 *   fsi           - Filesystem info
 *   start         - Starting cluster of the chain
 *   skip_clusters - Number of clusters to skip
 *   result        - Output: cluster number at skip_clusters position
 *   flags         - Flags to pass to fat32_next_cluster (NC_ALLOC, NC_CLEAR, etc.)
 *
 * Returns:
 *   0 on success, error code on failure
 */
int
fat32_seek_cluster(fat32fs_info_t *fsi, uint32_t start, uint32_t skip_clusters,
                   uint32_t *result, unsigned int flags)
{
    uint32_t current_cluster = start;
    uint32_t i;
    int error;

#ifdef FAT32_DBG_CLUSTERS
    cmn_err(CE_NOTE, "fat32_seek_cluster: start=%u skip=%u flags=0x%x",
            start, skip_clusters, flags);
#endif

    /* Handle the no-skip case */
    if (skip_clusters == 0) {
        *result = start;
        return 0;
    }

    /* Seek by following the chain */
    for (i = 0; i < skip_clusters; i++) {
        error = fat32_next_cluster(fsi, current_cluster, &current_cluster, flags);
        if (error) {
#ifdef FAT32_DBG_CLUSTERS
            cmn_err(CE_NOTE, "fat32_seek_cluster: failed at skip %u/%u",
                    i, skip_clusters);
#endif
            return error;
        }
    }

    *result = current_cluster;
    return 0;
}

/*
 * fat32_vnode_update_cache - Update cluster cache with new entry (caller must NOT hold lock)
 *
 * Adds a cluster index->cluster mapping to the LRU cache.
 * Most recently used entries are at the end of the array.
 * If entry with same index already exists, it is moved to MRU instead of duplicated.
 */
void
fat32_vnode_update_cache(fat32fs_vnode_t *fv, uint32_t index, uint32_t cluster)
{
    int i;
    int empty_slot = -1;
    int lru_slot = 0;

    mutex_lock(&fv->fv_cache_lock, PZERO);

    /* Find insertion slot: prefer empty slot or duplicate, else use LRU (slot 0) */
    for (i = 0; i < FAT32_CLS_CACHE_SIZE; i++) {
        if (fv->fv_cls_cache[i].cluster == 0 ||
            fv->fv_cls_cache[i].index == index) {
            /* Treat both empty slots and duplicates the same */
            empty_slot = i;
            break;
        }
    }

    if (empty_slot != -1) {
        lru_slot = empty_slot;
    } else {
        /* Evict LRU entry at position 0 */
        lru_slot = 0;
    }

    /* Shift entries down to make room at the end (MRU position) */
    if (lru_slot < FAT32_CLS_CACHE_SIZE - 1) {
        for (i = lru_slot; i < FAT32_CLS_CACHE_SIZE - 1; i++) {
            fv->fv_cls_cache[i] = fv->fv_cls_cache[i + 1];
        }
    }

    /* Insert new entry at MRU position (end of array) */
    fv->fv_cls_cache[FAT32_CLS_CACHE_SIZE - 1].index = index;
    fv->fv_cls_cache[FAT32_CLS_CACHE_SIZE - 1].cluster = cluster;

    mutex_unlock(&fv->fv_cache_lock);
}

/*
 * fat32_vnode_seek_cluster - Seek to cluster in vnode's cluster chain with LRU cache
 *
 * Uses an LRU cache to avoid repeatedly traversing the cluster chain from the start.
 * Cache lookup finds the closest cached cluster index <= target, then seeks from there.
 */
int
fat32_vnode_seek_cluster(fat32fs_vnode_t *fv, uint32_t skip_clusters,
                         uint32_t *result, unsigned int flags)
{
    uint32_t best_index = 0;
    uint32_t best_cluster = fv->fv_cluster;
    uint32_t remaining_skip;
    uint32_t found_cluster;
    int i;
    int error;

    /* Take cache lock and search for best cached entry */
    mutex_lock(&fv->fv_cache_lock, PZERO);

    for (i = 0; i < FAT32_CLS_CACHE_SIZE; i++) {
        if (fv->fv_cls_cache[i].cluster == 0) {
            continue;
        }

        /* Find highest index that doesn't exceed target */
        if (fv->fv_cls_cache[i].index <= skip_clusters &&
            fv->fv_cls_cache[i].index > best_index) {
            best_index = fv->fv_cls_cache[i].index;
            best_cluster = fv->fv_cls_cache[i].cluster;
        }
    }

    mutex_unlock(&fv->fv_cache_lock);

    /* Calculate remaining clusters to seek */
    remaining_skip = skip_clusters - best_index;

    /* Seek from the cached position (or start if no cache hit) */
    if (remaining_skip > 0) {
        error = fat32_seek_cluster(fv->fv_fsi, best_cluster, remaining_skip,
                                   &found_cluster, flags);
        if (error) {
            return error;
        }
    } else {
        /* Exact cache hit */
        found_cluster = best_cluster;
    }

    /* Update cache with new entry */
    fat32_vnode_update_cache(fv, skip_clusters, found_cluster);

    *result = found_cluster;
    return 0;
}

/*
 * fat32_clear_cluster - Zero out a cluster on disk
 */
int
fat32_clear_cluster(fat32fs_info_t *fsi, uint32_t cluster)
{
    buf_t *bp;
    uint32_t sector;
    int i;

    sector = fat32_cluster_to_sector(fsi, cluster);

    for (i = 0; i < fsi->fsi_sectors_per_cluster; i++) {
        bp = getblk(fsi->fsi_dev, sector + i, 1);
        clrbuf(bp);
        bdwrite(bp);
    }
    return 0;
}

