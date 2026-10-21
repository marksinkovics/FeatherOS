#ifndef FEATHEROS_LIBC_SYS_STAT_H
#define FEATHEROS_LIBC_SYS_STAT_H

#include <sys/cdefs.h>
#include <sys/types.h>

#define S_IFMT 0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#define S_ISCHR(mode) (((mode) & S_IFMT) == S_IFCHR)

// There are no users yet: every file is owned by user 0, and the permission bits only say
// whether it can be written (0644 or 0444; 0755 or 0555 for directories). Times are 0 because
// FAT16 timestamps are not read.
struct stat
{
    dev_t st_dev;
    ino_t st_ino;
    mode_t st_mode;
    nlink_t st_nlink;
    uid_t st_uid;
    gid_t st_gid;
    dev_t st_rdev;
    off_t st_size;
    blksize_t st_blksize;
    blkcnt_t st_blocks;
    time_t st_atime;
    time_t st_mtime;
    time_t st_ctime;
};

__BEGIN_DECLS

int stat(const char* path, struct stat* status);
int fstat(int descriptor, struct stat* status);
// The mode is ignored, since there are no permissions yet.
int mkdir(const char* path, mode_t mode);

__END_DECLS

#endif
