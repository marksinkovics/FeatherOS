from smoke.cases.base import KernelTest


class Smp(KernelTest):
    expected = (
        "KERN/SMP: tasks ran concurrently on 3 CPUs; locked counter exact; "
        "busy CPUs counted; wake-ups across CPUs kept; "
        "current task stable across migration"
    )
    ends_on_panic = True
    failure = "multi-CPU scheduling test did not pass"
    passed = "tasks ran concurrently on several CPUs"
