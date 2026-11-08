#include "libc/lock.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace
{

// Bytes read from /dev/random ahead of use, so callers that take a few bytes at a time (Swift's
// random number generator takes 8) do not open the device each time.
constexpr size_t PoolSize = 256;
constinit FeatherOS::LibC::Lock pool_lock;
unsigned char pool[PoolSize] GUARDED_BY(pool_lock);
size_t pool_left GUARDED_BY(pool_lock) = 0;

[[noreturn]] void NoRandomness()
{
    constexpr char Message[] = "arc4random_buf: cannot read /dev/random\n";
    write(STDERR_FILENO, Message, sizeof(Message) - 1);
    abort();
}

void Refill() REQUIRES(pool_lock)
{
    const int descriptor = open("/dev/random", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
    {
        NoRandomness();
    }
    size_t filled = 0;
    while (filled < PoolSize)
    {
        const ssize_t count = read(descriptor, pool + filled, PoolSize - filled);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            NoRandomness();
        }
        filled += static_cast<size_t>(count);
    }
    close(descriptor);
    pool_left = PoolSize;
}

} // namespace

void arc4random_buf(void* buffer, size_t size)
{
    const int saved_errno = errno;
    auto* bytes = static_cast<unsigned char*>(buffer);
    {
        const FeatherOS::LibC::Guard<FeatherOS::LibC::Lock> guard(pool_lock);
        while (size > 0)
        {
            if (pool_left == 0)
            {
                Refill();
            }
            const size_t count = size < pool_left ? size : pool_left;
            pool_left -= count;
            memcpy(bytes, pool + pool_left, count);
            // Used bytes are cleared, so a later reader of this memory does not see them.
            memset(pool + pool_left, 0, count);
            bytes += count;
            size -= count;
        }
    }
    errno = saved_errno;
}
