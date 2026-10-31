#include <catch2/catch_test_macros.hpp>

#include "kernel/network/socket.h"
#include "kernel/network/tcp.h"
#include "kernel/network/tcp_connection.h"
#include "support/network_stubs.h"
#include "support/scheduler_stubs.h"

#include <string>
#include <vector>

using namespace FeatherOS::Network;
namespace Sockets = FeatherOS::Network::Sockets;
namespace User = FeatherOS::User;

namespace
{

const IpAddress Peer = MakeIpAddress(MakeIpv4(10, 0, 2, 2));
constexpr uint16_t PeerPort = 7777;
constexpr uint32_t PeerIss = 9000;

// A wait that nothing would end gives up instead of spinning.
bool WaitedTooLong()
{
    return TestScheduler::wait_count > 20;
}

struct SentSegment
{
    IpAddress destination;
    TcpSegment fields;
    std::string payload;
};

std::vector<SentSegment> TakeSent()
{
    std::vector<SentSegment> segments;
    for (const TestNetwork::SentPacket& packet : TestNetwork::sent)
    {
        REQUIRE(packet.protocol == IpProtocolTcp);
        REQUIRE(packet.checksum_offset == TcpChecksumOffset);
        SentSegment segment{};
        segment.destination = packet.destination;
        REQUIRE(ParseTcpSegment(packet.payload.data(), packet.payload.size(), &segment.fields));
        segment.payload.assign(reinterpret_cast<const char*>(segment.fields.payload),
                               segment.fields.payload_length);
        segment.fields.payload = nullptr;
        segments.push_back(segment);
    }
    TestNetwork::sent.clear();
    return segments;
}

std::vector<SentSegment> Service()
{
    Sockets::ServiceTcp(TestScheduler::ticks * 10);
    return TakeSent();
}

void Deliver(uint16_t local_port, uint32_t sequence, uint32_t acknowledgment, uint8_t flags,
             const std::string& payload = "", const IpAddress& source = Peer,
             const IpAddress& destination = TestNetwork::local_address)
{
    TcpSegment segment{};
    segment.source_port = PeerPort;
    segment.destination_port = local_port;
    segment.sequence = sequence;
    segment.acknowledgment = acknowledgment;
    segment.flags = flags;
    segment.window = 8192;
    segment.payload = reinterpret_cast<const uint8_t*>(payload.data());
    segment.payload_length = payload.size();
    Sockets::DeliverTcp({.address = source, .port = PeerPort},
                        {.address = destination, .port = local_port}, segment);
}

struct StreamSocket
{
    explicit StreamSocket(bool ipv6 = false)
    {
        TestNetwork::Reset();
        TestScheduler::Reset();
        REQUIRE(Sockets::Create(ipv6 ? User::SocketFamilyIpv6 : User::SocketFamilyIpv4,
                                User::SocketStream, 0, &socket) == 0);
        REQUIRE(Sockets::IsStream(socket));
    }

    ~StreamSocket()
    {
        if (socket != nullptr)
        {
            Sockets::Close(socket);
        }
        TestScheduler::on_wait = nullptr;
        // Lets lingering connections time out, one retransmission a round, so later tests start
        // from no sockets.
        for (int round = 0; round < 16; ++round)
        {
            TestScheduler::ticks += 100000;
            Sockets::ServiceTcp(TestScheduler::ticks * 10);
        }
        TestNetwork::Reset();
    }

    StreamSocket(const StreamSocket&) = delete;
    StreamSocket& operator=(const StreamSocket&) = delete;

    // Connects to remote, which answers the SYN while Connect waits. Returns our port and ISS.
    void Connect(uint16_t* local_port, uint32_t* iss, const IpAddress& remote = Peer)
    {
        TestScheduler::on_wait = [&]
        {
            const std::vector<SentSegment> sent = Service();
            REQUIRE(sent.size() == 1);
            REQUIRE(sent[0].fields.flags == TcpSyn);
            REQUIRE(sent[0].destination == remote);
            *local_port = sent[0].fields.source_port;
            *iss = sent[0].fields.sequence;
            Deliver(*local_port, PeerIss, *iss + 1, TcpSyn | TcpAck, "", remote,
                    SourceAddressFor(remote));
            TestScheduler::on_wait = nullptr;
        };
        REQUIRE(Sockets::Connect(socket, {.address = remote, .port = PeerPort}, WaitedTooLong) ==
                0);
        REQUIRE(Service().size() == 1);
    }

    Sockets::Socket* socket = nullptr;
};

std::string Receive(Sockets::Socket* socket)
{
    std::string text(256, '\0');
    const uint64_t count = Sockets::ReceiveFrom(socket, reinterpret_cast<uint8_t*>(text.data()),
                                                text.size(), nullptr, WaitedTooLong);
    REQUIRE_FALSE(User::IsSyscallError(count));
    text.resize(count);
    return text;
}

uint64_t Send(Sockets::Socket* socket, std::string text)
{
    return Sockets::SendTo(socket, reinterpret_cast<uint8_t*>(text.data()), text.size(), nullptr,
                           WaitedTooLong);
}

} // namespace

TEST_CASE("a stream socket connects, exchanges data and sees the peer's FIN")
{
    StreamSocket tcp;
    uint16_t port = 0;
    uint32_t iss = 0;
    tcp.Connect(&port, &iss);
    REQUIRE(port >= 49152);

    Sockets::Endpoint name{};
    REQUIRE(Sockets::Name(tcp.socket, false, &name) == 0);
    REQUIRE(name.address == TestNetwork::local_address);
    REQUIRE(name.port == port);
    REQUIRE(Sockets::Name(tcp.socket, true, &name) == 0);
    REQUIRE(name.address == Peer);
    REQUIRE(name.port == PeerPort);
    REQUIRE(Sockets::PollEvents(tcp.socket) == User::PollWritable);

    REQUIRE(Send(tcp.socket, "hello") == 5);
    std::vector<SentSegment> sent = Service();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].payload == "hello");
    REQUIRE(sent[0].fields.sequence == iss + 1);

    Deliver(port, PeerIss + 1, iss + 6, TcpAck | TcpPush, "world");
    REQUIRE(Sockets::PollEvents(tcp.socket) == (User::PollReadable | User::PollWritable));
    REQUIRE(Receive(tcp.socket) == "world");
    Deliver(port, PeerIss + 6, iss + 6, TcpAck | TcpFin);
    REQUIRE(Receive(tcp.socket) == "");
    sent = Service();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.acknowledgment == PeerIss + 7);
}

TEST_CASE("a reset answering SYN fails the connect")
{
    StreamSocket tcp;
    TestScheduler::on_wait = []
    {
        const std::vector<SentSegment> sent = Service();
        REQUIRE(sent.size() == 1);
        Deliver(sent[0].fields.source_port, 0, sent[0].fields.sequence + 1, TcpReset | TcpAck);
    };
    REQUIRE(Sockets::Connect(tcp.socket, {.address = Peer, .port = PeerPort}, WaitedTooLong) ==
            User::SyscallErrorConnectionRefused);
    REQUIRE(Send(tcp.socket, "x") == User::SyscallErrorConnectionRefused);
}

TEST_CASE("a connect without a source address fails at once")
{
    StreamSocket tcp(true);
    const IpAddress saved = TestNetwork::local_ipv6_address;
    TestNetwork::local_ipv6_address = {.version = IpVersion::V6, .bytes = {}};
    const IpAddress router = {.version = IpVersion::V6, .bytes = {0xFE, 0xC0, 15, 2}};
    REQUIRE(Sockets::Connect(tcp.socket, {.address = router, .port = 80}, WaitedTooLong) ==
            User::SyscallErrorNetworkUnreachable);
    TestNetwork::local_ipv6_address = saved;
    REQUIRE(Sockets::Connect(tcp.socket, {.address = Peer, .port = 80}, WaitedTooLong) ==
            User::SyscallErrorFamilyNotSupported);
}

TEST_CASE("an interrupted connect gives up")
{
    StreamSocket tcp;
    REQUIRE(Sockets::Connect(tcp.socket, {.address = Peer, .port = PeerPort}, [] { return true; }) ==
            User::SyscallErrorInterrupted);
}

TEST_CASE("a segment for no socket is answered with a reset")
{
    StreamSocket unused;
    Deliver(4321, 77, 0, TcpSyn);
    std::vector<SentSegment> sent = Service();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].destination == Peer);
    REQUIRE(sent[0].fields.source_port == 4321);
    REQUIRE(sent[0].fields.destination_port == PeerPort);
    REQUIRE(sent[0].fields.flags == (TcpReset | TcpAck));
    REQUIRE(sent[0].fields.acknowledgment == 78);

    Deliver(4321, 77, 1234, TcpAck, "data");
    sent = Service();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.flags == TcpReset);
    REQUIRE(sent[0].fields.sequence == 1234);

    // A reset never answers a reset.
    Deliver(4321, 77, 1234, TcpReset);
    REQUIRE(Service().empty());
}

TEST_CASE("a listener accepts a connection once its handshake completes")
{
    StreamSocket listener;
    REQUIRE(Sockets::Bind(listener.socket,
                          {.address = MakeIpAddress(0), .port = 8080}) == 0);
    REQUIRE(Sockets::Listen(listener.socket, 4) == 0);
    REQUIRE(Sockets::PollEvents(listener.socket) == 0);

    Deliver(8080, PeerIss, 0, TcpSyn);
    std::vector<SentSegment> sent = Service();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.flags == (TcpSyn | TcpAck));
    REQUIRE(sent[0].fields.source_port == 8080);
    REQUIRE(sent[0].fields.acknowledgment == PeerIss + 1);
    REQUIRE(sent[0].fields.maximum_segment_size == 1460);
    const uint32_t iss = sent[0].fields.sequence;
    REQUIRE(Sockets::PollEvents(listener.socket) == 0);

    Deliver(8080, PeerIss + 1, iss + 1, TcpAck | TcpPush, "ping");
    REQUIRE(Sockets::PollEvents(listener.socket) == User::PollReadable);
    Sockets::Socket* connection = nullptr;
    Sockets::Endpoint peer{};
    REQUIRE(Sockets::Accept(listener.socket, &connection, &peer, WaitedTooLong) == 0);
    REQUIRE(peer.address == Peer);
    REQUIRE(peer.port == PeerPort);
    REQUIRE(Receive(connection) == "ping");
    REQUIRE(Send(connection, "pong") == 4);
    sent = Service();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].payload == "pong");
    REQUIRE(sent[0].fields.acknowledgment == PeerIss + 5);

    // With nothing more waiting, Accept times out like a receive.
    REQUIRE(Sockets::SetReceiveTimeout(listener.socket, 10) == 0);
    TestScheduler::on_wait = [] { ++TestScheduler::ticks; };
    Sockets::Socket* none = nullptr;
    REQUIRE(Sockets::Accept(listener.socket, &none, &peer, WaitedTooLong) ==
            User::SyscallErrorWouldBlock);
    TestScheduler::on_wait = nullptr;

    Sockets::Close(connection);
    sent = Service();
    REQUIRE(sent.size() == 1);
    REQUIRE((sent[0].fields.flags & TcpFin) != 0);
}

TEST_CASE("a full backlog drops SYNs, and closing the listener resets waiting connections")
{
    StreamSocket listener;
    REQUIRE(Sockets::Bind(listener.socket, {.address = MakeIpAddress(0), .port = 8081}) == 0);
    REQUIRE(Sockets::Listen(listener.socket, 1) == 0);
    Deliver(8081, PeerIss, 0, TcpSyn);
    std::vector<SentSegment> sent = Service();
    REQUIRE(sent.size() == 1);
    const uint32_t iss = sent[0].fields.sequence;
    Deliver(8081, PeerIss + 1, iss + 1, TcpAck);

    const IpAddress other = MakeIpAddress(MakeIpv4(10, 0, 2, 3));
    Deliver(8081, 100, 0, TcpSyn, "", other);
    REQUIRE(Service().empty());

    Sockets::Close(listener.socket);
    listener.socket = nullptr;
    sent = Service();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.flags == (TcpReset | TcpAck));
    REQUIRE(sent[0].fields.sequence == iss + 1);
}

TEST_CASE("closing a socket with unread data resets the connection")
{
    StreamSocket tcp;
    uint16_t port = 0;
    uint32_t iss = 0;
    tcp.Connect(&port, &iss);
    Deliver(port, PeerIss + 1, iss + 1, TcpAck, "unread");
    REQUIRE(Service().size() == 1);
    Sockets::Close(tcp.socket);
    tcp.socket = nullptr;
    const std::vector<SentSegment> sent = Service();
    REQUIRE(sent.size() == 1);
    REQUIRE((sent[0].fields.flags & TcpReset) != 0);
    // The connection is gone, so the peer's next segment gets a reset of its own.
    Deliver(port, PeerIss + 7, iss + 1, TcpAck, "more");
    REQUIRE(Service()[0].fields.flags == TcpReset);
}

TEST_CASE("a closed socket lingers until its connection has closed")
{
    StreamSocket tcp;
    uint16_t port = 0;
    uint32_t iss = 0;
    tcp.Connect(&port, &iss);
    Sockets::Close(tcp.socket);
    tcp.socket = nullptr;
    std::vector<SentSegment> sent = Service();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.flags == (TcpFin | TcpAck));

    Deliver(port, PeerIss + 1, iss + 2, TcpFin | TcpAck);
    sent = Service();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.acknowledgment == PeerIss + 2);
    // TIME-WAIT still answers the peer's FIN.
    Deliver(port, PeerIss + 1, iss + 2, TcpFin | TcpAck);
    REQUIRE(Service().size() == 1);

    TestScheduler::ticks += 6000;
    REQUIRE(Service().empty());
    Deliver(port, PeerIss + 2, iss + 2, TcpAck);
    REQUIRE(Service()[0].fields.flags == TcpReset);
}

TEST_CASE("shutting down the write side sends FIN and fails later sends")
{
    StreamSocket tcp;
    uint16_t port = 0;
    uint32_t iss = 0;
    tcp.Connect(&port, &iss);
    REQUIRE(Sockets::Shutdown(tcp.socket, User::SocketShutdownWrite) == 0);
    const std::vector<SentSegment> sent = Service();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.flags == (TcpFin | TcpAck));
    REQUIRE(Send(tcp.socket, "x") == User::SyscallErrorBrokenPipe);
    REQUIRE((Sockets::PollEvents(tcp.socket) & User::PollWritable) != 0);

    Deliver(port, PeerIss + 1, iss + 2, TcpAck, "still");
    REQUIRE(Receive(tcp.socket) == "still");
    REQUIRE(Sockets::Shutdown(tcp.socket, User::SocketShutdownRead) == 0);
    REQUIRE(Receive(tcp.socket) == "");
    REQUIRE(Sockets::Shutdown(tcp.socket, 3) == User::SyscallErrorInvalidArgument);
}

TEST_CASE("a reset connection reports the reset to readers, writers and poll")
{
    StreamSocket tcp;
    uint16_t port = 0;
    uint32_t iss = 0;
    tcp.Connect(&port, &iss);
    Deliver(port, PeerIss + 1, 0, TcpReset);
    REQUIRE(Sockets::PollEvents(tcp.socket) ==
            (User::PollError | User::PollHangUp | User::PollReadable | User::PollWritable));
    uint8_t byte;
    REQUIRE(Sockets::ReceiveFrom(tcp.socket, &byte, 1, nullptr, WaitedTooLong) ==
            User::SyscallErrorConnectionReset);
    REQUIRE(Send(tcp.socket, "x") == User::SyscallErrorConnectionReset);
}

TEST_CASE("TCP binds conflict only with sockets that are not connected")
{
    StreamSocket first;
    REQUIRE(Sockets::Bind(first.socket, {.address = MakeIpAddress(0), .port = 9000}) == 0);
    Sockets::Socket* second = nullptr;
    REQUIRE(Sockets::Create(User::SocketFamilyIpv4, User::SocketStream, User::SocketProtocolTcp,
                            &second) == 0);
    REQUIRE(Sockets::Bind(second, {.address = MakeIpAddress(0), .port = 9000}) ==
            User::SyscallErrorAddressInUse);
    // The same port number over UDP is a different port.
    Sockets::Socket* udp = nullptr;
    REQUIRE(Sockets::Create(User::SocketFamilyIpv4, User::SocketDatagram, 0, &udp) == 0);
    REQUIRE(Sockets::Bind(udp, {.address = MakeIpAddress(0), .port = 9000}) == 0);
    REQUIRE(Sockets::Listen(udp, 1) == User::SyscallErrorOperationNotSupported);
    Sockets::Close(udp);
    Sockets::Close(second);
    REQUIRE(Sockets::Create(User::SocketFamilyIpv4, User::SocketDatagram, User::SocketProtocolTcp,
                            &second) == User::SyscallErrorProtocolNotSupported);
}

TEST_CASE("a connected UDP socket sends to its peer and receives only from it")
{
    TestNetwork::Reset();
    TestScheduler::Reset();
    Sockets::Socket* udp = nullptr;
    REQUIRE(Sockets::Create(User::SocketFamilyIpv4, User::SocketDatagram, 0, &udp) == 0);
    uint8_t byte = 'x';
    REQUIRE(Sockets::SendTo(udp, &byte, 1, nullptr, nullptr) ==
            User::SyscallErrorDestinationRequired);
    REQUIRE(Sockets::Shutdown(udp, User::SocketShutdownRead) == User::SyscallErrorNotConnected);
    REQUIRE(Sockets::Connect(udp, {.address = Peer, .port = 53}, nullptr) == 0);
    REQUIRE(Sockets::SendTo(udp, &byte, 1, nullptr, nullptr) == 1);
    REQUIRE(TestNetwork::sent.size() == 1);
    REQUIRE(TestNetwork::sent[0].destination == Peer);

    Sockets::Endpoint local{};
    REQUIRE(Sockets::Name(udp, false, &local) == 0);
    const uint8_t data[] = {'a'};
    Sockets::DeliverUdp({.address = MakeIpAddress(MakeIpv4(10, 0, 2, 3)), .port = 53},
                        {.address = TestNetwork::local_address, .port = local.port}, data, 1);
    REQUIRE(Sockets::PollEvents(udp) == User::PollWritable);
    Sockets::DeliverUdp({.address = Peer, .port = 53},
                        {.address = TestNetwork::local_address, .port = local.port}, data, 1);
    REQUIRE(Sockets::PollEvents(udp) == (User::PollReadable | User::PollWritable));
    Sockets::Close(udp);
}

TEST_CASE("a stream socket that is not connected refuses data calls")
{
    StreamSocket tcp;
    uint8_t byte;
    REQUIRE(Sockets::ReceiveFrom(tcp.socket, &byte, 1, nullptr, nullptr) ==
            User::SyscallErrorNotConnected);
    REQUIRE(Send(tcp.socket, "x") == User::SyscallErrorNotConnected);
    REQUIRE(Sockets::Shutdown(tcp.socket, User::SocketShutdownBoth) ==
            User::SyscallErrorNotConnected);
    Sockets::Endpoint peer{};
    REQUIRE(Sockets::Name(tcp.socket, true, &peer) == User::SyscallErrorNotConnected);
    REQUIRE(Sockets::PollEvents(tcp.socket) == (User::PollWritable | User::PollHangUp));
    Sockets::Socket* connection = nullptr;
    REQUIRE(Sockets::Accept(tcp.socket, &connection, &peer, nullptr) ==
            User::SyscallErrorInvalidArgument);
}

TEST_CASE("a non-blocking connect opens the connection in the background")
{
    StreamSocket tcp;
    const Sockets::Endpoint peer = {.address = Peer, .port = PeerPort};
    REQUIRE(Sockets::Connect(tcp.socket, peer, WaitedTooLong, true) ==
            User::SyscallErrorInProgress);
    REQUIRE(Sockets::PollEvents(tcp.socket) == 0);
    REQUIRE(Sockets::TakeError(tcp.socket) == 0);
    REQUIRE(Sockets::Connect(tcp.socket, peer, WaitedTooLong, true) ==
            User::SyscallErrorAlreadyInProgress);
    std::vector<SentSegment> sent = Service();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.flags == TcpSyn);
    const uint16_t port = sent[0].fields.source_port;
    const uint32_t iss = sent[0].fields.sequence;
    Deliver(port, PeerIss, iss + 1, TcpSyn | TcpAck);
    REQUIRE(Sockets::PollEvents(tcp.socket) == User::PollWritable);
    REQUIRE(Sockets::TakeError(tcp.socket) == 0);
    REQUIRE(Sockets::Connect(tcp.socket, peer, WaitedTooLong, true) ==
            User::SyscallErrorAlreadyConnected);

    uint8_t byte;
    REQUIRE(Sockets::ReceiveFrom(tcp.socket, &byte, 1, nullptr, WaitedTooLong, true) ==
            User::SyscallErrorWouldBlock);
    // The peer acknowledges nothing, so the send buffer fills.
    std::vector<uint8_t> data(TcpMaximumBufferSize + 100, 'd');
    REQUIRE(Sockets::SendTo(tcp.socket, data.data(), data.size(), nullptr, WaitedTooLong, true) ==
            TcpMaximumBufferSize);
    REQUIRE(Sockets::SendTo(tcp.socket, data.data(), 1, nullptr, WaitedTooLong, true) ==
            User::SyscallErrorWouldBlock);
    REQUIRE(TestScheduler::wait_count == 0);
}

TEST_CASE("a refused non-blocking connect reports why through poll and the error option")
{
    StreamSocket tcp;
    const Sockets::Endpoint peer = {.address = Peer, .port = PeerPort};
    REQUIRE(Sockets::Connect(tcp.socket, peer, WaitedTooLong, true) ==
            User::SyscallErrorInProgress);
    const std::vector<SentSegment> sent = Service();
    REQUIRE(sent.size() == 1);
    Deliver(sent[0].fields.source_port, 0, sent[0].fields.sequence + 1, TcpReset | TcpAck);
    REQUIRE((Sockets::PollEvents(tcp.socket) & (User::PollError | User::PollWritable)) ==
            (User::PollError | User::PollWritable));
    REQUIRE(Sockets::TakeError(tcp.socket) == User::SyscallErrorConnectionRefused);
    REQUIRE(Sockets::Connect(tcp.socket, peer, WaitedTooLong, true) ==
            User::SyscallErrorConnectionRefused);
    REQUIRE(TestScheduler::wait_count == 0);
}

TEST_CASE("a non-blocking accept fails while no connection is ready")
{
    StreamSocket listener;
    REQUIRE(Sockets::Bind(listener.socket, {.address = MakeIpAddress(0), .port = 8082}) == 0);
    REQUIRE(Sockets::Listen(listener.socket, 4) == 0);
    Sockets::Socket* connection = nullptr;
    Sockets::Endpoint peer{};
    REQUIRE(Sockets::Accept(listener.socket, &connection, &peer, WaitedTooLong, true) ==
            User::SyscallErrorWouldBlock);

    Deliver(8082, PeerIss, 0, TcpSyn);
    const std::vector<SentSegment> sent = Service();
    REQUIRE(sent.size() == 1);
    Deliver(8082, PeerIss + 1, sent[0].fields.sequence + 1, TcpAck);
    REQUIRE(Sockets::Accept(listener.socket, &connection, &peer, WaitedTooLong, true) == 0);
    REQUIRE(peer.address == Peer);
    REQUIRE(Sockets::Accept(listener.socket, &connection, &peer, WaitedTooLong, true) ==
            User::SyscallErrorWouldBlock);
    REQUIRE(TestScheduler::wait_count == 0);
    Sockets::Close(connection);
}

TEST_CASE("taking the link down ends connections to other hosts but not loopback ones")
{
    const IpAddress loopback = MakeIpAddress(MakeIpv4(127, 0, 0, 1));
    StreamSocket external;
    uint16_t port = 0;
    uint32_t iss = 0;
    external.Connect(&port, &iss);
    StreamSocket local;
    local.Connect(&port, &iss, loopback);
    StreamSocket later;

    TestNetwork::link_up = false;
    Sockets::DropExternalConnections();
    REQUIRE(Service().empty());
    REQUIRE((Sockets::PollEvents(external.socket) & User::PollError) != 0);
    REQUIRE(Send(external.socket, "x") == User::SyscallErrorNetworkDown);
    REQUIRE(Sockets::TakeError(external.socket) == User::SyscallErrorNetworkDown);

    REQUIRE(Send(local.socket, "still") == 5);
    const std::vector<SentSegment> sent = Service();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].destination == loopback);
    REQUIRE(sent[0].payload == "still");

    REQUIRE(Sockets::Connect(later.socket, {.address = Peer, .port = PeerPort}, WaitedTooLong) ==
            User::SyscallErrorNetworkDown);
}
