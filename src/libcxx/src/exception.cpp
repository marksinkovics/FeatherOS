#ifdef FEATHEROS_HOST_TEST
#include "libcxx/include/atomic"
#include "libcxx/include/exception"
#else
#include <atomic>
#include <exception>
#endif

#include <stdlib.h>

// NOLINTBEGIN(bugprone-std-namespace-modification)
namespace FEATHEROS_STD_NAMESPACE
{

namespace
{
constinit atomic<terminate_handler> TerminateHandler{nullptr};
} // namespace

exception::~exception() = default;

const char* exception::what() const noexcept
{
    return "std::exception";
}

bad_exception::~bad_exception() = default;

const char* bad_exception::what() const noexcept
{
    return "std::bad_exception";
}

terminate_handler get_terminate() noexcept
{
    return TerminateHandler.load();
}

terminate_handler set_terminate(terminate_handler handler) noexcept
{
    return TerminateHandler.exchange(handler);
}

// [terminate.handler]: a handler that returns still ends the program.
void terminate() noexcept
{
    if (const terminate_handler handler = TerminateHandler.load())
    {
        handler();
    }
    abort();
}

int uncaught_exceptions() noexcept
{
    return 0;
}

} // namespace FEATHEROS_STD_NAMESPACE
// NOLINTEND(bugprone-std-namespace-modification)
