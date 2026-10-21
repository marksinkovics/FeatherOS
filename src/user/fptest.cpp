#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{

constexpr char Help[] = "usage: fptest [workers 1-16]\n"
                        "Checks that each program keeps its floating-point and SIMD registers:\n"
                        "the workers fill every vector register with their own pattern, spin\n"
                        "while other workers run on the same CPUs, and check the pattern.";

#if defined(__x86_64__)
constexpr int RegisterCount = 16;
#elif defined(__aarch64__)
constexpr int RegisterCount = 32;
#endif
constexpr uint64_t SpinIterations = 30000000;

struct alignas(16) Registers
{
    uint64_t words[RegisterCount * 2];
};

// Loads every vector register from before, spins without touching them, and stores them to
// after, all in one asm block so the compiler cannot use the registers in between.
void HoldRegisters(const Registers& before, Registers* after)
{
    uint64_t counter = SpinIterations;
#if defined(__x86_64__)
    asm volatile("movdqa 0(%1), %%xmm0\n\tmovdqa 16(%1), %%xmm1\n\t"
                 "movdqa 32(%1), %%xmm2\n\tmovdqa 48(%1), %%xmm3\n\t"
                 "movdqa 64(%1), %%xmm4\n\tmovdqa 80(%1), %%xmm5\n\t"
                 "movdqa 96(%1), %%xmm6\n\tmovdqa 112(%1), %%xmm7\n\t"
                 "movdqa 128(%1), %%xmm8\n\tmovdqa 144(%1), %%xmm9\n\t"
                 "movdqa 160(%1), %%xmm10\n\tmovdqa 176(%1), %%xmm11\n\t"
                 "movdqa 192(%1), %%xmm12\n\tmovdqa 208(%1), %%xmm13\n\t"
                 "movdqa 224(%1), %%xmm14\n\tmovdqa 240(%1), %%xmm15\n"
                 "1:\n\tdec %0\n\tjnz 1b\n\t"
                 "movdqa %%xmm0, 0(%2)\n\tmovdqa %%xmm1, 16(%2)\n\t"
                 "movdqa %%xmm2, 32(%2)\n\tmovdqa %%xmm3, 48(%2)\n\t"
                 "movdqa %%xmm4, 64(%2)\n\tmovdqa %%xmm5, 80(%2)\n\t"
                 "movdqa %%xmm6, 96(%2)\n\tmovdqa %%xmm7, 112(%2)\n\t"
                 "movdqa %%xmm8, 128(%2)\n\tmovdqa %%xmm9, 144(%2)\n\t"
                 "movdqa %%xmm10, 160(%2)\n\tmovdqa %%xmm11, 176(%2)\n\t"
                 "movdqa %%xmm12, 192(%2)\n\tmovdqa %%xmm13, 208(%2)\n\t"
                 "movdqa %%xmm14, 224(%2)\n\tmovdqa %%xmm15, 240(%2)"
                 : "+r"(counter)
                 : "r"(before.words), "r"(after->words)
                 : "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
                   "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
#elif defined(__aarch64__)
    asm volatile("ldp q0, q1, [%1, #0]\n\tldp q2, q3, [%1, #32]\n\t"
                 "ldp q4, q5, [%1, #64]\n\tldp q6, q7, [%1, #96]\n\t"
                 "ldp q8, q9, [%1, #128]\n\tldp q10, q11, [%1, #160]\n\t"
                 "ldp q12, q13, [%1, #192]\n\tldp q14, q15, [%1, #224]\n\t"
                 "ldp q16, q17, [%1, #256]\n\tldp q18, q19, [%1, #288]\n\t"
                 "ldp q20, q21, [%1, #320]\n\tldp q22, q23, [%1, #352]\n\t"
                 "ldp q24, q25, [%1, #384]\n\tldp q26, q27, [%1, #416]\n\t"
                 "ldp q28, q29, [%1, #448]\n\tldp q30, q31, [%1, #480]\n"
                 "1:\n\tsubs %0, %0, #1\n\tb.ne 1b\n\t"
                 "stp q0, q1, [%2, #0]\n\tstp q2, q3, [%2, #32]\n\t"
                 "stp q4, q5, [%2, #64]\n\tstp q6, q7, [%2, #96]\n\t"
                 "stp q8, q9, [%2, #128]\n\tstp q10, q11, [%2, #160]\n\t"
                 "stp q12, q13, [%2, #192]\n\tstp q14, q15, [%2, #224]\n\t"
                 "stp q16, q17, [%2, #256]\n\tstp q18, q19, [%2, #288]\n\t"
                 "stp q20, q21, [%2, #320]\n\tstp q22, q23, [%2, #352]\n\t"
                 "stp q24, q25, [%2, #384]\n\tstp q26, q27, [%2, #416]\n\t"
                 "stp q28, q29, [%2, #448]\n\tstp q30, q31, [%2, #480]"
                 : "+r"(counter)
                 : "r"(before.words), "r"(after->words)
                 : "memory", "cc", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9",
                   "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20",
                   "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
#endif
}

int RunWorker(uint64_t worker)
{
    Registers before;
    Registers after = {};
    for (int index = 0; index < RegisterCount * 2; ++index)
    {
        before.words[index] = (worker << 56) | (static_cast<uint64_t>(index) << 32) | 0xFEA7;
    }
    HoldRegisters(before, &after);
    return memcmp(before.words, after.words, sizeof(before.words)) == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0))
    {
        puts(Help);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "worker") == 0)
    {
        return RunWorker(strtoul(argv[2], nullptr, 10));
    }
    const long workers = argc == 2 ? strtol(argv[1], nullptr, 10) : 8;
    if (argc > 2 || workers < 1 || workers > 16)
    {
        puts("usage: fptest [workers 1-16]");
        return 1;
    }

    pid_t children[16];
    for (long worker = 0; worker < workers; ++worker)
    {
        char number[4];
        snprintf(number, sizeof(number), "%ld", worker + 1);
        char* arguments[] = {argv[0], const_cast<char*>("worker"), number, nullptr};
        if (posix_spawn(&children[worker], "fptest", nullptr, nullptr, arguments, environ) != 0)
        {
            printf("fptest: could not start worker %ld\n", worker + 1);
            return 1;
        }
    }
    long kept = 0;
    for (long worker = 0; worker < workers; ++worker)
    {
        int status = 0;
        if (waitpid(children[worker], &status, 0) == children[worker] && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0)
        {
            ++kept;
        }
    }
    printf("fptest: %ld of %ld workers kept their registers\n", kept, workers);
    return kept == workers ? 0 : 1;
}
