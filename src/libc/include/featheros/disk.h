#ifndef FEATHEROS_LIBC_FEATHEROS_DISK_H
#define FEATHEROS_LIBC_FEATHEROS_DISK_H

#include <sys/cdefs.h>

__BEGIN_DECLS

// Unmounts every mount of the removable disk called disk ("sda", or "/dev/sda"), then flushes
// the drive and stops it, so it can be unplugged. All or nothing for the mounts: EBUSY, with
// nothing unmounted, while a file on any of them is open, a working directory or another mount
// point lies inside one. ENOENT for an unknown disk, ENOTSUP for one that is not removable (the
// boot disk, vda), EIO if a sync or the flush failed (the mounts already unmounted stay so, the
// drive is not stopped). Ejecting an ejected disk returns 0. Afterwards the disk is listed as
// ejected in /proc/partitions until it is unplugged, its I/O fails and a mount of its volumes
// gives ENODEV.
int eject(const char* disk);

__END_DECLS

#endif
