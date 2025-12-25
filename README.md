# IRIX FAT32 Filesystem Driver

A loadable kernel module providing FAT32 filesystem support for SGI IRIX 6.5.

## Overview

This driver implements a fully functional FAT32 filesystem handler for IRIX 6.5 systems. It supports reading and writing FAT32-formatted media, enabling IRIX systems to access modern storage devices formatted with FAT32.

### Features

- Full read/write support for FAT32 filesystems
- Loadable kernel module (no kernel rebuild required for testing)
- Permanent installation option for production use
- Support for multiple IRIX platforms: IP30 (Octane), IP32 (O2), IP35 (Fuel/Tezro/Origin 350)
- Big-endian MIPS CPU compatible
- Cluster-based file/directory access optimized for IRIX block layer cache

### Technical Details

- **Compiler**: MIPSPro C 7.4.4
- **Language Standard**: C99
- **Architecture**: Big-endian MIPS
- **Inode Strategy**: Uses FAT32 cluster directory number/starting entry index/size for ino_t (root = 1)
- **Endianness Handling**: RLE16/WLE16/RLE32/WLE32 macros for little-endian FAT data

## Prerequisites

- IRIX 6.5.x system
- MIPSPro C compiler 7.4.4
- Root access for installation and loading
- Development headers installed (`/var/sysgen/` directory)

## Building

### Configure Build Options

Edit the [Makefile](Makefile) and set the `CPUBOARD` variable to match your system:

```make
CPUBOARD=IP30    # For Octane
CPUBOARD=IP32    # For O2
CPUBOARD=IP35    # For Fuel, Tezro, Origin 350
```

For loadable module mode (default):
```make
BUILTIN=0
```

For permanent kernel built-in mode:
```make
BUILTIN=1
```

### Compile the Module

```sh
smake
```

This builds:
- `fat32.o` - The loadable kernel module
- `mount_fat32` - User-space mount utility

### Clean Build Artifacts

```sh
smake clean
```

## Installation

### Option 1: Loadable Module (Recommended for Testing)

Load the module into the running kernel without rebooting:

```sh
# Build the module
smake

# Load into kernel
smake load

# Verify it's loaded
smake list
```

To unload the module:

```sh
smake unload
```

To reload (useful during development):

```sh
smake reload
```

### Option 2: Permanent Installation

Install the module as a permanent part of the kernel.

First, set `BUILTIN=1` in the [Makefile](Makefile):

```make
BUILTIN=1
```

Then build and install:

```sh
# Build the module for built-in installation
smake

# Install files to system directories
smake install
```

This copies:
- `fat32` → `/var/sysgen/master.d/` (master configuration)
- `fat32.o` → `/var/sysgen/boot/` (kernel module)
- `fat32.sm` → `/var/sysgen/system/` (system configuration)
- `mount_fat32` → `/sbin/` (mount utility)

After installation:

```sh
# Rebuild the kernel
autoconfig

# Reboot to load the new kernel
shutdown -y -g0 -i6
```

Or use the provided shortcut:

```sh
smake reboot
```

## Usage

### Mounting a FAT32 Filesystem

The driver can mount either:
- A specific FAT32 partition (e.g., `/dev/dsk/dks1d4s7`)
- A whole volume, which will mount the first partition (e.g., `/dev/dsk/dks1d4vol`)

Using the mount utility:

```sh
# Create mount point
mkdir -p /mnt/fat32

# Mount a specific FAT32 partition
/sbin/mount_fat32 /dev/dsk/dks1d4s7 /mnt/fat32

# Or mount the first partition on a whole volume
/sbin/mount_fat32 /dev/dsk/dks1d4vol /mnt/fat32

# Mount with specific partition number (0-3)
/sbin/mount_fat32 -o part=1 /dev/dsk/dks1d4vol /mnt/fat32

# Mount with custom ownership (all files owned by uid 1000, gid 100)
/sbin/mount_fat32 -o uid=1000,gid=100 /dev/dsk/dks1d4vol /mnt/fat32

# Combine options
/sbin/mount_fat32 -o part=2,uid=1000,gid=100,ro /dev/dsk/dks1d4vol /mnt/fat32
```

### Mount Options

The following options can be passed via `-o`:

| Option | Description |
|--------|-------------|
| `part=N` | Mount partition N (0-3) from MBR. If not specified, auto-detects first FAT32 partition. |
| `uid=N` | Set owner uid for all files (default: 0/root) |
| `gid=N` | Set owner gid for all files (default: 0/root) |
| `ro` | Mount read-only |
| `rw` | Mount read-write (default) |

Using the Makefile convenience target (mounts to `/fat`):

```sh
smake mount
```

### Working with DOS-Partitioned Drives

IRIX does not natively understand DOS partition tables. To access specific FAT32 partitions on a DOS-partitioned drive, you can use XLV (IRIX Logical Volume Manager) to define volumes that match the FAT partition boundaries:

```sh
# Use xlv_make to create a logical volume matching the FAT partition
# Example: Create a volume for a partition starting at block 2048, length 20971520 blocks
xlv_make -d partition_name /dev/dsk/dks1d4vol 2048 20971520
```

This allows you to create device nodes (e.g., `/dev/dsk/partition_name`) that align with individual FAT32 partitions on drives with DOS partition tables.

### Unmounting

```sh
umount /mnt/fat32
```

## Development

### Testing on Live System

You can test the driver on a live IRIX 6.5.22 system:

```
Host: octopus
User: root
Password: p00pp00p
Source Tree: /root/fat32
```

Connect via telnet:

```sh
telnet octopus
```

### Source Structure

- `fat32fs.c` - Main filesystem driver initialization
- `fat32_vfs.c` - VFS operations (mount, unmount, statvfs)
- `fat32_vnode.c` - Vnode operations (getattr, setattr, access, etc.)
- `fat32_dir.c` - Directory operations (lookup, readdir, create, remove)
- `fat32_file.c` - File I/O operations (read, write)
- `fat32_util.c` - Utility functions (cluster chains, FAT access)
- `fat32_cls.c` - Cluster allocation and management
- `fat32fs.h` - Main header with IRIX kernel interface
- `fat32.h` - FAT32 data structures and constants
- `mount_fat32.c` - User-space mount utility

### Important Implementation Notes

- Files and directories are accessed in cluster chunks
- Boot sectors and FAT area are accessed in single cluster chunks
- This design is critical because IRIX block layer cache doesn't handle overlapping chunks
- Little-endian data from FAT is handled using RLE16/WLE16/RLE32/WLE32 macros

## Makefile Targets

| Target | Description |
|--------|-------------|
| `all` | Build the loadable module and mount utility (default) |
| `load` | Load the driver into the running kernel |
| `unload` | Unload the driver from the kernel |
| `reload` | Unload then reload the driver |
| `list` | List currently loaded modules |
| `clean` | Remove build artifacts |
| `install` | Install for permanent kernel integration |
| `mount` | Quick mount test volume to `/fat` |
| `reboot` | Reboot the system |
| `help` | Display Makefile help |

## License

This is an IRIX kernel module developed for SGI IRIX 6.5 systems.
