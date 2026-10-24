#ifndef FEATHEROS_LIBC_SPAWN_H
#define FEATHEROS_LIBC_SPAWN_H

#include <sys/cdefs.h>
#include <sys/types.h>

// glibc's values. POSIX_SPAWN_SETSID makes the child a session leader, which keeps the console
// and Ctrl+C from reaching past it. POSIX_SPAWN_SETPGROUP puts it in the attributes' process
// group: 0 for a new one it leads, or an existing group that is the caller's or holds one of its
// children. No other POSIX flags are supported.
#define POSIX_SPAWN_SETPGROUP 0x02
#define POSIX_SPAWN_SETSID 0x80
// FeatherOS: the child's group does not take the console. With POSIX_SPAWN_SETPGROUP for a new
// group, that makes a background job, whose console reads fail with EIO; in the caller's own
// group it changes nothing. Without it, the child's group becomes the foreground group, as if
// the caller had called tcsetpgrp.
#define POSIX_SPAWN_BACKGROUND 0x1000

#define POSIX_SPAWN_MAXIMUM_FILE_ACTIONS 8

// File actions run in order before the child starts, on descriptors it then inherits: the C
// library applies them to the caller's own descriptors around the Spawn syscall and puts those
// back afterwards, which is safe while programs have one thread.
struct __posix_spawn_file_action
{
    int kind;
    int descriptor;
    int source;
    int flags;
    char path[64];
};

typedef struct
{
    int count;
    struct __posix_spawn_file_action actions[POSIX_SPAWN_MAXIMUM_FILE_ACTIONS];
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
// Each fails with ENOMEM past POSIX_SPAWN_MAXIMUM_FILE_ACTIONS, and addopen with ENAMETOOLONG
// for a path over 63 characters.
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t* file_actions, int descriptor,
                                     int target);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t* file_actions, int descriptor,
                                     const char* path, int flags, mode_t mode);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t* file_actions, int descriptor);
int posix_spawnattr_init(posix_spawnattr_t* attributes);
int posix_spawnattr_destroy(posix_spawnattr_t* attributes);
int posix_spawnattr_getflags(const posix_spawnattr_t* attributes, short* flags);
int posix_spawnattr_setflags(posix_spawnattr_t* attributes, short flags);
int posix_spawnattr_getpgroup(const posix_spawnattr_t* attributes, pid_t* process_group);
int posix_spawnattr_setpgroup(posix_spawnattr_t* attributes, pid_t process_group);

__END_DECLS

#endif
