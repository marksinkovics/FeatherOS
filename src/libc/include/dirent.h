#ifndef FEATHEROS_LIBC_DIRENT_H
#define FEATHEROS_LIBC_DIRENT_H

#include <sys/cdefs.h>
#include <sys/types.h>

#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8

// Listings do not include "." and "..". d_ino numbers entries from 1 within their directory.
struct dirent
{
    ino_t d_ino;
    unsigned char d_type;
    char d_name[64];
};

__BEGIN_DECLS

typedef struct FeatherDirectory DIR;

DIR* opendir(const char* path);
struct dirent* readdir(DIR* directory);
void rewinddir(DIR* directory);
int closedir(DIR* directory);

__END_DECLS

#endif
