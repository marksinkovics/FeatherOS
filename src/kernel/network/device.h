#ifndef FEATHEROS_NETWORK_DEVICE_H
#define FEATHEROS_NETWORK_DEVICE_H

#include "ethernet.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Network::Device
{

bool Initialize(const void* platform_data, uint64_t hhdm_offset);
// Resets the device, which then takes and sends no frames until Restart.
void Stop();
// Starts a stopped device again with empty queues.
bool Restart();
MacAddress Address();
// Drops the frame, returning false, when every transmit buffer is still with the device.
bool Transmit(const uint8_t* frame, size_t length);
// Blocks until a frame arrives, which is never while stopped. Only one task may receive.
size_t Receive(uint8_t (&frame)[MaximumFrameSize]);

} // namespace FeatherOS::Network::Device

#endif
