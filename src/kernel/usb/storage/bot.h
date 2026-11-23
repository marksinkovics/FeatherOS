#ifndef FEATHEROS_USB_STORAGE_BOT_H
#define FEATHEROS_USB_STORAGE_BOT_H

#include "kernel/usb/host_controller.h"
#include "scsi.h"

#include <span>
#include <stdint.h>

namespace FeatherOS::Usb::Storage::Bot
{

constexpr uint32_t CbwSize = 31;
constexpr uint32_t CswSize = 13;
constexpr uint32_t CbwSignature = 0x43425355;
constexpr uint32_t CswSignature = 0x53425355;

constexpr uint32_t CommandTimeoutMs = 5000;
constexpr uint32_t DataTimeoutMs = 20000;
constexpr uint32_t SlowCommandTimeoutMs = 60000;

void PackCbw(uint8_t* out, uint32_t tag, uint32_t length, Scsi::Direction direction,
             const Scsi::Cdb& cdb);

enum class CswCheck : uint8_t
{
    Passed,
    Failed,
    PhaseError,
    Invalid,
};

CswCheck CheckCsw(std::span<const uint8_t> bytes, uint32_t actual, uint32_t tag, uint32_t requested,
                  uint32_t* residue);

enum class Stage : uint8_t
{
    Command,
    DataIn,
    DataOut,
    Status,
};

// Buffer(Command) holds CbwSize bytes and Buffer(Status) CswSize; the data buffer is the
// same for DataIn and DataOut. Recover() is the whole of BOT reset recovery.
class Pipe
{
public:
    virtual std::span<uint8_t> Buffer(Stage stage) = 0;
    virtual bool Start(Stage stage, uint32_t length, Usb::Status* status) = 0;
    virtual Usb::Status Finish(uint64_t deadline_ms, uint32_t* actual) = 0;
    virtual Usb::Status Recover() = 0;
    virtual uint64_t NowMilliseconds() = 0;

protected:
    Pipe() = default;
    ~Pipe() = default;
    Pipe(const Pipe&) = default;
    Pipe& operator=(const Pipe&) = default;
};

struct Command
{
    Scsi::Cdb cdb;
    Scsi::Direction direction;
    uint32_t length;
    uint32_t timeout_ms;
};

enum class Outcome : uint8_t
{
    Passed,
    CheckCondition,
    TransportFailed,
    NoDevice,
};

struct Result
{
    Outcome outcome;
    uint32_t actual;
    uint32_t residue;
    bool recovered;
};

Result Execute(Pipe& pipe, uint32_t tag, const Command& command);

} // namespace FeatherOS::Usb::Storage::Bot

#endif
