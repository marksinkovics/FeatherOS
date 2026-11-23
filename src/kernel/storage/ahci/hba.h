#ifndef FEATHEROS_STORAGE_AHCI_HBA_H
#define FEATHEROS_STORAGE_AHCI_HBA_H

#include "kernel/power.h"
#include "kernel/storage/ahci/port.h"
#include "kernel/storage/ahci/registers.h"
#include "kernel/storage/controller_hardware.h"
#include "kernel/storage/disk.h"
#include "kernel/storage/environment.h"
#include "kernel/storage/write_policy.h"

#include <atomic>
#include <stdint.h>

namespace FeatherOS::Storage::Ahci
{

constexpr uint32_t HandoffWaitMs = 25;
constexpr uint32_t HandoffBusyWaitMs = 2000;
constexpr uint32_t HbaResetMs = 1000;

class Hba
{
public:
    static Hba* Create(ControllerHardware& hardware, Environment& environment, uint16_t index,
                       WritePolicy policy, const char* location);
    ~Hba();
    Hba(const Hba&) = delete;
    Hba& operator=(const Hba&) = delete;

    // The worker's job: false when the HBA was refused.
    bool Start();
    void Shutdown(Power::Action action);
    static void OnInterrupt(void* context);

    uint32_t DiskCount() const;
    // The port with a disk numbered number, or null.
    Port* DiskPort(uint32_t number);

private:
    Hba(ControllerHardware& hardware, Environment& environment, uint16_t index, WritePolicy policy,
        const char* location);

    void Handoff();
    void LogPolicy() const;
    void LogController() const;

    ControllerHardware& hardware_;
    Environment& environment_;
    const uint16_t index_;
    const WritePolicy policy_;
    char location_[MaxDiskLocation] = {};

    Capabilities caps_{};
    uint32_t implemented_ = 0;
    InterruptKind interrupts_ = InterruptKind::Polling;
    Port ports_[Reg::MaxPorts];
    std::atomic<bool> started_ = false;
};

} // namespace FeatherOS::Storage::Ahci

#endif
