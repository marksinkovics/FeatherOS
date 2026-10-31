#include "tcp_connection.h"

#include "kernel/memory/heap.h"
#include "user/syscall_abi.h"

namespace FeatherOS::Network
{
namespace
{

constexpr uint64_t InitialRto = 1000;
// Linux's floor; RFC 6298's one second would slow recovery on a fast link.
constexpr uint64_t MinimumRto = 200;
constexpr uint64_t MaximumRto = 60000;
constexpr uint64_t ClockGranularity = 10;
constexpr uint32_t SynRetries = 5;
constexpr uint32_t DataRetries = 8;
// Twice a maximum segment lifetime of 30 s.
constexpr uint64_t TimeWaitDuration = 60000;
constexpr uint64_t OrphanFinWait2Duration = 60000;
constexpr uint32_t InitialWindowSegments = 10;
// The size every host must accept when the peer sends no MSS option (RFC 9293 3.7.1).
constexpr uint16_t DefaultMss = 536;
constexpr size_t MaximumWindow = 0xFFFF;
// The smallest shift that lets a window field cover the largest receive ring.
constexpr uint8_t ReceiveWindowScale = 3;
static_assert((MaximumWindow << ReceiveWindowScale) >= TcpMaximumBufferSize &&
              (MaximumWindow << (ReceiveWindowScale - 1)) < TcpMaximumBufferSize);
constexpr size_t MinimumBufferSize = 4096;
// Far above the largest send ring.
constexpr uint32_t MaximumCongestionWindow = 1U << 20;

size_t Smaller(size_t left, size_t right)
{
    return left < right ? left : right;
}

void BufferWrite(TcpBuffer* buffer, size_t offset, const uint8_t* data, size_t count)
{
    for (size_t index = 0; index < count; ++index)
    {
        buffer->data[(buffer->start + offset + index) % buffer->capacity] = data[index];
    }
}

void BufferCopy(const TcpBuffer& buffer, size_t offset, uint8_t* data, size_t count)
{
    for (size_t index = 0; index < count; ++index)
    {
        data[index] = buffer.data[(buffer.start + offset + index) % buffer.capacity];
    }
}

void BufferDrop(TcpBuffer* buffer, size_t count)
{
    if (count != 0)
    {
        buffer->start = (buffer->start + count) % buffer->capacity;
        buffer->length -= count;
    }
}

// Grows the ring toward needed bytes from its start, each byte keeping its offset, and returns the
// capacity, which stays the same when memory runs out.
size_t BufferReserve(TcpBuffer* buffer, size_t needed)
{
    if (needed <= buffer->capacity || buffer->capacity == TcpMaximumBufferSize)
    {
        return buffer->capacity;
    }
    size_t capacity = buffer->capacity == 0 ? MinimumBufferSize : buffer->capacity;
    while (capacity < needed && capacity < TcpMaximumBufferSize)
    {
        capacity *= 2;
    }
    capacity = Smaller(capacity, TcpMaximumBufferSize);
    auto* data = static_cast<uint8_t*>(Memory::Allocate(capacity));
    if (data == nullptr)
    {
        return buffer->capacity;
    }
    BufferCopy(*buffer, 0, data, buffer->capacity);
    if (buffer->data != nullptr)
    {
        Memory::Deallocate(buffer->data);
    }
    buffer->data = data;
    buffer->capacity = capacity;
    buffer->start = 0;
    return capacity;
}

void BufferRelease(TcpBuffer* buffer)
{
    if (buffer->data != nullptr)
    {
        Memory::Deallocate(buffer->data);
    }
    buffer->data = nullptr;
    buffer->capacity = 0;
    buffer->start = 0;
    buffer->length = 0;
}

} // namespace

void TcpConnection::Start(uint16_t local, uint16_t remote, uint32_t initial_sequence, uint16_t mss)
{
    // The memory is fresh from the heap, so there are no rings to free yet.
    send_buffer.data = nullptr;
    send_buffer.capacity = 0;
    send_buffer.start = 0;
    send_buffer.length = 0;
    receive_buffer.data = nullptr;
    receive_buffer.capacity = 0;
    receive_buffer.start = 0;
    receive_buffer.length = 0;
    error = 0;
    deadline = 0;
    rto = InitialRto;
    smoothed_rtt = 0;
    rtt_variance = 0;
    timing_start = 0;
    timed_sequence = 0;
    initial_send_sequence = initial_sequence;
    send_unacknowledged = initial_sequence;
    send_next = initial_sequence;
    send_maximum = initial_sequence;
    send_window = 0;
    window_update_sequence = 0;
    window_update_acknowledgment = 0;
    local_mss = mss;
    send_mss = mss < DefaultMss ? mss : DefaultMss;
    congestion_window = InitialWindowSegments * send_mss;
    slow_start_threshold = UINT32_MAX;
    recover = initial_sequence;
    receive_next = 0;
    advertised_edge = 0;
    out_of_order_count = 0;
    duplicates_owed = 0;
    fin_sequence = 0;
    retries = 0;
    duplicate_acknowledgments = 0;
    reset_sequence = 0;
    local_port = local;
    remote_port = remote;
    send_scale = 0;
    receive_scale = 0;
    state = TcpState::Closed;
    offers_window_scale = true;
    timing = false;
    rtt_measured = false;
    acknowledgment_pending = false;
    reset_pending = false;
    fin_queued = false;
    fin_seen = false;
    fin_received = false;
    fin_acknowledged = false;
    probe_pending = false;
    fast_retransmit = false;
    fast_recovery = false;
    orphaned = false;
}

void TcpConnection::Connect(uint16_t local, uint16_t remote, uint32_t initial_sequence,
                            uint16_t mss)
{
    Start(local, remote, initial_sequence, mss);
    state = TcpState::SynSent;
}

void TcpConnection::Accept(const TcpSegment& syn, uint32_t initial_sequence, uint16_t mss)
{
    Start(syn.destination_port, syn.source_port, initial_sequence, mss);
    state = TcpState::SynReceived;
    receive_next = syn.sequence + 1;
    advertised_edge = receive_next;
    if (syn.maximum_segment_size != 0)
    {
        send_mss = syn.maximum_segment_size < mss ? syn.maximum_segment_size : mss;
    }
    congestion_window = InitialWindowSegments * send_mss;
    offers_window_scale = syn.has_window_scale;
    if (syn.has_window_scale)
    {
        send_scale = syn.window_scale;
        receive_scale = ReceiveWindowScale;
    }
    send_window = syn.window;
    window_update_sequence = syn.sequence;
}

void TcpConnection::Receive(const TcpSegment& segment, uint64_t now)
{
    if (state == TcpState::Closed)
    {
        return;
    }
    if (state == TcpState::SynSent)
    {
        ReceiveSynSent(segment, now);
        return;
    }
    const uint8_t flags = segment.flags;
    // The peer did not see our SYN-ACK and sent its SYN again.
    if (state == TcpState::SynReceived && (flags & (TcpSyn | TcpAck | TcpReset)) == TcpSyn &&
        segment.sequence + 1 == receive_next)
    {
        send_next = initial_send_sequence;
        return;
    }
    if (!Acceptable(segment))
    {
        if ((flags & TcpReset) == 0)
        {
            acknowledgment_pending = true;
            if (state == TcpState::TimeWait && (flags & TcpFin) != 0)
            {
                EnterTimeWait(now);
            }
        }
        return;
    }
    if ((flags & TcpReset) != 0)
    {
        // Only a reset at exactly the next sequence number counts; one elsewhere in the window
        // gets a challenge ACK, so a blind attacker has to guess (RFC 5961 3).
        if (segment.sequence != receive_next)
        {
            acknowledgment_pending = true;
        }
        else if (state == TcpState::Closing || state == TcpState::LastAck ||
                 state == TcpState::TimeWait)
        {
            state = TcpState::Closed;
            deadline = 0;
        }
        else
        {
            Fail(User::SyscallErrorConnectionReset);
        }
        return;
    }
    // A SYN in a synchronized state gets a challenge ACK too (RFC 5961 4).
    if ((flags & TcpSyn) != 0)
    {
        acknowledgment_pending = true;
        return;
    }
    if ((flags & TcpAck) == 0 || !ProcessAcknowledgment(segment, now))
    {
        return;
    }
    if (state == TcpState::Established || state == TcpState::FinWait1 ||
        state == TcpState::FinWait2)
    {
        ProcessData(segment);
        if ((flags & TcpFin) != 0)
        {
            NoteFin(segment);
        }
        if (fin_seen && fin_sequence == receive_next)
        {
            ProcessFin(now);
        }
    }
}

void TcpConnection::ReceiveSynSent(const TcpSegment& segment, uint64_t now)
{
    const bool has_ack = (segment.flags & TcpAck) != 0;
    if (has_ack && (SequenceAtOrBefore(segment.acknowledgment, initial_send_sequence) ||
                    SequenceBefore(send_maximum, segment.acknowledgment)))
    {
        return;
    }
    if ((segment.flags & TcpReset) != 0)
    {
        if (has_ack)
        {
            Fail(User::SyscallErrorConnectionRefused);
        }
        return;
    }
    if ((segment.flags & TcpSyn) == 0)
    {
        return;
    }
    receive_next = segment.sequence + 1;
    advertised_edge = receive_next;
    if (segment.maximum_segment_size != 0 && segment.maximum_segment_size < local_mss)
    {
        send_mss = segment.maximum_segment_size;
    }
    else if (segment.maximum_segment_size != 0)
    {
        send_mss = local_mss;
    }
    congestion_window = InitialWindowSegments * send_mss;
    // Our SYN offered scaling, so the peer's choice decides; its SYN's own window is not scaled.
    offers_window_scale = segment.has_window_scale;
    if (segment.has_window_scale)
    {
        send_scale = segment.window_scale;
        receive_scale = ReceiveWindowScale;
    }
    send_window = segment.window;
    window_update_sequence = segment.sequence;
    window_update_acknowledgment = segment.acknowledgment;
    acknowledgment_pending = true;
    if (!has_ack)
    {
        // A simultaneous open: answer with SYN-ACK.
        state = TcpState::SynReceived;
        send_next = initial_send_sequence;
        timing = false;
        return;
    }
    send_unacknowledged = segment.acknowledgment;
    if (timing)
    {
        timing = false;
        SampleRoundTrip(now - timing_start);
    }
    state = TcpState::Established;
    retries = 0;
    deadline = 0;
}

bool TcpConnection::Acceptable(const TcpSegment& segment) const
{
    const uint32_t length = SequenceLength(segment);
    const uint32_t window = ReceiveWindow();
    const uint32_t first = segment.sequence;
    const auto in_window = [this, window](uint32_t sequence)
    {
        return SequenceAtOrBefore(receive_next, sequence) &&
               SequenceBefore(sequence, receive_next + window);
    };
    if (length == 0)
    {
        return window == 0 ? first == receive_next : in_window(first);
    }
    return window != 0 && (in_window(first) || in_window(first + length - 1));
}

bool TcpConnection::ProcessAcknowledgment(const TcpSegment& segment, uint64_t now)
{
    const uint32_t acknowledgment = segment.acknowledgment;
    const uint32_t window = static_cast<uint32_t>(segment.window) << send_scale;
    if (state == TcpState::SynReceived)
    {
        if (!SequenceBefore(send_unacknowledged, acknowledgment) ||
            SequenceBefore(send_maximum, acknowledgment))
        {
            return false;
        }
        state = fin_queued ? TcpState::FinWait1 : TcpState::Established;
        window_update_sequence = segment.sequence;
        window_update_acknowledgment = acknowledgment;
        send_window = window;
    }
    if (SequenceBefore(send_maximum, acknowledgment))
    {
        acknowledgment_pending = true;
        return false;
    }
    if (SequenceBefore(acknowledgment, send_unacknowledged))
    {
        return true;
    }
    retries = 0;
    const uint32_t in_flight = send_maximum - send_unacknowledged;
    if (SequenceBefore(send_unacknowledged, acknowledgment))
    {
        uint32_t acknowledged = acknowledgment - send_unacknowledged;
        // The SYN takes the first sequence number but no byte of the buffer.
        if (send_unacknowledged == initial_send_sequence)
        {
            --acknowledged;
        }
        if (fin_queued && acknowledged > send_buffer.length)
        {
            fin_acknowledged = true;
        }
        BufferDrop(&send_buffer, Smaller(acknowledged, send_buffer.length));
        send_unacknowledged = acknowledgment;
        if (SequenceBefore(send_next, acknowledgment))
        {
            send_next = acknowledgment;
        }
        if (timing && SequenceAtOrBefore(timed_sequence, acknowledgment))
        {
            timing = false;
            SampleRoundTrip(now - timing_start);
        }
        if (fast_recovery && SequenceBefore(acknowledgment, recover))
        {
            // A partial acknowledgment: the segment after it was lost too, so it goes again at
            // once, and the window deflates by what left the network (RFC 6582 3.2 step 5).
            congestion_window =
                congestion_window > acknowledged ? congestion_window - acknowledged : 0;
            if (acknowledged >= send_mss || congestion_window < send_mss)
            {
                congestion_window += send_mss;
            }
            fast_retransmit = true;
            timing = false;
        }
        else if (fast_recovery)
        {
            fast_recovery = false;
            const uint32_t flight = send_maximum - acknowledgment;
            const uint32_t limit = (flight > send_mss ? flight : send_mss) + send_mss;
            congestion_window = slow_start_threshold < limit ? slow_start_threshold : limit;
        }
        else if (congestion_window < slow_start_threshold)
        {
            congestion_window += acknowledged < send_mss ? acknowledged : send_mss;
        }
        else
        {
            const uint32_t increase =
                static_cast<uint32_t>(send_mss) * send_mss / congestion_window;
            congestion_window += increase == 0 ? 1 : increase;
        }
        if (congestion_window > MaximumCongestionWindow)
        {
            congestion_window = MaximumCongestionWindow;
        }
        duplicate_acknowledgments = 0;
        deadline = send_unacknowledged == send_maximum ? 0 : now + rto;
    }
    else if (segment.payload_length == 0 && (segment.flags & TcpFin) == 0 &&
             window == send_window && in_flight != 0)
    {
        // Each duplicate means a segment left the network, so another may enter it.
        if (fast_recovery)
        {
            congestion_window += send_mss;
        }
        // Duplicates of data sent before a timeout start no recovery (RFC 6582 4.1).
        else if (++duplicate_acknowledgments == 3 && SequenceBefore(recover, acknowledgment))
        {
            const uint32_t half = in_flight / 2;
            slow_start_threshold = half > 2U * send_mss ? half : 2U * send_mss;
            congestion_window = slow_start_threshold + 3U * send_mss;
            recover = send_maximum;
            fast_recovery = true;
            fast_retransmit = true;
            timing = false;
        }
    }
    if (SequenceBefore(window_update_sequence, segment.sequence) ||
        (window_update_sequence == segment.sequence &&
         SequenceAtOrBefore(window_update_acknowledgment, acknowledgment)))
    {
        send_window = window;
        window_update_sequence = segment.sequence;
        window_update_acknowledgment = acknowledgment;
    }
    if (!fin_acknowledged)
    {
        return true;
    }
    if (state == TcpState::FinWait1)
    {
        state = TcpState::FinWait2;
        if (orphaned)
        {
            deadline = now + OrphanFinWait2Duration;
        }
    }
    else if (state == TcpState::Closing)
    {
        EnterTimeWait(now);
    }
    else if (state == TcpState::LastAck)
    {
        state = TcpState::Closed;
        deadline = 0;
        return false;
    }
    return true;
}

void TcpConnection::ProcessData(const TcpSegment& segment)
{
    if (segment.payload_length == 0)
    {
        return;
    }
    const uint8_t* data = segment.payload;
    size_t length = segment.payload_length;
    uint32_t sequence = segment.sequence;
    if (SequenceBefore(sequence, receive_next))
    {
        const uint32_t seen = receive_next - sequence;
        if (seen >= length)
        {
            acknowledgment_pending = true;
            return;
        }
        data += seen;
        length -= seen;
        sequence = receive_next;
    }
    // Every byte in the window has a place in the ring, which stays the same as the reader takes
    // bytes from the front. The ring grows to reach it; when memory runs out, or a scaled window
    // rounded up promised a few bytes past the largest ring, the rest is dropped for the peer to
    // send again.
    const size_t offset = sequence - receive_next;
    size_t count = Smaller(length, ReceiveWindow() - offset);
    const size_t room = BufferReserve(&receive_buffer, receive_buffer.length + offset + count) -
                        receive_buffer.length;
    count = offset < room ? Smaller(count, room - offset) : 0;
    if (count == 0)
    {
        acknowledgment_pending = true;
        return;
    }
    BufferWrite(&receive_buffer, receive_buffer.length + offset, data, count);
    if (offset != 0)
    {
        StoreOutOfOrder(sequence, sequence + static_cast<uint32_t>(count));
        ++duplicates_owed;
        return;
    }
    uint32_t end = sequence + static_cast<uint32_t>(count);
    size_t filled = 0;
    while (filled < out_of_order_count && SequenceAtOrBefore(out_of_order[filled].begin, end))
    {
        if (SequenceBefore(end, out_of_order[filled].end))
        {
            end = out_of_order[filled].end;
        }
        ++filled;
    }
    for (size_t index = filled; index < out_of_order_count; ++index)
    {
        out_of_order[index - filled] = out_of_order[index];
    }
    out_of_order_count -= filled;
    receive_buffer.length += end - receive_next;
    receive_next = end;
    duplicates_owed = 0;
    acknowledgment_pending = true;
}

void TcpConnection::StoreOutOfOrder(uint32_t begin, uint32_t end)
{
    size_t first = 0;
    while (first < out_of_order_count && SequenceBefore(out_of_order[first].end, begin))
    {
        ++first;
    }
    size_t last = first;
    while (last < out_of_order_count && SequenceAtOrBefore(out_of_order[last].begin, end))
    {
        if (SequenceBefore(out_of_order[last].begin, begin))
        {
            begin = out_of_order[last].begin;
        }
        if (SequenceBefore(end, out_of_order[last].end))
        {
            end = out_of_order[last].end;
        }
        ++last;
    }
    if (first == last)
    {
        // Without room, the highest range goes, since the lower ones fill the gap sooner. Its
        // bytes stay in the ring until the peer sends them again.
        if (out_of_order_count == OutOfOrderRanges)
        {
            if (first == out_of_order_count)
            {
                return;
            }
            --out_of_order_count;
        }
        for (size_t index = out_of_order_count; index > first; --index)
        {
            out_of_order[index] = out_of_order[index - 1];
        }
        ++out_of_order_count;
    }
    else
    {
        const size_t merged = last - first - 1;
        for (size_t index = last; index < out_of_order_count; ++index)
        {
            out_of_order[index - merged] = out_of_order[index];
        }
        out_of_order_count -= merged;
    }
    out_of_order[first].begin = begin;
    out_of_order[first].end = end;
}

void TcpConnection::NoteFin(const TcpSegment& segment)
{
    const uint32_t sequence = segment.sequence + static_cast<uint32_t>(segment.payload_length);
    // Past the window, the data before the FIN did not all fit, and the peer sends it again.
    if (SequenceAtOrBefore(receive_next, sequence) &&
        SequenceAtOrBefore(sequence, receive_next + ReceiveWindow()))
    {
        fin_seen = true;
        fin_sequence = sequence;
    }
    acknowledgment_pending = true;
}

void TcpConnection::ProcessFin(uint64_t now)
{
    ++receive_next;
    fin_received = true;
    acknowledgment_pending = true;
    if (state == TcpState::Established)
    {
        state = TcpState::CloseWait;
    }
    else if (state == TcpState::FinWait1)
    {
        if (fin_acknowledged)
        {
            EnterTimeWait(now);
        }
        else
        {
            state = TcpState::Closing;
        }
    }
    else if (state == TcpState::FinWait2)
    {
        EnterTimeWait(now);
    }
}

bool TcpConnection::OnTimer(uint64_t now)
{
    if (deadline == 0 || now < deadline)
    {
        return false;
    }
    deadline = 0;
    if (state == TcpState::TimeWait || state == TcpState::FinWait2)
    {
        state = TcpState::Closed;
        return true;
    }
    if (state == TcpState::Closed)
    {
        return false;
    }
    rto = rto * 2 < MaximumRto ? rto * 2 : MaximumRto;
    timing = false;
    fast_retransmit = false;
    fast_recovery = false;
    duplicate_acknowledgments = 0;
    // The peer's window is closed: probe it with a byte, for as long as the peer answers.
    if (Synchronized() && state != TcpState::SynReceived && send_window == 0 &&
        send_buffer.length != 0)
    {
        send_next = send_unacknowledged;
        probe_pending = true;
        return false;
    }
    const uint32_t in_flight = send_maximum - send_unacknowledged;
    if (in_flight == 0)
    {
        return false;
    }
    const bool opening = state == TcpState::SynSent || state == TcpState::SynReceived;
    if (++retries > (opening ? SynRetries : DataRetries))
    {
        if (Synchronized())
        {
            reset_pending = true;
            reset_sequence = send_next;
        }
        Fail(User::SyscallErrorTimedOut);
        return true;
    }
    const uint32_t half = in_flight / 2;
    slow_start_threshold = half > 2U * send_mss ? half : 2U * send_mss;
    congestion_window = send_mss;
    recover = send_maximum;
    send_next = send_unacknowledged;
    return false;
}

size_t TcpConnection::NextSegment(uint64_t now, uint8_t* segment)
{
    if (reset_pending)
    {
        reset_pending = false;
        return WriteHeader(segment, reset_sequence, TcpReset | TcpAck);
    }
    if (state == TcpState::Closed)
    {
        return 0;
    }
    if ((state == TcpState::SynSent || state == TcpState::SynReceived) &&
        send_next == initial_send_sequence)
    {
        const uint8_t flags = state == TcpState::SynSent ? TcpSyn : TcpSyn | TcpAck;
        const size_t length = WriteHeader(segment, initial_send_sequence, flags);
        if (!timing && retries == 0)
        {
            timing = true;
            timed_sequence = initial_send_sequence + 1;
            timing_start = now;
        }
        send_next = initial_send_sequence + 1;
        if (SequenceBefore(send_maximum, send_next))
        {
            send_maximum = send_next;
        }
        if (deadline == 0)
        {
            deadline = now + rto;
        }
        acknowledgment_pending = false;
        return length;
    }
    if (duplicates_owed != 0)
    {
        --duplicates_owed;
        acknowledgment_pending = false;
        return WriteHeader(segment, send_next, TcpAck);
    }
    const bool sends_data =
        state != TcpState::SynSent && state != TcpState::SynReceived && state != TcpState::TimeWait;
    if (sends_data && fast_retransmit)
    {
        fast_retransmit = false;
        const size_t count = Smaller(send_buffer.length, send_mss);
        if (count != 0)
        {
            const size_t header = WriteHeader(segment, send_unacknowledged, TcpAck);
            BufferCopy(send_buffer, 0, segment + header, count);
            deadline = now + rto;
            acknowledgment_pending = false;
            return header + count;
        }
    }
    const size_t unsent = sends_data ? Unsent() : 0;
    if (unsent != 0)
    {
        const uint32_t in_flight = send_next - send_unacknowledged;
        const uint32_t window = send_window < congestion_window ? send_window : congestion_window;
        size_t usable = window > in_flight ? window - in_flight : 0;
        if (usable == 0 && probe_pending)
        {
            usable = 1;
        }
        probe_pending = false;
        if (usable != 0)
        {
            const size_t offset = send_next - send_unacknowledged;
            const size_t count = Smaller(Smaller(unsent, usable), send_mss);
            const bool last = offset + count == send_buffer.length;
            const bool with_fin = last && fin_queued;
            const size_t header = WriteHeader(
                segment, send_next, TcpAck | (last ? TcpPush : 0) | (with_fin ? TcpFin : 0));
            BufferCopy(send_buffer, offset, segment + header, count);
            if (!timing && send_next == send_maximum)
            {
                timing = true;
                timed_sequence = send_next + static_cast<uint32_t>(count);
                timing_start = now;
            }
            send_next += static_cast<uint32_t>(count) + (with_fin ? 1 : 0);
            if (SequenceBefore(send_maximum, send_next))
            {
                send_maximum = send_next;
            }
            if (deadline == 0)
            {
                deadline = now + rto;
            }
            acknowledgment_pending = false;
            return header + count;
        }
        // The window is closed: the persist timer probes it.
        if (deadline == 0)
        {
            deadline = now + rto;
        }
    }
    else if (sends_data && fin_queued && !fin_acknowledged &&
             send_next == send_unacknowledged + send_buffer.length)
    {
        const size_t length = WriteHeader(segment, send_next, TcpFin | TcpAck);
        ++send_next;
        if (SequenceBefore(send_maximum, send_next))
        {
            send_maximum = send_next;
        }
        if (deadline == 0)
        {
            deadline = now + rto;
        }
        acknowledgment_pending = false;
        return length;
    }
    if (acknowledgment_pending)
    {
        acknowledgment_pending = false;
        return WriteHeader(segment, send_next, TcpAck);
    }
    return 0;
}

uint64_t TcpConnection::Deadline() const
{
    return deadline;
}

size_t TcpConnection::Write(const uint8_t* data, size_t length)
{
    if (!CanWrite())
    {
        return 0;
    }
    const size_t capacity =
        BufferReserve(&send_buffer, send_buffer.length + Smaller(length, TcpMaximumBufferSize));
    const size_t count = Smaller(length, capacity - send_buffer.length);
    BufferWrite(&send_buffer, send_buffer.length, data, count);
    send_buffer.length += count;
    return count;
}

size_t TcpConnection::Read(uint8_t* data, size_t capacity)
{
    const size_t count = Smaller(capacity, receive_buffer.length);
    BufferCopy(receive_buffer, 0, data, count);
    BufferDrop(&receive_buffer, count);
    if (count != 0 && !fin_received && Synchronized() &&
        ReceiveWindow() != advertised_edge - receive_next)
    {
        acknowledgment_pending = true;
    }
    return count;
}

void TcpConnection::Close()
{
    switch (state)
    {
    case TcpState::SynSent:
        state = TcpState::Closed;
        deadline = 0;
        break;
    case TcpState::SynReceived:
        fin_queued = true;
        break;
    case TcpState::Established:
        fin_queued = true;
        state = TcpState::FinWait1;
        break;
    case TcpState::CloseWait:
        fin_queued = true;
        state = TcpState::LastAck;
        break;
    default:
        break;
    }
}

void TcpConnection::Abort()
{
    if (Synchronized())
    {
        reset_pending = true;
        reset_sequence = send_next;
    }
    state = TcpState::Closed;
    deadline = 0;
    acknowledgment_pending = false;
}

void TcpConnection::Drop(uint64_t failure)
{
    Fail(failure);
    reset_pending = false;
}

void TcpConnection::Release()
{
    BufferRelease(&send_buffer);
    BufferRelease(&receive_buffer);
}

void TcpConnection::Orphan(uint64_t now)
{
    orphaned = true;
    if (state == TcpState::FinWait2)
    {
        deadline = now + OrphanFinWait2Duration;
    }
}

TcpState TcpConnection::State() const
{
    return state;
}

uint64_t TcpConnection::Error() const
{
    return error;
}

size_t TcpConnection::Readable() const
{
    return receive_buffer.length;
}

size_t TcpConnection::Writable() const
{
    return CanWrite() ? TcpMaximumBufferSize - send_buffer.length : 0;
}

bool TcpConnection::ReceivedFin() const
{
    return fin_received;
}

bool TcpConnection::Opening() const
{
    return state == TcpState::SynSent || state == TcpState::SynReceived;
}

bool TcpConnection::CanWrite() const
{
    return (state == TcpState::Established || state == TcpState::CloseWait) && !fin_queued;
}

bool TcpConnection::Idle() const
{
    return state == TcpState::Closed && !reset_pending;
}

bool TcpConnection::AcknowledgmentPending() const
{
    return acknowledgment_pending;
}

void TcpConnection::Fail(uint64_t failure)
{
    error = failure;
    state = TcpState::Closed;
    deadline = 0;
    acknowledgment_pending = false;
}

void TcpConnection::EnterTimeWait(uint64_t now)
{
    state = TcpState::TimeWait;
    deadline = now + TimeWaitDuration;
}

void TcpConnection::SampleRoundTrip(uint64_t sample)
{
    if (!rtt_measured)
    {
        rtt_measured = true;
        smoothed_rtt = sample;
        rtt_variance = sample / 2;
    }
    else
    {
        const uint64_t difference =
            smoothed_rtt > sample ? smoothed_rtt - sample : sample - smoothed_rtt;
        rtt_variance = (3 * rtt_variance + difference) / 4;
        smoothed_rtt = (7 * smoothed_rtt + sample) / 8;
    }
    const uint64_t spread =
        4 * rtt_variance > ClockGranularity ? 4 * rtt_variance : ClockGranularity;
    rto = smoothed_rtt + spread;
    rto = rto < MinimumRto ? MinimumRto : (rto > MaximumRto ? MaximumRto : rto);
}

uint32_t TcpConnection::ReceiveWindow() const
{
    // The right edge moves on only once the space is worth a segment, not byte by byte (RFC 9293
    // 3.8.6.2.2), which also keeps duplicate ACKs' windows the same, as the peer requires. A
    // scaled window moves in steps of 2^shift, and rounding never pulls back an edge already
    // advertised (RFC 7323 2.4), so holding the edge may round up past the space.
    const size_t limit = Smaller(TcpMaximumBufferSize, MaximumWindow << receive_scale);
    const size_t space = receive_buffer.length < limit ? limit - receive_buffer.length : 0;
    // A FIN into a closed window takes receive_next past the edge.
    const size_t advertised =
        SequenceBefore(receive_next, advertised_edge) ? advertised_edge - receive_next : 0;
    const size_t step = size_t{1} << receive_scale;
    const size_t grown = space & ~(step - 1);
    if (space >= advertised + Smaller(limit / 2, send_mss) && grown >= advertised)
    {
        return static_cast<uint32_t>(grown);
    }
    return static_cast<uint32_t>((advertised + step - 1) & ~(step - 1));
}

size_t TcpConnection::WriteHeader(uint8_t* segment, uint32_t sequence, uint8_t flags)
{
    const bool syn = (flags & TcpSyn) != 0;
    const uint32_t window = ReceiveWindow();
    const TcpSegment fields = {
        .source_port = local_port,
        .destination_port = remote_port,
        .sequence = sequence,
        .acknowledgment = (flags & TcpAck) != 0 ? receive_next : 0,
        .flags = flags,
        // A SYN's own window is never scaled (RFC 7323 2.2).
        .window =
            static_cast<uint16_t>(syn ? Smaller(window, MaximumWindow) : window >> receive_scale),
        .maximum_segment_size = syn ? local_mss : static_cast<uint16_t>(0),
        .has_window_scale = syn && offers_window_scale,
        .window_scale = ReceiveWindowScale,
        .payload = nullptr,
        .payload_length = 0,
    };
    if ((flags & TcpAck) != 0)
    {
        advertised_edge = receive_next + (syn ? fields.window : window);
    }
    return WriteTcpSegment(segment, MaximumSegmentLength, fields);
}

size_t TcpConnection::Unsent() const
{
    const size_t offset = send_next - send_unacknowledged;
    return offset < send_buffer.length ? send_buffer.length - offset : 0;
}

bool TcpConnection::Synchronized() const
{
    return state != TcpState::Closed && state != TcpState::SynSent;
}

} // namespace FeatherOS::Network
