#ifndef FEATHEROS_LIBC_SYS_CDEFS_H
#define FEATHEROS_LIBC_SYS_CDEFS_H

// The headers are plain C; C++ programs see the same functions with C linkage.
#ifdef __cplusplus
#define __BEGIN_DECLS                                                                              \
    extern "C"                                                                                     \
    {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif

#endif
