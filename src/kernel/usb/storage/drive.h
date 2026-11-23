#ifndef FEATHEROS_USB_STORAGE_DRIVE_H
#define FEATHEROS_USB_STORAGE_DRIVE_H

#include "kernel/storage/disk.h"
#include "kernel/storage/disk_driver.h"
#include "kernel/sync/mutex.h"
#include "kernel/sync/spinlock.h"
#include "kernel/usb/descriptors.h"
#include "kernel/usb/device.h"
#include "kernel/usb/host_controller.h"
#include "kernel/usb/storage/bot.h"
#include "kernel/usb/storage/environment.h"
#include "kernel/usb/storage/scsi.h"
#include "user/thread_safety.h"

#include <atomic>
#include <span>
#include <stdint.h>

namespace FeatherOS::Usb::Storage
{

constexpr uint32_t RecoveryTimeoutMs = 30000;
constexpr uint32_t SettleTimeoutMs = 2000;
constexpr uint32_t ProbeBudgetMs = 10000;
constexpr uint32_t WaitBudgetMs = 5000;
constexpr uint32_t WaitStepMs = 100;
constexpr uint32_t SenseRetries = 3;
constexpr uint32_t TransportRetries = 2;
constexpr uint32_t CommandPageBytes = 256;

// One bulk-only mass-storage interface. Two references keep it alive: the USB binding's, dropped
// by detach, and the disk's, dropped by Release.
class Drive final : public FeatherOS::Storage::DiskDriver
{
public:
    // The BOT layer's view of the drive for any task but the manager: every transfer goes
    // through Device::Submit, and a timeout is answered by the manager's recovery.
    class TaskPipe final : public Bot::Pipe
    {
    public:
        explicit TaskPipe(Drive& drive);

        std::span<uint8_t> Buffer(Bot::Stage stage) override;
        bool Start(Bot::Stage stage, uint32_t length, Usb::Status* status) override;
        Usb::Status Finish(uint64_t deadline_ms, uint32_t* actual) override;
        Usb::Status Recover() override;
        uint64_t NowMilliseconds() override;

    private:
        Drive& drive_;
    };

    // Called by the manager task; null when the buffers cannot be allocated.
    static Drive* Create(Device& device, const InterfaceView& interface, Environment& environment);

    // Reads the number of LUNs the way GET MAX LUN reports it; manager task only.
    uint8_t MaxLun();
    // The worker's job: false when the drive was refused or went away.
    bool Probe();
    // A counted reference to the registered disk, for the caller to Release; null before the
    // probe registers it and after the drive is gone.
    FeatherOS::Storage::Disk* AcquireDisk();
    // Manager task, from the driver's service hook; true asks for a port reset.
    bool Service(Device& device);
    // Manager task, from the driver's detach: drops every Device pointer and the binding's
    // reference.
    void Detached();
    void Retain();
    TaskPipe& Transport();

    FeatherOS::Storage::DiskInfo Info() override;
    bool Gone() override;
    FeatherDisk::Status Read(uint64_t sector, uint32_t count, uint8_t* buffer) override;
    FeatherDisk::Status Write(uint64_t sector, uint32_t count, const uint8_t* buffer) override;
    FeatherDisk::Status Flush() override;
    FeatherDisk::Status Stop() override;
    void Release() override;

private:
    enum class Disposition : uint8_t
    {
        Good,
        NoMedium,
        ReadOnly,
        Failed,
        NoDevice,
    };

    Drive(Device& device, const InterfaceView& interface, Environment& environment);
    ~Drive() = default;

    bool Allocate();
    bool Usable();
    void MarkLost();
    FeatherOS::Storage::Disk* TakeDisk();
    void Free();
    Bot::Result Run(const Bot::Command& command, Scsi::Sense* sense) REQUIRES(io_lock_);
    Disposition Perform(const Bot::Command& command, const uint8_t* out_data,
                        uint64_t wait_deadline_ms, bool probing, Bot::Result* result)
        REQUIRES(io_lock_);
    Disposition Receive(const Scsi::Cdb& cdb, uint32_t length, uint64_t wait_deadline_ms,
                        Bot::Result* result) REQUIRES(io_lock_);
    FeatherDisk::Status Move(uint64_t sector, uint32_t count, uint8_t* in, const uint8_t* out);
    bool ProbeLocked() REQUIRES(io_lock_);
    bool Publish(const FeatherOS::Storage::DiskInfo& info);
    Usb::Status RecoverEndpoints(Device& device);
    void LogRecovery(Bot::Stage stage, Usb::Status cause);
    void LogLine(const char* message);
    void Name(char* out, size_t capacity);
    static void OnTransferDone(DeviceTransfer& transfer);
    static bool ReadyDone(void* context);
    static bool ReadyIdle(void* context);
    static bool ReadyReply(void* context);

    Environment& environment_;
    HostController& host_;
    const uint32_t slot_;
    const uint8_t interface_;
    uint8_t in_endpoint_ = 0;
    uint8_t out_endpoint_ = 0;
    char location_[FeatherOS::Storage::MaxDiskLocation] = {};
    Memory::Dma::Buffer command_page_{};
    Memory::Dma::Buffer command_view_{};
    Memory::Dma::Buffer status_view_{};
    Memory::Dma::Buffer data_{};
    std::atomic<uint32_t> references_ = 1;
    TaskPipe pipe_;

    Sync::SpinLock state_lock_;
    Device* device_ GUARDED_BY(state_lock_) = nullptr;
    bool gone_ GUARDED_BY(state_lock_) = false;
    bool broken_ GUARDED_BY(state_lock_) = false;
    bool busy_ GUARDED_BY(state_lock_) = false;
    bool done_ GUARDED_BY(state_lock_) = false;
    Usb::Status status_ GUARDED_BY(state_lock_) = Usb::Status::Success;
    uint32_t actual_ GUARDED_BY(state_lock_) = 0;
    uint32_t request_serial_ GUARDED_BY(state_lock_) = 0;
    uint32_t reply_serial_ GUARDED_BY(state_lock_) = 0;
    Usb::Status reply_status_ GUARDED_BY(state_lock_) = Usb::Status::Success;
    Bot::Stage last_stage_ GUARDED_BY(state_lock_) = Bot::Stage::Command;
    Usb::Status last_status_ GUARDED_BY(state_lock_) = Usb::Status::Success;
    bool registered_ GUARDED_BY(state_lock_) = false;
    FeatherOS::Storage::Disk* disk_ GUARDED_BY(state_lock_) = nullptr;
    char name_[FeatherOS::Storage::MaxDiskName] GUARDED_BY(state_lock_) = {};
    bool ejected_ GUARDED_BY(state_lock_) = false;
    FeatherOS::Storage::DiskInfo info_ GUARDED_BY(state_lock_) = {};
    DeviceTransfer transfer_ GUARDED_BY(state_lock_);

    Sync::Mutex io_lock_;
    uint32_t tag_ GUARDED_BY(io_lock_) = 0;
    bool sync_unsupported_ GUARDED_BY(io_lock_) = false;
};

} // namespace FeatherOS::Usb::Storage

#endif
