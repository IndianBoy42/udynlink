# C++ API Bridge for udynlink

## Goal

Design an optional layer that allows a C++ host firmware to expose modern, idiomatic C++ interfaces (classes, virtual dispatch) that dynamically-loaded C++ modules can consume — while respecting the r9/LOT PIC model, no-RTTI/no-exceptions constraints, and the tight SRAM budgets of Cortex-M targets.

**Scope**: Host-only interfaces (modules consume what the host provides). Fail-on-import semantics (missing factory = load failure, same as existing C externs). C modules can also use these interfaces via manual vtable dispatch.

## Key Technical Insight: The r9 Asymmetry

| Direction | r9 Safety | Why |
|-----------|-----------|-----|
| **Module → Host** | **Safe** | Host code compiled with standard ARM EABI. `r9` is callee-saved (AAPCS), so the host function preserves the module's `r9` automatically. |
| **Host → Module** | **Dangerous** | Module virtual functions assume `r9` = module's LOT. Host won't set `r9` via `UDYNLINK_PREPARE_CALL`. **Crash.** |

**Host-provided C++ objects are naturally safe** for module consumption — no thunks, no r9 management. The host compiler's callee-saved r9 convention does all the work.

## Architecture: "Embedded COM" (Pure Virtual Interfaces + C Factories)

### Rules

1. **Interfaces are pure virtual structs** — no data members, no constructors, no virtual destructors
2. **C-style factory functions** — modules resolve `extern "C"` factories, not C++ constructors
3. **`release()` instead of `virtual ~Destructor()`** — avoids the Itanium ABI's two-destructor problem
4. **Host-only OOP** — modules consume host interfaces. For host→module callbacks, use C function pointers with `void* ctx`
5. **Simple ownership** — single owner, `release()` when done. No reference counting.
6. **C-compatible** — interface structs have a well-defined C layout (vptr as first member) for use from C modules

### Complete Example

#### Shared Header (`api.h`) — Usable from both C and C++

```cpp
#ifdef __cplusplus

// C++: natural syntax with virtual dispatch
class ICounter {
public:
    virtual void increment() = 0;
    virtual int get() = 0;
    virtual void release() = 0;
};

extern "C" ICounter* create_counter(int initial_value);

#else

// C: manual vtable dispatch
typedef struct ICounter ICounter;

typedef struct ICounter_vtable {
    void (*increment)(ICounter* self);
    int  (*get)(ICounter* self);
    void (*release)(ICounter* self);
} ICounter_vtable;

struct ICounter {
    const ICounter_vtable* vptr;
};

static inline void ICounter_increment(ICounter* self) { self->vptr->increment(self); }
static inline int  ICounter_get(ICounter* self)      { return self->vptr->get(self); }
static inline void ICounter_release(ICounter* self)  { self->vptr->release(self); }

ICounter* create_counter(int initial_value);

#endif
```

#### Host Implementation

```cpp
// Host firmware — standard ARM EABI, no -msingle-pic-base
class HostCounter : public ICounter {
    int value_;
public:
    HostCounter(int v) : value_(v) {}
    void increment() override { value_++; }
    int get() override { return value_; }
    void release() override { /* operator delete or pool free */ }
};

// Factory — resolved at module load time via udynlink_external_resolve_symbol
extern "C" ICounter* create_counter(int initial_value) {
    return new HostCounter(initial_value);
}

// In udynlink_external_resolve_symbol:
//   if (!strcmp(name, "create_counter")) return (uintptr_t)&create_counter;
```

#### Module Usage (C++)

```cpp
#include "api.h"

extern "C" int module_entry() {
    ICounter* c = create_counter(42);  // factory resolved at load time
    c->increment();                     // virtual call → host code → r9 preserved automatically
    int val = c->get();
    c->release();
    return val;  // 43
}
```

#### Module Usage (C)

```c
#include "api.h"

int module_entry(void) {
    ICounter* c = create_counter(42);
    ICounter_increment(c);
    int val = ICounter_get(c);
    ICounter_release(c);
    return val;
}
```

### Why This Works — r9 Preservation Proof

```
Module:  serial->write("hi", 2);

Compiler generates (Thumb-2, -msingle-pic-base):
  ldr  r0, [serial]          ; r0 = vptr (points to host vtable in flash)
  ldr  r3, [r0, #offset]     ; r3 = function pointer from vtable entry
  blx  r3                    ; call host function

Host function (standard ARM EABI, no -msingle-pic-base):
  push {r4, r5, ..., r9, lr}  ; host compiler saves r9 — callee-saved per AAPCS!
  ...                         ; host code runs normally
  pop  {r4, r5, ..., r9, pc}  ; r9 restored — module's LOT pointer intact!
```

**No thunks. No UDYNLINK_PREPARE_CALL. No r9 management. The AAPCS callee-saved convention makes it work.**

### Callbacks (Host → Module)

For events from host to module, the interface provides a C callback registration:

```cpp
class ISerial {
public:
    virtual void write(const char* data, size_t len) = 0;
    virtual void set_callback(void (*cb)(void* ctx, int event), void* ctx) = 0;
    virtual void release() = 0;
};
```

The **host** is responsible for using `UDYNLINK_CALL` or the `udynlink_deps` thunk mechanism when invoking the callback, because the callback points to module code that requires `r9 = module's ram_base`.

## Deliverables

### D1: Header `udynlink/udynlink_cpp_api.h`
- `UDEFINE_INTERFACE_BEGIN(name)` / `UDEFINE_INTERFACE_END` — macros for dual C/C++ interface definition
- `UDEFINE_FACTORY(iface_name, func_name, params)` — macro for factory function declaration
- `URELEASE(self)` — inline helper for C modules
- Convention-documented inline helper macros for C vtable dispatch
- Works with `-fno-rtti -fno-exceptions`

### D2: Documentation `docs/cpp-api.md`
- Architecture rationale (r9 asymmetry, COM-style)
- Defining interfaces (C++ and C side-by-side)
- Host-side implementation guide
- Module-side usage guide (C++ and C)
- Callback pattern (host→module) with r9 considerations
- Limitations and constraints

### D3: Test Case `tests/test-cpp-virtual-dispatch/`
- Host provides `ICounter` interface via `create_counter()` factory
- C++ module uses `ICounter` with natural `->method()` syntax
- C module uses `ICounter` via `ICounter_*()` inline helpers
- Tests all 3 load modes (COPY_ALL, COPY_TEXT_DATA, XIP)
- Tests on at least MPS2-AN386 and STM32F429

### D4: Update docs index
- Add `docs/cpp-api.md` to `docs/README.md` table
- Update `AGENTS.md` if needed

## Task Breakdown

### T1: Create `udynlink/udynlink_cpp_api.h` (agent)
- Design the macro system for dual C/C++ interface definitions
- Must produce ABI-compatible layouts (Itanium C++ ABI vtable layout)
- Test with `-fno-rtti -fno-exceptions` compilation
- Context: see `include/udynlink/` for existing header conventions
- Files: new `include/udynlink/udynlink_cpp_api.h`, possibly update CMake install targets

### T2: Create test case `test-cpp-virtual-dispatch` (agent)
- Host-side: implement `ICounter` in `test_qemu.c`, add factory to `test_resolve_symbol()`
- Module-side C++: `mod_vtable_cpp.cpp` that uses `ICounter` with virtual dispatch
- Module-side C: `mod_vtable_c.c` that uses `ICounter` via manual vtable dispatch
- `test_data.py` with module definitions and required output patterns
- Context: see `tests/test-call-ergonomics-cpp/` and `tests/test-helloworld-cpp/` for C++ test examples
- Context: see `tests/qemu_host/src/main.c` for `test_resolve_symbol()` and `udynlink_external_resolve_symbol()`
- **Key verification**: the ASM output must show virtual calls going through the vtable without r9 corruption

### T3: Write `docs/cpp-api.md` (agent)
- Full guide covering all patterns and constraints
- Update `docs/README.md` to add the new guide

### T4: Run tests on QEMU (quick/shell)
- `just test-mps2` and `just test-f429` with the new test case
- Verify all load modes pass
- Verify both C and C++ module variants

## Constraints & Risks

1. **No virtual destructors** — `release()` only. Itanium ABI generates two destructor variants in the vtable.
2. **No RTTI** — `dynamic_cast`/`typeid` don't work. Interface negotiation must be explicit.
3. **No exceptions** — Factory functions return `nullptr` on failure.
4. **No STL types across boundary** — No `std::string`, `std::vector`, etc. Raw pointers + sizes only.
5. **Vtable layout** — Relies on Itanium C++ ABI (GCC/Clang on ARM). MSVC would break but is irrelevant for Cortex-M.
6. **Interface inheritance** — NOT supported in MVP. Flat interfaces only. Inheritance complicates vtable layout and the dual C/C++ header.
7. **Module-to-module C++** — Out of scope. Modules communicate via C API.
8. **Factory symbol names** — Must be C-linkage (`extern "C"`). C++ mangled names are not resolvable by `udynlink_external_resolve_symbol`.
