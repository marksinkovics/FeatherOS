#ifndef FEATHEROS_LIBC_SPAWN_H
#define FEATHEROS_LIBC_SPAWN_H

#include <sys/cdefs.h>
#include <sys/types.h>

// File actions and attributes exist so portable code compiles, but none can be added yet, so
// they never change how a child starts.
typedef struct
{
    int unused;
} posix_spawn_file_actions_t;

typedef struct
{
    int unused;
} posix_spawnattr_t;

__BEGIN_DECLS

// path names a program in /system, as "/system/ls" or "ls". The child takes the console until
// it exits, and argv[0] is replaced by the program's name. Returns 0 or an errno value.
int posix_spawn(pid_t* pid, const char* path, const posix_spawn_file_actions_t* file_actions,
                const posix_spawnattr_t* attributes, char* const argv[], char* const envp[]);
// Searches PATH, which only ever finds programs in /system.
int posix_spawnp(pid_t* pid, const char* file, const posix_spawn_file_actions_t* file_actions,
                 const posix_spawnattr_t* attributes, char* const argv[], char* const envp[]);
int posix_spawn_file_actions_init(posix_spawn_file_actions_t* file_actions);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t* file_actions);
int posix_spawnattr_init(posix_spawnattr_t* attributes);
int posix_spawnattr_destroy(posix_spawnattr_t* attributes);

__END_DECLS

#endif
