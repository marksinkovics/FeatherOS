#ifndef FEATHEROS_STORAGE_WORKER_H
#define FEATHEROS_STORAGE_WORKER_H

namespace FeatherOS::Storage::Worker
{

using Job = void (*)(void* context);

// Starts the task; false when it could not be created.
bool Start();
// Queues a job for the task; false when the queue is full.
bool Post(Job job, void* context);
// Runs the oldest queued job on the calling task; false when there is none. The task's loop is
// this, and host tests call it by hand.
bool RunNext();

} // namespace FeatherOS::Storage::Worker

#endif
