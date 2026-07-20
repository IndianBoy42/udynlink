// Module exercising inline, static inline, virtual dispatch, and weak
// template instantiations under --strip-mangled-syms.
//
// Each pattern is annotated with the ELF signature it produces so the
// test can reason about what the strip pipeline should do.

#include <stdio.h>

// ---------------------------------------------------------------------------
// 1. Plain `inline` (non-static).  Emits STB_WEAK mangled name when its
//    address is taken.  The volatile function pointer dance below forces
//    emission under -Os/-O3.
// ---------------------------------------------------------------------------
inline __attribute__((noinline, used))
int inline_add(int a, int b) {
    return a + b;
}

// ---------------------------------------------------------------------------
// 2. `static inline`.  Should be STB_LOCAL, not subject to the demote
//    block (it never reaches "exported"/"weak" classification).
// ---------------------------------------------------------------------------
static inline __attribute__((always_inline))
int static_inline_mul(int a, int b) {
    return a * b;
}

// ---------------------------------------------------------------------------
// 3. Function template explicit instantiation.  Emits STB_WEAK mangled.
// ---------------------------------------------------------------------------
template <typename T>
__attribute__((noinline, used))
int templated_dbl(T v) {
    return (int)v * 2;
}
template int templated_dbl<int>(int);

// ---------------------------------------------------------------------------
// 4. C++ class with a virtual method.  Exercises the vtable path (which
//    is discarded as a local section — see the comprehensive test for
//    the full explanation).  The direct member function is a separate
//    STB_GLOBAL mangled symbol.
// ---------------------------------------------------------------------------
struct Widget {
    int value;
    Widget() : value(42) {}
    int get() const { return value; }
    int doubled() const { return value * 2; }
};

static Widget the_widget __attribute__((used));

// ---------------------------------------------------------------------------
// 5. The entry point.  All the above symbols are called from here so
//    --gc-sections keeps them alive.
// ---------------------------------------------------------------------------
extern "C" int test(void) {
    int s = 0;

    // inline_add — STB_WEAK mangled.
    s += inline_add(2, 3);
    if (s != 5) return 0;

    // static_inline_mul — STB_LOCAL, not subject to demote.
    s += static_inline_mul(4, 5);
    if (s != 25) return 0;

    // templated_dbl<int> — STB_WEAK mangled.
    s += templated_dbl<int>(10);
    if (s != 45) return 0;

    // Widget methods — STB_WEAK mangled.
    s += the_widget.get();
    if (s != 87) return 0;
    s += the_widget.doubled();
    if (s != 171) return 0;

    return 1;
}
