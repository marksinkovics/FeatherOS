#include "bot.h"

#include <algorithm>

namespace FeatherOS::Usb::Storage::Bot
{
namespace
{

constexpr uint8_t DirectionIn = 0x80;

void PutLittleEndian(uint8_t* out, uint32_t value)
{
    for (uint32_t i = 0; i < 4; i++)
    {
        out[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

uint32_t GetLittleEndian(const uint8_t* in)
{
    uint32_t value = 0;
    for (uint32_t i = 0; i < 4; i++)
    {
        value |= static_cast<uint32_t>(in[i]) << (8 * i);
    }
    return value;
}

Status RunStage(Pipe& pipe, Stage stage, uint32_t length, uint32_t timeout_ms, uint32_t* actual)
{
    *actual = 0;
    Status status = Status::Success;
    if (!pipe.Start(stage, length, &status))
    {
        return status;
    }
    return pipe.Finish(pipe.NowMilliseconds() + timeout_ms, actual);
}

Result Recovered(Pipe& pipe)
{
    Result result{Outcome::TransportFailed, 0, 0, true};
    if (pipe.Recover() == Status::NoDevice)
    {
        result.outcome = Outcome::NoDevice;
    }
    return result;
}

Result Failed(Pipe& pipe, Status status)
{
    if (status == Status::NoDevice)
    {
        return Result{Outcome::NoDevice, 0, 0, false};
    }
    return Recovered(pipe);
}

} // namespace

void PackCbw(uint8_t* out, uint32_t tag, uint32_t length, Scsi::Direction direction,
             const Scsi::Cdb& cdb)
{
    std::fill(out, out + CbwSize, uint8_t{0});
    PutLittleEndian(out, CbwSignature);
    PutLittleEndian(out + 4, tag);
    PutLittleEndian(out + 8, length);
    out[12] = direction == Scsi::Direction::In ? DirectionIn : 0;
    out[13] = 0;
    out[14] = cdb.length;
    std::copy(cdb.bytes, cdb.bytes + sizeof(cdb.bytes), out + 15);
}

CswCheck CheckCsw(std::span<const uint8_t> bytes, uint32_t actual, uint32_t tag, uint32_t requested,
                  uint32_t* residue)
{
    if (actual != CswSize || bytes.size() < CswSize)
    {
        return CswCheck::Invalid;
    }
    if (GetLittleEndian(&bytes[0]) != CswSignature || GetLittleEndian(&bytes[4]) != tag)
    {
        return CswCheck::Invalid;
    }
    const uint32_t csw_residue = GetLittleEndian(&bytes[8]);
    const uint8_t status = bytes[12];
    if (status > 2 || csw_residue > requested)
    {
        return CswCheck::Invalid;
    }
    *residue = csw_residue;
    switch (status)
    {
    case 0:
        return CswCheck::Passed;
    case 1:
        return CswCheck::Failed;
    default:
        return CswCheck::PhaseError;
    }
}

Result Execute(Pipe& pipe, uint32_t tag, const Command& command)
{
    const bool has_data = command.direction != Scsi::Direction::None && command.length != 0;
    const uint32_t length = has_data ? command.length : 0;

    PackCbw(pipe.Buffer(Stage::Command).data(), tag, length, command.direction, command.cdb);
    uint32_t actual = 0;
    Status status = RunStage(pipe, Stage::Command, CbwSize, CommandTimeoutMs, &actual);
    if (status != Status::Success)
    {
        return Failed(pipe, status);
    }

    uint32_t data_actual = 0;
    if (has_data)
    {
        const Stage stage =
            command.direction == Scsi::Direction::In ? Stage::DataIn : Stage::DataOut;
        status = RunStage(pipe, stage, length, command.timeout_ms, &data_actual);
        if (!Succeeded(status) && status != Status::Stall)
        {
            return Failed(pipe, status);
        }
    }

    uint32_t csw_actual = 0;
    status = RunStage(pipe, Stage::Status, CswSize, command.timeout_ms, &csw_actual);
    if (status == Status::Stall)
    {
        status = RunStage(pipe, Stage::Status, CswSize, command.timeout_ms, &csw_actual);
    }
    if (!Succeeded(status))
    {
        return Failed(pipe, status);
    }

    uint32_t residue = 0;
    const CswCheck check = CheckCsw(pipe.Buffer(Stage::Status), csw_actual, tag, length, &residue);
    if (check == CswCheck::PhaseError || check == CswCheck::Invalid)
    {
        return Recovered(pipe);
    }

    Result result{Outcome::Passed, std::min(data_actual, length - residue), residue, false};
    if (check == CswCheck::Failed)
    {
        result.outcome = Outcome::CheckCondition;
    }
    else if (Scsi::IsReadOrWrite(command.cdb) && (residue != 0 || data_actual != length))
    {
        result.outcome = Outcome::TransportFailed;
    }
    return result;
}

} // namespace FeatherOS::Usb::Storage::Bot
