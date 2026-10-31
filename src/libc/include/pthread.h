#ifndef FEATHEROS_LIBC_PTHREAD_H
#define FEATHEROS_LIBC_PTHREAD_H

#include <sys/cdefs.h>

#include <time.h>

typedef unsigned long pthread_t;

// The only setting is the detach state: every thread gets a kernel-managed stack of up to 63
// pages.
typedef struct
{
    int detach_state;
} pthread_attr_t;

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

// Every type sleeps in the kernel while it waits. ERRORCHECK refuses a second lock by the owner
// (EDEADLK) and an unlock by another thread (EPERM); RECURSIVE counts the owner's locks.
#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_ERRORCHECK 1
#define PTHREAD_MUTEX_RECURSIVE 2
#define PTHREAD_MUTEX_DEFAULT PTHREAD_MUTEX_NORMAL

typedef struct
{
    // 0 unlocked, 1 locked, 2 locked with threads that may be waiting.
    int state;
    int type;
    pthread_t owner;
    unsigned count;
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER {0, PTHREAD_MUTEX_DEFAULT, 0, 0}

typedef struct
{
    int type;
} pthread_mutexattr_t;

// Waiters sleep until the sequence number moves on; wakeups may be spurious, as POSIX allows.
typedef struct
{
    unsigned sequence;
    // The clock pthread_cond_timedwait's deadline is on.
    clockid_t clock;
} pthread_cond_t;

#define PTHREAD_COND_INITIALIZER {0, CLOCK_REALTIME}

typedef struct
{
    clockid_t clock;
} pthread_condattr_t;

typedef int pthread_once_t;

#define PTHREAD_ONCE_INIT 0

__BEGIN_DECLS

// A process has at most 16 threads, the main one included; past that pthread_create returns
// EAGAIN. The functions return an error number instead of setting errno.
int pthread_create(pthread_t* thread, const pthread_attr_t* attributes,
                   void* (*start)(void* argument), void* argument);
int pthread_join(pthread_t thread, void** value);
// In the process's last thread it exits as exit(0) does, after atexit handlers and stdio.
__attribute__((noreturn)) void pthread_exit(void* value);
pthread_t pthread_self(void);
int pthread_equal(pthread_t first, pthread_t second);
// A detached thread's resources go back once it ends; it can no longer be joined.
int pthread_detach(pthread_t thread);
int pthread_attr_init(pthread_attr_t* attributes);
int pthread_attr_destroy(pthread_attr_t* attributes);
int pthread_attr_setdetachstate(pthread_attr_t* attributes, int state);
int pthread_attr_getdetachstate(const pthread_attr_t* attributes, int* state);

int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attributes);
int pthread_mutex_destroy(pthread_mutex_t* mutex);
int pthread_mutex_lock(pthread_mutex_t* mutex);
int pthread_mutex_trylock(pthread_mutex_t* mutex);
int pthread_mutex_unlock(pthread_mutex_t* mutex);
int pthread_mutexattr_init(pthread_mutexattr_t* attributes);
int pthread_mutexattr_destroy(pthread_mutexattr_t* attributes);
int pthread_mutexattr_settype(pthread_mutexattr_t* attributes, int type);
int pthread_mutexattr_gettype(const pthread_mutexattr_t* attributes, int* type);

int pthread_cond_init(pthread_cond_t* condition, const pthread_condattr_t* attributes);
int pthread_cond_destroy(pthread_cond_t* condition);
int pthread_cond_wait(pthread_cond_t* condition, pthread_mutex_t* mutex);
// deadline is on the condition's clock: CLOCK_REALTIME unless pthread_condattr_setclock chose
// CLOCK_MONOTONIC.
int pthread_cond_timedwait(pthread_cond_t* condition, pthread_mutex_t* mutex,
                           const struct timespec* deadline);
int pthread_cond_signal(pthread_cond_t* condition);
int pthread_cond_broadcast(pthread_cond_t* condition);
int pthread_condattr_init(pthread_condattr_t* attributes);
int pthread_condattr_destroy(pthread_condattr_t* attributes);
int pthread_condattr_setclock(pthread_condattr_t* attributes, clockid_t clock);
int pthread_condattr_getclock(const pthread_condattr_t* attributes, clockid_t* clock);

int pthread_once(pthread_once_t* once, void (*routine)(void));

__END_DECLS

#endif
