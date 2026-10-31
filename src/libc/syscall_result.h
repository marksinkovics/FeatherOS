#ifndef FEATHEROS_LIBC_SYSCALL_RESULT_H
#define FEATHEROS_LIBC_SYSCALL_RESULT_H

#include "user/syscall_abi.h"

#include <errno.h>
#include <stdint.h>

namespace FeatherOS::LibC
{

constexpr int ErrorNumber(uint64_t result)
{
    switch (result)
    {
    case User::SyscallErrorNotFound:
        return ENOENT;
    case User::SyscallErrorInvalidArgument:
        return EINVAL;
    // Linux also fails a background program's terminal read with EIO when it has no signals to
    // stop it.
    case User::SyscallErrorNotForeground:
        return EIO;
    case User::SyscallErrorInterrupted:
        return EINTR;
    case User::SyscallErrorTooManyOpenFiles:
        return EMFILE;
    case User::SyscallErrorBusy:
        return EBUSY;
    case User::SyscallErrorWouldBlock:
        return EAGAIN;
    case User::SyscallErrorNotPermitted:
        return EPERM;
    case User::SyscallErrorBadDescriptor:
        return EBADF;
    case User::SyscallErrorBadAddress:
        return EFAULT;
    case User::SyscallErrorReadOnly:
        return EROFS;
    case User::SyscallErrorExists:
        return EEXIST;
    case User::SyscallErrorNotDirectory:
        return ENOTDIR;
    case User::SyscallErrorNotEmpty:
        return ENOTEMPTY;
    case User::SyscallErrorNotSeekable:
        return ESPIPE;
    case User::SyscallErrorNameTooLong:
        return ENAMETOOLONG;
    case User::SyscallErrorIsDirectory:
        return EISDIR;
    case User::SyscallErrorNotChild:
        return ECHILD;
    case User::SyscallErrorNoMemory:
        return ENOMEM;
    case User::SyscallErrorNotExecutable:
        return ENOEXEC;
    case User::SyscallErrorRange:
        return ERANGE;
    case User::SyscallErrorBrokenPipe:
        return EPIPE;
    case User::SyscallErrorTimedOut:
        return ETIMEDOUT;
    case User::SyscallErrorCrossDevice:
        return EXDEV;
    case User::SyscallErrorNotSocket:
        return ENOTSOCK;
    case User::SyscallErrorFamilyNotSupported:
        return EAFNOSUPPORT;
    case User::SyscallErrorProtocolNotSupported:
        return EPROTONOSUPPORT;
    case User::SyscallErrorMessageTooLong:
        return EMSGSIZE;
    case User::SyscallErrorAddressInUse:
        return EADDRINUSE;
    case User::SyscallErrorAddressNotAvailable:
        return EADDRNOTAVAIL;
    case User::SyscallErrorNetworkDown:
        return ENETDOWN;
    case User::SyscallErrorNetworkUnreachable:
        return ENETUNREACH;
    case User::SyscallErrorHostUnreachable:
        return EHOSTUNREACH;
    case User::SyscallErrorNoBuffers:
        return ENOBUFS;
    case User::SyscallErrorDestinationRequired:
        return EDESTADDRREQ;
    case User::SyscallErrorOptionNotSupported:
        return ENOPROTOOPT;
    case User::SyscallErrorConnectionRefused:
        return ECONNREFUSED;
    case User::SyscallErrorConnectionReset:
        return ECONNRESET;
    case User::SyscallErrorNotConnected:
        return ENOTCONN;
    case User::SyscallErrorAlreadyConnected:
        return EISCONN;
    case User::SyscallErrorOperationNotSupported:
        return EOPNOTSUPP;
    case User::SyscallErrorAlreadyInProgress:
        return EALREADY;
    case User::SyscallErrorInProgress:
        return EINPROGRESS;
    default:
        return EIO;
    }
}

// The POSIX convention: a failed call sets errno and returns -1.
template <typename Result> Result Check(uint64_t result)
{
    if (User::IsSyscallError(result))
    {
        errno = ErrorNumber(result);
        return static_cast<Result>(-1);
    }
    return static_cast<Result>(result);
}

} // namespace FeatherOS::LibC

#endif
