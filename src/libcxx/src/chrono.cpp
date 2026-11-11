// Programs see the library's headers as the system headers they are; the host tests build this
// file against their own copy, in a namespace of its own beside the host's library.
#ifdef FEATHEROS_HOST_TEST
#include "libcxx/include/chrono"
#else
#include <chrono>
#endif

#include <time.h>

// This is the standard library, so it defines what std declares.
// NOLINTBEGIN(bugprone-std-namespace-modification)
namespace FEATHEROS_STD_NAMESPACE::chrono
{

namespace
{

template <class Clock> typename Clock::time_point Now(clockid_t clock) noexcept
{
    timespec now{};
    clock_gettime(clock, &now);
    return typename Clock::time_point(seconds(now.tv_sec) + nanoseconds(now.tv_nsec));
}

} // namespace

system_clock::time_point system_clock::now() noexcept
{
    return Now<system_clock>(CLOCK_REALTIME);
}

steady_clock::time_point steady_clock::now() noexcept
{
    return Now<steady_clock>(CLOCK_MONOTONIC);
}

} // namespace FEATHEROS_STD_NAMESPACE::chrono
// NOLINTEND(bugprone-std-namespace-modification)
