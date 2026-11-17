#ifndef FEATHEROS_LIBC_SYS_MOUNT_H
#define FEATHEROS_LIBC_SYS_MOUNT_H

#include <sys/cdefs.h>

__BEGIN_DECLS

#define MS_RDONLY 1

// Linux's signature, but mountflags may only be 0 or MS_RDONLY, and data must be null.
// filesystemtype and source name one of the kernel's filesystem instances, as /proc/mounts lists
// them ("tmpfs" and "tmp"); target need not exist, but its parent must. ENODEV for an unknown
// type, ENOENT for an unknown source, EBUSY if target is a mount point or the instance is mounted
// elsewhere. A filesystem that refuses writes mounts read-only even without MS_RDONLY, as
// /proc/mounts then shows.
int mount(const char* source, const char* target, const char* filesystemtype,
          unsigned long mountflags, const void* data);
// EINVAL if target is not a mount point, EBUSY while a file on it is open, a working directory or
// another mount point lies inside it. The filesystem's data is flushed first.
int umount(const char* target);

__END_DECLS

#endif
