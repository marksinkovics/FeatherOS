#ifndef FEATHEROS_STORAGE_PROBE_WAIT_H
#define FEATHEROS_STORAGE_PROBE_WAIT_H

#include "kernel/storage/environment.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Storage::ProbeWait
{

constexpr uint32_t MaxPending = 16;
constexpr size_t NameCapacity = 16;

// A controller's bring-up job is about to be posted; key identifies the job. False when the
// table is full.
bool Begin(const void* key, const char* name);
// The job ended, whatever its result, or could not be posted. An unknown key is ignored.
void End(Environment& environment, const void* key);
uint32_t Pending();
// Blocks until no bring-up is pending or the deadline passes. True when none is pending; else
// the names of the late ones, comma-separated, go to late.
bool Wait(Environment& environment, uint64_t deadline_ms, char* late, size_t capacity);
// A wait ends by a tick only; without a running timer the deadline would never pass.
bool MayWait(bool timer_running);
// Host tests only.
void Reset();

} // namespace FeatherOS::Storage::ProbeWait

#endif
