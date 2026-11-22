#include "registers.h"

namespace FeatherOS::Usb::Xhci
{

uint32_t PortscWriteValue(uint32_t current, uint32_t clear_bits)
{
    return (current & PortSc::PreservedMask) | (clear_bits & PortSc::ChangeMask);
}

} // namespace FeatherOS::Usb::Xhci
