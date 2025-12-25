#!smake
#
# Makefile for IRIX FAT32
#
# This builds a loadable kernel module for FAT32 fs support on IRIX 6.5
#
# Uses smake (SGI's parallel make) and follows IRIX loadable module conventions
#

# Target CPU board - change this based on your system
# Supportted values IP32 (O2) IP30 (Octane) IP35 (Fuel, Tezro, Origin 350)
CPUBOARD=IP30
BUILTIN=0
# Include the IRIX kernel loadable I/O module makefile
# This provides $(CC), $(LD), $(CFLAGS), $(LDFLAGS), $(ML), etc.

#if $(BUILTIN) == "1"
include /var/sysgen/Makefile.kernio
#else
include /var/sysgen/Makefile.kernloadio
#endif

COMMON_FLAGS=
COMMON_LDFLAGS=-v
COMMON_CFLAGS=$(BUILTIN_CFLAGS)

LDFLAGS_IP35=-nostdlib -64 -mips4
LDFLAGS_IP30=-nostdlib -64 -mips4
LDFLAGS_IP32=-nostdlib -n32 -mips3
MYCFLAGS_IP35=-mips4 -DPTE_64BIT
MYCFLAGS_IP30=-mips4 -DPTE_64BIT -DHEART_INVALIDATE_WAR
MYCFLAGS_IP32=-mips3

#if $(CPUBOARD) == "IP30"
MYCFLAGS=$(MYCFLAGS_IP30) $(COMMON_FLAGS) $(COMMON_CFLAGS)
LDFLAGS=$(LDFLAGS_IP30) $(COMMON_FLAGS) $(COMMON_LDFLAGS)
#elif $(CPUBOARD) == "IP32"
MYCFLAGS=$(MYCFLAGS_IP32) $(COMMON_FLAGS) $(COMMON_CFLAGS)
LDFLAGS=$(LDFLAGS_IP32) $(COMMON_FLAGS) $(COMMON_LDFLAGS)
#elif $(CPUBOARD) == "IP35"
MYCFLAGS=$(MYCFLAGS_IP35) $(COMMON_FLAGS) $(COMMON_CFLAGS)
LDFLAGS=$(LDFLAGS_IP35) $(COMMON_FLAGS) $(COMMON_LDFLAGS)
#else
#endif
# Define module loader tool
ML=ml

# Source files
SRCS = fat32fs.c fat32_vfs.c fat32_vnode.c fat32_dir.c fat32_file.c fat32_util.c fat32_cls.c
OBJS = $(SRCS:.c=.o)

# Header dependencies
HDRS = fat32fs.h fat32.h

# Module name
MODULE = fat32.o

# User-space mount utility
MOUNT_PROG = mount_fat32

# Default target
all: $(MODULE) $(MOUNT_PROG)

# Link all object files into a single loadable module
# Use ld with -r flag to create relocatable object (kernel module)
$(MODULE): $(OBJS)
	$(LD) $(LDFLAGS) -r $(OBJS) -o $(MODULE)

# Compile C files to object files
# Makefile.kernloadio should provide proper CFLAGS for loadable modules
.c.o:
	$(CC) $(CFLAGS) $(MYCFLAGS) -c $<

# Dependencies
fat32fs.o: fat32fs.c $(HDRS)
fat32_vfs.o: fat32_vfs.c $(HDRS)
fat32_vnode.o: fat32_vnode.c $(HDRS)

# Build user-space mount utility
$(MOUNT_PROG): mount_fat32.c
	$(CC) -o $(MOUNT_PROG) mount_fat32.c

# Load the driver into the running kernel using ml (module loader)
# Register as character device to allow ml loading (even though we don't use it)
# -c = character device
# -p fat32_ = driver prefix
# -s 13 = major device number (arbitrary, change if conflicts)
# -v = verbose output
load: $(MODULE)
	@echo "Loading FAT32 fs module..."
	$(ML) ld -v -j $(MODULE) -p fat32

# Unload the driver from the kernel
# ml unld = unload module
# -v = verbose
# -p fat32_ = driver prefix to unload
unload:
	$(ML) unld -v -p fat32_

# Reload the driver (unload then load)
reload: unload load

# List loaded modules
list:
	$(ML) list

# Clean build artifacts
clean:
	rm -f $(OBJS) $(MODULE) $(MOUNT_PROG)

# Install target (for permanent installation)
install: $(MODULE) $(MOUNT_PROG)
	@echo "Installing FAT32 filesystem driver..."
	cp fat32 /var/sysgen/master.d/
	cp fat32.o /var/sysgen/boot/
	cp fat32.sm /var/sysgen/system/
	cp $(MOUNT_PROG) /sbin/
	@echo ""
	@echo "Installation complete. To enable FAT32 support:"
	@echo "  1. Run 'autoconfig' to rebuild the kernel"
	@echo "  2. Reboot the system"

reboot:
	shutdown -y -g0 -i6

mount: $(MOUNT_PROG)
	mkdir -p /fat
	./mount_fat32 /dev/dsk/dks1d4vol /fat

# Help target
help:
	@echo "IRIX FAT32 fs Makefile"
	@echo ""
	@echo "Build using smake (SGI's parallel make)"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build fat32.o loadable module (default)"
	@echo "  load     - Load the driver into the running kernel"
	@echo "  unload   - Unload the driver from the kernel"
	@echo "  reload   - Unload then reload the driver"
	@echo "  list     - List loaded modules"
	@echo "  install  - Install systemwide for building in into kernel with autoconfig"
	@echo "  clean    - Remove build artifacts"
	@echo "  help     - Show this help message"
	@echo ""
	@echo "Variables:"
	@echo "  CPUBOARD - Target CPU board (currently: $(CPUBOARD))"
	@echo ""
	@echo "Usage:"
	@echo "  smake            # Build module"
	@echo "  smake load       # Load into kernel"
	@echo "  smake unload     # Unload from kernel"
	@echo "  smake reload     # Reload (unload + load)"
	@echo "  smake list       # Show loaded modules"
	@echo ""
	@echo "Module Loader Commands:"
	@echo "  ml ld -p fat32_ fat32.o   # Load module"
	@echo "  ml unld -p fat32         # Unload by prefix"
	@echo "  ml list                   # List modules"

.PHONY: all load unload reload list clean install help reboot mount
