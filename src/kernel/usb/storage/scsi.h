#ifndef FEATHEROS_USB_STORAGE_SCSI_H
#define FEATHEROS_USB_STORAGE_SCSI_H

#include <span>
#include <stdint.h>

namespace FeatherOS::Usb::Storage::Scsi
{

constexpr uint32_t InquiryBytes = 36;
constexpr uint32_t RequestSenseBytes = 18;
constexpr uint32_t ReadCapacity10Bytes = 8;
constexpr uint32_t ReadCapacity16Bytes = 32;
constexpr uint32_t ModeSenseBytes = 192;
constexpr uint32_t MaxTransferBytes = 64 * 1024;

namespace Opcode
{
constexpr uint8_t TestUnitReady = 0x00;
constexpr uint8_t RequestSense = 0x03;
constexpr uint8_t Read6 = 0x08;
constexpr uint8_t Write6 = 0x0A;
constexpr uint8_t Inquiry = 0x12;
constexpr uint8_t ModeSense6 = 0x1A;
constexpr uint8_t StartStopUnit = 0x1B;
constexpr uint8_t ReadCapacity10 = 0x25;
constexpr uint8_t Read10 = 0x28;
constexpr uint8_t Write10 = 0x2A;
constexpr uint8_t SynchronizeCache10 = 0x35;
constexpr uint8_t Read16 = 0x88;
constexpr uint8_t Write16 = 0x8A;
constexpr uint8_t ServiceActionIn16 = 0x9E;
} // namespace Opcode

constexpr uint8_t ReadCapacity16Action = 0x10;

enum class Direction : uint8_t
{
    None,
    In,
    Out,
};

struct Cdb
{
    uint8_t bytes[16];
    uint8_t length;
};

Cdb Inquiry(uint8_t allocation);
Cdb TestUnitReady();
Cdb RequestSense(uint8_t allocation);
Cdb ReadCapacity10();
Cdb ReadCapacity16(uint32_t allocation);
Cdb ModeSense6(uint8_t page, uint8_t allocation);
Cdb Read(uint64_t lba, uint32_t blocks);
Cdb Write(uint64_t lba, uint32_t blocks);
Cdb SynchronizeCache();
Cdb StartStopUnit(bool start, bool load_eject);

bool IsReadOrWrite(const Cdb& cdb);
bool IsSynchronizeCache(const Cdb& cdb);

constexpr uint8_t DirectAccessType = 0x00;
constexpr uint8_t RbcType = 0x0E;

struct InquiryData
{
    uint8_t qualifier;
    uint8_t type;
    bool removable;
    char vendor[9];
    char product[17];
    char revision[5];
};

bool ParseInquiry(std::span<const uint8_t> bytes, InquiryData* data);
bool IsDisk(const InquiryData& data);

struct Capacity
{
    uint64_t blocks;
    uint32_t block_size;
};

enum class CapacityResult : uint8_t
{
    Ok,
    NeedSixteen,
    Invalid,
};

CapacityResult ParseCapacity10(std::span<const uint8_t> bytes, Capacity* capacity);
bool ParseCapacity16(std::span<const uint8_t> bytes, Capacity* capacity);

bool ParseWriteProtect(std::span<const uint8_t> bytes, bool* write_protected);

struct Sense
{
    uint8_t key;
    uint8_t asc;
    uint8_t ascq;
};

bool ParseSense(std::span<const uint8_t> bytes, Sense* sense);

enum class Action : uint8_t
{
    Success,
    Retry,
    WaitAndRetry,
    NoMedium,
    MediumChanged,
    WriteProtected,
    Unsupported,
    Fail,
};

Action Classify(const Sense& sense, bool synchronize_cache);

} // namespace FeatherOS::Usb::Storage::Scsi

#endif
