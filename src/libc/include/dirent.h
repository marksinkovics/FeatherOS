#ifndef FEATHEROS_LIBC_DIRENT_H
#define FEATHEROS_LIBC_DIRENT_H

#include <limits.h>
#include <sys/cdefs.h>
#include <sys/types.h>

// d_type, with Linux's values. An ext2 device node, FIFO or socket is DT_UNKNOWN, since the
// filesystem core does not say which it is.
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12

// Listings do not include "." and "..". d_ino is the st_ino that lstat would report, or 0 when the
// filesystem cannot tell.
struct dirent
{
    ino_t d_ino;
    unsigned char d_type;
    char d_name[NAME_MAX + 1];
};

__BEGIN_DECLS

typedef struct FeatherDirectory DIR;

DIR* opendir(const char* path);
struct dirent* readdir(DIR* directory);
void rewinddir(DIR* directory);
int closedir(DIR* directory);

__END_DECLS

#endif
