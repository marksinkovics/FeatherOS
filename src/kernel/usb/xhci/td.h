#ifndef FEATHEROS_USB_XHCI_TD_H
#define FEATHEROS_USB_XHCI_TD_H

#include "kernel/usb/xhci/trb.h"

#include <span>
#include <stdint.h>

namespace FeatherOS::Usb::Xhci
{

// 0 when length exceeds MaxBulkTransferBytes, max_packet is 0 or out is too small.
uint32_t BuildNormalTd(uint64_t physical, uint32_t length, bool in, uint32_t max_packet,
                       std::span<Trb> out);
uint32_t TdBytesBefore(uint64_t physical, uint32_t length, uint32_t index);
uint32_t TdTrbLength(uint64_t physical, uint32_t length, uint32_t index);

} // namespace FeatherOS::Usb::Xhci

#endif
