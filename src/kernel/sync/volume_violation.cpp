#include "kernel/panic.h"
#include "kernel/sync/mutex.h"

namespace FeatherOS::Sync
{

void VolumeLockViolation()
{
    Panic("VFS: two volume locks");
}

} // namespace FeatherOS::Sync
