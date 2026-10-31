#include <catch2/catch_test_macros.hpp>

#include "kernel/files.h"
#include "kernel/network/checksum.h"
#include "kernel/network/icmp.h"
#include "kernel/network/icmpv6.h"
#include "kernel/network/ip.h"
#include "kernel/network/socket.h"
#include "kernel/network/udp.h"
#include "support/network_stubs.h"
#include "support/scheduler_stubs.h"

#include <vector>

using namespace FeatherOS::Network;
namespace Sockets = FeatherOS::Network::Sockets;
namespace User = FeatherOS::User;

namespace
{

const IpAddress Gateway = MakeIpAddress(MakeIpv4(10, 0, 2, 2));
const IpAddress Ipv6Router = {.version = IpVersion::V6,
                              .bytes = {0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2}};

// Host tests cannot block, so every wait that would block is cut short.
bool AlwaysInterrupted()
{
    return true;
}

struct TestSocket
{
    TestSocket(bool ipv6, uint64_t protocol)
    {
        TestNetwork::Reset();
        TestScheduler::Reset();
        REQUIRE(Sockets::Create(ipv6 ? User::SocketFamilyIpv6 : User::SocketFamilyIpv4,
                                User::SocketDatagram, protocol, &socket) == 0);
    }

    ~TestSocket()
    {
        Sockets::Close(socket);
    }

    TestSocket(const TestSocket&) = delete;
    TestSocket& operator=(const TestSocket&) = delete;

    Sockets::Socket* socket = nullptr;
};

struct IcmpSocket : TestSocket
{
    explicit IcmpSocket(bool ipv6 = false)
        : TestSocket(ipv6, ipv6 ? User::SocketProtocolIcmpv6 : User::SocketProtocolIcmp)
    {
    }
};

struct UdpSocket : TestSocket
{
    explicit UdpSocket(bool ipv6 = false) : TestSocket(ipv6, 0)
    {
    }
};

uint64_t SendUdp(Sockets::Socket* socket, const std::vector<uint8_t>& payload,
                 const Sockets::Endpoint& destination)
{
    std::vector<uint8_t> copy = payload;
    return Sockets::SendTo(socket, copy.data(), copy.size(), &destination, nullptr);
}

std::vector<uint8_t> Echo(uint8_t type, uint16_t identifier, uint16_t sequence, size_t data = 4)
{
    std::vector<uint8_t> message(IcmpHeaderSize + data, 0xA5);
    message[0] = type;
    WriteBig16(message.data() + 6, sequence);
    RewriteIcmpEcho(message.data(), message.size(), type, identifier);
    return message;
}

uint64_t Send(Sockets::Socket* socket, std::vector<uint8_t> message,
              const IpAddress& destination = Gateway)
{
    const Sockets::Endpoint target = {.address = destination, .port = 0};
    return Sockets::SendTo(socket, message.data(), message.size(), &target, nullptr);
}

uint16_t SentIdentifier()
{
    REQUIRE(!TestNetwork::sent.empty());
    return ReadBig16(TestNetwork::sent.back().payload.data() + 4);
}

} // namespace

TEST_CASE("datagram sockets are UDP or the family's ICMP version")
{
    Sockets::Socket* socket = nullptr;
    REQUIRE(Sockets::Create(User::SocketFamilyIpv6, User::SocketDatagram,
                            User::SocketProtocolIcmp,
                            &socket) == User::SyscallErrorProtocolNotSupported);
    REQUIRE(Sockets::Create(User::SocketFamilyIpv4, User::SocketDatagram,
                            User::SocketProtocolIcmpv6,
                            &socket) == User::SyscallErrorProtocolNotSupported);
    REQUIRE(Sockets::Create(7, User::SocketDatagram, User::SocketProtocolIcmp, &socket) ==
            User::SyscallErrorFamilyNotSupported);
    REQUIRE(Sockets::Create(User::SocketFamilyIpv4, 1, User::SocketProtocolIcmp, &socket) ==
            User::SyscallErrorProtocolNotSupported);
    REQUIRE(Sockets::Create(User::SocketFamilyIpv4, User::SocketDatagram, 6, &socket) ==
            User::SyscallErrorProtocolNotSupported);
    REQUIRE(socket == nullptr);
    REQUIRE(Sockets::Create(User::SocketFamilyIpv4, User::SocketDatagram,
                            User::SocketProtocolUdp, &socket) == 0);
    Sockets::Close(socket);
    REQUIRE(Sockets::Create(User::SocketFamilyIpv6, User::SocketDatagram, 0, &socket) == 0);
    Sockets::Close(socket);
    REQUIRE(Sockets::Create(User::SocketFamilyIpv4,
                            User::SocketDatagram | User::SocketCloseOnExec,
                            User::SocketProtocolIcmp, &socket) == 0);
    Sockets::Close(socket);
    REQUIRE(Sockets::Create(User::SocketFamilyIpv6, User::SocketDatagram,
                            User::SocketProtocolIcmpv6, &socket) == 0);
    Sockets::Close(socket);
}

TEST_CASE("sending binds an identifier and computes the checksum")
{
    IcmpSocket icmp;
    REQUIRE(Send(icmp.socket, Echo(IcmpEchoRequest, 0, 1)) == IcmpHeaderSize + 4);
    REQUIRE(TestNetwork::sent.size() == 1);
    const TestNetwork::SentPacket& packet = TestNetwork::sent[0];
    REQUIRE(packet.destination == Gateway);
    REQUIRE(packet.protocol == 1);
    REQUIRE(packet.checksum_offset == NoChecksum);
    REQUIRE(InternetChecksum(packet.payload.data(), packet.payload.size()) == 0);
    const uint16_t identifier = SentIdentifier();
    REQUIRE(identifier != 0);

    // The identifier the program wrote is replaced, and stays the same for later requests.
    REQUIRE(Send(icmp.socket, Echo(IcmpEchoRequest, 999, 2)) == IcmpHeaderSize + 4);
    REQUIRE(SentIdentifier() == identifier);
}

TEST_CASE("only well-formed echo requests are sent")
{
    IcmpSocket icmp;
    REQUIRE(Send(icmp.socket, Echo(IcmpEchoReply, 0, 1)) == User::SyscallErrorInvalidArgument);
    REQUIRE(Send(icmp.socket, std::vector<uint8_t>(IcmpHeaderSize - 1, IcmpEchoRequest)) ==
            User::SyscallErrorInvalidArgument);
    REQUIRE(Send(icmp.socket, Echo(IcmpEchoRequest, 0, 1, Sockets::MaximumDatagramSize)) ==
            User::SyscallErrorMessageTooLong);
    IpAddress ipv6 = Gateway;
    ipv6.version = IpVersion::V6;
    REQUIRE(Send(icmp.socket, Echo(IcmpEchoRequest, 0, 1), ipv6) ==
            User::SyscallErrorFamilyNotSupported);
    REQUIRE(TestNetwork::sent.empty());
}

TEST_CASE("an IPv6 socket leaves the checksum to the IP layer, which knows the source")
{
    IcmpSocket icmp(true);
    std::vector<uint8_t> request(IcmpHeaderSize + 4, 0xA5);
    request[0] = Icmpv6EchoRequest;
    request[1] = 0;
    REQUIRE(Send(icmp.socket, request, Ipv6Router) == request.size());
    REQUIRE(TestNetwork::sent.size() == 1);
    REQUIRE(TestNetwork::sent[0].destination == Ipv6Router);
    REQUIRE(TestNetwork::sent[0].protocol == 58);
    REQUIRE(TestNetwork::sent[0].checksum_offset == Icmpv6ChecksumOffset);
    REQUIRE(SentIdentifier() != 0);

    REQUIRE(Send(icmp.socket, Echo(IcmpEchoRequest, 0, 1), Ipv6Router) ==
            User::SyscallErrorInvalidArgument);
    REQUIRE(Send(icmp.socket, request, Gateway) == User::SyscallErrorFamilyNotSupported);
    REQUIRE(TestNetwork::sent.size() == 1);
}

TEST_CASE("IP layer failures become socket errors")
{
    IcmpSocket icmp;
    TestNetwork::status = SendStatus::HostUnreachable;
    REQUIRE(Send(icmp.socket, Echo(IcmpEchoRequest, 0, 1)) == User::SyscallErrorHostUnreachable);
    TestNetwork::status = SendStatus::NoRoute;
    REQUIRE(Send(icmp.socket, Echo(IcmpEchoRequest, 0, 1)) ==
            User::SyscallErrorNetworkUnreachable);
    TestNetwork::status = SendStatus::NoBuffers;
    REQUIRE(Send(icmp.socket, Echo(IcmpEchoRequest, 0, 1)) == User::SyscallErrorNoBuffers);
    TestNetwork::status = SendStatus::NetworkDown;
    REQUIRE(Send(icmp.socket, Echo(IcmpEchoRequest, 0, 1)) == User::SyscallErrorNetworkDown);
}

TEST_CASE("binding claims an identifier for one socket")
{
    IcmpSocket first;
    IcmpSocket second;
    const Sockets::Endpoint any = {.address = MakeIpAddress(0), .port = 77};
    REQUIRE(Sockets::Bind(first.socket, any) == 0);
    REQUIRE(Sockets::Bind(first.socket, any) == User::SyscallErrorInvalidArgument);
    REQUIRE(Sockets::Bind(second.socket, any) == User::SyscallErrorAddressInUse);
    REQUIRE(Sockets::Bind(second.socket, {.address = Gateway, .port = 78}) ==
            User::SyscallErrorAddressNotAvailable);
    IpAddress ipv6 = MakeIpAddress(0);
    ipv6.version = IpVersion::V6;
    REQUIRE(Sockets::Bind(second.socket, {.address = ipv6, .port = 78}) ==
            User::SyscallErrorFamilyNotSupported);
    REQUIRE(Sockets::Bind(second.socket, {.address = TestNetwork::local_address, .port = 78}) ==
            0);

    REQUIRE(Send(first.socket, Echo(IcmpEchoRequest, 0, 1)) == IcmpHeaderSize + 4);
    REQUIRE(SentIdentifier() == 77);
}

TEST_CASE("an echo reply reaches the socket that owns its identifier")
{
    IcmpSocket first;
    IcmpSocket second;
    REQUIRE(Sockets::Bind(first.socket, {.address = MakeIpAddress(0), .port = 10}) == 0);
    REQUIRE(Sockets::Bind(second.socket, {.address = MakeIpAddress(0), .port = 11}) == 0);

    const std::vector<uint8_t> reply = Echo(IcmpEchoReply, 11, 3);
    Sockets::DeliverEcho(Gateway, 11, reply.data(), reply.size());
    REQUIRE(TestScheduler::wake_count == 1);

    uint8_t buffer[64];
    Sockets::Endpoint source{};
    REQUIRE(Sockets::ReceiveFrom(first.socket, buffer, sizeof(buffer), &source,
                                 AlwaysInterrupted) == User::SyscallErrorInterrupted);
    REQUIRE(Sockets::ReceiveFrom(second.socket, buffer, sizeof(buffer), &source,
                                 AlwaysInterrupted) == reply.size());
    REQUIRE(source.address == Gateway);
    REQUIRE(source.port == 11);
    REQUIRE(std::vector<uint8_t>(buffer, buffer + reply.size()) == reply);
}

TEST_CASE("replies for another identifier or IP version are dropped")
{
    IcmpSocket icmp;
    REQUIRE(Sockets::Bind(icmp.socket, {.address = MakeIpAddress(0), .port = 12}) == 0);
    const std::vector<uint8_t> reply = Echo(IcmpEchoReply, 12, 1);
    Sockets::DeliverEcho(Gateway, 13, reply.data(), reply.size());
    Sockets::DeliverEcho(Ipv6Router, 12, reply.data(), reply.size());
    REQUIRE(TestScheduler::wake_count == 0);
    uint8_t buffer[64];
    REQUIRE(Sockets::ReceiveFrom(icmp.socket, buffer, sizeof(buffer), nullptr,
                                 AlwaysInterrupted) == User::SyscallErrorInterrupted);
}

TEST_CASE("a full receive queue drops later datagrams, and short buffers truncate")
{
    IcmpSocket icmp;
    REQUIRE(Sockets::Bind(icmp.socket, {.address = MakeIpAddress(0), .port = 14}) == 0);
    for (uint16_t sequence = 0; sequence < Sockets::ReceiveQueueLength + 1; ++sequence)
    {
        const std::vector<uint8_t> reply = Echo(IcmpEchoReply, 14, sequence);
        Sockets::DeliverEcho(Gateway, 14, reply.data(), reply.size());
    }
    uint8_t buffer[64];
    for (uint16_t sequence = 0; sequence < Sockets::ReceiveQueueLength; ++sequence)
    {
        REQUIRE(Sockets::ReceiveFrom(icmp.socket, buffer, IcmpHeaderSize, nullptr,
                                     AlwaysInterrupted) == IcmpHeaderSize);
        REQUIRE(ReadBig16(buffer + 6) == sequence);
    }
    REQUIRE(Sockets::ReceiveFrom(icmp.socket, buffer, sizeof(buffer), nullptr,
                                 AlwaysInterrupted) == User::SyscallErrorInterrupted);
}

TEST_CASE("a receive gives up once its timeout passes")
{
    IcmpSocket icmp;
    REQUIRE(Sockets::SetReceiveTimeout(icmp.socket, 100) == 0);
    TestScheduler::ticks = 50;
    TestScheduler::on_wait = [] { TestScheduler::ticks += 6; };
    uint8_t buffer[64];
    REQUIRE(Sockets::ReceiveFrom(icmp.socket, buffer, sizeof(buffer), nullptr, nullptr) ==
            User::SyscallErrorWouldBlock);
    // 100 ms is 10 ticks at the stubs' 100 Hz.
    REQUIRE(TestScheduler::last_wait_deadline == 60);
    REQUIRE(TestScheduler::wait_count == 2);
    REQUIRE(Sockets::SetReceiveTimeout(icmp.socket, 25ULL * 60 * 60 * 1000) ==
            User::SyscallErrorInvalidArgument);
}

TEST_CASE("a UDP datagram gets a header from a dynamic port, and the IP layer its checksum")
{
    UdpSocket udp;
    const std::vector<uint8_t> payload = {1, 2, 3};
    REQUIRE(SendUdp(udp.socket, payload, {.address = Gateway, .port = 53}) == payload.size());
    REQUIRE(TestNetwork::sent.size() == 1);
    const TestNetwork::SentPacket& packet = TestNetwork::sent[0];
    REQUIRE(packet.destination == Gateway);
    REQUIRE(packet.protocol == IpProtocolUdp);
    REQUIRE(packet.checksum_offset == UdpChecksumOffset);
    UdpDatagram datagram{};
    REQUIRE(ParseUdpDatagram(packet.payload.data(), packet.payload.size(), &datagram));
    REQUIRE(datagram.source_port >= 49152);
    REQUIRE(datagram.destination_port == 53);
    REQUIRE(std::vector<uint8_t>(datagram.payload, datagram.payload + datagram.payload_length) ==
            payload);

    // The port stays with the socket.
    const uint16_t port = datagram.source_port;
    REQUIRE(SendUdp(udp.socket, {}, {.address = Gateway, .port = 53}) == 0);
    REQUIRE(ParseUdpDatagram(TestNetwork::sent[1].payload.data(),
                             TestNetwork::sent[1].payload.size(), &datagram));
    REQUIRE(datagram.source_port == port);
    REQUIRE(datagram.payload_length == 0);
}

TEST_CASE("UDP needs a destination port and the socket's IP version")
{
    UdpSocket udp(true);
    REQUIRE(SendUdp(udp.socket, {1}, {.address = Ipv6Router, .port = 0}) ==
            User::SyscallErrorInvalidArgument);
    REQUIRE(SendUdp(udp.socket, {1}, {.address = Gateway, .port = 53}) ==
            User::SyscallErrorFamilyNotSupported);
    REQUIRE(SendUdp(udp.socket, std::vector<uint8_t>(Sockets::MaximumDatagramSize + 1),
                    {.address = Ipv6Router, .port = 53}) == User::SyscallErrorMessageTooLong);
    REQUIRE(TestNetwork::sent.empty());
    TestNetwork::status = SendStatus::TooLong;
    REQUIRE(SendUdp(udp.socket, {1}, {.address = Ipv6Router, .port = 53}) ==
            User::SyscallErrorMessageTooLong);
}

TEST_CASE("UDP and ICMP ports are separate, as are the IP versions")
{
    UdpSocket udp;
    UdpSocket udp_ipv6(true);
    IcmpSocket icmp;
    IpAddress ipv6_any = MakeIpAddress(0);
    ipv6_any.version = IpVersion::V6;
    REQUIRE(Sockets::Bind(udp.socket, {.address = MakeIpAddress(0), .port = 68}) == 0);
    REQUIRE(Sockets::Bind(udp_ipv6.socket, {.address = ipv6_any, .port = 68}) == 0);
    REQUIRE(Sockets::Bind(icmp.socket, {.address = MakeIpAddress(0), .port = 68}) == 0);

    UdpSocket second;
    REQUIRE(Sockets::Bind(second.socket, {.address = MakeIpAddress(0), .port = 68}) ==
            User::SyscallErrorAddressInUse);
}

TEST_CASE("a UDP datagram reaches the socket bound to its port and address")
{
    UdpSocket any;
    UdpSocket local;
    REQUIRE(Sockets::Bind(any.socket, {.address = MakeIpAddress(0), .port = 68}) == 0);
    REQUIRE(Sockets::Bind(local.socket, {.address = TestNetwork::local_address, .port = 69}) ==
            0);
    const std::vector<uint8_t> payload = {7, 8, 9};
    const Sockets::Endpoint server = {.address = Gateway, .port = 67};
    const IpAddress broadcast = MakeIpAddress(0xFFFFFFFF);

    Sockets::DeliverUdp(server, {.address = broadcast, .port = 68}, payload.data(),
                        payload.size());
    Sockets::DeliverUdp(server, {.address = broadcast, .port = 69}, payload.data(),
                        payload.size());
    Sockets::DeliverUdp({.address = Ipv6Router, .port = 67}, {.address = Ipv6Router, .port = 68},
                        payload.data(), payload.size());
    REQUIRE(TestScheduler::wake_count == 1);

    uint8_t buffer[16];
    Sockets::Endpoint source{};
    REQUIRE(Sockets::ReceiveFrom(any.socket, buffer, sizeof(buffer), &source,
                                 AlwaysInterrupted) == payload.size());
    REQUIRE(source.address == Gateway);
    REQUIRE(source.port == 67);
    REQUIRE(std::vector<uint8_t>(buffer, buffer + payload.size()) == payload);
    REQUIRE(Sockets::ReceiveFrom(local.socket, buffer, sizeof(buffer), &source,
                                 AlwaysInterrupted) == User::SyscallErrorInterrupted);

    Sockets::DeliverUdp(server, {.address = TestNetwork::local_address, .port = 69},
                        payload.data(), payload.size());
    REQUIRE(Sockets::ReceiveFrom(local.socket, buffer, sizeof(buffer), &source,
                                 AlwaysInterrupted) == payload.size());
}

TEST_CASE("echo replies and UDP datagrams do not cross")
{
    UdpSocket udp;
    IcmpSocket icmp;
    REQUIRE(Sockets::Bind(udp.socket, {.address = MakeIpAddress(0), .port = 30}) == 0);
    REQUIRE(Sockets::Bind(icmp.socket, {.address = MakeIpAddress(0), .port = 31}) == 0);
    const std::vector<uint8_t> reply = Echo(IcmpEchoReply, 30, 1);
    Sockets::DeliverEcho(Gateway, 30, reply.data(), reply.size());
    Sockets::DeliverUdp({.address = Gateway, .port = 9}, {.address = Gateway, .port = 31},
                        reply.data(), reply.size());
    REQUIRE(TestScheduler::wake_count == 0);
}

TEST_CASE("only a port with a socket takes UDP datagrams")
{
    UdpSocket udp;
    REQUIRE(Sockets::Bind(udp.socket, {.address = MakeIpAddress(0), .port = 30}) == 0);
    const std::vector<uint8_t> payload = {1};
    const Sockets::Endpoint local = {.address = TestNetwork::local_address, .port = 30};
    REQUIRE(Sockets::DeliverUdp({.address = Gateway, .port = 9}, local, payload.data(),
                                payload.size()));
    REQUIRE_FALSE(Sockets::DeliverUdp({.address = Gateway, .port = 9},
                                      {.address = TestNetwork::local_address, .port = 31},
                                      payload.data(), payload.size()));

    // A connected socket takes only its peer's datagrams.
    REQUIRE(Sockets::Connect(udp.socket, {.address = Gateway, .port = 53}, nullptr) == 0);
    REQUIRE_FALSE(Sockets::DeliverUdp({.address = Gateway, .port = 9}, local, payload.data(),
                                      payload.size()));
    REQUIRE(Sockets::DeliverUdp({.address = Gateway, .port = 53}, local, payload.data(),
                                payload.size()));
}

TEST_CASE("a port unreachable error refuses the next call on a connected UDP socket")
{
    UdpSocket udp;
    const Sockets::Endpoint peer = {.address = Gateway, .port = 9};
    REQUIRE(Sockets::Connect(udp.socket, peer, nullptr) == 0);
    Sockets::Endpoint local{};
    REQUIRE(Sockets::Name(udp.socket, false, &local) == 0);
    local.address = TestNetwork::local_address;

    Sockets::DeliverPortUnreachable(local, {.address = Gateway, .port = 10});
    REQUIRE(TestScheduler::wake_count == 0);
    Sockets::DeliverPortUnreachable(local, peer);
    REQUIRE(TestScheduler::wake_count == 1);
    REQUIRE((Sockets::PollEvents(udp.socket) & User::PollError) != 0);

    uint8_t buffer[8];
    REQUIRE(Sockets::ReceiveFrom(udp.socket, buffer, sizeof(buffer), nullptr,
                                 AlwaysInterrupted) == User::SyscallErrorConnectionRefused);
    REQUIRE((Sockets::PollEvents(udp.socket) & User::PollError) == 0);
    REQUIRE(Sockets::ReceiveFrom(udp.socket, buffer, sizeof(buffer), nullptr,
                                 AlwaysInterrupted) == User::SyscallErrorInterrupted);

    Sockets::DeliverPortUnreachable(local, peer);
    std::vector<uint8_t> message = {1};
    REQUIRE(Sockets::SendTo(udp.socket, message.data(), message.size(), nullptr, nullptr) ==
            User::SyscallErrorConnectionRefused);
    REQUIRE(TestNetwork::sent.empty());
    REQUIRE(Sockets::SendTo(udp.socket, message.data(), message.size(), nullptr, nullptr) ==
            message.size());
}

TEST_CASE("a socket that is not connected ignores port unreachable errors")
{
    UdpSocket udp;
    REQUIRE(Sockets::Bind(udp.socket, {.address = MakeIpAddress(0), .port = 30}) == 0);
    Sockets::DeliverPortUnreachable({.address = TestNetwork::local_address, .port = 30},
                                    {.address = Gateway, .port = 9});
    REQUIRE(TestScheduler::wake_count == 0);
    REQUIRE(SendUdp(udp.socket, {1}, {.address = Gateway, .port = 9}) == 1);
}

TEST_CASE("addresses convert between the syscall ABI and the stack")
{
    const User::SocketAddress ipv4 = {
        .family = User::SocketFamilyIpv4, .port = 7, .scope = 0, .address = {10, 0, 2, 2}};
    const Sockets::Endpoint endpoint = Sockets::FromUserAddress(ipv4);
    REQUIRE(endpoint.address == Gateway);
    REQUIRE(endpoint.port == 7);
    const User::SocketAddress back = Sockets::ToUserAddress(endpoint);
    REQUIRE(back.family == User::SocketFamilyIpv4);
    REQUIRE(back.port == 7);
    REQUIRE(back.address[0] == 10);
    REQUIRE(back.address[3] == 2);

    User::SocketAddress ipv6 = {.family = User::SocketFamilyIpv6, .port = 0, .scope = 0,
                                .address = {}};
    ipv6.address[0] = 0xFE;
    ipv6.address[15] = 2;
    const Sockets::Endpoint ipv6_endpoint = Sockets::FromUserAddress(ipv6);
    REQUIRE(ipv6_endpoint.address.version == IpVersion::V6);
    REQUIRE(ipv6_endpoint.address.bytes[15] == 2);
    REQUIRE(Sockets::ToUserAddress(ipv6_endpoint).family == User::SocketFamilyIpv6);

    REQUIRE(Sockets::FromUserAddress({.family = 1, .port = 0, .scope = 0, .address = {}})
                .address.version == IpVersion::None);
}

TEST_CASE("a socket is a descriptor")
{
    FeatherOS::Files::Table table{};
    FeatherOS::Files::OpenConsole(&table);
    const uint64_t descriptor = FeatherOS::Files::CreateSocket(
        &table, User::SocketFamilyIpv4, User::SocketDatagram | User::SocketCloseOnExec,
        User::SocketProtocolIcmp);
    REQUIRE(descriptor == 3);
    REQUIRE(FeatherOS::Files::KindOf(&table, descriptor) == FeatherOS::Files::Kind::Socket);
    REQUIRE(FeatherOS::Files::DescriptorFlags(&table, descriptor, false, 0) ==
            User::DescriptorCloseOnExec);

    Sockets::Socket* socket = nullptr;
    REQUIRE(FeatherOS::Files::FindSocket(&table, descriptor, &socket) == 0);
    REQUIRE(socket != nullptr);
    REQUIRE(FeatherOS::Files::FindSocket(&table, 0, &socket) == User::SyscallErrorNotSocket);
    REQUIRE(FeatherOS::Files::FindSocket(&table, 9, &socket) ==
            User::SyscallErrorBadDescriptor);

    User::FileStatus status{};
    REQUIRE(FeatherOS::Files::StatDescriptor(&table, descriptor, &status) == 0);
    REQUIRE(status.type == User::FileType::Socket);
    REQUIRE(FeatherOS::Files::Seek(&table, descriptor, 0, User::SeekFromStart) ==
            User::SyscallErrorNotSeekable);

    REQUIRE(FeatherOS::Files::CreateSocket(&table, User::SocketFamilyIpv4, 1,
                                           User::SocketProtocolIcmp) ==
            User::SyscallErrorProtocolNotSupported);
    REQUIRE(FeatherOS::Files::Close(&table, descriptor) == 0);
    REQUIRE(FeatherOS::Files::CountOpen(&table) == 3);
    FeatherOS::Files::CloseAll(&table);
}

TEST_CASE("a non-blocking datagram receive fails at once, and the error option takes ICMP errors")
{
    UdpSocket udp;
    const Sockets::Endpoint peer = {.address = Gateway, .port = 9};
    REQUIRE(Sockets::Connect(udp.socket, peer, nullptr) == 0);
    uint8_t buffer[8];
    // A wait would end in SyscallErrorInterrupted instead.
    REQUIRE(Sockets::ReceiveFrom(udp.socket, buffer, sizeof(buffer), nullptr, AlwaysInterrupted,
                                 true) == User::SyscallErrorWouldBlock);

    Sockets::Endpoint local{};
    REQUIRE(Sockets::Name(udp.socket, false, &local) == 0);
    local.address = TestNetwork::local_address;
    REQUIRE(Sockets::TakeError(udp.socket) == 0);
    Sockets::DeliverPortUnreachable(local, peer);
    REQUIRE(Sockets::TakeError(udp.socket) == User::SyscallErrorConnectionRefused);
    REQUIRE(Sockets::TakeError(udp.socket) == 0);

    REQUIRE(Sockets::ReceiveTimeout(udp.socket) == 0);
    REQUIRE(Sockets::SetReceiveTimeout(udp.socket, 1500) == 0);
    REQUIRE(Sockets::ReceiveTimeout(udp.socket) == 1500);
}

TEST_CASE("a socket descriptor can be non-blocking")
{
    FeatherOS::Files::Table table{};
    const uint64_t descriptor = FeatherOS::Files::CreateSocket(
        &table, User::SocketFamilyIpv6, User::SocketStream | User::SocketNonBlocking, 0);
    REQUIRE(descriptor == 0);
    REQUIRE(FeatherOS::Files::DescriptorFlags(&table, descriptor, false, 0) == 0);
    REQUIRE(FeatherOS::Files::StatusFlags(&table, descriptor, false, 0) ==
            (User::OpenRead | User::OpenWrite | User::OpenNonBlocking));
    Sockets::Socket* socket = nullptr;
    bool nonblocking = false;
    REQUIRE(FeatherOS::Files::FindSocket(&table, descriptor, &socket, &nonblocking) == 0);
    REQUIRE(nonblocking);
    REQUIRE(FeatherOS::Files::StatusFlags(&table, descriptor, true, 0) ==
            (User::OpenRead | User::OpenWrite));
    REQUIRE(FeatherOS::Files::FindSocket(&table, descriptor, &socket, &nonblocking) == 0);
    REQUIRE_FALSE(nonblocking);
    FeatherOS::Files::CloseAll(&table);
}
