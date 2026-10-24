// A signal handler returns here, and the SignalReturn syscall puts back what it interrupted.
asm(".text\n"
    ".global __featheros_signal_return\n"
    ".type __featheros_signal_return, %function\n"
    "__featheros_signal_return:\n\t"
    "mov x8, #34\n\t"
    "svc #0\n\t"
    "brk #0\n"
    ".size __featheros_signal_return, . - __featheros_signal_return\n");
