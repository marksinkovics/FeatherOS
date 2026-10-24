#ifndef FEATHEROS_LIBC_TIME_H
#define FEATHEROS_LIBC_TIME_H

#include <stddef.h>
#include <sys/cdefs.h>
#include <sys/types.h>

// The wall clock has one-second resolution; CLOCK_MONOTONIC counts milliseconds since the
// kernel's timer started. Every time is UTC: there are no time zones.
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1

typedef int clockid_t;

struct tm
{
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

struct timespec
{
    time_t tv_sec;
    long tv_nsec;
};

__BEGIN_DECLS

time_t time(time_t* result);
// Dates before 1970 are not supported and fail with EOVERFLOW.
struct tm* gmtime(const time_t* seconds);
struct tm* gmtime_r(const time_t* seconds, struct tm* result);
struct tm* localtime(const time_t* seconds);
struct tm* localtime_r(const time_t* seconds, struct tm* result);
// Normalizes out-of-range fields, such as day 32, and fills in tm_wday and tm_yday.
time_t mktime(struct tm* time);
time_t timegm(struct tm* time);
size_t strftime(char* buffer, size_t size, const char* format, const struct tm* time);
char* asctime(const struct tm* time);
char* ctime(const time_t* seconds);
int clock_gettime(clockid_t clock, struct timespec* result);
// A signal cuts it short with EINTR and the time left in remaining.
int nanosleep(const struct timespec* duration, struct timespec* remaining);

__END_DECLS

#endif
