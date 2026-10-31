#ifndef FEATHEROS_NETWORK_SOCKET_H
#define FEATHEROS_NETWORK_SOCKET_H

#include "address.h"
#include "tcp.h"
#include "user/syscall_abi.h"

#include <stddef.h>
#include <stdint.h>

// Sockets behind socket descriptors: TCP sockets, UDP sockets, and ICMP datagram sockets, which
// send echo requests and receive the replies, like Linux's and macOS's unprivileged ping sockets.
// A socket holds one IP version. Functions return the value a syscall hands back: a result, or a
// User::SyscallError code. Buffers are kernel memory.
//
// TCP output all goes through one task, which the receive task and callers wake: sending can wait
// on address resolution, which only the receive task can complete. A closed TCP socket stays
// behind until its connection has closed, and that task frees it.
namespace FeatherOS::Network::Sockets
{

struct Socket;

// Datagrams a socket holds until they are received; more are dropped.
constexpr uint32_t ReceiveQueueLength = 8;
constexpr size_t MaximumDatagramSize = 1480;

// For an ICMP socket, the port is the echo identifier.
struct Endpoint
{
    IpAddress address;
    uint16_t port;
};

// True once the caller should stop waiting, such as after Ctrl+C.
using InterruptCheck = bool (*)();

// Calls that take nonblocking fail with SyscallErrorWouldBlock where they would wait, unless noted.

// type may include User::SocketCloseOnExec and User::SocketNonBlocking, which the caller handles.
uint64_t Create(uint64_t family, uint64_t type, uint64_t protocol, Socket** socket);
// A TCP connection with unread data is reset; otherwise it sends what is buffered and then FIN.
void Close(Socket* socket);
bool IsStream(const Socket* socket);
// Port 0 picks a free one; the address must be unspecified or local. A UDP socket bound to an
// address receives only what is sent to it. TCP binds as if with SO_REUSEADDR: only a socket
// that is bound but not connected, such as a listener, holds the port against others.
uint64_t Bind(Socket* socket, const Endpoint& local);
// TCP opens the connection and waits until it is established or failed; non-blocking, it fails
// with SyscallErrorInProgress instead, and the connection opens in the background. A datagram
// socket records the peer: sends without a destination go to it, and only its datagrams are
// received.
uint64_t Connect(Socket* socket, const Endpoint& remote, InterruptCheck interrupted,
                 bool nonblocking = false);
uint64_t Listen(Socket* socket, uint64_t backlog);
// Waits for an established connection on a listening socket and hands it out as a new socket.
uint64_t Accept(Socket* socket, Socket** connection, Endpoint* peer, InterruptCheck interrupted,
                bool nonblocking = false);
// Without a destination, to the connected peer. TCP waits until all of message is buffered for
// sending, or non-blocking what fits, and fails with SyscallErrorBrokenPipe once the socket can no
// longer send. A datagram socket binds a free port on first use, and never waits for space, though
// it may wait for address resolution. For an ICMP socket, message is an echo request of the
// socket's IP version; its identifier becomes the socket's port and its checksum is computed here,
// both in place.
uint64_t SendTo(Socket* socket, uint8_t* message, uint64_t length, const Endpoint* destination,
                InterruptCheck interrupted, bool nonblocking = false);
// Waits for data. TCP returns what is buffered, up to capacity, and 0 at the end of the stream;
// a datagram socket copies what fits of one datagram and drops the rest. Fails with
// SyscallErrorWouldBlock once the receive timeout passes. source may be null.
uint64_t ReceiveFrom(Socket* socket, uint8_t* buffer, uint64_t capacity, Endpoint* source,
                     InterruptCheck interrupted, bool nonblocking = false);
// how is a User::SocketShutdown* value.
uint64_t Shutdown(Socket* socket, uint64_t how);
// The local address and port, or with peer the connected peer's.
uint64_t Name(Socket* socket, bool peer, Endpoint* endpoint);
// User::Poll* events.
int16_t PollEvents(Socket* socket);
// 0 waits forever.
uint64_t SetReceiveTimeout(Socket* socket, uint64_t milliseconds);
uint64_t ReceiveTimeout(Socket* socket);
// User::SocketOptionError: a failed TCP connection's error, which stays, or a UDP socket's from an
// ICMP message, which this takes; 0 for none.
uint64_t TakeError(Socket* socket);

// From the IP layer: a checked echo reply, for the ICMP socket of its version bound to its
// identifier.
void DeliverEcho(const IpAddress& source, uint16_t identifier, const uint8_t* message,
                 size_t length);
// From the IP layer: a checked UDP payload, for the socket bound to the destination's port.
// Returns false when no socket takes datagrams there.
bool DeliverUdp(const Endpoint& source, const Endpoint& destination, const uint8_t* payload,
                size_t length);
// From the IP layer: a port unreachable error for a UDP datagram from local to remote. A socket
// connected to remote fails its next send or receive with SyscallErrorConnectionRefused.
void DeliverPortUnreachable(const Endpoint& local, const Endpoint& remote);
// From the IP layer: a checked TCP segment to one of our addresses. Without a connection or a
// listener for it, a reset is sent back.
void DeliverTcp(const Endpoint& source, const Endpoint& destination, const TcpSegment& segment);

// For a link that went down: every TCP connection to an address other than a loopback one
// closes with SyscallErrorNetworkDown, sending nothing more, and queued resets are dropped.
void DropExternalConnections();

// Starts the task that sends TCP segments and runs TCP timers.
bool StartTcp();
// One round of that task's work at now, in milliseconds: sends every segment that is due and
// frees closed sockets. Returns when the next timer is due, or 0 for none.
uint64_t ServiceTcp(uint64_t now);

// Between the syscall ABI's addresses and the stack's. An unknown family gives IpVersion::None.
Endpoint FromUserAddress(const User::SocketAddress& address);
User::SocketAddress ToUserAddress(const Endpoint& endpoint);

} // namespace FeatherOS::Network::Sockets

#endif
