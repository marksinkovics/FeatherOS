#include <catch2/catch_test_macros.hpp>

#include "kernel/network/tcp.h"
#include "kernel/network/tcp_connection.h"
#include "user/syscall_abi.h"

#include <memory>
#include <string>
#include <vector>

using namespace FeatherOS::Network;
namespace User = FeatherOS::User;

namespace
{

constexpr uint16_t LocalPort = 49152;
constexpr uint16_t RemotePort = 80;
constexpr uint16_t LocalMss = 1460;
constexpr uint32_t Iss = 1000;
constexpr uint32_t PeerIss = 5000;
// The largest window without scaling, which the peers below do not offer unless a test says so.
constexpr size_t UnscaledWindow = 0xFFFF;
constexpr int NoWindowScale = -1;

struct Sent
{
    TcpSegment fields;
    std::string payload;
};

// Drives one connection against a scripted peer.
struct Harness
{
    std::vector<Sent> Drain()
    {
        std::vector<Sent> sent;
        uint8_t buffer[TcpConnection::MaximumSegmentLength];
        for (size_t length = connection->NextSegment(now, buffer); length != 0;
             length = connection->NextSegment(now, buffer))
        {
            Sent segment{};
            REQUIRE(ParseTcpSegment(buffer, length, &segment.fields));
            REQUIRE(segment.fields.source_port == LocalPort);
            REQUIRE(segment.fields.destination_port == RemotePort);
            segment.payload.assign(reinterpret_cast<const char*>(segment.fields.payload),
                                   segment.fields.payload_length);
            segment.fields.payload = nullptr;
            sent.push_back(segment);
            REQUIRE(sent.size() < 100);
        }
        return sent;
    }

    ~Harness()
    {
        connection->Release();
    }

    void Deliver(uint32_t sequence, uint32_t acknowledgment, uint8_t flags,
                 const std::string& payload = "", uint16_t window = 8192, uint16_t mss = 0,
                 int window_scale = NoWindowScale)
    {
        TcpSegment segment{};
        segment.source_port = RemotePort;
        segment.destination_port = LocalPort;
        segment.sequence = sequence;
        segment.acknowledgment = acknowledgment;
        segment.flags = flags;
        segment.window = window;
        segment.maximum_segment_size = mss;
        segment.has_window_scale = window_scale != NoWindowScale;
        segment.window_scale = static_cast<uint8_t>(window_scale);
        segment.payload = reinterpret_cast<const uint8_t*>(payload.data());
        segment.payload_length = payload.size();
        connection->Receive(segment, now);
    }

    // Connects to a peer that offers mss, window and optionally window scaling, and acknowledges
    // the handshake.
    void Establish(uint16_t mss = 1460, uint16_t window = 8192, int window_scale = NoWindowScale)
    {
        connection->Connect(LocalPort, RemotePort, Iss, LocalMss);
        REQUIRE(Drain().size() == 1);
        Deliver(PeerIss, Iss + 1, TcpSyn | TcpAck, "", window, mss, window_scale);
        REQUIRE(connection->State() == TcpState::Established);
        REQUIRE(Drain().size() == 1);
    }

    std::string Read()
    {
        std::string text(TcpMaximumBufferSize, '\0');
        text.resize(connection->Read(reinterpret_cast<uint8_t*>(text.data()), text.size()));
        return text;
    }

    size_t Write(const std::string& text)
    {
        return connection->Write(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }

    std::unique_ptr<TcpConnection> connection = std::make_unique<TcpConnection>();
    uint64_t now = 0;
};

} // namespace

TEST_CASE("an active open sends SYN with our MSS and acknowledges the SYN-ACK")
{
    Harness tcp;
    tcp.connection->Connect(LocalPort, RemotePort, Iss, LocalMss);
    REQUIRE(tcp.connection->Opening());
    std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.flags == TcpSyn);
    REQUIRE(sent[0].fields.sequence == Iss);
    REQUIRE(sent[0].fields.maximum_segment_size == LocalMss);
    REQUIRE(sent[0].fields.has_window_scale);
    REQUIRE(sent[0].fields.window_scale == 3);
    REQUIRE(sent[0].fields.window == UnscaledWindow);

    tcp.now = 40;
    tcp.Deliver(PeerIss, Iss + 1, TcpSyn | TcpAck, "", 8192, 1400);
    REQUIRE(tcp.connection->State() == TcpState::Established);
    REQUIRE_FALSE(tcp.connection->Opening());
    REQUIRE(tcp.connection->CanWrite());
    sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.flags == TcpAck);
    REQUIRE(sent[0].fields.sequence == Iss + 1);
    REQUIRE(sent[0].fields.acknowledgment == PeerIss + 1);
    REQUIRE(sent[0].fields.window == UnscaledWindow);
    REQUIRE(tcp.connection->Deadline() == 0);
}

TEST_CASE("a passive open answers SYN with SYN-ACK and opens on the ACK")
{
    Harness tcp;
    TcpSegment syn{};
    syn.source_port = RemotePort;
    syn.destination_port = LocalPort;
    syn.sequence = PeerIss;
    syn.flags = TcpSyn;
    syn.window = 8192;
    syn.maximum_segment_size = 1400;
    tcp.connection->Accept(syn, Iss, LocalMss);
    REQUIRE(tcp.connection->State() == TcpState::SynReceived);
    std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.flags == (TcpSyn | TcpAck));
    REQUIRE(sent[0].fields.sequence == Iss);
    REQUIRE(sent[0].fields.acknowledgment == PeerIss + 1);
    REQUIRE(sent[0].fields.maximum_segment_size == LocalMss);
    REQUIRE_FALSE(sent[0].fields.has_window_scale);

    // The peer missed the SYN-ACK and sends its SYN again.
    tcp.Deliver(PeerIss, 0, TcpSyn);
    sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.flags == (TcpSyn | TcpAck));

    tcp.Deliver(PeerIss + 1, Iss + 1, TcpAck, "hi");
    REQUIRE(tcp.connection->State() == TcpState::Established);
    REQUIRE(tcp.Read() == "hi");
}

TEST_CASE("a reset answering SYN refuses the connection")
{
    Harness tcp;
    tcp.connection->Connect(LocalPort, RemotePort, Iss, LocalMss);
    tcp.Drain();
    // Without an acceptable acknowledgment a reset is ignored.
    tcp.Deliver(0, Iss + 7, TcpReset | TcpAck);
    tcp.Deliver(0, 0, TcpReset);
    REQUIRE(tcp.connection->State() == TcpState::SynSent);
    tcp.Deliver(0, Iss + 1, TcpReset | TcpAck);
    REQUIRE(tcp.connection->State() == TcpState::Closed);
    REQUIRE(tcp.connection->Error() == User::SyscallErrorConnectionRefused);
    REQUIRE(tcp.Drain().empty());
    REQUIRE(tcp.connection->Idle());
}

TEST_CASE("an unanswered SYN is sent again with backoff until the open times out")
{
    Harness tcp;
    tcp.connection->Connect(LocalPort, RemotePort, Iss, LocalMss);
    tcp.Drain();
    uint64_t interval = 1000;
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        REQUIRE(tcp.connection->Deadline() == tcp.now + interval);
        tcp.now += interval - 1;
        REQUIRE_FALSE(tcp.connection->OnTimer(tcp.now));
        tcp.now += 1;
        REQUIRE_FALSE(tcp.connection->OnTimer(tcp.now));
        const std::vector<Sent> sent = tcp.Drain();
        REQUIRE(sent.size() == 1);
        REQUIRE(sent[0].fields.flags == TcpSyn);
        REQUIRE(sent[0].fields.sequence == Iss);
        interval *= 2;
    }
    tcp.now += interval;
    REQUIRE(tcp.connection->OnTimer(tcp.now));
    REQUIRE(tcp.connection->Error() == User::SyscallErrorTimedOut);
    // The peer never answered, so it gets no reset.
    REQUIRE(tcp.Drain().empty());
}

TEST_CASE("data goes out in segments the peer's MSS and window allow")
{
    Harness tcp;
    tcp.Establish(100, 250);
    REQUIRE(tcp.Write(std::string(350, 'x')) == 350);
    std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 3);
    REQUIRE(sent[0].fields.sequence == Iss + 1);
    REQUIRE(sent[0].payload.size() == 100);
    REQUIRE(sent[0].fields.flags == TcpAck);
    REQUIRE(sent[1].fields.sequence == Iss + 101);
    REQUIRE(sent[2].payload.size() == 50);

    tcp.Deliver(PeerIss + 1, Iss + 251, TcpAck, "", 250);
    sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.sequence == Iss + 251);
    REQUIRE(sent[0].payload.size() == 100);
    REQUIRE((sent[0].fields.flags & TcpPush) != 0);

    tcp.Deliver(PeerIss + 1, Iss + 351, TcpAck);
    REQUIRE(tcp.connection->Writable() == TcpMaximumBufferSize);
    REQUIRE(tcp.connection->Deadline() == 0);
}

TEST_CASE("received data is buffered in order and acknowledged")
{
    Harness tcp;
    tcp.Establish();
    tcp.Deliver(PeerIss + 1, Iss + 1, TcpAck | TcpPush, "abc");
    std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.acknowledgment == PeerIss + 4);
    REQUIRE(sent[0].fields.window == UnscaledWindow - 3);

    // A retransmission overlapping what arrived only adds the new part.
    tcp.Deliver(PeerIss + 1, Iss + 1, TcpAck, "abcdef");
    // A segment beyond the next expected byte waits for the gap and gets a duplicate ACK.
    tcp.Deliver(PeerIss + 20, Iss + 1, TcpAck, "zz");
    sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.acknowledgment == PeerIss + 7);
    REQUIRE(tcp.Read() == "abcdef");
}

TEST_CASE("a segment outside the window only gets an ACK")
{
    Harness tcp;
    tcp.Establish();
    tcp.Deliver(PeerIss + 100000, Iss + 1, TcpAck, "far");
    REQUIRE(tcp.connection->Readable() == 0);
    const std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.acknowledgment == PeerIss + 1);
}

TEST_CASE("out-of-order segments wait in the receive ring until the gap fills")
{
    Harness tcp;
    tcp.Establish();
    tcp.Deliver(PeerIss + 4, Iss + 1, TcpAck, "def");
    tcp.Deliver(PeerIss + 10, Iss + 1, TcpAck, "jkl");
    REQUIRE(tcp.connection->Readable() == 0);
    // Each gets its own duplicate ACK with the same window, so the peer can count them.
    std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 2);
    for (const Sent& acknowledgment : sent)
    {
        REQUIRE(acknowledgment.fields.acknowledgment == PeerIss + 1);
        REQUIRE(acknowledgment.fields.window == UnscaledWindow);
    }

    tcp.Deliver(PeerIss + 1, Iss + 1, TcpAck, "abc");
    sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.acknowledgment == PeerIss + 7);
    tcp.Deliver(PeerIss + 7, Iss + 1, TcpAck, "ghi");
    sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.acknowledgment == PeerIss + 13);
    REQUIRE(tcp.Read() == "abcdefghijkl");
}

TEST_CASE("overlapping and repeated segments merge with the data around them")
{
    Harness tcp;
    tcp.Establish();
    tcp.Deliver(PeerIss + 5, Iss + 1, TcpAck, "efg");
    tcp.Deliver(PeerIss + 11, Iss + 1, TcpAck, "klm");
    // One segment bridges both ranges, another repeats part of one.
    tcp.Deliver(PeerIss + 6, Iss + 1, TcpAck, "fghijkl");
    tcp.Deliver(PeerIss + 11, Iss + 1, TcpAck, "kl");
    REQUIRE(tcp.Drain().size() == 4);
    tcp.Deliver(PeerIss + 1, Iss + 1, TcpAck, "abcdef");
    const std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.acknowledgment == PeerIss + 14);
    REQUIRE(tcp.Read() == "abcdefghijklm");
}

TEST_CASE("four out-of-order ranges are kept, the lowest ones")
{
    Harness tcp;
    tcp.Establish();
    const std::string text = "abcdefghijklmnop";
    const auto deliver = [&tcp, &text](uint32_t offset, size_t length)
    { tcp.Deliver(PeerIss + 1 + offset, Iss + 1, TcpAck, text.substr(offset, length)); };
    deliver(2, 1);
    deliver(5, 1);
    deliver(8, 1);
    deliver(14, 1);
    // A fifth range below the highest pushes it out; one above them all is not kept.
    deliver(11, 1);
    deliver(15, 1);
    REQUIRE(tcp.Drain().size() == 6);

    deliver(0, 2);
    deliver(3, 2);
    deliver(6, 2);
    deliver(9, 2);
    deliver(12, 2);
    std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.acknowledgment == PeerIss + 15);
    deliver(14, 2);
    sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.acknowledgment == PeerIss + 17);
    REQUIRE(tcp.Read() == text);
}

TEST_CASE("an out-of-order segment is cut at the window's edge")
{
    Harness tcp;
    tcp.Establish();
    const auto end = PeerIss + 1 + static_cast<uint32_t>(UnscaledWindow);
    tcp.Deliver(end - 2, Iss + 1, TcpAck, "wxyz");
    tcp.Deliver(PeerIss + 1, Iss + 1, TcpAck, std::string(UnscaledWindow - 2, 'a'));
    REQUIRE(tcp.connection->Readable() == UnscaledWindow);
    const std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.back().fields.acknowledgment == end);
    REQUIRE(sent.back().fields.window == 0);
    REQUIRE(tcp.Read().substr(UnscaledWindow - 2) == "wx");
}

TEST_CASE("a FIN ahead of missing data is taken once the data arrives")
{
    Harness tcp;
    tcp.Establish();
    tcp.Deliver(PeerIss + 4, Iss + 1, TcpAck | TcpFin, "def");
    REQUIRE(tcp.connection->State() == TcpState::Established);
    std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.acknowledgment == PeerIss + 1);
    tcp.Deliver(PeerIss + 1, Iss + 1, TcpAck, "abc");
    REQUIRE(tcp.connection->State() == TcpState::CloseWait);
    REQUIRE(tcp.connection->ReceivedFin());
    sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.acknowledgment == PeerIss + 8);
    REQUIRE(tcp.Read() == "abcdef");
}

TEST_CASE("the advertised window grows only by whole segments")
{
    Harness tcp;
    tcp.Establish();
    tcp.Deliver(PeerIss + 1, Iss + 1, TcpAck, std::string(100, 'a'));
    REQUIRE(tcp.Drain()[0].fields.window == UnscaledWindow - 100);
    REQUIRE(tcp.Read().size() == 100);
    REQUIRE_FALSE(tcp.connection->AcknowledgmentPending());
    // Less than a segment was read, so the right edge stays where it was.
    tcp.Deliver(PeerIss + 101, Iss + 1, TcpAck, "b");
    REQUIRE(tcp.Drain()[0].fields.window == UnscaledWindow - 101);
}

TEST_CASE("a lost segment is sent again after the RTO, and the connection gives up eventually")
{
    Harness tcp;
    tcp.Establish();
    tcp.Write("hello");
    std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    // The handshake took no time, so the RTO is at its floor.
    REQUIRE(tcp.connection->Deadline() == 200);
    tcp.now = 200;
    REQUIRE_FALSE(tcp.connection->OnTimer(tcp.now));
    sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.sequence == Iss + 1);
    REQUIRE(sent[0].payload == "hello");
    REQUIRE(tcp.connection->Deadline() == 600);

    for (int attempt = 0; attempt < 7; ++attempt)
    {
        tcp.now = tcp.connection->Deadline();
        REQUIRE_FALSE(tcp.connection->OnTimer(tcp.now));
        REQUIRE(tcp.Drain().size() == 1);
    }
    tcp.now = tcp.connection->Deadline();
    REQUIRE(tcp.connection->OnTimer(tcp.now));
    REQUIRE(tcp.connection->Error() == User::SyscallErrorTimedOut);
    sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.flags == (TcpReset | TcpAck));
    REQUIRE(tcp.connection->Idle());
}

TEST_CASE("an acknowledgment from the peer restarts the retry count")
{
    Harness tcp;
    tcp.Establish();
    tcp.Write("one");
    tcp.Drain();
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        tcp.now = tcp.connection->Deadline();
        tcp.connection->OnTimer(tcp.now);
        tcp.Drain();
    }
    tcp.Deliver(PeerIss + 1, Iss + 4, TcpAck);
    tcp.Write("two");
    tcp.Drain();
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        tcp.now = tcp.connection->Deadline();
        REQUIRE_FALSE(tcp.connection->OnTimer(tcp.now));
        tcp.Drain();
    }
    REQUIRE(tcp.connection->State() == TcpState::Established);
}

TEST_CASE("three duplicate ACKs send the first unacknowledged segment again")
{
    Harness tcp;
    tcp.Establish(100);
    tcp.Write(std::string(400, 'y'));
    REQUIRE(tcp.Drain().size() == 4);
    tcp.Deliver(PeerIss + 1, Iss + 101, TcpAck);
    for (int duplicate = 0; duplicate < 2; ++duplicate)
    {
        tcp.Deliver(PeerIss + 1, Iss + 101, TcpAck);
        REQUIRE(tcp.Drain().empty());
    }
    tcp.Deliver(PeerIss + 1, Iss + 101, TcpAck);
    const std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.sequence == Iss + 101);
    REQUIRE(sent[0].payload.size() == 100);
}

TEST_CASE("further duplicate ACKs in fast recovery let new data out")
{
    Harness tcp;
    tcp.Establish(100);
    tcp.Write(std::string(2000, 'n'));
    REQUIRE(tcp.Drain().size() == 10);
    for (int duplicate = 0; duplicate < 3; ++duplicate)
    {
        tcp.Deliver(PeerIss + 1, Iss + 1, TcpAck);
    }
    std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.sequence == Iss + 1);
    // Half the flight plus the three segments the duplicates stand for: 800 bytes against 1000
    // in flight, so only the third duplicate after them lets a new segment out.
    for (int duplicate = 0; duplicate < 2; ++duplicate)
    {
        tcp.Deliver(PeerIss + 1, Iss + 1, TcpAck);
        REQUIRE(tcp.Drain().empty());
    }
    tcp.Deliver(PeerIss + 1, Iss + 1, TcpAck);
    sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.sequence == Iss + 1001);
}

TEST_CASE("a partial acknowledgment in fast recovery sends the next lost segment at once")
{
    Harness tcp;
    tcp.Establish(100);
    tcp.Write(std::string(1000, 'p'));
    REQUIRE(tcp.Drain().size() == 10);
    for (int duplicate = 0; duplicate < 3; ++duplicate)
    {
        tcp.Deliver(PeerIss + 1, Iss + 1, TcpAck);
    }
    REQUIRE(tcp.Drain().size() == 1);
    // The retransmission filled the first hole, and the fourth segment is missing too.
    tcp.Deliver(PeerIss + 1, Iss + 301, TcpAck);
    std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.sequence == Iss + 301);
    REQUIRE(sent[0].payload.size() == 100);

    // Acknowledging all that was sent before recovery ends it, with a window of two segments
    // here, since nothing is left in flight.
    tcp.Deliver(PeerIss + 1, Iss + 1001, TcpAck);
    tcp.Write(std::string(1000, 'q'));
    sent = tcp.Drain();
    REQUIRE(sent.size() == 2);
    REQUIRE(sent[0].fields.sequence == Iss + 1001);
}

TEST_CASE("duplicate ACKs for data sent before a timeout start no fast recovery")
{
    Harness tcp;
    tcp.Establish(100);
    tcp.Write(std::string(500, 'r'));
    REQUIRE(tcp.Drain().size() == 5);
    tcp.now = tcp.connection->Deadline();
    REQUIRE_FALSE(tcp.connection->OnTimer(tcp.now));
    REQUIRE(tcp.Drain().size() == 1);
    for (int duplicate = 0; duplicate < 3; ++duplicate)
    {
        tcp.Deliver(PeerIss + 1, Iss + 1, TcpAck);
    }
    REQUIRE(tcp.Drain().empty());
}

TEST_CASE("a closed window is probed one byte at a time until it opens")
{
    Harness tcp;
    tcp.Establish(1460, 0);
    tcp.Write("abc");
    REQUIRE(tcp.Drain().empty());
    REQUIRE(tcp.connection->Deadline() == 200);
    tcp.now = 200;
    tcp.connection->OnTimer(tcp.now);
    std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].payload == "a");

    // The peer takes the byte but keeps its window closed.
    tcp.Deliver(PeerIss + 1, Iss + 2, TcpAck, "", 0);
    REQUIRE(tcp.Drain().empty());
    tcp.now = tcp.connection->Deadline();
    tcp.connection->OnTimer(tcp.now);
    sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].payload == "b");

    tcp.Deliver(PeerIss + 1, Iss + 3, TcpAck, "", 100);
    sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].payload == "c");
}

TEST_CASE("reading a full receive buffer reopens the window with an update")
{
    Harness tcp;
    tcp.Establish();
    uint32_t sequence = PeerIss + 1;
    while (tcp.connection->Readable() < UnscaledWindow)
    {
        const size_t room = UnscaledWindow - tcp.connection->Readable();
        tcp.Deliver(sequence, Iss + 1, TcpAck, std::string(room < 1460 ? room : 1460, 'z'));
        sequence += static_cast<uint32_t>(room < 1460 ? room : 1460);
    }
    std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.window == 0);

    // A probe into the closed window is answered without taking data.
    tcp.Deliver(sequence, Iss + 1, TcpAck, "p");
    sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.acknowledgment == sequence);

    std::string part(100, '\0');
    REQUIRE(tcp.connection->Read(reinterpret_cast<uint8_t*>(part.data()), part.size()) == 100);
    REQUIRE_FALSE(tcp.connection->AcknowledgmentPending());
    std::string half(UnscaledWindow / 2, '\0');
    tcp.connection->Read(reinterpret_cast<uint8_t*>(half.data()), half.size());
    REQUIRE(tcp.connection->AcknowledgmentPending());
    sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.window == 100 + UnscaledWindow / 2);
}

TEST_CASE("an active close sends FIN and waits in TIME-WAIT after the peer's")
{
    Harness tcp;
    tcp.Establish();
    tcp.connection->Close();
    REQUIRE(tcp.connection->State() == TcpState::FinWait1);
    REQUIRE_FALSE(tcp.connection->CanWrite());
    REQUIRE(tcp.Write("late") == 0);
    std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.flags == (TcpFin | TcpAck));
    REQUIRE(sent[0].fields.sequence == Iss + 1);

    tcp.Deliver(PeerIss + 1, Iss + 2, TcpAck);
    REQUIRE(tcp.connection->State() == TcpState::FinWait2);
    tcp.Deliver(PeerIss + 1, Iss + 2, TcpFin | TcpAck, "bye");
    REQUIRE(tcp.connection->State() == TcpState::TimeWait);
    REQUIRE(tcp.connection->ReceivedFin());
    REQUIRE(tcp.Read() == "bye");
    sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.acknowledgment == PeerIss + 5);

    // The peer's FIN again, as if our ACK was lost: acknowledged again.
    tcp.Deliver(PeerIss + 4, Iss + 2, TcpFin | TcpAck);
    REQUIRE(tcp.Drain().size() == 1);
    tcp.now = 59999;
    REQUIRE_FALSE(tcp.connection->OnTimer(tcp.now));
    tcp.now = 60000;
    REQUIRE(tcp.connection->OnTimer(tcp.now));
    REQUIRE(tcp.connection->State() == TcpState::Closed);
    REQUIRE(tcp.connection->Error() == 0);
}

TEST_CASE("buffered data goes out before the FIN, in the same segment")
{
    Harness tcp;
    tcp.Establish();
    tcp.Write("last");
    tcp.connection->Close();
    const std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].payload == "last");
    REQUIRE(sent[0].fields.flags == (TcpAck | TcpPush | TcpFin));
    tcp.Deliver(PeerIss + 1, Iss + 6, TcpAck);
    REQUIRE(tcp.connection->State() == TcpState::FinWait2);
}

TEST_CASE("a passive close waits for the application, then for the ACK of its FIN")
{
    Harness tcp;
    tcp.Establish();
    tcp.Deliver(PeerIss + 1, Iss + 1, TcpFin | TcpAck);
    REQUIRE(tcp.connection->State() == TcpState::CloseWait);
    REQUIRE(tcp.connection->ReceivedFin());
    REQUIRE(tcp.connection->CanWrite());
    REQUIRE(tcp.Drain().size() == 1);

    tcp.connection->Close();
    REQUIRE(tcp.connection->State() == TcpState::LastAck);
    const std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.flags == (TcpFin | TcpAck));
    tcp.Deliver(PeerIss + 2, Iss + 2, TcpAck);
    REQUIRE(tcp.connection->State() == TcpState::Closed);
    REQUIRE(tcp.connection->Idle());
    REQUIRE(tcp.connection->Error() == 0);
}

TEST_CASE("both sides closing at once pass through CLOSING")
{
    Harness tcp;
    tcp.Establish();
    tcp.connection->Close();
    tcp.Drain();
    tcp.Deliver(PeerIss + 1, Iss + 1, TcpFin | TcpAck);
    REQUIRE(tcp.connection->State() == TcpState::Closing);
    tcp.Deliver(PeerIss + 2, Iss + 2, TcpAck);
    REQUIRE(tcp.connection->State() == TcpState::TimeWait);
}

TEST_CASE("a FIN-WAIT-2 without a socket gives up on the peer's FIN")
{
    Harness tcp;
    tcp.Establish();
    tcp.connection->Close();
    tcp.Drain();
    tcp.Deliver(PeerIss + 1, Iss + 2, TcpAck);
    REQUIRE(tcp.connection->Deadline() == 0);
    tcp.connection->Orphan(tcp.now);
    tcp.now = tcp.connection->Deadline();
    REQUIRE(tcp.now == 60000);
    REQUIRE(tcp.connection->OnTimer(tcp.now));
    REQUIRE(tcp.connection->State() == TcpState::Closed);
}

TEST_CASE("only a reset at the next sequence number resets the connection")
{
    Harness tcp;
    tcp.Establish();
    tcp.Deliver(PeerIss + 10, 0, TcpReset);
    REQUIRE(tcp.connection->State() == TcpState::Established);
    std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.flags == TcpAck);

    // So does a SYN in an open connection.
    tcp.Deliver(PeerIss + 1, 0, TcpSyn);
    REQUIRE(tcp.Drain().size() == 1);

    tcp.Deliver(PeerIss + 1, 0, TcpReset);
    REQUIRE(tcp.connection->State() == TcpState::Closed);
    REQUIRE(tcp.connection->Error() == User::SyscallErrorConnectionReset);
    REQUIRE(tcp.Drain().empty());
}

TEST_CASE("a reset during TIME-WAIT closes without an error")
{
    Harness tcp;
    tcp.Establish();
    tcp.connection->Close();
    tcp.Drain();
    tcp.Deliver(PeerIss + 1, Iss + 2, TcpFin | TcpAck);
    REQUIRE(tcp.connection->State() == TcpState::TimeWait);
    tcp.Deliver(PeerIss + 2, 0, TcpReset);
    REQUIRE(tcp.connection->State() == TcpState::Closed);
    REQUIRE(tcp.connection->Error() == 0);
}

TEST_CASE("aborting an open connection sends a reset")
{
    Harness tcp;
    tcp.Establish();
    tcp.Write("unsent");
    tcp.connection->Abort();
    REQUIRE(tcp.connection->State() == TcpState::Closed);
    REQUIRE_FALSE(tcp.connection->Idle());
    const std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.flags == (TcpReset | TcpAck));
    REQUIRE(sent[0].fields.sequence == Iss + 1);
    REQUIRE(tcp.connection->Idle());
}

TEST_CASE("dropping a connection closes it with the error and sends nothing")
{
    Harness tcp;
    tcp.Establish();
    tcp.Write("unsent");
    tcp.connection->Drop(User::SyscallErrorNetworkDown);
    REQUIRE(tcp.connection->State() == TcpState::Closed);
    REQUIRE(tcp.connection->Error() == User::SyscallErrorNetworkDown);
    REQUIRE(tcp.connection->Idle());
    REQUIRE(tcp.Drain().empty());
}

TEST_CASE("an acknowledgment of data never sent is answered, not taken")
{
    Harness tcp;
    tcp.Establish();
    tcp.Write("abc");
    tcp.Drain();
    tcp.Deliver(PeerIss + 1, Iss + 50, TcpAck);
    REQUIRE(tcp.connection->Writable() == TcpMaximumBufferSize - 3);
    REQUIRE(tcp.Drain().size() == 1);
}

namespace
{

size_t PayloadBytes(const std::vector<Sent>& sent)
{
    size_t total = 0;
    for (const Sent& segment : sent)
    {
        total += segment.payload.size();
    }
    return total;
}

} // namespace

TEST_CASE("windows are scaled when the SYN-ACK offers scaling too")
{
    Harness tcp;
    tcp.Establish(1460, 1000, 2);
    tcp.Deliver(PeerIss + 1, Iss + 1, TcpAck | TcpPush, "abc", 1000);
    std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    // Our shift is 3, so the window covers the largest receive ring; holding the right edge
    // rounds 262141 bytes up to the next step.
    REQUIRE(sent[0].fields.window == TcpMaximumBufferSize >> 3);
    // The peer's shift is 2, so its window of 1000 is 4000 bytes, its SYN-ACK's excepted.
    REQUIRE(tcp.Write(std::string(10000, 'x')) == 10000);
    REQUIRE(PayloadBytes(tcp.Drain()) == 4000);
}

TEST_CASE("without the peer's window scale option neither side scales")
{
    Harness tcp;
    tcp.Establish(1460, 1000);
    tcp.Deliver(PeerIss + 1, Iss + 1, TcpAck, "", 1000);
    REQUIRE(tcp.Write(std::string(10000, 'x')) == 10000);
    const std::vector<Sent> sent = tcp.Drain();
    REQUIRE(PayloadBytes(sent) == 1000);
    REQUIRE(sent.back().fields.window == UnscaledWindow);
}

TEST_CASE("a passive open scales windows when the SYN offers it")
{
    Harness tcp;
    TcpSegment syn{};
    syn.source_port = RemotePort;
    syn.destination_port = LocalPort;
    syn.sequence = PeerIss;
    syn.flags = TcpSyn;
    syn.window = 1000;
    syn.maximum_segment_size = 1460;
    syn.has_window_scale = true;
    syn.window_scale = 12;
    tcp.connection->Accept(syn, Iss, LocalMss);
    std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.has_window_scale);
    REQUIRE(sent[0].fields.window_scale == 3);
    REQUIRE(sent[0].fields.window == UnscaledWindow);

    tcp.Deliver(PeerIss + 1, Iss + 1, TcpAck, "", 2);
    REQUIRE(tcp.connection->State() == TcpState::Established);
    REQUIRE(tcp.Write(std::string(20000, 'x')) == 20000);
    REQUIRE(PayloadBytes(tcp.Drain()) == 2 << 12);
    // The data segments advertised the whole ring, scaled, past the SYN-ACK's unscaled window.
    tcp.Deliver(PeerIss + 1, Iss + 1, TcpAck, "hi", 2);
    sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.window == TcpMaximumBufferSize >> 3);
}

TEST_CASE("a scaled window moves in steps and never pulls back its right edge")
{
    Harness tcp;
    tcp.Establish(1460, 8192, 0);
    tcp.Deliver(PeerIss + 1, Iss + 1, TcpAck, std::string(100, 'a'));
    std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    // 262044 bytes of space round up to the next step, past the edge of 262144 bytes from
    // PeerIss + 1, rather than down, before it.
    REQUIRE(sent[0].fields.window == 262048 / 8);
    REQUIRE(tcp.Read().size() == 100);
    REQUIRE_FALSE(tcp.connection->AcknowledgmentPending());

    tcp.Deliver(PeerIss + 101, Iss + 1, TcpAck, std::string(3000, 'b'));
    sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.window == 259048 / 8);
    REQUIRE(tcp.Read().size() == 3000);
    REQUIRE(tcp.connection->AcknowledgmentPending());
    sent = tcp.Drain();
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].fields.window == TcpMaximumBufferSize / 8);
}

TEST_CASE("the receive ring grows past the unscaled window and keeps out-of-order data")
{
    Harness tcp;
    tcp.Establish(1460, 8192, 7);
    std::string text;
    for (size_t index = 0; index < 200000; ++index)
    {
        text.push_back(static_cast<char>('a' + index % 26));
    }
    // The second half arrives first, so it waits past the first half's end.
    const size_t half = text.size() / 2;
    for (size_t offset = half; offset < text.size(); offset += 1460)
    {
        tcp.Deliver(PeerIss + 1 + static_cast<uint32_t>(offset), Iss + 1, TcpAck,
                    text.substr(offset, 1460));
    }
    REQUIRE(tcp.connection->Readable() == 0);
    for (size_t offset = 0; offset < half; offset += 1460)
    {
        tcp.Deliver(PeerIss + 1 + static_cast<uint32_t>(offset), Iss + 1, TcpAck,
                    text.substr(offset, offset + 1460 < half ? 1460 : half - offset));
    }
    REQUIRE(tcp.connection->Readable() == text.size());
    const std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.back().fields.acknowledgment == PeerIss + 1 + text.size());
    REQUIRE(tcp.Read() == text);
}

TEST_CASE("the send ring grows on demand up to its largest size")
{
    Harness tcp;
    tcp.Establish(1460, 8192, 7);
    REQUIRE(tcp.Write(std::string(1000, 'x')) == 1000);
    REQUIRE(tcp.Write(std::string(TcpMaximumBufferSize, 'y')) == TcpMaximumBufferSize - 1000);
    REQUIRE(tcp.connection->Writable() == 0);
    REQUIRE(tcp.Write("z") == 0);
    const std::vector<Sent> sent = tcp.Drain();
    REQUIRE(sent.front().payload.substr(0, 1001) == std::string(1000, 'x') + "y");
    tcp.Deliver(PeerIss + 1, Iss + 1 + static_cast<uint32_t>(PayloadBytes(sent)), TcpAck);
    REQUIRE(tcp.connection->Writable() == PayloadBytes(sent));
}
