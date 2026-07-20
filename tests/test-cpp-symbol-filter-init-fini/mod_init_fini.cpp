// Module exercising C++ global-object constructors AND a C
// __attribute__((constructor)) function under --strip-mangled-syms.
//
// The two C++ ctors print in the order they appear in the .init_array,
// interleaved with the C ctor.  If the init/fini pipeline is broken
// (e.g. because the demote block accidentally drops __init_array),
// the order will be wrong or the prints will be missing.

#include <stdio.h>

struct Tagged {
    int n;
    const char *tag;
    __attribute__((used))
    Tagged(int v, const char *t) : n(v), tag(t) {
        printf("ctor %s %d\n", tag, n);
    }
};

// Construction order is the order of definition; the init_array is
// populated in that order by the compiler.
static Tagged t_a(1, "a");
static Tagged t_b(2, "b");

extern "C" void c_ctor_first(void) __attribute__((constructor));
extern "C" void c_ctor_first(void) { printf("c_ctor first\n"); }

extern "C" void c_ctor_last(void) __attribute__((constructor));
extern "C" void c_ctor_last(void) { printf("c_ctor last\n"); }

static Tagged t_c(3, "c");

extern "C" int test(void) {
    if (t_a.n != 1 || t_b.n != 2 || t_c.n != 3) return 0;
    return 1;
}
