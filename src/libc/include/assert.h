// No include guard: the standard lets a program include assert.h again after changing NDEBUG.
#include <sys/cdefs.h>

#undef assert

#ifdef NDEBUG
#define assert(expression) ((void)0)
#else

__BEGIN_DECLS
__attribute__((__noreturn__)) void __assert_fail(const char* expression, const char* file,
                                                 unsigned int line, const char* function);
__END_DECLS

#define assert(expression)                                                                         \
    ((expression) ? (void)0 : __assert_fail(#expression, __FILE__, __LINE__, __func__))
#endif
