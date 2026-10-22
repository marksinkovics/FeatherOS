#ifndef FEATHEROS_LIBC_SPAWN_H
#define FEATHEROS_LIBC_SPAWN_H

#include <sys/cdefs.h>
#include <sys/types.h>

// glibc's values. POSIX_SPAWN_SETSID makes the child a session leader, which keeps the console
// and Ctrl+C from reaching past it. POSIX_SPAWN_SETPGROUP, with process group 0, starts it in the
// background: it does not take the console from its parent. No other flags are supported.
#define POSIX_SPAWN_SETPGROUP 0x02
#define POSIX_SPAWN_SETSID 0x80

// File actions exist so portable code compiles, but none can be added yet.
typedef struct
{
    int unused;
} posix_spawn_file_actions_t;

typedef struct
{
    short flags;
    pid_t process_group;
} posix_spawnattr_t;

__BEGIN_DECLS

// path names a program in /system, as "/system/ls" or "ls". Unless the attributes say otherwise,
// the child takes the console until it exits. argv[0] is replaced by the program's name.
// Returns 0 or an errno value.
int posix_spawn(pid_t* pid, const char* path, const posix_spawn_file_actions_t* file_actions,
                const posix_spawnattr_t* attributes, char* const argv[], char* const envp[]);
// Searches PATH, which only ever finds programs in /system.
int posix_spawnp(pid_t* pid, const char* file, const posix_spawn_file_actions_t* file_actions,
                 const posix_spawnattr_t* attributes, char* const argv[], char* const envp[]);
int posix_spawn_file_actions_init(posix_spawn_file_actions_t* file_actions);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t* file_actions);
int posix_spawnattr_init(posix_spawnattr_t* attributes);
int posix_spawnattr_destroy(posix_spawnattr_t* attributes);
int posix_spawnattr_getflags(const posix_spawnattr_t* attributes, short* flags);
int posix_spawnattr_setflags(posix_spawnattr_t* attributes, short flags);
int posix_spawnattr_getpgroup(const posix_spawnattr_t* attributes, pid_t* process_group);
// Only 0, a new group, is supported.
int posix_spawnattr_setpgroup(posix_spawnattr_t* attributes, pid_t process_group);

__END_DECLS

#endif
