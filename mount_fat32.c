/*
 * mount_fat32 - Mount a FAT32 filesystem
 *
 * Simple mount utility for FAT32 filesystems on IRIX
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/fstyp.h>
#include <sys/syssgi.h>
#include <mntent.h>

#define FSTYPE_FAT32 "fat32"

static void
usage(void)
{
    fprintf(stderr, "Usage: mount_fat32 [-r] [-o options] special mountpoint\n");
    fprintf(stderr, "  -r           mount read-only\n");
    fprintf(stderr, "  -o options   mount options (comma-separated)\n");
    fprintf(stderr, "               part=N    - mount partition N (0-3) from MBR\n");
    fprintf(stderr, "               uid=N     - set owner uid for all files\n");
    fprintf(stderr, "               gid=N     - set owner gid for all files\n");
    fprintf(stderr, "               ro        - mount read-only\n");
    fprintf(stderr, "               rw        - mount read-write\n");
    fprintf(stderr, "  special      device to mount (e.g., /dev/dsk/dks0d1s7)\n");
    fprintf(stderr, "  mountpoint   directory to mount on\n");
    exit(1);
}

int
main(int argc, char **argv)
{
    char *special = NULL;
    char *mountpoint = NULL;
    int flags = 0;
    int c;
    int error;
    struct stat st;
    char resolved_path[MAXPATHLEN];
    char mount_data[256];
    char *options = NULL;

    mount_data[0] = '\0';

    /* Parse command line options */
    while ((c = getopt(argc, argv, "ro:")) != -1) {
        switch (c) {
        case 'r':
            flags |= MS_RDONLY;
            break;
        case 'o':
            options = optarg;
            /* Parse mount options */
            if (strstr(optarg, "ro")) {
                flags |= MS_RDONLY;
            }
            if (strstr(optarg, "rw")) {
                flags &= ~MS_RDONLY;
            }
            /* Copy all options to mount_data for kernel */
            strncpy(mount_data, optarg, sizeof(mount_data) - 1);
            mount_data[sizeof(mount_data) - 1] = '\0';
            break;
        default:
            usage();
        }
    }

    /* Get special device and mount point */
    if (optind + 2 != argc) {
        usage();
    }

    special = argv[optind];
    mountpoint = argv[optind + 1];

    /* Resolve mount point to absolute path */
    if (realpath(mountpoint, resolved_path) == NULL) {
        fprintf(stderr, "mount_fat32: realpath failed for %s: %s\n", mountpoint, strerror(errno));
        exit(1);
    }
    mountpoint = resolved_path;

    printf("mount_fat32: mounting %s on %s\n", special, mountpoint);

    /* Check if FAT32 filesystem is registered in the kernel */
    printf("mount_fat32: checking if FAT32 filesystem is registered...\n");
    error = sysfs(GETFSIND, FSTYPE_FAT32);
    if (error < 0) {
        fprintf(stderr, "mount_fat32: FAT32 filesystem not registered in kernel\n");
        fprintf(stderr, "mount_fat32: Make sure fat32.o module is loaded (use 'ml ld')\n");
        fprintf(stderr, "mount_fat32: sysfs(GETFSIND, \"%s\") failed: %s\n",
                FSTYPE_FAT32, strerror(errno));
        exit(1);
    }
    printf("mount_fat32: FAT32 filesystem is registered with index %d\n", error);

    /* Check if mount point exists and is a directory */
    if (stat(mountpoint, &st) < 0) {
        fprintf(stderr, "mount_fat32: %s: %s\n", mountpoint, strerror(errno));
        exit(1);
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "mount_fat32: %s: not a directory\n", mountpoint);
        exit(1);
    }

    /* Perform the mount */
    printf("mount_fat32: calling mount(2) with:\n");
    printf("  special=%s\n", special);
    printf("  dir=%s\n", mountpoint);
    printf("  flags=0x%x %s\n", flags, (flags & MS_RDONLY) ? "(read-only)" : "(read-write)");
    printf("  fstype=%s\n", FSTYPE_FAT32);
    if (mount_data[0]) {
        printf("  options=%s\n", mount_data);
    }

    error = mount(special, mountpoint, flags | MS_FSS | MS_DATA, FSTYPE_FAT32,
                  mount_data, strlen(mount_data));

    if (error < 0) {
        fprintf(stderr, "mount_fat32: mount failed: %s\n", strerror(errno));
        exit(1);
    }

    /* Update /etc/mtab */
    {
        FILE *fp;
        struct mntent mnt;
        char opts[64];

        fp = setmntent(MOUNTED, "a");
        if (fp) {
            /* Lock the file to prevent concurrent updates */
            if (lockf(fileno(fp), F_LOCK, 0) < 0) {
                fprintf(stderr, "mount_fat32: warning: failed to lock %s: %s\n", MOUNTED, strerror(errno));
            }

            mnt.mnt_fsname = special;
            mnt.mnt_dir = mountpoint;
            mnt.mnt_type = FSTYPE_FAT32;
            if (options && options[0]) {
                snprintf(opts, sizeof(opts), "%s,%s",
                         (flags & MS_RDONLY) ? "ro" : "rw", options);
            } else {
                sprintf(opts, "%s", (flags & MS_RDONLY) ? "ro" : "rw");
            }
            mnt.mnt_opts = opts;
            mnt.mnt_freq = 0;
            mnt.mnt_passno = 0;

            addmntent(fp, &mnt);
            endmntent(fp);
        } else {
            fprintf(stderr, "mount_fat32: warning: failed to update %s: %s\n", MOUNTED, strerror(errno));
        }
    }

    printf("mount_fat32: mount successful\n");
    return 0;
}
