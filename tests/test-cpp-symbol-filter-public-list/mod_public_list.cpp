// Module that lets the host verify the --public-symbols allowlist
// works as a positive filter: only the listed names are addressable
// by the host; everything else is demoted to local.
//
// The host test (test_qemu.c) calls only the listed functions and
// asserts the non-listed ones are NOT findable via udynlink_lookup_symbol.

#include <stdio.h>

extern "C" int a(int x) { return x + 1; }
extern "C" int b(int x) { return x + 2; }
extern "C" int c(int x) { return x + 3; }
extern "C" int d(int x) { return x + 4; }

// Mangled helpers — should be demoted to local under
// --strip-mangled-syms and --strip-non-public-syms combined.
template <typename T>
__attribute__((noinline, used))
int helper(T v) { return (int)v * 7; }
template int helper<int>(int);

extern "C" int test(void) {
    // The host allowlists `a` and `c` only.  `b` and `d` should NOT be
    // findable by the host.  Inside the module they're still callable.
    int s = 0;
    s += a(1);
    s += b(1);
    s += c(1);
    s += d(1);
    s += helper<int>(1);
    if (s != (1+1) + (1+2) + (1+3) + (1+4) + 7) return 0;
    return 1;
}
