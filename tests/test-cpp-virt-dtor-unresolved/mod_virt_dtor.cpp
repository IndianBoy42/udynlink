// C++ module with a virtual destructor.  GCC emits a deleting destructor
// that references `_ZdlPvj` (operator delete(void*, unsigned int)).
// Speculatively also defines `__cxa_pure_virtual`-reaching slots so we can
// exercise both "should never run" stub families in one module.
#include <stdio.h>

struct ParameterEntry {
    int marker;
    ParameterEntry(int m) : marker(m) {}
    virtual ~ParameterEntry() = default;
    virtual int id() const = 0;
    virtual const char *name() const = 0;
};

struct IntParam : ParameterEntry {
    int v;
    IntParam(int x) : ParameterEntry(0), v(x) {}
    int id() const override { return v + 1; }
    const char *name() const override { return "int"; }
};

extern "C" int dispatch_id(const ParameterEntry *p) { return p->id(); }

extern "C" int test(void) {
    IntParam g{41};
    // The vtable for IntParam references _ZdlPvj in its deleting-destructor
    // slot.  The ParameterEntry base vtable references __cxa_pure_virtual
    // in its id()/name() slots.  Both are required at load time even though
    // the module never executes them.
    return dispatch_id(&g);
}
