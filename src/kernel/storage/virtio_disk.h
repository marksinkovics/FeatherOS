#ifndef FEATHEROS_STORAGE_VIRTIO_DISK_H
#define FEATHEROS_STORAGE_VIRTIO_DISK_H

namespace FeatherOS::BlockDevice
{

// Registers the virtio-blk device found by Initialize as vda; false when the registry refuses it.
bool RegisterVirtioDisk();

} // namespace FeatherOS::BlockDevice

#endif
