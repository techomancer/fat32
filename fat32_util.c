#include "fat32fs.h"

/*
 * fat32_log2 - Calculate log2 of a power of 2
 */
uint32_t
fat32_log2(uint32_t n)
{
    uint32_t bit = 0;
    while (n > 1) {
        n >>= 1;
        bit++;
    }
    return bit;
}

/*
 * fat32_pack_ino - Pack directory location into 64-bit inode number
 *
 * Packs the directory start cluster, offset of the entry (in bytes), and
 * number of slots used by the entry into a 64-bit inode number.
 *
 * Layout:
 *  Bits 0-7:   Number of slots (entries)
 *  Bits 8-35:  Slot index (offset / 32)
 *  Bits 36-63: Directory start cluster
 */
ino_t
fat32_pack_ino(uint32_t dir_cluster, uint32_t offset, uint32_t num_slots)
{
    uint64_t slot_index = offset / 32;
    uint64_t ino;

    /* Cluster is 28 bits in FAT32 */
    ino = ((uint64_t)(dir_cluster & 0x0FFFFFFF) << 36) |
          ((slot_index & 0x0FFFFFFF) << 8) |
          (num_slots & 0xFF);

    return (ino_t)ino;
}

/*
 * fat32_unpack_ino - Unpack 64-bit inode number
 *
 * Extracts directory start cluster, offset (in bytes), and number of slots
 * from the inode number.
 */
static void
fat32_unpack_ino(ino_t ino, uint32_t *dir_cluster, uint32_t *offset, uint32_t *num_slots)
{
    uint64_t val = (uint64_t)ino;

    if (dir_cluster)
        *dir_cluster = (uint32_t)((val >> 36) & 0x0FFFFFFF);

    if (offset)
        *offset = (uint32_t)(((val >> 8) & 0x0FFFFFFF) * 32);

    if (num_slots)
        *num_slots = (uint32_t)(val & 0xFF);
}

void
fat32_set_dname(uint32_t pos, unsigned short s, char *dname, uint32_t *d_name_size) {
    char c;
    if (pos >= *d_name_size)
        return;
    if (s == FAT32_LFN_FILL)
        return;

    if (s >= 128)
        c = '?';
    else
        c = (char)s;
    dname[pos] = c;
    if (c == '\0')
        *d_name_size = pos;
}

/*
 * Days in each month
 */
static const uint8_t days_in_month[] = {
    0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static int
is_leap(int year)
{
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

/*
 * fat32_unix_to_fat_time - Convert UNIX timespec to FAT32 date/time
 */
void
fat32_unix_to_fat_time(timespec_t *ts, uint16_t *date, uint16_t *time, uint8_t *tenths)
{
    time_t seconds;
    int year, month, day;
    int hour, min, sec;
    int days_in_year;
    int i;

    /* FAT32 epoch starts at 1980 */
    /* UNIX epoch starts at 1970 */
    /* Offset is 315532800 seconds (1970-1980) */

    if (ts->tv_sec < 315532800) {
        /* Before 1980, clamp to min FAT date */
        if (date) *date = (1 << 5) | 1; /* 1980-01-01 */
        if (time) *time = 0;
        if (tenths) *tenths = 0;
        return;
    }

    seconds = ts->tv_sec - 315532800;

    /* Calculate time of day */
    sec = seconds % 60;
    min = (seconds / 60) % 60;
    hour = (seconds / 3600) % 24;

    /* Calculate date */
    day = seconds / 86400;
    year = 1980;

    while (1) {
        days_in_year = is_leap(year) ? 366 : 365;
        if (day < days_in_year)
            break;
        day -= days_in_year;
        year++;
    }

    /* day is now 0-based day of year */
    month = 1;
    for (i = 1; i <= 12; i++) {
        int dim = days_in_month[i];
        if (i == 2 && is_leap(year))
            dim++;

        if (day < dim)
            break;
        day -= dim;
        month++;
    }
    day++; /* 1-based day of month */

    /* Pack values */
    if (date) {
        *date = ((year - 1980) << 9) | (month << 5) | day;
    }

    if (time) {
        *time = (hour << 11) | (min << 5) | (sec / 2);
    }

    if (tenths) {
        /* FAT tenths is 10ms units, 0-199 */
        /* Includes the odd second if present */
        *tenths = (uint8_t)((ts->tv_nsec / 10000000) + (sec % 2) * 100);
    }
}

/*
 * fat32_fat_to_unix_time - Convert FAT32 date/time to UNIX timespec
 */
void
fat32_fat_to_unix_time(uint16_t date, uint16_t time, uint8_t tenths, timespec_t *ts)
{
    int year, month, day;
    int hour, min, sec;
    time_t seconds;
    int i;

    /* Extract fields */
    year = 1980 + ((date >> 9) & 0x7F);
    month = (date >> 5) & 0x0F;
    day = date & 0x1F;

    hour = (time >> 11) & 0x1F;
    min = (time >> 5) & 0x3F;
    sec = (time & 0x1F) * 2;

    /* Add tenths/odd second */
    /* tenths is 0-199 (10ms units) */
    if (tenths > 199) tenths = 199;
    sec += tenths / 100;
    ts->tv_nsec = (tenths % 100) * 10000000;

    /* Validate date components */
    if (month < 1) month = 1;
    if (month > 12) month = 12;
    if (day < 1) day = 1;

    /* Calculate seconds since 1980 */
    seconds = 0;

    /* Add years */
    for (i = 1980; i < year; i++) {
        seconds += (is_leap(i) ? 366 : 365) * 86400;
    }

    /* Add months */
    for (i = 1; i < month; i++) {
        int dim = days_in_month[i];
        if (i == 2 && is_leap(year))
            dim++;
        seconds += dim * 86400;
    }

    /* Add days */
    seconds += (day - 1) * 86400;

    /* Add time */
    seconds += hour * 3600 + min * 60 + sec;

    /* Add 1970-1980 offset */
    ts->tv_sec = seconds + 315532800;
}

/*
 * fat32_name_is_short - Determine if filename qualifies as 8.3 short name
 *
 * Returns 1 if the name is a valid 8.3 short name (uppercase, valid chars,
 * correct length), 0 otherwise.
 */
int
fat32_name_is_short(const char *name, int name_len)
{
    int i;
    int dot_pos = -1;

    /* Special case for . and .. */
    if (name[0] == '.') {
        if (name_len == 1) return 1;
        if (name_len == 2 && name[1] == '.') return 1;
        return 0; /* .foo is invalid 8.3 */
    }

    if (name_len > 12) return 0;

    for (i = 0; i < name_len; i++) {
        char c = name[i];

        if (c == '.') {
            if (dot_pos != -1) return 0; /* Multiple dots */
            dot_pos = i;
            continue;
        }

        /* Check for lowercase */
        if (c >= 'a' && c <= 'z') return 0;

        /* Check for invalid characters */
        /* Valid: A-Z 0-9 $ % ' - _ @ ~ ` ! ( ) { } ^ # & */
        /* Also extended chars > 127 are valid in 8.3 */
        if ((unsigned char)c < 128 &&
            !((c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '$' || c == '%' || c == '\'' || c == '-' || c == '_' ||
              c == '@' || c == '~' || c == '`' || c == '!' || c == '(' ||
              c == ')' || c == '{' || c == '}' || c == '^' || c == '#' ||
              c == '&')) {
            return 0;
        }
    }

    if (dot_pos == -1) {
        if (name_len > 8) return 0;
    } else {
        int base_len = dot_pos;
        int ext_len = name_len - dot_pos - 1;
        if (base_len == 0 || base_len > 8) return 0;
        if (ext_len == 0 || ext_len > 3) return 0;
    }

    return 1;
}

/*
 * fat32_vnode_init - Initialize a fat32fs_vnode structure
 */
void
fat32_vnode_init(fat32fs_vnode_t *fv, fat32fs_info_t *fsi)
{
    /* Set magic marker */
    fv->fv_magic[0] = 'F';
    fv->fv_magic[1] = 'T';
    fv->fv_magic[2] = '3';
    fv->fv_magic[3] = '2';

    /* Set structure size */
    fv->fv_structsize = sizeof(fat32fs_vnode_t);
    fv->fv_cluster = FAT32_CLUSTER_BAD;
    fv->fv_parent_cluster = FAT32_CLUSTER_BAD;

    /* Set filesystem info pointer */
    fv->fv_fsi = fsi;

    /* Set ownership from mount options */
    fv->fv_uid = fsi->fsi_uid;
    fv->fv_gid = fsi->fsi_gid;

    /* Initialize cluster cache lock and clear cache */
    mutex_init(&fv->fv_cache_lock, MUTEX_DEFAULT, "fat32_cls_cache");
    bzero(fv->fv_cls_cache, sizeof(fv->fv_cls_cache));
}

/*
 * fat32_vnode_new - Allocate and initialize a new fat32fs_vnode
 */
fat32fs_vnode_t *
fat32_vnode_new(fat32fs_info_t *fsi)
{
    fat32fs_vnode_t *fv;

    fv = (fat32fs_vnode_t *)kmem_zalloc(sizeof(fat32fs_vnode_t), KM_SLEEP);
    if (fv) {
        fat32_vnode_init(fv, fsi);
    }

    return fv;
}

/*
 * fat32_vnode_reset - Reset vnode fields without touching lock or fsi
 *
 * Used to reuse an fv structure in search loops without reinitializing the lock.
 * Preserves: fv_magic, fv_structsize, fv_lock, fv_fsi
 */
void
fat32_vnode_reset(fat32fs_vnode_t *fv)
{
    /* Clear all fields except magic, structsize, lock, and fsi */
    fv->fv_vnode = NULL;
    /* fv_bhv is intentionally not cleared - only used after vnode creation */
    /* fv_fsi is preserved */

    fv->fv_cluster = FAT32_CLUSTER_BAD;
    fv->fv_type = VNON;
    fv->fv_mode = 0;
    fv->fv_uid = fv->fv_fsi->fsi_uid;
    fv->fv_gid = fv->fv_fsi->fsi_gid;
    fv->fv_size = 0;

    bzero(&fv->fv_atime, sizeof(timespec_t));
    bzero(&fv->fv_mtime, sizeof(timespec_t));
    bzero(&fv->fv_ctime, sizeof(timespec_t));

    fv->fv_attr = 0;
    fv->fv_diroff = 0;
    fv->fv_dirsz = 0;
    fv->fv_parent_cluster = FAT32_CLUSTER_BAD;
    fv->fv_parent_vp = NULL;

    /* fv_lock is preserved */

    fv->fv_flags = 0;
}

/*
 * fat32_vnode_delete - Mark vnode as deleted and free it
 */
void
fat32_vnode_delete(fat32fs_vnode_t *fv)
{
    if (!fv) {
        return;
    }

    /* Release parent vnode reference if we have one */
    if (fv->fv_parent_vp) {
        FAT32_VN_RELE(fv->fv_parent_vp);
        fv->fv_parent_vp = NULL;
    }

    /* Destroy the mrlock if it was initialized */
    mrfree(&fv->fv_lock);

    /* Destroy the cluster cache mutex */
    mutex_destroy(&fv->fv_cache_lock);

    /* Mark as deleted by clearing structsize */
    fv->fv_structsize = 0;

    /* Free the structure */
    kmem_free(fv, sizeof(fat32fs_vnode_t));
}

/*
 * fat32_vnode_validate - Validate vnode structure
 * Returns 0 on success, panics on validation failure in debug mode
 */
int
fat32_vnode_validate(fat32fs_vnode_t *fv, const char *func, int line)
{
    if (!fv) {
        cmn_err(CE_PANIC, "fat32_vnode_validate: NULL pointer at %s:%d", func, line);
        return EINVAL;
    }

#ifdef FAT32_DBG
    /* Check magic marker */
    if (fv->fv_magic[0] != 'F' || fv->fv_magic[1] != 'T' ||
        fv->fv_magic[2] != '3' || fv->fv_magic[3] != '2') {
        cmn_err(CE_PANIC, "fat32_vnode_validate: bad magic 0x%02x%02x%02x%02x at %s:%d",
                fv->fv_magic[0], fv->fv_magic[1], fv->fv_magic[2], fv->fv_magic[3],
                func, line);
        return EINVAL;
    }

    /* Check if deleted */
    if (fv->fv_structsize == 0) {
        cmn_err(CE_PANIC, "fat32_vnode_validate: use after free (structsize=0) at %s:%d", func, line);
        return EINVAL;
    }

    /* Check structsize */
    if (fv->fv_structsize != sizeof(fat32fs_vnode_t)) {
        cmn_err(CE_PANIC, "fat32_vnode_validate: bad structsize %u (expected %u) at %s:%d",
                fv->fv_structsize, sizeof(fat32fs_vnode_t), func, line);
        return EINVAL;
    }
#endif

    return 0;
}

#ifdef FAT32_DBG
/*
 * fat32_bhv_to_fv_checked - Debug version of BHV_TO_FV with validation
 */
fat32fs_vnode_t *
fat32_bhv_to_fv_checked(bhv_desc_t *bdp, const char *func, int line)
{
    fat32fs_vnode_t *fv;

    fv = (fat32fs_vnode_t *)BHV_PDATA(bdp);

    if (!fv) {
        cmn_err(CE_PANIC, "BHV_TO_FV: NULL pointer at %s:%d", func, line);
        return NULL;
    }

    /* Check magic marker */
    if (fv->fv_magic[0] != 'F' || fv->fv_magic[1] != 'T' ||
        fv->fv_magic[2] != '3' || fv->fv_magic[3] != '2') {
        cmn_err(CE_PANIC, "BHV_TO_FV: bad magic 0x%02x%02x%02x%02x at %s:%d",
                fv->fv_magic[0], fv->fv_magic[1], fv->fv_magic[2], fv->fv_magic[3],
                func, line);
        return NULL;
    }

    /* Check if deleted */
    if (fv->fv_structsize == 0) {
        cmn_err(CE_PANIC, "BHV_TO_FV: use after free (structsize=0) at %s:%d", func, line);
        return NULL;
    }

    /* Check structsize */
    if (fv->fv_structsize != sizeof(fat32fs_vnode_t)) {
        cmn_err(CE_PANIC, "BHV_TO_FV: bad structsize %u (expected %u) at %s:%d",
                fv->fv_structsize, sizeof(fat32fs_vnode_t), func, line);
        return NULL;
    }

    return fv;
}
#endif

/*
 * fat32_checksum - Calculate checksum for short filename
 */
uint8_t
fat32_checksum(const uint8_t *name)
{
    uint8_t sum = 0;
    int i;

    for (i = 0; i < 11; i++) {
        sum = (((sum & 1) << 7) | ((sum & 0xFE) >> 1)) + name[i];
    }
    return sum;
}

/*
 * fat32_generate_short_name - Generate unique 8.3 name from location
 *
 * Uses the strategy: CCCCVVVOOOO
 * C = Cluster hex digit
 * O = Offset/32 hex digit
 * V = C ^ O (Overlap)
 */
void
fat32_generate_short_name(uint32_t cluster, uint32_t offset, char *short_name)
{
    uint32_t slot = offset / 32;
    uint8_t c_nib[7];
    uint8_t o_nib[7];
    uint8_t res_nib[11];
    int i;
    const char hex[] = "0123456789ABCDEF";

    /* Extract nibbles (28 bits each) */
    for (i = 0; i < 7; i++) {
        c_nib[6 - i] = (cluster >> (i * 4)) & 0xF;
        o_nib[6 - i] = (slot >> (i * 4)) & 0xF;
    }

    /* Construct result */
    /* First 4 chars are Cluster high nibbles */
    for (i = 0; i < 4; i++) res_nib[i] = c_nib[i];

    /* Middle 3 chars are XOR overlap */
    for (i = 0; i < 3; i++) res_nib[4 + i] = c_nib[4 + i] ^ o_nib[i];

    /* Last 4 chars are Offset low nibbles */
    for (i = 0; i < 4; i++) res_nib[7 + i] = o_nib[3 + i];

    /* Convert to hex chars */
    for (i = 0; i < 11; i++) {
        short_name[i] = hex[res_nib[i]];
    }
}
