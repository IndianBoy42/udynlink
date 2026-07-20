#include <stdio.h>

// Non-static templated helper marked STV_HIDDEN. Without --strip-hidden-syms
// it survives as a named _Z*-mangled, STV_HIDDEN, STB_GLOBAL symbol in the
// module symbol table (pure table bloat). With the flag it is demoted to a
// nameless INTERNAL entry that the loader still relocates correctly.
//
// The noinline + volatile function pointer dance in run_filter_test()
// keeps the helper alive: --gc-sections would otherwise drop a templated
// function that is only called by value from the same TU, and -Os happily
// inlines short bodies. Both attributes plus the volatile pointer are
// required to ensure the symbol actually exists in the linked ELF.
template <typename T>
__attribute__((noinline, visibility("hidden")))
int internal_templated(T v) {
    int sum = 0;
    for (int i = 0; i < 4; ++i) sum += (int)v + i;
    return sum;
}

extern "C" int run_filter_test(int arg) {
    // Volatile function pointer: prevents the compiler from inlining the
    // call to internal_templated and keeps the symbol referenced for
    // --gc-sections.
    int (*volatile fp)(int) = internal_templated<int>;
    int r = fp(arg);
    printf("filtered %d\n", r);
    return 0;
}

extern "C" int test(void) {
    return run_filter_test(7) == 0;
}
