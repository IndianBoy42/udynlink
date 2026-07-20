// Comprehensive C++ module exercising every way GCC emits symbols.
// Used by both the QEMU integration test (test_qemu.c calls `test()`)
// and the pytest suite (test_cpp_symbol_emission_all.py).
//
// Each section is annotated with:
//   * the ELF pattern it produces (STB_*, STT_*, STV_*)
//   * what the --strip-* flags should do to it
//   * what the host relies on (so the loader must NOT break it)

#include <stdio.h>

// ---------------------------------------------------------------------------
// 1. Plain extern "C" functions.  Should NEVER be touched by any --strip-*
//    flag.  These are the only symbols the host can reliably call by name.
// ---------------------------------------------------------------------------
extern "C" int add(int a, int b)        { return a + b; }
extern "C" int sub(int a, int b)        { return a - b; }
extern "C" int mul(int a, int b)        { return a * b; }

// 2. C++ class with default visibility.  Member functions emit mangled
//    STB_WEAK symbols.  Vtable emits _ZTV* as STB_WEAK.  Typeinfo is
//    suppressed by -fno-rtti so we don't see _ZTI* here.  No virtual
//    destructor — that would force the compiler to emit a deleting
//    destructor that calls operator delete, which the host doesn't supply.
// ---------------------------------------------------------------------------
struct Base {
    virtual int vmethod(int x) { return x * 2; }
    int method(int x) { return x + 1; }
};
struct Derived : Base {
    int vmethod(int x) override __attribute__((used)) {
        return Base::vmethod(x) + 1;
    }
};

static Derived the_derived __attribute__((used));

// ---------------------------------------------------------------------------
// 3. Static inline helper (TU-local).  Emits STB_LOCAL mangled name.  The
//    demote block only looks at "exported"/"weak" classes, so this should
//    be a no-op.  We just verify the symbol is correctly handled.
// ---------------------------------------------------------------------------
static inline int static_inline_helper(int x) __attribute__((always_inline));
static inline int static_inline_helper(int x) { return x * 3; }

// ---------------------------------------------------------------------------
// 4. Function template (STB_WEAK mangled).  Explicit instantiation forces
//    emission of _Z* symbols in this TU.  With --strip-weak-sym-names or
//    --strip-mangled-syms these should be demoted.
// ---------------------------------------------------------------------------
template <typename T>
__attribute__((noinline, used))
int templated(T v) {
    int sum = 0;
    for (int i = 0; i < 4; ++i) sum += (int)v + i;
    return sum;
}

template int templated<int>(int);

// ---------------------------------------------------------------------------
// 5. Inline (non-static) function.  Emits STB_WEAK mangled name when its
//    address is taken (otherwise inlined at call sites).
// ---------------------------------------------------------------------------
inline __attribute__((noinline, used))
int inline_helper(int x) {
    return x + 100;
}

// ---------------------------------------------------------------------------
// 6. extern "C" function taking a function pointer to a C++ mangled
//    function.  Exercises the GOT reloc path: the C++ symbol is referenced
//    from a function whose body is in a wrapper, and the wrapper calls
//    it through the GOT.
// ---------------------------------------------------------------------------
typedef int (*mangled_fn_t)(int);

extern "C" int call_mangled_through_got(mangled_fn_t fp, int arg) {
    mangled_fn_t volatile vfp = fp;
    return vfp(arg);
}

extern "C" int call_via_templated(int x) {
    return templated<int>(x);
}

// ---------------------------------------------------------------------------
// 7. Global C++ objects with constructors.  Exercises __init_array entry
//    emission and the cpp_init_fini.c glue that mkmodule compiles in for
//    C++ modules.
// ---------------------------------------------------------------------------
struct Counter {
    int n;
    __attribute__((used))
    Counter() : n(0) { printf("ctor1\n"); n = 1; }
};

struct Counter2 {
    int n;
    __attribute__((used))
    Counter2() : n(0) { printf("ctor2\n"); n = 2; }
};

static Counter c1 __attribute__((used));
static Counter2 c2 __attribute__((used));

// ---------------------------------------------------------------------------
// 8. extern "C" function with __attribute__((constructor)).  Mixed C and
//    C++ init array: ensures both code paths survive under --strip-mangled-syms.
// ---------------------------------------------------------------------------
extern "C" void c_ctor_fn(void) __attribute__((constructor));
extern "C" void c_ctor_fn(void) {}

// ---------------------------------------------------------------------------
// 9. Weak function.  A weak `int weak_fn(int)` so --strip-weak-sym-names
//    can be exercised.  Marked used to keep the symbol.
// ---------------------------------------------------------------------------
extern "C" __attribute__((weak, used))
int weak_fn(int x) { return x - 5; }

// ---------------------------------------------------------------------------
// 10. Hidden helper (STV_HIDDEN mangled).  Exercises --strip-hidden-syms
//     and the interaction with the prologue wrapper.
// ---------------------------------------------------------------------------
template <typename T>
__attribute__((noinline, used, visibility("hidden")))
int hidden_templated(T v) {
    int sum = 0;
    for (int i = 0; i < 4; ++i) sum += (int)v + i + 100;
    return sum;
}

template int hidden_templated<int>(int);

// ---------------------------------------------------------------------------
// 12. Namespace-qualified function.  Produces _ZN* mangled name.
//     Under --strip-mangled-syms this is demoted to internal (nameless).
// ---------------------------------------------------------------------------
namespace util {
    int calc(int x) __attribute__((noinline, used));
    int calc(int x) { return x * 3 + 1; }
}
// ---------------------------------------------------------------------------
// 13. Class with destructor (non-virtual).  Produces _ZN*D*Ev destructor
//     as a WEAK symbol in the module symtab.
// ---------------------------------------------------------------------------
struct DtorTest {
    int val;
    __attribute__((used))
    ~DtorTest() {}
    __attribute__((used))
    DtorTest() : val(7) {}
};
static DtorTest g_dtor __attribute__((used));

// ---------------------------------------------------------------------------
// 14. Template variable (C++14 data symbol).  Produces _Z* data symbol
//     (STB_WEAK OBJECT) for --strip-mangled-syms / --strip-weak-sym-names.
// ---------------------------------------------------------------------------
template <typename T>
__attribute__((used)) T g_default = T(100);
template int g_default<int>;

// ---------------------------------------------------------------------------
// 15. Static member variable (data with C++ linkage) + member function.
//     Member variable is EXPORTED (STB_GLOBAL OBJECT), member function
//     is WEAK.  Exercises the data-section relocation path.
// ---------------------------------------------------------------------------
struct DataHolder {
    static int counter;
    __attribute__((noinline, used))
    static int get_next() { return ++counter; }
};
int DataHolder::counter = 0;

// ---------------------------------------------------------------------------
// 16. Multiple inheritance vtable.  MixinA and MixinB each have their
//     own vtable; MultiDerived overrides both.  Exercises the secondary
//     vtable pointer and this-adjustment in the thunk.
// ---------------------------------------------------------------------------
struct MixinA {
    virtual int mix_a(int x) { return x + 1; }
};
struct MixinB {
    virtual int mix_b(int x) { return x + 2; }
};
struct MultiDerived : MixinA, MixinB {
    int mix_a(int x) override { return MixinA::mix_a(x) * 10; }
    int mix_b(int x) override { return MixinB::mix_b(x) * 20; }
};
static MultiDerived g_multi __attribute__((used));


// ---------------------------------------------------------------------------
// 11. extern "C" entry: the host calls this, it exercises the lot/got
//     for the C++ helpers, the vtable, and verifies the demoted symbols
//     still resolve through the loader's reloc application.
// ---------------------------------------------------------------------------
extern "C" int test(void) {
    // Plain extern "C" — preserved by all flags.
    int a = add(1, 2);
    int b = sub(10, 3);
    int c = mul(4, 5);
    if (a != 3 || b != 7 || c != 20) return 0;

    // C++ class vtable call — exercises the GOT reloc to _ZTV.
    Base* bp = &the_derived;
    if (bp->vmethod(7) != 15) return 0;  // (7*2)+1

    // C++ template weak instantiation — demoted under --strip-mangled-syms.
    if (templated<int>(5) != 5+0+5+1+5+2+5+3) return 0;

    // C++ inline weak — demoted under --strip-mangled-syms/--strip-weak-sym-names.
    if (inline_helper(1) != 101) return 0;

    // extern "C" weak — demoted under --strip-weak-sym-names.
    if (weak_fn(10) != 5) return 0;

    // Hidden C++ template — demoted under --strip-hidden-syms.
    if (hidden_templated<int>(5) != 5+0+5+1+5+2+5+3 + 400) return 0;

    // GOT call to mangled function (wrapped via extern "C").
    if (call_mangled_through_got(call_via_templated, 3) != 3+0+3+1+3+2+3+3) return 0;

    // C++ ctors ran at load time.
    if (c1.n != 1 || c2.n != 2) return 0;

    // Namespace function — _ZN* mangled.
    if (util::calc(3) != 10) return 0;  // 3*3+1


    // Destructor symbol exists (the symbol table has _ZN*D*Ev).
    if (g_dtor.val != 7) return 0;


    // Template variable data symbol.
    if (g_default<int> != 100) return 0;

    // Static member variable + function.
    if (DataHolder::get_next() != 1) return 0;
    if (DataHolder::get_next() != 2) return 0;
    if (DataHolder::counter != 2) return 0;

    // Multiple inheritance vtable (secondary vtable + this-adjust).
    {
        MixinA* ma = &g_multi;
        MixinB* mb = &g_multi;
        if (ma->mix_a(5) != 60) return 0;  // (5+1)*10
        if (mb->mix_b(3) != 100) return 0;  // (3+2)*20
    }

    return 1;
}
