#include "scsi.h"

namespace FeatherOS::Usb::Storage::Scsi
{
namespace
{

constexpr uint64_t TenByteLimit = 1ULL << 32;
constexpr uint32_t MaxTenByteBlocks = 0xFFFF;

constexpr uint8_t SenseNotReady = 2;
constexpr uint8_t SenseUnitAttention = 6;
constexpr uint8_t SenseIllegalRequest = 5;
constexpr uint8_t SenseDataProtect = 7;
constexpr uint8_t SenseAbortedCommand = 0xB;

void PutBigEndian(Cdb& cdb, uint32_t offset, uint64_t value, uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; i++)
    {
        cdb.bytes[offset + i] = static_cast<uint8_t>(value >> (8 * (bytes - 1 - i)));
    }
}

uint64_t GetBigEndian(std::span<const uint8_t> bytes, uint32_t offset, uint32_t count)
{
    uint64_t value = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        value = (value << 8) | bytes[offset + i];
    }
    return value;
}

Cdb NewCdb(uint8_t opcode, uint8_t length)
{
    Cdb cdb{};
    cdb.bytes[0] = opcode;
    cdb.length = length;
    return cdb;
}

bool FitsTenByte(uint64_t lba, uint32_t blocks)
{
    return blocks <= MaxTenByteBlocks && lba <= TenByteLimit && blocks <= TenByteLimit - lba;
}

Cdb Transfer(uint8_t opcode10, uint8_t opcode16, uint64_t lba, uint32_t blocks)
{
    if (FitsTenByte(lba, blocks))
    {
        Cdb cdb = NewCdb(opcode10, 10);
        PutBigEndian(cdb, 2, lba, 4);
        PutBigEndian(cdb, 7, blocks, 2);
        return cdb;
    }
    Cdb cdb = NewCdb(opcode16, 16);
    PutBigEndian(cdb, 2, lba, 8);
    PutBigEndian(cdb, 10, blocks, 4);
    return cdb;
}

void CopyField(const uint8_t* source, uint32_t size, char* out)
{
    uint32_t first = 0;
    uint32_t last = size;
    while (first < last && (source[first] == ' ' || source[first] == 0))
    {
        first++;
    }
    while (last > first && (source[last - 1] == ' ' || source[last - 1] == 0))
    {
        last--;
    }
    uint32_t count = 0;
    for (uint32_t i = first; i < last; i++)
    {
        out[count++] =
            (source[i] >= 0x20 && source[i] <= 0x7E) ? static_cast<char>(source[i]) : '?';
    }
    out[count] = '\0';
}

bool ValidBlockSize(uint32_t block_size)
{
    return block_size == 512 || block_size == 1024 || block_size == 2048 || block_size == 4096;
}

bool MakeCapacity(uint64_t last_lba, uint32_t block_size, Capacity* capacity)
{
    if (!ValidBlockSize(block_size) || last_lba == UINT64_MAX)
    {
        return false;
    }
    const uint64_t blocks = last_lba + 1;
    if (blocks > UINT64_MAX / block_size)
    {
        return false;
    }
    capacity->blocks = blocks;
    capacity->block_size = block_size;
    return true;
}

} // namespace

Cdb Inquiry(uint8_t allocation)
{
    Cdb cdb = NewCdb(Opcode::Inquiry, 6);
    cdb.bytes[4] = allocation;
    return cdb;
}

Cdb TestUnitReady()
{
    return NewCdb(Opcode::TestUnitReady, 6);
}

Cdb RequestSense(uint8_t allocation)
{
    Cdb cdb = NewCdb(Opcode::RequestSense, 6);
    cdb.bytes[4] = allocation;
    return cdb;
}

Cdb ReadCapacity10()
{
    return NewCdb(Opcode::ReadCapacity10, 10);
}

Cdb ReadCapacity16(uint32_t allocation)
{
    Cdb cdb = NewCdb(Opcode::ServiceActionIn16, 16);
    cdb.bytes[1] = ReadCapacity16Action;
    PutBigEndian(cdb, 10, allocation, 4);
    return cdb;
}

Cdb ModeSense6(uint8_t page, uint8_t allocation)
{
    Cdb cdb = NewCdb(Opcode::ModeSense6, 6);
    cdb.bytes[2] = page;
    cdb.bytes[4] = allocation;
    return cdb;
}

Cdb Read(uint64_t lba, uint32_t blocks)
{
    return Transfer(Opcode::Read10, Opcode::Read16, lba, blocks);
}

Cdb Write(uint64_t lba, uint32_t blocks)
{
    return Transfer(Opcode::Write10, Opcode::Write16, lba, blocks);
}

Cdb SynchronizeCache()
{
    return NewCdb(Opcode::SynchronizeCache10, 10);
}

Cdb StartStopUnit(bool start, bool load_eject)
{
    Cdb cdb = NewCdb(Opcode::StartStopUnit, 6);
    cdb.bytes[4] = static_cast<uint8_t>((start ? 1U : 0U) | (load_eject ? 2U : 0U));
    return cdb;
}

bool IsReadOrWrite(const Cdb& cdb)
{
    switch (cdb.bytes[0])
    {
    case Opcode::Read6:
    case Opcode::Write6:
    case Opcode::Read10:
    case Opcode::Write10:
    case Opcode::Read16:
    case Opcode::Write16:
        return true;
    default:
        return false;
    }
}

bool IsSynchronizeCache(const Cdb& cdb)
{
    return cdb.bytes[0] == Opcode::SynchronizeCache10;
}

bool ParseInquiry(std::span<const uint8_t> bytes, InquiryData* data)
{
    if (bytes.size() < InquiryBytes)
    {
        return false;
    }
    data->qualifier = static_cast<uint8_t>(bytes[0] >> 5);
    data->type = bytes[0] & 0x1FU;
    data->removable = (bytes[1] & 0x80U) != 0;
    CopyField(&bytes[8], 8, data->vendor);
    CopyField(&bytes[16], 16, data->product);
    CopyField(&bytes[32], 4, data->revision);
    return true;
}

bool IsDisk(const InquiryData& data)
{
    return data.qualifier == 0 && (data.type == DirectAccessType || data.type == RbcType);
}

CapacityResult ParseCapacity10(std::span<const uint8_t> bytes, Capacity* capacity)
{
    if (bytes.size() < ReadCapacity10Bytes)
    {
        return CapacityResult::Invalid;
    }
    const uint64_t last_lba = GetBigEndian(bytes, 0, 4);
    if (last_lba == 0xFFFFFFFFULL)
    {
        return CapacityResult::NeedSixteen;
    }
    const auto block_size = static_cast<uint32_t>(GetBigEndian(bytes, 4, 4));
    return MakeCapacity(last_lba, block_size, capacity) ? CapacityResult::Ok
                                                        : CapacityResult::Invalid;
}

bool ParseCapacity16(std::span<const uint8_t> bytes, Capacity* capacity)
{
    if (bytes.size() < 12)
    {
        return false;
    }
    const uint64_t last_lba = GetBigEndian(bytes, 0, 8);
    const auto block_size = static_cast<uint32_t>(GetBigEndian(bytes, 8, 4));
    return MakeCapacity(last_lba, block_size, capacity);
}

bool ParseWriteProtect(std::span<const uint8_t> bytes, bool* write_protected)
{
    if (bytes.size() < 4)
    {
        return false;
    }
    *write_protected = (bytes[2] & 0x80U) != 0;
    return true;
}

bool ParseSense(std::span<const uint8_t> bytes, Sense* sense)
{
    if (bytes.empty())
    {
        return false;
    }
    const uint8_t response = bytes[0] & 0x7FU;
    if (response == 0x70 || response == 0x71)
    {
        if (bytes.size() < 14)
        {
            return false;
        }
        sense->key = bytes[2] & 0x0FU;
        sense->asc = bytes[12];
        sense->ascq = bytes[13];
        return true;
    }
    if (response == 0x72 || response == 0x73)
    {
        if (bytes.size() < 8)
        {
            return false;
        }
        sense->key = bytes[1] & 0x0FU;
        sense->asc = bytes[2];
        sense->ascq = bytes[3];
        return true;
    }
    return false;
}

Action Classify(const Sense& sense, bool synchronize_cache)
{
    switch (sense.key)
    {
    case 0:
    case 1:
        return Action::Success;
    case SenseNotReady:
        if (sense.asc == 0x04 && sense.ascq == 0x01)
        {
            return Action::WaitAndRetry;
        }
        return sense.asc == 0x3A ? Action::NoMedium : Action::Fail;
    case SenseIllegalRequest:
        return synchronize_cache ? Action::Unsupported : Action::Fail;
    case SenseUnitAttention:
        if (sense.asc == 0x29)
        {
            return Action::Retry;
        }
        return sense.asc == 0x28 ? Action::MediumChanged : Action::Fail;
    case SenseDataProtect:
        return Action::WriteProtected;
    case SenseAbortedCommand:
        return Action::Retry;
    default:
        return Action::Fail;
    }
}

} // namespace FeatherOS::Usb::Storage::Scsi
