#include "kernel/arch/floating_point.h"

namespace FeatherOS::Arch::FloatingPoint
{
namespace
{

// CPACR_EL1.FPEN = 0b11: FP and SIMD instructions do not trap at EL0 or EL1.
constexpr uint64_t CpacrFloatingPointEnable = 3ULL << 20;

} // namespace

void InitializeCurrentCpu()
{
    uint64_t cpacr;
    asm volatile("mrs %0, CPACR_EL1" : "=r"(cpacr));
    asm volatile("msr CPACR_EL1, %0\n\tisb" : : "r"(cpacr | CpacrFloatingPointEnable) : "memory");
}

// All zero: round to nearest, no exception traps, and cleared registers.
void InitializeState(State* state)
{
    for (uint8_t& byte : state->bytes)
    {
        byte = 0;
    }
}

// The kernel is built for armv8-a+nofp, so the assembler needs FP and SIMD enabled for these
// instructions only.
void Save(State* state)
{
    asm volatile(".arch_extension fp\n\t"
                 ".arch_extension simd\n\t"
                 "stp q0, q1, [%0, #0]\n\t"
                 "stp q2, q3, [%0, #32]\n\t"
                 "stp q4, q5, [%0, #64]\n\t"
                 "stp q6, q7, [%0, #96]\n\t"
                 "stp q8, q9, [%0, #128]\n\t"
                 "stp q10, q11, [%0, #160]\n\t"
                 "stp q12, q13, [%0, #192]\n\t"
                 "stp q14, q15, [%0, #224]\n\t"
                 "stp q16, q17, [%0, #256]\n\t"
                 "stp q18, q19, [%0, #288]\n\t"
                 "stp q20, q21, [%0, #320]\n\t"
                 "stp q22, q23, [%0, #352]\n\t"
                 "stp q24, q25, [%0, #384]\n\t"
                 "stp q26, q27, [%0, #416]\n\t"
                 "stp q28, q29, [%0, #448]\n\t"
                 "stp q30, q31, [%0, #480]\n\t"
                 "mrs x9, fpcr\n\t"
                 "mrs x10, fpsr\n\t"
                 "str x9, [%0, #512]\n\t"
                 "str x10, [%0, #520]"
                 :
                 : "r"(state->bytes)
                 : "x9", "x10", "memory");
}

void Restore(const State* state)
{
    asm volatile(".arch_extension fp\n\t"
                 ".arch_extension simd\n\t"
                 "ldp q0, q1, [%0, #0]\n\t"
                 "ldp q2, q3, [%0, #32]\n\t"
                 "ldp q4, q5, [%0, #64]\n\t"
                 "ldp q6, q7, [%0, #96]\n\t"
                 "ldp q8, q9, [%0, #128]\n\t"
                 "ldp q10, q11, [%0, #160]\n\t"
                 "ldp q12, q13, [%0, #192]\n\t"
                 "ldp q14, q15, [%0, #224]\n\t"
                 "ldp q16, q17, [%0, #256]\n\t"
                 "ldp q18, q19, [%0, #288]\n\t"
                 "ldp q20, q21, [%0, #320]\n\t"
                 "ldp q22, q23, [%0, #352]\n\t"
                 "ldp q24, q25, [%0, #384]\n\t"
                 "ldp q26, q27, [%0, #416]\n\t"
                 "ldp q28, q29, [%0, #448]\n\t"
                 "ldp q30, q31, [%0, #480]\n\t"
                 "ldr x9, [%0, #512]\n\t"
                 "ldr x10, [%0, #520]\n\t"
                 "msr fpcr, x9\n\t"
                 "msr fpsr, x10"
                 :
                 : "r"(state->bytes)
                 : "x9", "x10", "memory");
}

} // namespace FeatherOS::Arch::FloatingPoint
