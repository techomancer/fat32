#ifndef _FAT32_H
#define _FAT32_H

#include <sys/types.h>

/*
 * FAT32 cluster value constants
 */
#define FAT32_CLUSTER_FREE      0x00000000
#define FAT32_CLUSTER_MIN       0x00000002
#define FAT32_CLUSTER_MAX       0x0FFFFFEF
#define FAT32_CLUSTER_BAD       0x0FFFFFF7
#define FAT32_CLUSTER_EOC_MIN   0x0FFFFFF8
#define FAT32_CLUSTER_EOC       0x0FFFFFFF
#define FAT32_CLUSTER_MASK      0x0FFFFFFF

/*
 * FAT32 directory entry attributes
 */
#define FAT32_ATTR_READ_ONLY    0x01
#define FAT32_ATTR_HIDDEN       0x02
#define FAT32_ATTR_SYSTEM       0x04
#define FAT32_ATTR_VOLUME_ID    0x08
#define FAT32_ATTR_DIRECTORY    0x10
#define FAT32_ATTR_ARCHIVE      0x20
#define FAT32_ATTR_LONG_NAME    0x0F
#define FAT32_ATTR_LONG_NAME_MASK (FAT32_ATTR_READ_ONLY | FAT32_ATTR_HIDDEN | \
                                   FAT32_ATTR_SYSTEM | FAT32_ATTR_VOLUME_ID)

/*
 * LFN constants
 */
#define FAT32_LFN_LAST          0x40
#define FAT32_LFN_DELETED       0xE5
#define FAT32_LFN_SEQ_MASK      0x1F
#define FAT32_LFN_FILL          0xFFFF  /* UCS-2 padding char after the NUL terminator */
#define FAT32_MAX_NAME          255     /* Maximum filename length for LFN */

/*
 * Little-endian accessor macros for 16-bit values
 * Note: IRIX MIPS is big-endian, FAT32 is little-endian
 * RLE16 = Read Little-Endian 16-bit
 * WLE16 = Write Little-Endian 16-bit
 */
#define RLE16(bytes) \
    ((uint16_t)(((uint8_t *)(bytes))[0]) | \
     ((uint16_t)(((uint8_t *)(bytes))[1]) << 8))

#define WLE16(val, bytes) \
    do { \
        ((uint8_t *)(bytes))[0] = (uint8_t)((val) & 0xFF); \
        ((uint8_t *)(bytes))[1] = (uint8_t)(((val) >> 8) & 0xFF); \
    } while (0)

/*
 * Little-endian accessor macros for 32-bit values
 * RLE32 = Read Little-Endian 32-bit
 * WLE32 = Write Little-Endian 32-bit
 */
#define RLE32(bytes) \
    ((uint32_t)(((uint8_t *)(bytes))[0]) | \
     ((uint32_t)(((uint8_t *)(bytes))[1]) << 8) | \
     ((uint32_t)(((uint8_t *)(bytes))[2]) << 16) | \
     ((uint32_t)(((uint8_t *)(bytes))[3]) << 24))

#define WLE32(val, bytes) \
    do { \
        ((uint8_t *)(bytes))[0] = (uint8_t)((val) & 0xFF); \
        ((uint8_t *)(bytes))[1] = (uint8_t)(((val) >> 8) & 0xFF); \
        ((uint8_t *)(bytes))[2] = (uint8_t)(((val) >> 16) & 0xFF); \
        ((uint8_t *)(bytes))[3] = (uint8_t)(((val) >> 24) & 0xFF); \
    } while (0)

/*
 * DOS Master Boot Record (MBR) - 512 bytes
 * Contains partition table at offset 446
 */
typedef struct fat32_mbr_partition {
    uint8_t  boot_indicator;        /* 0x80 = bootable, 0x00 = not bootable */
    uint8_t  start_head;
    uint8_t  start_sector;          /* bits 0-5: sector, bits 6-7: high bits of cylinder */
    uint8_t  start_cylinder;        /* bits 0-7: low 8 bits of cylinder */
    uint8_t  system_id;             /* 0x0B = FAT32, 0x0C = FAT32 LBA */
    uint8_t  end_head;
    uint8_t  end_sector;
    uint8_t  end_cylinder;
    uint8_t  relative_sector[4];    /* LBA of first sector in partition (LE32) */
    uint8_t  total_sectors[4];      /* Number of sectors in partition (LE32) */
} fat32_mbr_partition_t;

typedef struct fat32_mbr {
    uint8_t  boot_code[446];
    fat32_mbr_partition_t partition[4];
    uint8_t  signature[2];          /* 0x55, 0xAA */
} fat32_mbr_t;

/*
 * FAT32 Boot Record / BIOS Parameter Block (BPB) - 512 bytes
 * Located at the first sector of the FAT32 partition
 */
typedef struct fat32_br {
    uint8_t  jmp_boot[3];           /* Jump instruction to boot code */
    uint8_t  oem_name[8];           /* OEM name string */
    uint8_t  bytes_per_sector[2];   /* Bytes per sector (LE16) - usually 512 */
    uint8_t  sectors_per_cluster;   /* Sectors per cluster - must be power of 2 */
    uint8_t  reserved_sectors[2];   /* Reserved sectors (LE16) - usually 32 for FAT32 */
    uint8_t  num_fats;              /* Number of FATs - usually 2 */
    uint8_t  root_entries[2];       /* Root dir entries (LE16) - 0 for FAT32 */
    uint8_t  total_sectors_16[2];   /* Total sectors (LE16) - 0 for FAT32 */
    uint8_t  media_type;            /* Media descriptor - 0xF8 for hard disk */
    uint8_t  fat_size_16[2];        /* Sectors per FAT (LE16) - 0 for FAT32 */
    uint8_t  sectors_per_track[2];  /* Sectors per track (LE16) */
    uint8_t  num_heads[2];          /* Number of heads (LE16) */
    uint8_t  hidden_sectors[4];     /* Hidden sectors (LE32) */
    uint8_t  total_sectors_32[4];   /* Total sectors (LE32) - used for FAT32 */

    /* FAT32-specific fields (offset 36) */
    uint8_t  fat_size_32[4];        /* Sectors per FAT (LE32) */
    uint8_t  ext_flags[2];          /* Extended flags (LE16) */
    uint8_t  fs_version[2];         /* Filesystem version (LE16) - 0:0 */
    uint8_t  root_cluster[4];       /* Root directory cluster (LE32) - usually 2 */
    uint8_t  fs_info[2];            /* FSInfo sector number (LE16) - usually 1 */
    uint8_t  backup_boot_sector[2]; /* Backup boot sector (LE16) - usually 6 */
    uint8_t  reserved[12];          /* Reserved for future use */
    uint8_t  drive_number;          /* Drive number for INT 13h */
    uint8_t  reserved1;             /* Reserved (used by Windows NT) */
    uint8_t  boot_signature;        /* Extended boot signature - 0x29 */
    uint8_t  volume_id[4];          /* Volume serial number (LE32) */
    uint8_t  volume_label[11];      /* Volume label */
    uint8_t  fs_type[8];            /* Filesystem type - "FAT32   " */
    uint8_t  boot_code[420];        /* Boot code */
    uint8_t  signature[2];          /* 0x55, 0xAA */
} fat32_br_t;

/*
 * FAT32 FSInfo structure - 512 bytes
 * Provides information about free cluster count and next free cluster
 */
typedef struct fat32_fsinfo {
    uint8_t  lead_signature[4];     /* Lead signature - 0x41615252 (LE32) */
    uint8_t  reserved1[480];        /* Reserved */
    uint8_t  struct_signature[4];   /* Structure signature - 0x61417272 (LE32) */
    uint8_t  free_count[4];         /* Free cluster count (LE32) - 0xFFFFFFFF if unknown */
    uint8_t  next_free[4];          /* Next free cluster hint (LE32) - 0xFFFFFFFF if unknown */
    uint8_t  reserved2[12];         /* Reserved */
    uint8_t  trail_signature[4];    /* Trail signature - 0xAA550000 (LE32) */
} fat32_fsinfo_t;

/*
 * FAT32 8.3 Directory Entry - 32 bytes
 * Classic short filename directory entry
 */
typedef struct fat32_dirent83 {
    uint8_t  name[11];              /* 8.3 filename (space-padded) */
    uint8_t  attr;                  /* File attributes */
    uint8_t  nt_reserved;           /* Reserved for Windows NT */
    uint8_t  create_time_tenth;     /* File creation time in tenths of second */
    uint8_t  create_time[2];        /* File creation time (LE16) */
    uint8_t  create_date[2];        /* File creation date (LE16) */
    uint8_t  last_access_date[2];   /* Last access date (LE16) */
    uint8_t  first_cluster_hi[2];   /* High word of first cluster (LE16) */
    uint8_t  write_time[2];         /* Last write time (LE16) */
    uint8_t  write_date[2];         /* Last write date (LE16) */
    uint8_t  first_cluster_lo[2];   /* Low word of first cluster (LE16) */
    uint8_t  file_size[4];          /* File size in bytes (LE32) */
} fat32_dirent83_t;

/*
 * FAT32 Long File Name (LFN) Directory Entry - 32 bytes
 * Used to store long filenames across multiple entries
 */
typedef struct fat32_dirent_lfn {
    uint8_t  sequence;              /* Sequence number (OR'd with 0x40 for last entry) */
    uint8_t  name1[10];             /* First 5 characters (UCS-2, LE16 per char) */
    uint8_t  attr;                  /* Attributes - always 0x0F for LFN */
    uint8_t  type;                  /* Type - always 0 for LFN */
    uint8_t  checksum;              /* Checksum of 8.3 name */
    uint8_t  name2[12];             /* Next 6 characters (UCS-2, LE16 per char) */
    uint8_t  first_cluster[2];      /* Always 0 for LFN (LE16) */
    uint8_t  name3[4];              /* Last 2 characters (UCS-2, LE16 per char) */
} fat32_dirent_lfn_t;


#define FAT32_DENTRY_SIZE       32
/*
 * FSInfo signature constants
 */
#define FAT32_FSINFO_LEAD_SIG   0x41615252
#define FAT32_FSINFO_STRUCT_SIG 0x61417272
#define FAT32_FSINFO_TRAIL_SIG  0xAA550000

/*
 * Boot sector signatures
 */
#define FAT32_BOOT_SIGNATURE    0x29
#define FAT32_SECTOR_SIGNATURE  0xAA55

#endif /* _FAT32_H */
