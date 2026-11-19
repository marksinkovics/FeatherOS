#include "filesystem.h"

namespace FeatherOS::Filesystem
{

bool Driver::Open(const char* /*path*/, Node* /*node*/, uint64_t* /*size*/)
{
    return false;
}

bool Driver::Stat(const char* /*path*/, FileInfo* /*info*/)
{
    return false;
}

bool Driver::StatNode(const Node& /*node*/, FileInfo* /*info*/)
{
    return false;
}

bool Driver::Read(const Node& /*node*/, uint64_t /*size*/, uint64_t /*offset*/, void* /*buffer*/,
                  uint64_t /*length*/, uint64_t* /*bytes_read*/)
{
    return false;
}

void Driver::Close(const Node& /*node*/) {}

bool Driver::Create(const char* /*path*/, Node* /*node*/, uint64_t* /*size*/)
{
    return false;
}

bool Driver::Write(Node* /*node*/, uint64_t* /*size*/, uint64_t /*offset*/, const void* /*buffer*/,
                   uint64_t /*length*/, uint64_t* /*bytes_written*/)
{
    return false;
}

bool Driver::Truncate(Node* /*node*/, uint64_t* /*size*/, uint64_t /*new_size*/)
{
    return false;
}

bool Driver::Remove(const char* /*path*/)
{
    return false;
}

bool Driver::MakeDirectory(const char* /*path*/)
{
    return false;
}

bool Driver::RemoveDirectory(const char* /*path*/)
{
    return false;
}

bool Driver::Rename(const char* /*from*/, const char* /*to*/)
{
    return false;
}

bool Driver::IsDirectory(const char* /*path*/)
{
    return false;
}

bool Driver::ReadDirectory(const char* /*path*/, uint32_t /*index*/, DirectoryEntry* /*entry*/)
{
    return false;
}

bool Driver::IsReadOnly(const char* /*path*/)
{
    return properties.read_only;
}

bool Driver::SupportsLinks(LinkKind /*kind*/)
{
    return false;
}

bool Driver::FindLink(const char* /*path*/, uint32_t* /*link_end*/)
{
    return false;
}

bool Driver::ReadLink(const char* /*path*/, char* /*buffer*/, uint32_t /*capacity*/,
                      uint32_t* /*length*/)
{
    return false;
}

bool Driver::SymbolicLink(const char* /*path*/, const char* /*target*/)
{
    return false;
}

bool Driver::Link(const char* /*from*/, const char* /*to*/)
{
    return false;
}

bool Driver::Sync()
{
    return true;
}

bool Driver::Detach()
{
    return Sync();
}

bool Driver::SyncsInBackground() const
{
    return false;
}

uint64_t Driver::LastError()
{
    return User::SyscallErrorFailed;
}

bool Driver::OpenForMount(MountMode /*mode*/, bool* writable)
{
    *writable = true;
    return true;
}

bool Driver::PrepareMount(MountMode mode)
{
    if (properties.read_only)
    {
        mounted_read_only = true;
        return true;
    }
    bool writable = false;
    if (!OpenForMount(mode, &writable))
    {
        return false;
    }
    mounted_read_only = mode == MountMode::ReadOnly || !writable;
    return true;
}

} // namespace FeatherOS::Filesystem
