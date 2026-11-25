#include <catch2/catch_test_macros.hpp>

#include "kernel/network/tcp.h"
#include "kernel/network/tcp_connection.h"
#include "user/syscall_abi.h"

#include <memory>
#include <string>
#include <vector>

using namespace FeatherOS::Network;
namespace User = FeatherOS::User;

// R5-05 (src/kernel/network/ip.cpp:249) and R5-06 (src/libc/netdb.cpp:298) are not reachable from
// here: ip.cpp and netdb.cpp are not in the host test target, so they are traces at the end of
// this file, not tests.

namespace
{

constexpr uint16_t LocalPort = 49152;
constexpr uint16_t RemotePort = 80;
constexpr uint16_t LocalMss = 1460;
constexpr uint32_t Iss = 1000;
constexpr uint32_t PeerIss = 5000;
constexpr size_t UnscaledWindow = 0xFFFF;

struct ReproHarness
{
    // Number of payload bytes in the segments sent; calls fn(payload_length) for each.
    template <typename Callback> void DrainRaw(Callback callback)
    {
        uint8_t buffer[TcpConnection::MaximumSegmentLength];
        for (size_t length = connection->NextSegment(now, buffer); length != 0;
             length = connection->NextSegment(now, buffer))
        {
            TcpSegment segment{};
            REQUIRE(ParseTcpSegment(buffer, length, &segment));
            callback(segment);
        }
    }

    size_t Drain()
    {
        size_t payload = 0;
        DrainRaw([&payload](const TcpSegment& segment) { payload += segment.payload_length; });
        return payload;
    }

    ~ReproHarness()
    {
        connection->Release();
    }

    void Deliver(uint32_t sequence, uint32_t acknowledgment, uint8_t flags,
                 const std::string& payload = "", uint16_t window = 8192)
    {
        TcpSegment segment{};
        segment.source_port = RemotePort;
        segment.destination_port = LocalPort;
        segment.sequence = sequence;
        segment.acknowledgment = acknowledgment;
        segment.flags = flags;
        segment.window = window;
        segment.payload = reinterpret_cast<const uint8_t*>(payload.data());
        segment.payload_length = payload.size();
        connection->Receive(segment, now);
    }

    void Establish(uint16_t window = 8192)
    {
        connection->Connect(LocalPort, RemotePort, Iss, LocalMss);
        Drain();
        Deliver(PeerIss, Iss + 1, TcpSyn | TcpAck, "", window);
        REQUIRE(connection->State() == TcpState::Established);
        Drain();
    }

    size_t Write(const std::string& text)
    {
        return connection->Write(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }

    std::unique_ptr<TcpConnection> connection = std::make_unique<TcpConnection>();
    uint64_t now = 0;
};

} // namespace

// The peer has filled our receive window exactly and probes with one byte past the edge, so its
// own sequence number (and that of every ACK it sends) is receive_next + 1. The ACK for our data
// must still be read.
TEST_CASE("R5-02 an ACK for our data is processed while our receive window is closed",
          "[.repro][R5-02]")
{
    ReproHarness tcp;
    tcp.Establish();
    REQUIRE(tcp.Write("hello") == 5);
    REQUIRE(tcp.Drain() == 5);
    REQUIRE(tcp.connection->Writable() == TcpMaximumBufferSize - 5);

    uint32_t sequence = PeerIss + 1;
    while (tcp.connection->Readable() < UnscaledWindow)
    {
        const size_t room = UnscaledWindow - tcp.connection->Readable();
        const size_t chunk = room < 1460 ? room : 1460;
        tcp.Deliver(sequence, Iss + 1, TcpAck, std::string(chunk, 'z'));
        sequence += static_cast<uint32_t>(chunk);
    }
    tcp.Drain();

    // The peer's pure ACK of our five bytes, sent from receive_next + 1 after a window probe.
    tcp.Deliver(sequence + 1, Iss + 1 + 5, TcpAck, "", 8192);
    tcp.Drain();
    REQUIRE(tcp.connection->Writable() == TcpMaximumBufferSize);
}

TEST_CASE("R5-03 unanswered zero-window probes eventually reset the connection", "[.repro][R5-03]")
{
    ReproHarness tcp;
    tcp.Establish(0);
    REQUIRE(tcp.Write("abc") == 3);
    REQUIRE(tcp.Drain() == 0);
    REQUIRE(tcp.connection->Deadline() != 0);

    // The peer never answers any probe. DataRetries is 8, so 100 unanswered probes is generous.
    for (int probe = 0; probe < 100 && tcp.connection->Deadline() != 0; ++probe)
    {
        tcp.now = tcp.connection->Deadline();
        tcp.connection->OnTimer(tcp.now);
        tcp.Drain();
    }
    REQUIRE(tcp.connection->State() == TcpState::Closed);
    REQUIRE(tcp.connection->Error() == User::SyscallErrorTimedOut);
}

TEST_CASE("R5-03 an orphaned connection with a closed peer window is freed", "[.repro][R5-03]")
{
    ReproHarness tcp;
    tcp.Establish(0);
    REQUIRE(tcp.Write("abc") == 3);
    tcp.connection->Close();
    tcp.Drain();
    tcp.connection->Orphan(tcp.now);

    for (int probe = 0; probe < 100 && tcp.connection->Deadline() != 0; ++probe)
    {
        tcp.now = tcp.connection->Deadline();
        tcp.connection->OnTimer(tcp.now);
        tcp.Drain();
    }
    // ServiceTcp frees an orphaned socket only once Idle() holds.
    REQUIRE(tcp.connection->Idle());
}

// Acknowledging 2^32 - 1 data bytes leaves send_unacknowledged equal to the initial sequence
// number again (the SYN took one number). The next ACK is then mistaken for one that covers the
// SYN and one acknowledged byte stays in the send ring.
TEST_CASE("R5-04 the SYN adjustment does not fire again after the sequence space wraps",
          "[.repro][R5-04]")
{
    ReproHarness tcp;
    tcp.Establish(0xFFFF);

    constexpr uint64_t WrapBytes = 0x100000000ULL - 1;
    constexpr uint64_t Target = WrapBytes + 10;
    const std::string chunk(UnscaledWindow, 'x');
    uint64_t written_total = 0;
    uint64_t sent_total = 0;
    uint64_t acked_total = 0;
    const auto acknowledge = [&](uint64_t upto)
    {
        tcp.Deliver(PeerIss + 1, static_cast<uint32_t>(Iss + 1 + upto), TcpAck, "", 0xFFFF);
        acked_total = upto;
    };

    uint64_t rounds = 0;
    while (acked_total < Target)
    {
        REQUIRE(++rounds < 400000);
        const uint64_t room = Target - written_total;
        if (room != 0)
        {
            written_total += tcp.connection->Write(
                reinterpret_cast<const uint8_t*>(chunk.data()),
                static_cast<size_t>(room < chunk.size() ? room : chunk.size()));
        }
        tcp.DrainRaw([&sent_total](const TcpSegment& segment)
                     { sent_total += segment.payload_length; });
        if (acked_total < WrapBytes)
        {
            const uint64_t upto = sent_total < WrapBytes ? sent_total : WrapBytes;
            if (upto > acked_total)
            {
                acknowledge(upto);
            }
            continue;
        }
        // send_unacknowledged is now back at the initial sequence number.
        if (sent_total > acked_total)
        {
            acknowledge(sent_total);
            REQUIRE(tcp.connection->Writable() ==
                    TcpMaximumBufferSize - (written_total - acked_total));
            break;
        }
    }
}

// R5-05 trace (trace-only: src/kernel/network/ip.cpp is not compiled into the host tests).
//  1. ReceiveIpv4 (ip.cpp:482-488) parses a frame addressed to our MAC with source 127.0.0.1 and
//     destination = our configured address; ParseIpv4Packet (ipv4.cpp:16-42) never reads the
//     source.
//  2. HandleIpv4 (ip.cpp:244) is entered with source_mac != nullptr. to_us (ip.cpp:249-250) is
//     true from the destination alone; the drop test (ip.cpp:253-257) is destination-only too.
//  3. UDP: HandleUdp (ip.cpp:262-264) -> Sockets::DeliverUdp (socket.cpp:1071-1092) queues the
//     datagram on a wildcard-bound socket with peer address 127.0.0.1. The only loopback-source
//     filter, IsUnicastIpv4Source (ip.cpp:234-241), runs only to decide on the port-unreachable
//     reply (ip.cpp:266). The IPv6 path drops ::1 sources from the wire at ip.cpp:327-331.
//  4. TCP: HandleTcp (ip.cpp:273) -> Sockets::DeliverTcp with the same unchecked source.
//
// R5-06 trace (trace-only: src/libc/netdb.cpp is not in the host test target).
//  1. AskOverUdp (netdb.cpp:285) opens an unconnected UDP socket, SetReceiveTimeout once
//     (netdb.cpp:293), sendto the query (netdb.cpp:294-295).
//  2. The for(;;) at netdb.cpp:298 calls recvfrom (netdb.cpp:302-303); it leaves only when
//     recvfrom fails (305-308) or Parse returns something other than Unrelated (313-317).
//  3. A datagram from another endpoint takes `continue` (309-312); one from the server with a
//     wrong ID parses as Unrelated and falls to the next iteration (318). Neither counts or checks
//     a deadline.
//  4. In the kernel ReceiveFrom (socket.cpp:894) computes ReceiveDeadline (socket.cpp:286-293) as
//     now + timeout on every call, so each stray datagram restarts the full wait; DeliverUdp
//     (socket.cpp:1077-1088) queues datagrams from any source on the unconnected socket, and the
//     port is sequential from 49152 (PickPortLocked, socket.cpp:184-198). One datagram per
//     timeout period holds getaddrinfo forever.
