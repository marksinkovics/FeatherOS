#include "socket.h"

#include "icmp.h"
#include "icmpv6.h"
#include "interface.h"
#include "ip.h"
#include "ipv4.h"
#include "ipv6.h"
#include "tcp_connection.h"
#include "udp.h"

#include "kernel/memory/heap.h"
#include "kernel/poll.h"
#include "kernel/random.h"
#include "kernel/scheduler.h"
#include "kernel/sync/spinlock.h"
#include "kernel/time/timer.h"

namespace FeatherOS::Network::Sockets
{
namespace
{

struct Datagram
{
    Endpoint source;
    size_t length;
    uint8_t data[MaximumDatagramSize];
};

enum class Kind : uint8_t
{
    Icmp,
    Udp,
    Tcp,
};

} // namespace

struct Socket
{
    Socket* next;
    // Every wait on the socket: data, space, a new connection, or the handshake.
    Scheduler::WaitQueue waiters;
    // Datagram sockets only.
    Datagram* queue;
    uint32_t queue_start;
    uint32_t queue_length;
    // TCP sockets from Connect, and connections a listener accepted.
    TcpConnection* connection;
    // An accepted connection's listener, until Accept hands the connection out.
    Socket* listener;
    uint64_t accept_order;
    uint64_t receive_timeout_ms;
    // A UDP socket's error from an ICMP message, which its next send or receive returns.
    uint64_t pending_error;
    // Unspecified unless bound to one of our addresses, which then is the only destination it
    // receives for. A TCP connection's is the address it uses.
    IpAddress local_address;
    Endpoint remote;
    uint32_t backlog;
    IpVersion version;
    Kind kind;
    uint16_t port;
    bool bound;
    // remote holds the peer: a datagram socket's from Connect, or a TCP connection's.
    bool connected;
    bool listening;
    bool read_shut;
    // Its descriptor is closed; a TCP connection still finishes before the socket is freed.
    bool orphaned;
};

namespace
{

// Guards the socket list, bindings, every receive queue and every TCP connection.
constinit Sync::SpinLock socket_lock;
Socket* sockets = nullptr;
// Echo identifiers take any value; UDP and TCP pick from the dynamic ports (RFC 6335 6).
constexpr uint16_t FirstDynamicPort = 49152;
uint16_t next_identifier = 1;
uint16_t next_dynamic_port = FirstDynamicPort;
uint64_t next_accept_order = 0;

// Resets for segments no connection takes, waiting for the TCP task to send them.
struct ResetReply
{
    Endpoint local;
    Endpoint remote;
    uint32_t sequence;
    uint32_t acknowledgment;
    uint8_t flags;
};

constexpr uint32_t MaximumResetReplies = 8;
ResetReply reset_replies[MaximumResetReplies];
uint32_t reset_reply_count = 0;
constinit Scheduler::WaitQueue tcp_waiters;
bool tcp_work_pending = false;

uint64_t NowMilliseconds()
{
    return Time::TickCount() * 1000 / Time::TicksPerSecond();
}

uint64_t TickAt(uint64_t milliseconds)
{
    return (milliseconds * Time::TicksPerSecond() + 999) / 1000;
}

uint16_t LocalMss(IpVersion version)
{
    return static_cast<uint16_t>(
        (version == IpVersion::V6 ? MaximumIpv6Payload : MaximumIpv4Payload) - TcpHeaderSize);
}

void Free(Socket* socket)
{
    if (socket->connection != nullptr)
    {
        socket->connection->Release();
        Memory::Deallocate(socket->connection);
    }
    if (socket->queue != nullptr)
    {
        Memory::Deallocate(socket->queue);
    }
    Memory::Deallocate(socket);
}

void UnlinkLocked(Socket* socket)
{
    for (Socket** link = &sockets; *link != nullptr; link = &(*link)->next)
    {
        if (*link == socket)
        {
            *link = socket->next;
            return;
        }
    }
}

void ChangedLocked(Socket* socket)
{
    Scheduler::WakeAll(socket->waiters);
    if (socket->listener != nullptr)
    {
        Scheduler::WakeAll(socket->listener->waiters);
    }
    Poll::Notify();
}

void PostTcpWorkLocked()
{
    tcp_work_pending = true;
    Scheduler::WakeAll(tcp_waiters);
}

// When binding, only sockets holding the port against a bind count; when picking a free port,
// every socket on it does.
bool PortInUseLocked(Kind kind, IpVersion version, uint16_t port, bool binding)
{
    for (const Socket* socket = sockets; socket != nullptr; socket = socket->next)
    {
        if (!socket->bound || socket->kind != kind || socket->version != version ||
            socket->port != port)
        {
            continue;
        }
        if (binding && kind == Kind::Tcp &&
            (socket->connection != nullptr || socket->listener != nullptr || socket->orphaned))
        {
            continue;
        }
        return true;
    }
    return false;
}

uint16_t PickPortLocked(Kind kind, IpVersion version)
{
    const uint16_t first = kind == Kind::Icmp ? 1 : FirstDynamicPort;
    uint16_t& next = kind == Kind::Icmp ? next_identifier : next_dynamic_port;
    for (uint32_t attempt = 0; attempt <= 0xFFFFU - first; ++attempt)
    {
        const uint16_t candidate = next;
        next = next == 0xFFFF ? first : next + 1;
        if (!PortInUseLocked(kind, version, candidate, false))
        {
            return candidate;
        }
    }
    return 0;
}

uint64_t BindLocked(Socket* socket, uint16_t port)
{
    if (port == 0)
    {
        port = PickPortLocked(socket->kind, socket->version);
        if (port == 0)
        {
            return User::SyscallErrorAddressInUse;
        }
    }
    else if (PortInUseLocked(socket->kind, socket->version, port, true))
    {
        return User::SyscallErrorAddressInUse;
    }
    socket->port = port;
    socket->bound = true;
    return 0;
}

uint64_t BoundPort(Socket* socket, uint16_t* port)
{
    Sync::SpinLockGuard guard(socket_lock);
    if (!socket->bound)
    {
        const uint64_t result = BindLocked(socket, 0);
        if (User::IsSyscallError(result))
        {
            return result;
        }
    }
    *port = socket->port;
    return 0;
}

void EnqueueLocked(Socket* socket, const Endpoint& source, const uint8_t* data, size_t length)
{
    if (socket->queue_length == ReceiveQueueLength || socket->read_shut)
    {
        return;
    }
    Datagram& datagram =
        socket->queue[(socket->queue_start + socket->queue_length) % ReceiveQueueLength];
    datagram.source = source;
    datagram.length = length;
    for (size_t index = 0; index < length; ++index)
    {
        datagram.data[index] = data[index];
    }
    ++socket->queue_length;
    ChangedLocked(socket);
}

uint64_t SendStatusResult(SendStatus status, uint64_t length)
{
    switch (status)
    {
    case SendStatus::Sent:
        return length;
    case SendStatus::NoRoute:
        return User::SyscallErrorNetworkUnreachable;
    case SendStatus::NetworkDown:
        return User::SyscallErrorNetworkDown;
    case SendStatus::HostUnreachable:
        return User::SyscallErrorHostUnreachable;
    case SendStatus::TooLong:
        return User::SyscallErrorMessageTooLong;
    case SendStatus::NoBuffers:
        return User::SyscallErrorNoBuffers;
    }
    return User::SyscallErrorFailed;
}

uint64_t TakePendingError(Socket* socket)
{
    Sync::SpinLockGuard guard(socket_lock);
    const uint64_t error = socket->pending_error;
    socket->pending_error = 0;
    return error;
}

bool Interrupted(InterruptCheck interrupted)
{
    return interrupted != nullptr && interrupted();
}

uint64_t ReceiveDeadline(const Socket* socket)
{
    if (socket->receive_timeout_ms == 0)
    {
        return 0;
    }
    return Time::TickCount() + (socket->receive_timeout_ms * Time::TicksPerSecond() + 999) / 1000;
}

bool IsLoopback(const IpAddress& address)
{
    return address.version == IpVersion::V6
               ? address == Ipv6Loopback
               : address.version == IpVersion::V4 && IsIpv4Loopback(ToIpv4(address));
}

bool TimedOut(uint64_t deadline)
{
    return deadline != 0 && Time::TickCount() >= deadline;
}

Socket* FindConnectionLocked(const Endpoint& source, const Endpoint& destination)
{
    for (Socket* socket = sockets; socket != nullptr; socket = socket->next)
    {
        if (socket->kind == Kind::Tcp && socket->connection != nullptr &&
            socket->connection->State() != TcpState::Closed && socket->port == destination.port &&
            socket->remote.port == source.port && socket->remote.address == source.address &&
            socket->local_address == destination.address)
        {
            return socket;
        }
    }
    return nullptr;
}

Socket* FindListenerLocked(const Endpoint& destination)
{
    for (Socket* socket = sockets; socket != nullptr; socket = socket->next)
    {
        if (socket->listening && socket->version == destination.address.version &&
            socket->port == destination.port &&
            (IsUnspecified(socket->local_address) || socket->local_address == destination.address))
        {
            return socket;
        }
    }
    return nullptr;
}

uint32_t PendingCountLocked(const Socket* listener)
{
    uint32_t count = 0;
    for (const Socket* socket = sockets; socket != nullptr; socket = socket->next)
    {
        if (socket->listener == listener)
        {
            ++count;
        }
    }
    return count;
}

// The oldest connection that finished its handshake, for Accept.
Socket* ReadyConnectionLocked(const Socket* listener)
{
    Socket* ready = nullptr;
    for (Socket* socket = sockets; socket != nullptr; socket = socket->next)
    {
        if (socket->listener == listener && !socket->connection->Opening() &&
            socket->connection->State() != TcpState::Closed &&
            (ready == nullptr || socket->accept_order < ready->accept_order))
        {
            ready = socket;
        }
    }
    return ready;
}

// RFC 9293 3.10.7.1: the reset answers the segment's acknowledgment, or acknowledges the segment.
void QueueResetLocked(const Endpoint& source, const Endpoint& destination,
                      const TcpSegment& segment)
{
    if (reset_reply_count == MaximumResetReplies)
    {
        return;
    }
    ResetReply& reply = reset_replies[reset_reply_count++];
    reply.local = destination;
    reply.remote = source;
    if ((segment.flags & TcpAck) != 0)
    {
        reply.sequence = segment.acknowledgment;
        reply.acknowledgment = 0;
        reply.flags = TcpReset;
    }
    else
    {
        reply.sequence = 0;
        reply.acknowledgment = segment.sequence + SequenceLength(segment);
        reply.flags = TcpReset | TcpAck;
    }
    PostTcpWorkLocked();
}

size_t WriteReset(const ResetReply& reply, uint8_t* segment, size_t capacity)
{
    const TcpSegment fields = {
        .source_port = reply.local.port,
        .destination_port = reply.remote.port,
        .sequence = reply.sequence,
        .acknowledgment = reply.acknowledgment,
        .flags = reply.flags,
        .window = 0,
        .maximum_segment_size = 0,
        .has_window_scale = false,
        .window_scale = 0,
        .payload = nullptr,
        .payload_length = 0,
    };
    return WriteTcpSegment(segment, capacity, fields);
}

uint64_t SendStream(Socket* socket, const uint8_t* message, uint64_t length,
                    InterruptCheck interrupted, bool nonblocking)
{
    uint64_t total = 0;
    Sync::SpinLockGuard guard(socket_lock);
    for (;;)
    {
        TcpConnection* connection = socket->connection;
        if (connection == nullptr)
        {
            return User::SyscallErrorNotConnected;
        }
        if (connection->CanWrite())
        {
            if (total == length)
            {
                return total;
            }
            const size_t count = connection->Write(message + total, length - total);
            if (count != 0)
            {
                total += count;
                PostTcpWorkLocked();
                continue;
            }
        }
        else if (!connection->Opening())
        {
            if (total != 0)
            {
                return total;
            }
            return connection->Error() != 0 ? connection->Error() : User::SyscallErrorBrokenPipe;
        }
        if (nonblocking)
        {
            return total != 0 ? total : User::SyscallErrorWouldBlock;
        }
        if (Interrupted(interrupted))
        {
            return total != 0 ? total : User::SyscallErrorInterrupted;
        }
        Scheduler::WaitOn(socket->waiters, socket_lock);
    }
}

uint64_t ReceiveStream(Socket* socket, uint8_t* buffer, uint64_t capacity, Endpoint* source,
                       InterruptCheck interrupted, bool nonblocking)
{
    const uint64_t deadline = ReceiveDeadline(socket);
    Sync::SpinLockGuard guard(socket_lock);
    for (;;)
    {
        TcpConnection* connection = socket->connection;
        if (connection == nullptr)
        {
            return User::SyscallErrorNotConnected;
        }
        if (source != nullptr)
        {
            *source = socket->remote;
        }
        if (socket->read_shut || capacity == 0)
        {
            return 0;
        }
        if (connection->Readable() != 0)
        {
            const size_t count = connection->Read(buffer, capacity);
            if (connection->AcknowledgmentPending())
            {
                PostTcpWorkLocked();
            }
            return count;
        }
        if (connection->ReceivedFin())
        {
            return 0;
        }
        if (connection->State() == TcpState::Closed)
        {
            return connection->Error();
        }
        if (nonblocking || TimedOut(deadline))
        {
            return User::SyscallErrorWouldBlock;
        }
        if (Interrupted(interrupted))
        {
            return User::SyscallErrorInterrupted;
        }
        Scheduler::WaitOn(socket->waiters, socket_lock, deadline);
    }
}

uint64_t RunTcpTask(void*)
{
    for (;;)
    {
        const uint64_t next = ServiceTcp(NowMilliseconds());
        Sync::SpinLockGuard guard(socket_lock);
        if (!tcp_work_pending)
        {
            Scheduler::WaitOn(tcp_waiters, socket_lock, next == 0 ? 0 : TickAt(next));
        }
        tcp_work_pending = false;
    }
}

} // namespace

uint64_t Create(uint64_t family, uint64_t type, uint64_t protocol, Socket** socket)
{
    if (socket == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    if (family != User::SocketFamilyIpv4 && family != User::SocketFamilyIpv6)
    {
        return User::SyscallErrorFamilyNotSupported;
    }
    const bool ipv6 = family == User::SocketFamilyIpv6;
    const uint64_t base_type = type & ~(User::SocketCloseOnExec | User::SocketNonBlocking);
    Kind kind;
    if (base_type == User::SocketStream && (protocol == 0 || protocol == User::SocketProtocolTcp))
    {
        kind = Kind::Tcp;
    }
    else if (base_type != User::SocketDatagram)
    {
        return User::SyscallErrorProtocolNotSupported;
    }
    else if (protocol == 0 || protocol == User::SocketProtocolUdp)
    {
        kind = Kind::Udp;
    }
    else if (protocol == (ipv6 ? User::SocketProtocolIcmpv6 : User::SocketProtocolIcmp))
    {
        kind = Kind::Icmp;
    }
    else
    {
        return User::SyscallErrorProtocolNotSupported;
    }
    auto* created = static_cast<Socket*>(Memory::AllocateZeroed(1, sizeof(Socket)));
    if (created == nullptr)
    {
        return User::SyscallErrorNoMemory;
    }
    if (kind != Kind::Tcp)
    {
        created->queue =
            static_cast<Datagram*>(Memory::AllocateZeroed(ReceiveQueueLength, sizeof(Datagram)));
        if (created->queue == nullptr)
        {
            Memory::Deallocate(created);
            return User::SyscallErrorNoMemory;
        }
    }
    created->version = ipv6 ? IpVersion::V6 : IpVersion::V4;
    created->kind = kind;
    created->local_address.version = created->version;
    {
        Sync::SpinLockGuard guard(socket_lock);
        created->next = sockets;
        sockets = created;
    }
    *socket = created;
    return 0;
}

void Close(Socket* socket)
{
    {
        Sync::SpinLockGuard guard(socket_lock);
        if (socket->listening)
        {
            for (Socket* pending = sockets; pending != nullptr; pending = pending->next)
            {
                if (pending->listener == socket)
                {
                    pending->listener = nullptr;
                    pending->orphaned = true;
                    pending->connection->Abort();
                    PostTcpWorkLocked();
                }
            }
        }
        TcpConnection* connection = socket->connection;
        if (connection != nullptr && !connection->Idle())
        {
            // Unread data would be lost unnoticed, so the peer learns of it from a reset (RFC
            // 2525 2.17).
            if (connection->Readable() != 0)
            {
                connection->Abort();
            }
            else
            {
                connection->Close();
            }
            connection->Orphan(NowMilliseconds());
            socket->orphaned = true;
            PostTcpWorkLocked();
            return;
        }
        UnlinkLocked(socket);
    }
    Free(socket);
}

bool IsStream(const Socket* socket)
{
    return socket->kind == Kind::Tcp;
}

uint64_t Bind(Socket* socket, const Endpoint& local)
{
    if (local.address.version != socket->version)
    {
        return User::SyscallErrorFamilyNotSupported;
    }
    if (!IsUnspecified(local.address) && !IsLocalAddress(local.address))
    {
        return User::SyscallErrorAddressNotAvailable;
    }
    Sync::SpinLockGuard guard(socket_lock);
    if (socket->bound)
    {
        return User::SyscallErrorInvalidArgument;
    }
    const uint64_t result = BindLocked(socket, local.port);
    if (!User::IsSyscallError(result))
    {
        socket->local_address = local.address;
    }
    return result;
}

uint64_t Connect(Socket* socket, const Endpoint& remote, InterruptCheck interrupted,
                 bool nonblocking)
{
    if (remote.address.version != socket->version)
    {
        return User::SyscallErrorFamilyNotSupported;
    }
    if (socket->kind != Kind::Tcp)
    {
        Sync::SpinLockGuard guard(socket_lock);
        if (!socket->bound)
        {
            const uint64_t bound = BindLocked(socket, 0);
            if (User::IsSyscallError(bound))
            {
                return bound;
            }
        }
        socket->remote = remote;
        socket->connected = true;
        return 0;
    }
    if (remote.port == 0 || IsUnspecified(remote.address))
    {
        return User::SyscallErrorAddressNotAvailable;
    }
    const IpAddress source = SourceAddressFor(remote.address);
    if (IsUnspecified(source))
    {
        return LinkIsUp() ? User::SyscallErrorNetworkUnreachable : User::SyscallErrorNetworkDown;
    }
    auto* connection = static_cast<TcpConnection*>(Memory::Allocate(sizeof(TcpConnection)));
    if (connection == nullptr)
    {
        return User::SyscallErrorNoMemory;
    }
    const auto initial_sequence = static_cast<uint32_t>(Random::Next());
    uint64_t refused = 0;
    {
        Sync::SpinLockGuard guard(socket_lock);
        if (socket->listening)
        {
            refused = User::SyscallErrorInvalidArgument;
        }
        else if (socket->connection != nullptr)
        {
            const TcpConnection* existing = socket->connection;
            if (existing->Opening())
            {
                refused = User::SyscallErrorAlreadyInProgress;
            }
            else
            {
                // A connection that failed, such as one a non-blocking Connect started, reports
                // why.
                refused = existing->State() == TcpState::Closed && existing->Error() != 0
                              ? existing->Error()
                              : User::SyscallErrorAlreadyConnected;
            }
        }
        else if (!socket->bound)
        {
            refused = BindLocked(socket, 0);
        }
        if (!User::IsSyscallError(refused))
        {
            connection->Connect(socket->port, remote.port, initial_sequence,
                                LocalMss(socket->version));
            socket->connection = connection;
            socket->remote = remote;
            socket->local_address = source;
            socket->connected = true;
            PostTcpWorkLocked();
            if (nonblocking)
            {
                return User::SyscallErrorInProgress;
            }
            while (connection->Opening())
            {
                if (Interrupted(interrupted))
                {
                    connection->Abort();
                    PostTcpWorkLocked();
                    return User::SyscallErrorInterrupted;
                }
                Scheduler::WaitOn(socket->waiters, socket_lock);
            }
            if (connection->State() != TcpState::Closed)
            {
                return 0;
            }
            return connection->Error() != 0 ? connection->Error()
                                            : User::SyscallErrorConnectionRefused;
        }
    }
    Memory::Deallocate(connection);
    return refused;
}

uint64_t Listen(Socket* socket, uint64_t backlog)
{
    if (socket->kind != Kind::Tcp)
    {
        return User::SyscallErrorOperationNotSupported;
    }
    Sync::SpinLockGuard guard(socket_lock);
    if (socket->connection != nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    if (!socket->bound)
    {
        const uint64_t bound = BindLocked(socket, 0);
        if (User::IsSyscallError(bound))
        {
            return bound;
        }
    }
    socket->listening = true;
    socket->backlog = static_cast<uint32_t>(
        backlog == 0
            ? 1
            : (backlog > User::MaximumListenBacklog ? User::MaximumListenBacklog : backlog));
    return 0;
}

uint64_t Accept(Socket* socket, Socket** connection, Endpoint* peer, InterruptCheck interrupted,
                bool nonblocking)
{
    const uint64_t deadline = ReceiveDeadline(socket);
    Sync::SpinLockGuard guard(socket_lock);
    if (!socket->listening)
    {
        return User::SyscallErrorInvalidArgument;
    }
    for (;;)
    {
        Socket* ready = ReadyConnectionLocked(socket);
        if (ready != nullptr)
        {
            ready->listener = nullptr;
            *connection = ready;
            *peer = ready->remote;
            return 0;
        }
        if (nonblocking || TimedOut(deadline))
        {
            return User::SyscallErrorWouldBlock;
        }
        if (Interrupted(interrupted))
        {
            return User::SyscallErrorInterrupted;
        }
        Scheduler::WaitOn(socket->waiters, socket_lock, deadline);
    }
}

uint64_t SendTo(Socket* socket, uint8_t* message, uint64_t length, const Endpoint* destination,
                InterruptCheck interrupted, bool nonblocking)
{
    if (socket->kind == Kind::Tcp)
    {
        return SendStream(socket, message, length, interrupted, nonblocking);
    }
    Endpoint target;
    if (destination != nullptr)
    {
        target = *destination;
    }
    else
    {
        Sync::SpinLockGuard guard(socket_lock);
        if (!socket->connected)
        {
            return User::SyscallErrorDestinationRequired;
        }
        target = socket->remote;
    }
    if (target.address.version != socket->version)
    {
        return User::SyscallErrorFamilyNotSupported;
    }
    if (length > MaximumDatagramSize)
    {
        return User::SyscallErrorMessageTooLong;
    }
    const bool ipv6 = socket->version == IpVersion::V6;
    if (socket->kind == Kind::Udp)
    {
        if (target.port == 0)
        {
            return User::SyscallErrorInvalidArgument;
        }
        const uint64_t error = TakePendingError(socket);
        if (error != 0)
        {
            return error;
        }
        uint16_t port;
        const uint64_t bound = BoundPort(socket, &port);
        if (User::IsSyscallError(bound))
        {
            return bound;
        }
        uint8_t datagram[UdpHeaderSize + MaximumDatagramSize];
        WriteUdpHeader(datagram, port, target.port, length);
        for (uint64_t index = 0; index < length; ++index)
        {
            datagram[UdpHeaderSize + index] = message[index];
        }
        return SendStatusResult(SendIp(target.address, IpProtocolUdp, datagram,
                                       UdpHeaderSize + length, UdpChecksumOffset),
                                length);
    }
    if (length < IcmpHeaderSize || message[0] != (ipv6 ? Icmpv6EchoRequest : IcmpEchoRequest) ||
        message[1] != 0)
    {
        return User::SyscallErrorInvalidArgument;
    }
    uint16_t port;
    const uint64_t bound = BoundPort(socket, &port);
    if (User::IsSyscallError(bound))
    {
        return bound;
    }
    if (ipv6)
    {
        WriteBig16(message + 4, port);
        return SendStatusResult(
            SendIp(target.address, IpProtocolIcmpv6, message, length, Icmpv6ChecksumOffset),
            length);
    }
    RewriteIcmpEcho(message, length, IcmpEchoRequest, port);
    return SendStatusResult(SendIp(target.address, IpProtocolIcmp, message, length), length);
}

uint64_t ReceiveFrom(Socket* socket, uint8_t* buffer, uint64_t capacity, Endpoint* source,
                     InterruptCheck interrupted, bool nonblocking)
{
    if (socket->kind == Kind::Tcp)
    {
        return ReceiveStream(socket, buffer, capacity, source, interrupted, nonblocking);
    }
    const uint64_t deadline = ReceiveDeadline(socket);
    Sync::SpinLockGuard guard(socket_lock);
    while (socket->queue_length == 0 || socket->pending_error != 0)
    {
        if (socket->pending_error != 0)
        {
            const uint64_t error = socket->pending_error;
            socket->pending_error = 0;
            return error;
        }
        if (socket->read_shut)
        {
            return 0;
        }
        if (nonblocking || TimedOut(deadline))
        {
            return User::SyscallErrorWouldBlock;
        }
        if (Interrupted(interrupted))
        {
            return User::SyscallErrorInterrupted;
        }
        Scheduler::WaitOn(socket->waiters, socket_lock, deadline);
    }
    const Datagram& datagram = socket->queue[socket->queue_start];
    const uint64_t count = datagram.length < capacity ? datagram.length : capacity;
    for (uint64_t index = 0; index < count; ++index)
    {
        buffer[index] = datagram.data[index];
    }
    if (source != nullptr)
    {
        *source = datagram.source;
    }
    socket->queue_start = (socket->queue_start + 1) % ReceiveQueueLength;
    --socket->queue_length;
    return count;
}

uint64_t Shutdown(Socket* socket, uint64_t how)
{
    if (how > User::SocketShutdownBoth)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Sync::SpinLockGuard guard(socket_lock);
    TcpConnection* connection = socket->connection;
    if (socket->kind == Kind::Tcp ? connection == nullptr || connection->Opening()
                                  : !socket->connected)
    {
        return User::SyscallErrorNotConnected;
    }
    if (how != User::SocketShutdownWrite)
    {
        socket->read_shut = true;
    }
    if (how != User::SocketShutdownRead && connection != nullptr)
    {
        connection->Close();
        PostTcpWorkLocked();
    }
    ChangedLocked(socket);
    return 0;
}

uint64_t Name(Socket* socket, bool peer, Endpoint* endpoint)
{
    Sync::SpinLockGuard guard(socket_lock);
    if (peer)
    {
        if (!socket->connected)
        {
            return User::SyscallErrorNotConnected;
        }
        *endpoint = socket->remote;
        return 0;
    }
    endpoint->address = socket->local_address;
    endpoint->port = socket->bound ? socket->port : 0;
    return 0;
}

int16_t PollEvents(Socket* socket)
{
    Sync::SpinLockGuard guard(socket_lock);
    if (socket->kind != Kind::Tcp)
    {
        return static_cast<int16_t>(
            User::PollWritable |
            (socket->queue_length != 0 || socket->read_shut ? User::PollReadable : 0) |
            (socket->pending_error != 0 ? User::PollError : 0));
    }
    if (socket->listening)
    {
        return ReadyConnectionLocked(socket) != nullptr ? User::PollReadable
                                                        : static_cast<int16_t>(0);
    }
    const TcpConnection* connection = socket->connection;
    if (connection == nullptr)
    {
        return User::PollWritable | User::PollHangUp;
    }
    if (connection->Opening())
    {
        return 0;
    }
    int events = 0;
    if (connection->Error() != 0)
    {
        events |= User::PollError;
    }
    if (connection->State() == TcpState::Closed)
    {
        events |= User::PollHangUp | User::PollReadable;
    }
    if (connection->Readable() != 0 || connection->ReceivedFin() || socket->read_shut)
    {
        events |= User::PollReadable;
    }
    // A socket that can no longer send is writable too: a write fails at once.
    if (connection->Writable() != 0 || !connection->CanWrite())
    {
        events |= User::PollWritable;
    }
    return static_cast<int16_t>(events);
}

uint64_t SetReceiveTimeout(Socket* socket, uint64_t milliseconds)
{
    constexpr uint64_t MaximumMilliseconds = 24ULL * 60 * 60 * 1000;
    if (milliseconds > MaximumMilliseconds)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Sync::SpinLockGuard guard(socket_lock);
    socket->receive_timeout_ms = milliseconds;
    return 0;
}

uint64_t ReceiveTimeout(Socket* socket)
{
    Sync::SpinLockGuard guard(socket_lock);
    return socket->receive_timeout_ms;
}

uint64_t TakeError(Socket* socket)
{
    {
        Sync::SpinLockGuard guard(socket_lock);
        if (socket->connection != nullptr)
        {
            return socket->connection->State() == TcpState::Closed ? socket->connection->Error()
                                                                   : 0;
        }
    }
    return TakePendingError(socket);
}

void DeliverEcho(const IpAddress& source, uint16_t identifier, const uint8_t* message,
                 size_t length)
{
    if (length > MaximumDatagramSize)
    {
        return;
    }
    Sync::SpinLockGuard guard(socket_lock);
    for (Socket* socket = sockets; socket != nullptr; socket = socket->next)
    {
        if (socket->bound && socket->kind == Kind::Icmp && socket->version == source.version &&
            socket->port == identifier && (!socket->connected || socket->remote.address == source))
        {
            EnqueueLocked(socket, {.address = source, .port = identifier}, message, length);
            return;
        }
    }
}

bool DeliverUdp(const Endpoint& source, const Endpoint& destination, const uint8_t* payload,
                size_t length)
{
    Sync::SpinLockGuard guard(socket_lock);
    for (Socket* socket = sockets; socket != nullptr; socket = socket->next)
    {
        if (socket->bound && socket->kind == Kind::Udp &&
            socket->version == source.address.version && socket->port == destination.port &&
            (IsUnspecified(socket->local_address) ||
             socket->local_address == destination.address) &&
            (!socket->connected ||
             (socket->remote.address == source.address && socket->remote.port == source.port)))
        {
            if (length <= MaximumDatagramSize)
            {
                EnqueueLocked(socket, source, payload, length);
            }
            return true;
        }
    }
    return false;
}

void DeliverPortUnreachable(const Endpoint& local, const Endpoint& remote)
{
    Sync::SpinLockGuard guard(socket_lock);
    for (Socket* socket = sockets; socket != nullptr; socket = socket->next)
    {
        if (socket->bound && socket->connected && socket->kind == Kind::Udp &&
            socket->version == local.address.version && socket->port == local.port &&
            (IsUnspecified(socket->local_address) || socket->local_address == local.address) &&
            socket->remote.address == remote.address && socket->remote.port == remote.port)
        {
            socket->pending_error = User::SyscallErrorConnectionRefused;
            ChangedLocked(socket);
            return;
        }
    }
}

void DeliverTcp(const Endpoint& source, const Endpoint& destination, const TcpSegment& segment)
{
    // A SYN may open a connection on a listener; what that needs is allocated up front, outside
    // the lock.
    const bool opening = (segment.flags & (TcpSyn | TcpAck | TcpReset)) == TcpSyn;
    Socket* child = nullptr;
    TcpConnection* child_connection = nullptr;
    if (opening)
    {
        child = static_cast<Socket*>(Memory::AllocateZeroed(1, sizeof(Socket)));
        child_connection = static_cast<TcpConnection*>(Memory::Allocate(sizeof(TcpConnection)));
    }
    const auto initial_sequence = opening ? static_cast<uint32_t>(Random::Next()) : 0;
    bool used = false;
    {
        Sync::SpinLockGuard guard(socket_lock);
        Socket* socket = FindConnectionLocked(source, destination);
        Socket* listener = socket == nullptr && opening ? FindListenerLocked(destination) : nullptr;
        if (socket != nullptr)
        {
            socket->connection->Receive(segment, NowMilliseconds());
            ChangedLocked(socket);
            PostTcpWorkLocked();
        }
        else if (listener == nullptr)
        {
            if ((segment.flags & TcpReset) == 0)
            {
                QueueResetLocked(source, destination, segment);
            }
        }
        // A full backlog drops the SYN; the peer sends it again.
        else if (child != nullptr && child_connection != nullptr &&
                 PendingCountLocked(listener) < listener->backlog)
        {
            child_connection->Accept(segment, initial_sequence,
                                     LocalMss(destination.address.version));
            child->connection = child_connection;
            child->version = destination.address.version;
            child->kind = Kind::Tcp;
            child->local_address = destination.address;
            child->port = destination.port;
            child->bound = true;
            child->remote = source;
            child->connected = true;
            child->listener = listener;
            child->accept_order = next_accept_order++;
            child->next = sockets;
            sockets = child;
            PostTcpWorkLocked();
            used = true;
        }
    }
    if (!used)
    {
        if (child != nullptr)
        {
            Memory::Deallocate(child);
        }
        if (child_connection != nullptr)
        {
            Memory::Deallocate(child_connection);
        }
    }
}

void DropExternalConnections()
{
    Sync::SpinLockGuard guard(socket_lock);
    for (Socket* socket = sockets; socket != nullptr; socket = socket->next)
    {
        TcpConnection* connection = socket->connection;
        if (connection != nullptr && connection->State() != TcpState::Closed &&
            !IsLoopback(socket->remote.address))
        {
            connection->Drop(User::SyscallErrorNetworkDown);
            ChangedLocked(socket);
        }
    }
    reset_reply_count = 0;
    // Closed sockets nobody holds any more are freed there.
    PostTcpWorkLocked();
}

bool StartTcp()
{
    Scheduler::TaskId task;
    return Scheduler::CreateTask(RunTcpTask, nullptr, &task) &&
           Scheduler::SetPriority(task, Scheduler::TaskPriority::High);
}

uint64_t ServiceTcp(uint64_t now)
{
    uint8_t segment[TcpConnection::MaximumSegmentLength];
    for (;;)
    {
        size_t length = 0;
        IpAddress destination = {};
        Socket* freed = nullptr;
        uint64_t next_deadline = 0;
        {
            Sync::SpinLockGuard guard(socket_lock);
            if (reset_reply_count != 0)
            {
                const ResetReply& reply = reset_replies[--reset_reply_count];
                length = WriteReset(reply, segment, sizeof(segment));
                destination = reply.remote.address;
            }
            for (Socket** link = &sockets; length == 0 && *link != nullptr; link = &(*link)->next)
            {
                Socket* socket = *link;
                TcpConnection* connection = socket->connection;
                if (connection == nullptr)
                {
                    continue;
                }
                if (connection->OnTimer(now))
                {
                    ChangedLocked(socket);
                }
                length = connection->NextSegment(now, segment);
                if (length != 0)
                {
                    destination = socket->remote.address;
                    break;
                }
                if (connection->Idle() && (socket->orphaned || socket->listener != nullptr))
                {
                    *link = socket->next;
                    freed = socket;
                    break;
                }
                const uint64_t deadline = connection->Deadline();
                if (deadline != 0 && (next_deadline == 0 || deadline < next_deadline))
                {
                    next_deadline = deadline;
                }
            }
        }
        if (freed != nullptr)
        {
            Free(freed);
            continue;
        }
        if (length == 0)
        {
            return next_deadline;
        }
        SendIp(destination, IpProtocolTcp, segment, length, TcpChecksumOffset);
    }
}

Endpoint FromUserAddress(const User::SocketAddress& address)
{
    Endpoint endpoint = {.address = {.version = IpVersion::None, .bytes = {}},
                         .port = address.port};
    if (address.family == User::SocketFamilyIpv4)
    {
        endpoint.address.version = IpVersion::V4;
        for (size_t index = 0; index < 4; ++index)
        {
            endpoint.address.bytes[index] = address.address[index];
        }
    }
    else if (address.family == User::SocketFamilyIpv6)
    {
        endpoint.address.version = IpVersion::V6;
        for (size_t index = 0; index < sizeof(endpoint.address.bytes); ++index)
        {
            endpoint.address.bytes[index] = address.address[index];
        }
    }
    return endpoint;
}

User::SocketAddress ToUserAddress(const Endpoint& endpoint)
{
    User::SocketAddress address = {.family = 0, .port = endpoint.port, .scope = 0, .address = {}};
    if (endpoint.address.version == IpVersion::V4)
    {
        address.family = User::SocketFamilyIpv4;
    }
    else if (endpoint.address.version == IpVersion::V6)
    {
        address.family = User::SocketFamilyIpv6;
    }
    for (size_t index = 0; index < sizeof(address.address); ++index)
    {
        address.address[index] = endpoint.address.bytes[index];
    }
    return address;
}

} // namespace FeatherOS::Network::Sockets
