// Module verifying that --strip-weak-sym-names demotes weak symbols
// to local (losing the host-override capability by design), but the
// module still loads and runs correctly.
//
// The test_qemu.c host does NOT attempt to override the weak symbol
// by name (that capability is intentionally lost under this flag —
// see docs/writing-modules.md "Controlling C++ Symbol-Table Size").
// It just verifies the module loads, the weak function returns its
// own default value, and the bin size drops vs. the same module
// without the flag.

#include <stdio.h>

extern "C" __attribute__((weak, used))
int weak_default(int x) { return x * 3; }

extern "C" __attribute__((used))
int normal_func(int x) { return x + 100; }

extern "C" int test(void) {
    // The weak function's default value must be applied (no host
    // override attempted in this test).
    if (weak_default(5) != 15) return 0;
    if (normal_func(5) != 105) return 0;
    return 1;
}
