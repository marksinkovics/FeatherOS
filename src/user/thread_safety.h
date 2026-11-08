#ifndef FEATHEROS_USER_THREAD_SAFETY_H
#define FEATHEROS_USER_THREAD_SAFETY_H

// Annotations for Clang's thread-safety analysis (-Wthread-safety), which clang-tidy runs: a lock
// type is a capability, data names the lock that guards it, and functions name the locks they
// need, take or give back. GCC builds the code and ignores them.
#if defined(__clang__)
#define THREAD_ANNOTATION(x) __attribute__((x))
#else
#define THREAD_ANNOTATION(x)
#endif

#define CAPABILITY(name) THREAD_ANNOTATION(capability(name))
#define SCOPED_CAPABILITY THREAD_ANNOTATION(scoped_lockable)
#define GUARDED_BY(lock) THREAD_ANNOTATION(guarded_by(lock))
#define PT_GUARDED_BY(lock) THREAD_ANNOTATION(pt_guarded_by(lock))
#define REQUIRES(...) THREAD_ANNOTATION(requires_capability(__VA_ARGS__))
#define ACQUIRE(...) THREAD_ANNOTATION(acquire_capability(__VA_ARGS__))
#define RELEASE(...) THREAD_ANNOTATION(release_capability(__VA_ARGS__))
#define TRY_ACQUIRE(...) THREAD_ANNOTATION(try_acquire_capability(__VA_ARGS__))
#define EXCLUDES(...) THREAD_ANNOTATION(locks_excluded(__VA_ARGS__))
#define ASSERT_CAPABILITY(lock) THREAD_ANNOTATION(assert_capability(lock))
#define RETURN_CAPABILITY(lock) THREAD_ANNOTATION(lock_returned(lock))
#define NO_THREAD_SAFETY_ANALYSIS THREAD_ANNOTATION(no_thread_safety_analysis)

#endif
