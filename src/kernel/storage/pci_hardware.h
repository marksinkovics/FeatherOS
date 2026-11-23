#ifndef FEATHEROS_STORAGE_PCI_HARDWARE_H
#define FEATHEROS_STORAGE_PCI_HARDWARE_H

#include "kernel/pci/msi.h"
#include "kernel/pci/pci.h"
#include "kernel/storage/controller_hardware.h"

#include <stdint.h>

namespace FeatherOS::Storage
{

class PciHardware final : public ControllerHardware
{
public:
    // Maps BAR register_bar and enables memory space and bus mastering with INTx disabled; null
    // when the BAR is not a memory BAR or nothing is left to allocate. The object is never freed.
    static PciHardware* Create(Pci::ConfigSpace& config, const Pci::Function& function,
                               uint32_t register_bar);

    uint16_t SubsystemVendor();
    uint16_t Subsystem();

    uint32_t Read32(uint32_t offset) override;
    void Write32(uint32_t offset, uint32_t value) override;
    uint64_t Read64(uint32_t offset) override;
    void Write64(uint32_t offset, uint64_t value) override;
    uint32_t ReadConfig32(uint16_t offset) override;
    bool AllocateDma(uint64_t bytes, uint64_t below, Memory::Dma::Buffer* buffer) override;
    void FreeDma(const Memory::Dma::Buffer& buffer) override;
    // MSI-X entry 0, then MSI, else polling.
    InterruptKind SetUpInterrupts(void (*handler)(void* context), void* context) override;

private:
    PciHardware(Pci::ConfigSpace& config, const Pci::Function& function);

    bool Map(uint32_t register_bar);
    bool EnableMsiX(const Pci::MsiMessage& message);

    Pci::ConfigSpace& config_;
    Pci::Function function_;
    uintptr_t base_ = 0;
};

} // namespace FeatherOS::Storage

#endif
