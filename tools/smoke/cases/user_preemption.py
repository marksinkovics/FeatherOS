from smoke.cases.base import KernelTest

USER_PREEMPTION_OUTPUTS = {"x86_64": b"UIu", "aarch64": b"AIa"}


class UserPreemption(KernelTest):
    expected = "KERN/SCHEDULER: user-mode preemption passed"
    failure = "user-mode timer preemption did not pass"
    passed = "user programs interleaved under timer preemption"

    def Check(self, result):
        failure = super().Check(result)
        if failure is not None:
            return failure
        if USER_PREEMPTION_OUTPUTS[self.args.arch] not in result.captured:
            return "user-program preemption did not interleave"
        return None
