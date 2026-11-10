from smoke.cases.base import KernelTest


class Scheduler(KernelTest):
    expected = (
        "KERN/SCHEDULER: preemption, wait, exit, reap, blocking, sleep, "
        "and priorities passed"
    )
    failure = "scheduler preemption test did not pass"
    passed = "timer preemption and task lifecycle passed"
