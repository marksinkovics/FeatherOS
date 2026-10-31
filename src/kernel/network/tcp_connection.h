#ifndef FEATHEROS_NETWORK_TCP_CONNECTION_H
#define FEATHEROS_NETWORK_TCP_CONNECTION_H

#include "tcp.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Network
{

// Each direction's ring starts empty and grows on demand up to this size.
constexpr size_t TcpMaximumBufferSize = 256 * 1024;

// A ring of bytes on the heap, for one direction of a connection.
struct TcpBuffer
{
    uint8_t* data;
    size_t capacity;
    size_t start;
    size_t length;
};

enum class TcpState : uint8_t
{
    Closed,
    SynSent,
    SynReceived,
    Established,
    FinWait1,
    FinWait2,
    CloseWait,
    Closing,
    LastAck,
    TimeWait,
};

// One TCP connection's state machine (RFC 9293), without addresses, locks or a clock of its own:
// the caller feeds it segments and the time in milliseconds, and pulls the segments it should
// send. Retransmission follows RFC 6298 with go-back-N after a timeout, congestion control RFC 5681
// slow start and congestion avoidance with NewReno fast recovery (RFC 6582). Out-of-order data
// waits in the receive ring where it belongs, and every segment with data is acknowledged at once,
// each out-of-order one with its own duplicate ACK. Windows are scaled (RFC 7323) when both SYNs
// offer it.
class TcpConnection
{
public:
    // A segment NextSegment writes is at most this long.
    static constexpr size_t MaximumSegmentLength = TcpHeaderSize + TcpMaximumOptionsSize + 1460;

    // Starts an active open: the next segment is the SYN. local_mss is what our side receives.
    void Connect(uint16_t local_port, uint16_t remote_port, uint32_t initial_sequence,
                 uint16_t local_mss);
    // Starts a passive open from a SYN a listener received: the next segment is the SYN-ACK.
    void Accept(const TcpSegment& syn, uint32_t initial_sequence, uint16_t local_mss);
    void Receive(const TcpSegment& segment, uint64_t now);
    // Runs the retransmission, persist and TIME-WAIT timers; true when the state or error changed.
    bool OnTimer(uint64_t now);
    // Writes the next segment to send into segment, which holds MaximumSegmentLength bytes, and
    // returns its length; 0 when there is nothing to send now.
    size_t NextSegment(uint64_t now, uint8_t* segment);
    // The time OnTimer next has work, or 0 for none.
    uint64_t Deadline() const;

    // Buffers what fits of data for sending and returns how much.
    size_t Write(const uint8_t* data, size_t length);
    // Takes received data in order and returns how much.
    size_t Read(uint8_t* data, size_t capacity);
    // Sends FIN once the buffered data is out; Read still works.
    void Close();
    // Sends RST if the peer knows of the connection, and closes it at once.
    void Abort();
    // Closes at once with error, sending nothing more, as when the peer can no longer be reached.
    void Drop(uint64_t error);
    // Frees both rings; the connection is not used again.
    void Release();
    // A closed connection's socket is gone, so FIN-WAIT-2 waits only so long for the peer's FIN.
    void Orphan(uint64_t now);

    TcpState State() const;
    // A User::SyscallError code once the connection failed: refused, reset or timed out; else 0.
    uint64_t Error() const;
    size_t Readable() const;
    size_t Writable() const;
    // The peer sent FIN, so after the buffered data no more arrives.
    bool ReceivedFin() const;
    // True from Connect or Accept until the connection is established or failed.
    bool Opening() const;
    // True while Write may queue data: established and not closed by Close.
    bool CanWrite() const;
    // Nothing left to send, RST included, so a closed connection can be freed.
    bool Idle() const;
    // An ACK waits to be sent, such as a window update after Read.
    bool AcknowledgmentPending() const;

private:
    void Start(uint16_t local_port, uint16_t remote_port, uint32_t initial_sequence,
               uint16_t local_mss);
    void ReceiveSynSent(const TcpSegment& segment, uint64_t now);
    bool Acceptable(const TcpSegment& segment) const;
    bool ProcessAcknowledgment(const TcpSegment& segment, uint64_t now);
    void ProcessData(const TcpSegment& segment);
    void StoreOutOfOrder(uint32_t begin, uint32_t end);
    void NoteFin(const TcpSegment& segment);
    void ProcessFin(uint64_t now);
    void Fail(uint64_t error);
    void EnterTimeWait(uint64_t now);
    void SampleRoundTrip(uint64_t sample);
    uint32_t ReceiveWindow() const;
    size_t WriteHeader(uint8_t* segment, uint32_t sequence, uint8_t flags);
    size_t Unsent() const;
    bool Synchronized() const;

    struct SequenceRange
    {
        uint32_t begin;
        uint32_t end;
    };
    // Received ranges past receive_next, in order, apart and not adjacent.
    static constexpr size_t OutOfOrderRanges = 4;

    TcpBuffer send_buffer;
    TcpBuffer receive_buffer;
    uint64_t error;
    uint64_t deadline;
    uint64_t rto;
    uint64_t smoothed_rtt;
    uint64_t rtt_variance;
    uint64_t timing_start;
    uint32_t timed_sequence;
    // Our side: the first unacknowledged sequence number, the next one to send, and the highest
    // sent, which go-back-N retransmission moves snd_nxt back from.
    uint32_t initial_send_sequence;
    uint32_t send_unacknowledged;
    uint32_t send_next;
    uint32_t send_maximum;
    uint32_t send_window;
    uint32_t window_update_sequence;
    uint32_t window_update_acknowledgment;
    uint32_t congestion_window;
    uint32_t slow_start_threshold;
    // Fast recovery lasts until everything sent before it began is acknowledged.
    uint32_t recover;
    uint32_t receive_next;
    // The right edge of the last window we advertised.
    uint32_t advertised_edge;
    SequenceRange out_of_order[OutOfOrderRanges];
    size_t out_of_order_count;
    // Duplicate ACKs for out-of-order segments, sent apart so the peer can count them.
    uint32_t duplicates_owed;
    // The peer's FIN, which may arrive before the data in front of it.
    uint32_t fin_sequence;
    uint32_t retries;
    uint32_t duplicate_acknowledgments;
    uint32_t reset_sequence;
    uint16_t local_port;
    uint16_t remote_port;
    uint16_t local_mss;
    uint16_t send_mss;
    // Shifts for the window fields, both 0 unless both SYNs carried the option.
    uint8_t send_scale;
    uint8_t receive_scale;
    TcpState state;
    // Our SYN or SYN-ACK carries the window scale option.
    bool offers_window_scale;
    bool timing;
    bool rtt_measured;
    bool acknowledgment_pending;
    bool reset_pending;
    bool fin_queued;
    bool fin_seen;
    bool fin_received;
    bool fin_acknowledged;
    bool probe_pending;
    bool fast_retransmit;
    bool fast_recovery;
    bool orphaned;
};

} // namespace FeatherOS::Network

#endif
