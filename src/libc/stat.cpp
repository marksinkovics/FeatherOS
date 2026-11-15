#include "libc/syscall_result.h"
#include "user/syscall.h"

#include <sys/stat.h>

namespace
{

constexpr blksize_t BlockSize = 512;

static_assert(S_IFMT == FeatherOS::User::ModeTypeMask && S_IFREG == FeatherOS::User::ModeRegular &&
              S_IFDIR == FeatherOS::User::ModeDirectory &&
              S_IFLNK == FeatherOS::User::ModeSymbolicLink &&
              S_IFCHR == FeatherOS::User::ModeCharacterDevice &&
              S_IFBLK == FeatherOS::User::ModeBlockDevice && S_IFIFO == FeatherOS::User::ModeFifo &&
              S_IFSOCK == FeatherOS::User::ModeSocket);

timespec ToTimespec(const FeatherOS::User::FileTime& time)
{
    return {static_cast<time_t>(time.seconds), static_cast<long>(time.nanoseconds)};
}

int Convert(uint64_t result, const FeatherOS::User::FileStatus& file_status, struct stat* status)
{
    if (FeatherOS::User::IsSyscallError(result))
    {
        return FeatherOS::LibC::Check<int>(result);
    }
    *status = {};
    status->st_dev = static_cast<dev_t>(file_status.device);
    status->st_ino = static_cast<ino_t>(file_status.node);
    status->st_mode = static_cast<mode_t>(file_status.mode);
    status->st_nlink = static_cast<nlink_t>(file_status.link_count);
    status->st_uid = static_cast<uid_t>(file_status.owner);
    status->st_gid = static_cast<gid_t>(file_status.group);
    status->st_size = static_cast<off_t>(file_status.size);
    status->st_blksize = BlockSize;
    status->st_blocks = static_cast<blkcnt_t>((file_status.size + BlockSize - 1) / BlockSize);
    status->st_atim = ToTimespec(file_status.access_time);
    status->st_mtim = ToTimespec(file_status.modify_time);
    status->st_ctim = ToTimespec(file_status.change_time);
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
