#include "drive.h"

#include "kernel/klogger.h"
#include "kernel/usb/text.h"

#include <algorithm>
#include <expected>
#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Usb::Storage
{
namespace
{

constexpr uint8_t ClassToInterface = 0x21;
constexpr uint8_t BulkOnlyReset = 0xFF;
constexpr uint8_t GetMaxLunRequest = 0xFE;
constexpr uint8_t InterfaceToHostClass = 0xA1;
constexpr uint8_t MaxLunLimit = 15;
constexpr uint32_t StatusOffset = 64;
constexpr uint32_t TagStep = 0x10000;

constinit KLogger logger;

// Lock order: registry_lock, then a drive's state_lock_.
constinit Sync::SpinLock registry_lock;
Drive* registered GUARDED_BY(registry_lock) = nullptr;

uint8_t* Bytes(const Memory::Dma::Buffer& buffer)
{
    return const_cast<uint8_t*>(buffer.address);
}

const char* StageName(Bot::Stage stage)
{
    switch (stage)
    {
    case Bot::Stage::Command:
        return "command";
    case Bot::Stage::DataIn:
        return "data-in";
    case Bot::Stage::DataOut:
        return "data-out";
    case Bot::Stage::Status:
        return "status";
    }
    return "unknown";
}

const char* StatusName(Usb::Status status)
{
    switch (status)
    {
    case Usb::Status::Success:
        return "success";
    case Usb::Status::ShortPacket:
        return "short packet";
    case Usb::Status::Stall:
        return "stall";
    case Usb::Status::TransactionError:
        return "transaction error";
    case Usb::Status::Babble:
        return "babble";
    case Usb::Status::Timeout:
        return "timeout";
    case Usb::Status::NoDevice:
        return "no device";
    case Usb::Status::Cancelled:
        return "cancelled";
    case Usb::Status::ControllerError:
        return "controller error";
    }
    return "error";
}

void PutDecimal(Text& text, uint64_t value)
{
    char digits[20];
    uint32_t count = 0;
    do
    {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count != 0)
    {
        text.Put(digits[--count]);
    }
}

void Copy(char* out, size_t capacity, const char* text)
{
    size_t length = 0;
    while (text[length] != '\0' && length + 1 < capacity)
    {
        out[length] = text[length];
        ++length;
    }
    out[length] = '\0';
}

bool ValidBlockSize(uint32_t size)
{
    return size == 512 || size == 1024 || size == 2048 || size == 4096;
}

std::span<const uint8_t> Received(std::span<uint8_t> buffer, uint32_t actual)
{
    return buffer.first(std::min<size_t>(actual, buffer.size()));
}

} // namespace

Drive::TaskPipe::TaskPipe(Drive& drive) : drive_(drive) {}

std::span<uint8_t> Drive::TaskPipe::Buffer(Bot::Stage stage)
{
    switch (stage)
    {
    case Bot::Stage::Command:
        return {Bytes(drive_.command_view_), Bot::CbwSize};
    case Bot::Stage::Status:
        return {Bytes(drive_.status_view_), Bot::CswSize};
    case Bot::Stage::DataIn:
    case Bot::Stage::DataOut:
        break;
    }
    return {Bytes(drive_.data_), static_cast<size_t>(drive_.data_.size)};
}

bool Drive::TaskPipe::Start(Bot::Stage stage, uint32_t length, Usb::Status* status)
{
    Drive& drive = drive_;
    Sync::SpinLockGuard guard(drive.state_lock_);
    if (drive.gone_ || drive.device_ == nullptr)
    {
        *status = Usb::Status::NoDevice;
        return false;
    }
    if (drive.busy_ || drive.broken_)
    {
        *status = Usb::Status::ControllerError;
        return false;
    }
    const bool in = stage == Bot::Stage::DataIn || stage == Bot::Stage::Status;
    DeviceTransfer& transfer = drive.transfer_;
    transfer.endpoint = in ? drive.in_endpoint_ : drive.out_endpoint_;
    transfer.buffer = stage == Bot::Stage::Command  ? &drive.command_view_
                      : stage == Bot::Stage::Status ? &drive.status_view_
                                                    : &drive.data_;
    transfer.length = length;
    transfer.callback = OnTransferDone;
    transfer.driver_context = &drive;
    drive.last_stage_ = stage;
    drive.busy_ = true;
    drive.done_ = false;
    drive.status_ = Usb::Status::Success;
    drive.actual_ = 0;
    if (!drive.device_->Submit(transfer))
    {
        drive.busy_ = false;
        const Usb::Status why = transfer.status;
        *status = why == Usb::Status::Cancelled || Succeeded(why) ? Usb::Status::NoDevice : why;
        return false;
    }
    return true;
}

Usb::Status Drive::TaskPipe::Finish(uint64_t deadline_ms, uint32_t* actual)
{
    Drive& drive = drive_;
    *actual = 0;
    drive.environment_.WaitUntil(&drive, deadline_ms, ReadyDone, &drive);
    Sync::SpinLockGuard guard(drive.state_lock_);
    if (drive.done_)
    {
        *actual = drive.actual_;
        drive.last_status_ = drive.status_;
        return drive.status_;
    }
    drive.last_status_ = drive.gone_ ? Usb::Status::NoDevice : Usb::Status::Timeout;
    return drive.last_status_;
}

Usb::Status Drive::TaskPipe::Recover()
{
    Drive& drive = drive_;
    Bot::Stage stage;
    Usb::Status cause;
    {
        Sync::SpinLockGuard guard(drive.state_lock_);
        if (drive.gone_ || drive.device_ == nullptr)
        {
            return Usb::Status::NoDevice;
        }
        if (drive.broken_)
        {
            return Usb::Status::ControllerError;
        }
        ++drive.request_serial_;
        stage = drive.last_stage_;
        cause = drive.last_status_;
        drive.device_->PostWork();
    }
    drive.LogRecovery(stage, cause);
    Environment& environment = drive.environment_;
    environment.WaitUntil(&drive, environment.NowMilliseconds() + RecoveryTimeoutMs, ReadyReply,
                          &drive);
    Usb::Status result;
    {
        Sync::SpinLockGuard guard(drive.state_lock_);
        if (drive.gone_)
        {
            return Usb::Status::NoDevice;
        }
        result = drive.reply_serial_ == drive.request_serial_ ? drive.reply_status_
                                                              : Usb::Status::Timeout;
    }
    // The cancelled transfer's callback can still be on its way to the event task.
    const bool settled = environment.WaitUntil(
        &drive, environment.NowMilliseconds() + SettleTimeoutMs, ReadyIdle, &drive);
    Sync::SpinLockGuard guard(drive.state_lock_);
    if (drive.gone_)
    {
        return Usb::Status::NoDevice;
    }
    if (!settled)
    {
        result = Usb::Status::Timeout;
    }
    if (result != Usb::Status::Success)
    {
        drive.broken_ = true;
    }
    return result;
}

uint64_t Drive::TaskPipe::NowMilliseconds()
{
    return drive_.environment_.NowMilliseconds();
}

Drive::Drive(Device& device, const InterfaceView& interface, Environment& environment)
    : environment_(environment), host_(device.Host()), slot_(device.slot),
      interface_(interface.number), pipe_(*this)
{
    for (uint32_t index = 0; index < interface.endpoint_count; ++index)
    {
        const EndpointDescriptor& endpoint = interface.endpoints[index];
        if (endpoint.type != EndpointType::Bulk)
        {
            continue;
        }
        if ((endpoint.address & 0x80U) != 0)
        {
            in_endpoint_ = in_endpoint_ == 0 ? endpoint.address : in_endpoint_;
        }
        else
        {
            out_endpoint_ = out_endpoint_ == 0 ? endpoint.address : out_endpoint_;
        }
    }
    Text text(location_, sizeof(location_));
    text.Words("USB ");
    text.Words(device.path);
    text.Finish();
    device_ = &device;
    tag_ = (device.slot + 1) * TagStep;
}

Drive* Drive::Create(Device& device, const InterfaceView& interface, Environment& environment)
{
    auto* drive = new Drive(device, interface, environment);
    if (drive->in_endpoint_ == 0 || drive->out_endpoint_ == 0 || !drive->Allocate())
    {
        drive->Free();
        delete drive;
        return nullptr;
    }
    return drive;
}

bool Drive::Allocate()
{
    if (!host_.AllocateBuffer(CommandPageBytes, &command_page_) ||
        !host_.AllocateBuffer(Scsi::MaxTransferBytes, &data_))
    {
        return false;
    }
    command_view_ = {command_page_.physical, command_page_.address, StatusOffset};
    status_view_ = {command_page_.physical + StatusOffset, command_page_.address + StatusOffset,
                    CommandPageBytes - StatusOffset};
    return true;
}

void Drive::Free()
{
    if (command_page_.size != 0)
    {
        host_.FreeBuffer(command_page_);
    }
    if (data_.size != 0)
    {
        host_.FreeBuffer(data_);
    }
}

void Drive::Retain()
{
    references_.fetch_add(1, std::memory_order_relaxed);
}

void Drive::Release()
{
    if (references_.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        Free();
        delete this;
    }
}

bool Drive::Gone()
{
    Sync::SpinLockGuard guard(state_lock_);
    return gone_;
}

Drive::TaskPipe& Drive::Transport()
{
    return pipe_;
}

void Drive::OnTransferDone(DeviceTransfer& transfer)
{
    auto* drive = static_cast<Drive*>(transfer.driver_context);
    Environment& environment = drive->environment_;
    {
        Sync::SpinLockGuard guard(drive->state_lock_);
        drive->status_ = transfer.status;
        drive->actual_ = transfer.actual;
        drive->busy_ = false;
        drive->done_ = true;
    }
    environment.Wake(drive);
}

bool Drive::ReadyDone(void* context)
{
    auto* drive = static_cast<Drive*>(context);
    Sync::SpinLockGuard guard(drive->state_lock_);
    return drive->done_ || drive->gone_;
}

bool Drive::ReadyIdle(void* context)
{
    auto* drive = static_cast<Drive*>(context);
    Sync::SpinLockGuard guard(drive->state_lock_);
    return !drive->busy_ || drive->gone_;
}

bool Drive::ReadyReply(void* context)
{
    auto* drive = static_cast<Drive*>(context);
    Sync::SpinLockGuard guard(drive->state_lock_);
    return drive->gone_ || drive->reply_serial_ == drive->request_serial_;
}

uint8_t Drive::MaxLun()
{
    uint8_t lun = 0;
    uint32_t actual = 0;
    const SetupPacket setup = {InterfaceToHostClass, GetMaxLunRequest, 0, interface_, 1};
    if (Succeeded(host_.Control(slot_, setup, command_view_, &actual)) && actual == 1)
    {
        lun = Bytes(command_view_)[0];
    }
    if (lun > MaxLunLimit)
    {
        lun = 0;
    }
    if (lun > 0)
    {
        char line[96];
        Text text(line, sizeof(line));
        text.Words("Storage: ");
        text.Words(location_);
        text.Words(": ");
        text.Decimal(static_cast<uint32_t>(lun) + 1);
        text.Words(" LUNs, only LUN 0 is used");
        if (text.Finish())
        {
            logger.Warning(line);
        }
    }
    return lun;
}

void Drive::LogLine(const char* message)
{
    char line[160];
    Text text(line, sizeof(line));
    text.Words("Storage: ");
    text.Words(location_);
    text.Words(": ");
    text.Words(message);
    if (text.Finish())
    {
        logger.Info(line);
    }
}

void Drive::LogRecovery(Bot::Stage stage, Usb::Status cause)
{
    char line[160];
    Text text(line, sizeof(line));
    text.Words("Storage: ");
    text.Words(location_);
    text.Words(": reset recovery after ");
    text.Words(StageName(stage));
    text.Put(' ');
    text.Words(StatusName(cause));
    if (text.Finish())
    {
        logger.Warning(line);
    }
}

Usb::Status Drive::RecoverEndpoints(Device& device)
{
    HostController& host = device.Host();
    if (!host.CancelEndpoint(slot_, in_endpoint_) || !host.CancelEndpoint(slot_, out_endpoint_))
    {
        return Usb::Status::ControllerError;
    }
    Memory::Dma::Buffer none{};
    uint32_t actual = 0;
    const SetupPacket reset = {ClassToInterface, BulkOnlyReset, 0, interface_, 0};
    const Usb::Status status = host.Control(slot_, reset, none, &actual);
    if (!Succeeded(status))
    {
        return status;
    }
    if (!host.ClearHalt(slot_, in_endpoint_) || !host.ClearHalt(slot_, out_endpoint_))
    {
        return Usb::Status::ControllerError;
    }
    return Usb::Status::Success;
}

bool Drive::Service(Device& device)
{
    {
        Sync::SpinLockGuard guard(state_lock_);
        if (gone_ || request_serial_ == reply_serial_)
        {
            return false;
        }
    }
    const Usb::Status result = RecoverEndpoints(device);
    {
        Sync::SpinLockGuard guard(state_lock_);
        reply_serial_ = request_serial_;
        reply_status_ = result;
    }
    environment_.Wake(this);
    const bool reset = result != Usb::Status::Success && result != Usb::Status::NoDevice;
    if (reset)
    {
        LogLine("port reset, the drive comes back as a new disk");
    }
    return reset;
}

bool Drive::Usable()
{
    Sync::SpinLockGuard guard(state_lock_);
    return !gone_ && !broken_;
}

void Drive::MarkLost()
{
    {
        Sync::SpinLockGuard guard(state_lock_);
        broken_ = true;
    }
    LogLine("the medium may have changed, the drive is dropped");
}

void Drive::Detached()
{
    {
        Sync::SpinLockGuard guard(state_lock_);
        device_ = nullptr;
        gone_ = true;
    }
    environment_.Wake(this);
    bool held;
    {
        Sync::SpinLockGuard guard(registry_lock);
        held = registered == this;
        if (held)
        {
            registered = nullptr;
        }
    }
    if (held)
    {
        Release();
    }
    Release();
}

Bot::Result Drive::Run(const Bot::Command& command, Scsi::Sense* sense)
{
    Bot::Result result = Bot::Execute(pipe_, ++tag_, command);
    if (result.outcome != Bot::Outcome::CheckCondition)
    {
        return result;
    }
    const Bot::Command request = {Scsi::RequestSense(Scsi::RequestSenseBytes), Scsi::Direction::In,
                                  Scsi::RequestSenseBytes, Bot::CommandTimeoutMs};
    const Bot::Result reply = Bot::Execute(pipe_, ++tag_, request);
    if (reply.outcome != Bot::Outcome::Passed)
    {
        return reply.outcome == Bot::Outcome::NoDevice
                   ? reply
                   : Bot::Result{Bot::Outcome::TransportFailed, 0, 0, reply.recovered};
    }
    if (!Scsi::ParseSense(Received(pipe_.Buffer(Bot::Stage::DataIn), reply.actual), sense))
    {
        *sense = {0x0F, 0, 0};
    }
    return result;
}

Drive::Disposition Drive::Perform(const Bot::Command& command, const uint8_t* out_data,
                                  uint64_t wait_deadline_ms, bool probing, Bot::Result* result)
{
    uint32_t retries = 0;
    uint32_t transport_retries = 0;
    const bool synchronize = Scsi::IsSynchronizeCache(command.cdb);
    // A command that returns data, but with a check condition, has lost it to REQUEST SENSE.
    const bool returns_data = command.direction == Scsi::Direction::In && command.length != 0;
    for (;;)
    {
        if (out_data != nullptr)
        {
            std::copy(out_data, out_data + command.length,
                      pipe_.Buffer(Bot::Stage::DataOut).data());
        }
        Scsi::Sense sense = {};
        *result = Run(command, &sense);
        switch (result->outcome)
        {
        case Bot::Outcome::Passed:
            return Disposition::Good;
        case Bot::Outcome::NoDevice:
            return Disposition::NoDevice;
        case Bot::Outcome::TransportFailed:
            // A recovery that failed leaves the drive unusable; the manager resets the port.
            if (++transport_retries > TransportRetries || !Usable())
            {
                return Disposition::Failed;
            }
            continue;
        case Bot::Outcome::CheckCondition:
            break;
        }
        switch (Scsi::Classify(sense, synchronize))
        {
        case Scsi::Action::Success:
            if (!returns_data && !(Scsi::IsReadOrWrite(command.cdb) && result->residue != 0))
            {
                return Disposition::Good;
            }
            [[fallthrough]];
        case Scsi::Action::Retry:
            if (++retries > SenseRetries)
            {
                return Disposition::Failed;
            }
            break;
        case Scsi::Action::MediumChanged:
            if (!probing)
            {
                MarkLost();
                return Disposition::Failed;
            }
            if (++retries > SenseRetries)
            {
                return Disposition::Failed;
            }
            break;
        case Scsi::Action::WaitAndRetry:
            if (environment_.NowMilliseconds() + WaitStepMs > wait_deadline_ms)
            {
                return Disposition::Failed;
            }
            environment_.Sleep(WaitStepMs);
            break;
        case Scsi::Action::NoMedium:
            return Disposition::NoMedium;
        case Scsi::Action::WriteProtected:
            return Disposition::ReadOnly;
        case Scsi::Action::Unsupported:
            sync_unsupported_ = true;
            return Disposition::Good;
        case Scsi::Action::Fail:
            return Disposition::Failed;
        }
    }
}

bool Drive::Publish(const FeatherOS::Storage::DiskInfo& info)
{
    Sync::SpinLockGuard registry(registry_lock);
    Sync::SpinLockGuard guard(state_lock_);
    if (gone_)
    {
        return false;
    }
    info_ = info;
    registered_ = true;
    if (registered == nullptr)
    {
        registered = this;
        Retain();
    }
    return true;
}

Drive* TestDrive()
{
    Sync::SpinLockGuard guard(registry_lock);
    if (registered != nullptr)
    {
        registered->Retain();
    }
    return registered;
}

Drive::Disposition Drive::Receive(const Scsi::Cdb& cdb, uint32_t length, uint64_t wait_deadline_ms,
                                  Bot::Result* result)
{
    const Bot::Command command = {cdb, Scsi::Direction::In, length, Bot::CommandTimeoutMs};
    return Perform(command, nullptr, wait_deadline_ms, true, result);
}

bool Drive::Probe()
{
    Sync::MutexGuard guard(io_lock_);
    return ProbeLocked();
}

bool Drive::ProbeLocked()
{
    const uint64_t deadline = environment_.NowMilliseconds() + ProbeBudgetMs;
    Bot::Result result{};
    const std::span<uint8_t> data = pipe_.Buffer(Bot::Stage::DataIn);
    const auto refuse = [this](const char* message)
    {
        LogLine(message);
        return false;
    };
    const auto failed = [&](Disposition disposition, const char* message)
    {
        return refuse(disposition == Disposition::NoDevice ? "unplugged during the probe"
                                                           : message);
    };

    Disposition step =
        Receive(Scsi::Inquiry(Scsi::InquiryBytes), Scsi::InquiryBytes, deadline, &result);
    if (step != Disposition::Good)
    {
        return failed(step, "INQUIRY failed; ignored");
    }
    Scsi::InquiryData inquiry = {};
    if (!Scsi::ParseInquiry(Received(data, result.actual), &inquiry))
    {
        return refuse("INQUIRY answered too little; ignored");
    }
    if (!Scsi::IsDisk(inquiry))
    {
        char line[96];
        Text text(line, sizeof(line));
        text.Words("device type ");
        text.Hex(inquiry.type, 2);
        text.Words(" is not a disk; ignored");
        return text.Finish() ? refuse(line) : refuse("not a disk; ignored");
    }

    const Bot::Command ready = {Scsi::TestUnitReady(), Scsi::Direction::None, 0,
                                Bot::CommandTimeoutMs};
    step = Perform(ready, nullptr, deadline, true, &result);
    if (step == Disposition::NoMedium)
    {
        return refuse("no medium; ignored");
    }
    if (step != Disposition::Good)
    {
        return failed(step, "did not become ready; ignored");
    }

    step = Receive(Scsi::ReadCapacity10(), Scsi::ReadCapacity10Bytes, deadline, &result);
    if (step != Disposition::Good)
    {
        return failed(step, "READ CAPACITY failed; ignored");
    }
    Scsi::Capacity capacity = {};
    const Scsi::CapacityResult parsed =
        Scsi::ParseCapacity10(Received(data, result.actual), &capacity);
    bool valid = parsed == Scsi::CapacityResult::Ok;
    if (parsed == Scsi::CapacityResult::NeedSixteen)
    {
        step = Receive(Scsi::ReadCapacity16(Scsi::ReadCapacity16Bytes), Scsi::ReadCapacity16Bytes,
                       deadline, &result);
        if (step != Disposition::Good)
        {
            return failed(step, "READ CAPACITY(16) failed; ignored");
        }
        valid = Scsi::ParseCapacity16(Received(data, result.actual), &capacity);
    }
    if (!valid || capacity.blocks == 0 || !ValidBlockSize(capacity.block_size))
    {
        char line[96];
        Text text(line, sizeof(line));
        text.Words("unusable capacity");
        if (valid && capacity.blocks != 0)
        {
            text.Words(", ");
            text.Decimal(capacity.block_size);
            text.Words("-byte sectors");
        }
        text.Words("; ignored");
        return text.Finish() ? refuse(line) : refuse("unusable capacity; ignored");
    }

    bool write_protected = false;
    step = Receive(Scsi::ModeSense6(0x3F, Scsi::ModeSenseBytes), Scsi::ModeSenseBytes, deadline,
                   &result);
    if (step == Disposition::NoDevice)
    {
        return failed(step, "");
    }
    if (step == Disposition::Good)
    {
        Scsi::ParseWriteProtect(Received(data, result.actual), &write_protected);
    }

    FeatherOS::Storage::DiskInfo info = {};
    info.kind = FeatherOS::Storage::DiskKind::Usb;
    info.sector_size = capacity.block_size;
    info.sector_count = capacity.blocks;
    info.max_transfer_bytes = Scsi::MaxTransferBytes;
    info.removable = inquiry.removable;
    info.read_only = write_protected;
    Copy(info.location, sizeof(info.location), location_);
    {
        Text text(info.model, sizeof(info.model));
        text.Words(inquiry.vendor);
        if (inquiry.vendor[0] != '\0' && inquiry.product[0] != '\0')
        {
            text.Put(' ');
        }
        text.Words(inquiry.product);
        text.Finish();
    }
    if (!Publish(info))
    {
        return refuse("unplugged during the probe");
    }

    char line[192];
    Text text(line, sizeof(line));
    text.Words("Storage: ");
    text.Words(location_);
    text.Words(", \"");
    text.Words(inquiry.vendor);
    text.Words("\" \"");
    text.Words(inquiry.product);
    text.Words("\", ");
    PutDecimal(text, capacity.blocks * capacity.block_size / 1024);
    text.Words(" KiB, ");
    text.Decimal(capacity.block_size);
    text.Words("-byte sectors");
    if (inquiry.removable)
    {
        text.Words(", removable");
    }
    if (write_protected)
    {
        text.Words(", write-protected");
    }
    if (text.Finish())
    {
        logger.Info(line);
    }
    return true;
}

FeatherOS::Storage::DiskInfo Drive::Info()
{
    Sync::SpinLockGuard guard(state_lock_);
    return info_;
}

FeatherDisk::Status Drive::Move(uint64_t sector, uint32_t count, uint8_t* in, const uint8_t* out)
{
    FeatherOS::Storage::DiskInfo info;
    {
        Sync::SpinLockGuard guard(state_lock_);
        if (!registered_ || gone_ || broken_ || ejected_)
        {
            return std::unexpected(FeatherDisk::Error::Io);
        }
        info = info_;
    }
    if (out != nullptr && info.read_only)
    {
        return std::unexpected(FeatherDisk::Error::ReadOnly);
    }
    if (sector >= info.sector_count || count > info.sector_count - sector)
    {
        return std::unexpected(FeatherDisk::Error::InvalidArgument);
    }
    const uint32_t per_command = info.max_transfer_bytes / info.sector_size;
    while (count != 0)
    {
        const uint32_t blocks = std::min(count, per_command);
        const uint32_t length = blocks * info.sector_size;
        const Bot::Command command = {out != nullptr ? Scsi::Write(sector, blocks)
                                                     : Scsi::Read(sector, blocks),
                                      out != nullptr ? Scsi::Direction::Out : Scsi::Direction::In,
                                      length, Bot::DataTimeoutMs};
        Bot::Result result{};
        Disposition disposition;
        {
            Sync::MutexGuard guard(io_lock_);
            disposition = Perform(command, out, environment_.NowMilliseconds() + WaitBudgetMs,
                                  false, &result);
            if (disposition == Disposition::Good && in != nullptr)
            {
                const std::span<uint8_t> data = pipe_.Buffer(Bot::Stage::DataIn);
                std::copy(data.begin(), data.begin() + length, in);
            }
        }
        if (disposition == Disposition::ReadOnly)
        {
            Sync::SpinLockGuard guard(state_lock_);
            info_.read_only = true;
            return std::unexpected(FeatherDisk::Error::ReadOnly);
        }
        if (disposition != Disposition::Good)
        {
            return std::unexpected(FeatherDisk::Error::Io);
        }
        sector += blocks;
        count -= blocks;
        in = in != nullptr ? in + length : nullptr;
        out = out != nullptr ? out + length : nullptr;
    }
    return {};
}

FeatherDisk::Status Drive::Read(uint64_t sector, uint32_t count, uint8_t* buffer)
{
    return Move(sector, count, buffer, nullptr);
}

FeatherDisk::Status Drive::Write(uint64_t sector, uint32_t count, const uint8_t* buffer)
{
    return Move(sector, count, nullptr, buffer);
}

FeatherDisk::Status Drive::Flush()
{
    {
        Sync::SpinLockGuard guard(state_lock_);
        if (!registered_ || gone_ || broken_ || ejected_)
        {
            return std::unexpected(FeatherDisk::Error::Io);
        }
    }
    Sync::MutexGuard guard(io_lock_);
    if (sync_unsupported_)
    {
        return {};
    }
    const Bot::Command command = {Scsi::SynchronizeCache(), Scsi::Direction::None, 0,
                                  Bot::SlowCommandTimeoutMs};
    Bot::Result result{};
    const Disposition disposition =
        Perform(command, nullptr, environment_.NowMilliseconds() + WaitBudgetMs, false, &result);
    if (disposition != Disposition::Good)
    {
        return std::unexpected(FeatherDisk::Error::Io);
    }
    return {};
}

FeatherDisk::Status Drive::Stop()
{
    bool removable;
    {
        Sync::SpinLockGuard guard(state_lock_);
        if (!registered_ || gone_ || broken_ || ejected_)
        {
            return std::unexpected(FeatherDisk::Error::Io);
        }
        removable = info_.removable;
    }
    Sync::MutexGuard guard(io_lock_);
    const Bot::Command command = {Scsi::StartStopUnit(false, removable), Scsi::Direction::None, 0,
                                  Bot::SlowCommandTimeoutMs};
    Bot::Result result{};
    const Disposition disposition =
        Perform(command, nullptr, environment_.NowMilliseconds() + WaitBudgetMs, false, &result);
    if (disposition != Disposition::Good)
    {
        return std::unexpected(FeatherDisk::Error::Io);
    }
    Sync::SpinLockGuard state(state_lock_);
    ejected_ = true;
    return {};
}

} // namespace FeatherOS::Usb::Storage
