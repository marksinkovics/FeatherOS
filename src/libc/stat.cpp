#include "libc/syscall_result.h"
#include "user/syscall.h"

#include <sys/stat.h>

namespace
{

constexpr blksize_t BlockSize = 512;

int Convert(uint64_t result, const FeatherOS::User::FileStatus& file_status, struct stat* status)
{
    if (FeatherOS::User::IsSyscallError(result))
    {
        return FeatherOS::LibC::Check<int>(result);
    }
    *status = {};
    const bool writable = !file_status.read_only;
    switch (file_status.type)
    {
    case FeatherOS::User::FileType::Directory:
        status->st_mode = S_IFDIR | (writable ? 0755 : 0555);
        break;
    case FeatherOS::User::FileType::Console:
        status->st_mode = S_IFCHR | 0666;
        break;
    default:
        status->st_mode = S_IFREG | (writable ? 0644 : 0444);
        break;
    }
    status->st_nlink = 1;
    status->st_size = static_cast<off_t>(file_status.size);
    status->st_blksize = BlockSize;
    status->st_blocks = static_cast<blkcnt_t>((file_status.size + BlockSize - 1) / BlockSize);
    return 0;
}

} // namespace

int stat(const char* path, struct stat* status)
{
    FeatherOS::User::FileStatus file_status = {};
    return Convert(FeatherOS::User::Stat(path, &file_status), file_status, status);
}

int fstat(int descriptor, struct stat* status)
{
    FeatherOS::User::FileStatus file_status = {};
    return Convert(FeatherOS::User::StatDescriptor(static_cast<uint64_t>(descriptor), &file_status),
                   file_status, status);
}
