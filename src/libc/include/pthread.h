#ifndef FEATHEROS_LIBC_PTHREAD_H
#define FEATHEROS_LIBC_PTHREAD_H

#include <sys/cdefs.h>

typedef unsigned long pthread_t;

// Attributes are accepted but have no settings: every thread gets a kernel-managed stack of
// up to 63 pages and starts joinable.
typedef struct
{
    int unused;
} pthread_attr_t;

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
int pthread_attr_init(pthread_attr_t* attributes);
int pthread_attr_destroy(pthread_attr_t* attributes);

__END_DECLS

#endif
