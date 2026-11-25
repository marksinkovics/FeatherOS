#ifndef FEATHEROS_STORAGE_AHCI_PORT_H
#define FEATHEROS_STORAGE_AHCI_PORT_H

#include "kernel/memory/dma.h"
#include "kernel/power.h"
#include "kernel/storage/ahci/fis.h"
#include "kernel/storage/ahci/registers.h"
#include "kernel/storage/ata/identify.h"
#include "kernel/storage/controller_hardware.h"
#include "kernel/storage/disk.h"
#include "kernel/storage/environment.h"
#include "kernel/storage/write_policy.h"
#include "kernel/sync/mutex.h"
#include "user/thread_safety.h"

#include <atomic>
#include <stdint.h>

namespace FeatherOS::Storage::Ahci
{

constexpr uint32_t CommandPageBytes = 4096;
constexpr uint32_t CommandListOffset = 0;
constexpr uint32_t ReceivedFisOffset = 0x400;
constexpr uint32_t CommandTableOffset = 0x500;
constexpr uint32_t PrdtOffset = 0x80;
constexpr uint32_t MaxPrdEntries = 8;
constexpr uint32_t BounceBytes = 128 * 1024;
constexpr uint32_t IoTimeoutMs = 30000;
constexpr uint32_t FlushTimeoutMs = 60000;
constexpr uint32_t StandbyTimeoutMs = 30000;
constexpr uint32_t EngineStopMs = 500;
constexpr uint32_t ReadyWaitMs = 10000;
constexpr uint32_t LinkWaitMs = 1000;
constexpr uint32_t PresenceWaitMs = 100;
constexpr uint32_t ResetHoldMs = 10;
constexpr uint32_t RecoveryWindowMs = 60000;
constexpr uint32_t RecoveriesBeforeDead = 3;

struct PortConfig
{
    ControllerHardware* hardware;
    Environment* environment;
    uint16_t controller_index;
    uint32_t number;
    WritePolicy policy;
    uint64_t dma_below;
    bool staggered_spin_up;
    const char* location;
};

// One command in flight per port (I9): slot 0, one PRD over the bounce buffer, under io_lock_.
class Port final : public DiskDriver
{
public:
    Port() = default;
    Port(const Port&) = delete;
    Port& operator=(const Port&) = delete;
    ~Port();

    void Initialize(const PortConfig& config);

    // Stops the engine so the HBA reset finds the port idle (I6).
    bool Quiesce();
    // Command page, spin-up, link, ready and signature: true for an ATA disk candidate. Anything
    // else leaves the port retired.
    bool Probe();
    void EnableInterrupts();
    // Starts the engine and runs IDENTIFY: true when the disk is accepted.
    bool Start();
    // Registers the disk and scans it.
    void Publish();
    void Shutdown(Power::Action action);

    uint32_t Number() const;
    bool Candidate() const;
    bool Active() const;
    bool Dead() const;
    uint64_t write_commands() const;
    uint64_t flush_commands() const;
    uint64_t standby_commands() const;
    bool write_cache() const;

    DiskInfo Info() override;
    bool Gone() override;
    FeatherDisk::Status Read(uint64_t sector, uint32_t count, uint8_t* buffer) override;
    FeatherDisk::Status Write(uint64_t sector, uint32_t count, const uint8_t* buffer) override;
    FeatherDisk::Status Flush() override;
    FeatherDisk::Status Stop() override;
    void Release() override;

private:
    struct Command
    {
        H2dFis fis;
        const char* name;
        uint32_t bytes;
        bool write;
        uint32_t timeout_ms;
    };

    struct Completion
    {
        Port* port;
        uint32_t is;
        uint32_t ci;
    };

    uint32_t Read(uint32_t offset) const;
    void Write(uint32_t offset, uint32_t value) const;
    bool WaitFor(uint32_t offset, uint32_t mask, uint32_t value, uint32_t timeout_ms) const;

    bool ProbeLocked() REQUIRES(io_lock_);
    bool Prepare() REQUIRES(io_lock_);
    bool Link() REQUIRES(io_lock_);
    bool Comreset() REQUIRES(io_lock_);
    bool Detect() REQUIRES(io_lock_);
    bool StartLocked() REQUIRES(io_lock_);
    bool Accept(const Ata::AtaIdentity& identity) REQUIRES(io_lock_);
    void Retire() REQUIRES(io_lock_);
    void ReleaseBuffers() REQUIRES(io_lock_);

    bool StopEngine(bool receive_fis) REQUIRES(io_lock_);
    void StartEngine() REQUIRES(io_lock_);

    FeatherDisk::Status Transfer(bool write, uint64_t sector, uint32_t count, uint8_t* in,
                                 const uint8_t* out);
    FeatherDisk::Status Issue(const Command& command) REQUIRES(io_lock_);
    bool AwaitCompletion(uint64_t deadline_ms, Completion* completion) REQUIRES(io_lock_);
    static bool ReadyCommand(void* context);
    // The command failed: recover the port (AHCI 6.2.2.1) and still return Io, or make it dead.
    FeatherDisk::Status Fail(const char* name, const char* what) REQUIRES(io_lock_);
    // Null when the port is running again, else why it is dead.
    const char* Recover(uint32_t is, uint32_t tfd) REQUIRES(io_lock_);
    void MakeDead() REQUIRES(io_lock_);

    void Name(char (&out)[48]) const;

    ControllerHardware* hardware_ = nullptr;
    Environment* environment_ = nullptr;
    uint16_t controller_index_ = 0;
    uint32_t number_ = 0;
    WritePolicy policy_ = WritePolicy::Allowed;
    uint64_t dma_below_ = 0;
    bool staggered_ = false;
    char location_[MaxDiskLocation] = {};

    Sync::Mutex io_lock_;
    Memory::Dma::Buffer page_ GUARDED_BY(io_lock_) = {};
    Memory::Dma::Buffer bounce_ GUARDED_BY(io_lock_) = {};
    bool pinned_ GUARDED_BY(io_lock_) = false;
    bool spun_up_ GUARDED_BY(io_lock_) = false;
    uint32_t signature_ GUARDED_BY(io_lock_) = 0;
    // The times of the last two recoveries: a third one inside the window kills the port.
    uint64_t recovered_at_[RecoveriesBeforeDead - 1] GUARDED_BY(io_lock_) = {};
    uint32_t recoveries_ GUARDED_BY(io_lock_) = 0;

    DiskInfo info_ = {};
    Ata::AtaIdentity identity_ = {};
    std::atomic<Disk*> disk_ = nullptr;
    std::atomic<bool> candidate_ = false;
    std::atomic<bool> started_ = false;
    std::atomic<bool> dead_ = false;
    std::atomic<bool> shutting_down_ = false;
    std::atomic<uint64_t> write_commands_ = 0;
    std::atomic<uint64_t> flush_commands_ = 0;
    std::atomic<uint64_t> standby_commands_ = 0;
};

} // namespace FeatherOS::Storage::Ahci

#endif
