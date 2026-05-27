# Plan: Better Host Ergonomics for Calling Module Functions

## Goal
Provide convenient, type-safe, and performant patterns for host firmware to call into dynamically loaded udynlink modules. Target deliverables:

1. **Amortized symbol lookups** — avoid O(N) string search on every call
2. **C convenience macros** — eliminate boilerplate (LOT base write + lookup + cast + call)
3. **C++ typed wrappers** — header-only RAII/template wrappers with real type safety

## Cross-Plan Dependency

This plan **builds on** the host-sets-r9 work defined in `.opencode/plans/micro-module-optimizations.md` (Task 1B). That plan introduces:
- `--no-prologue` module build flag
- `UDYNLINK_ARCH_FLAG_NO_PROLOGUE` header bit
- `UDYNLINK_PREPARE_CALL(p_mod)` macro — universal macro that handles both prologued and non-prologued modules

**Assumption:** The `UDYNLINK_PREPARE_CALL()` macro from Task 1B is already implemented and available in `udynlink/udynlink.h`. Our new ergonomic layer sits **on top of** it.

## Current State (Baseline)

Today, calling a module function from host C code requires ~10 lines of boilerplate per call site:

```c
// 1. Set LOT base (MANDATORY, repeated before EVERY call)
*(uint32_t*)UDYNLINK_LOT_BASE_ADDR = mod.ram_base;

// 2. String lookup through symbol table
udynlink_sym_t sym;
if (udynlink_lookup_symbol(&mod, "hello", &sym) == NULL) { /* error */ }

// 3. Unsafe cast from uintptr_t to function pointer
int (*p_func)(int) = (int (*)(int))sym.val;

// 4. Call
int result = p_func(42);
```

**Problems identified:**
- **String lookup on every call**: `udynlink_lookup_symbol()` does a linear scan of the symbol table. For hot paths, this is wasteful.
- **Manual LOT base management**: The write to `0x20000000` (or `UDYNLINK_LOT_BASE_ADDR`) is easy to forget, especially when switching between multiple loaded modules.
- **Unsafe cast**: `sym.val` is `uintptr_t`. Casting to a function pointer is technically UB in ISO C/C++ (though de-facto standard). No compile-time signature checking.
- **No helper exists in the public API**: The only wrapper is `run_test_func()` in test-only code, hardcoded to symbol `"test"` and signature `int (*)(void)`.
- **C++ is even worse**: The host must manually call `udynlink_cpp_init()` after load, then still do all the C boilerplate.

## Proposed Architecture

### Layer 0: Core C API (unchanged)
`udynlink/udynlink.h` and `udynlink/udynlink.c` remain minimal and portable. No bloat added.

**Included from Task 1B:** `UDYNLINK_PREPARE_CALL(p_mod)` macro handles LOT base for both prologued and no-prologue modules.

### Phase 1: C Convenience Layer (`udynlink/udynlink_call.h`)

**Design principles:**
- Target **C11 or newer** (user confirmed C11 acceptable, `_Generic` allowed)
- Provide **separate macros** for different strategies — user can choose the right one for their context
- Pure inline / macro — no new `.c` files, no link-time dependencies
- Error-code based failure handling (no assert, no silent swallowing)

#### 1a. LOT Base Management Macros (building on Task 1B)

```c
/* Option A: Let the module prologue handle r9 (legacy / prologued modules).
 * Just writes to UDYNLINK_LOT_BASE_ADDR. */
#define UDYNLINK_SET_LOT_BASE(p_mod) \
    (*(uint32_t*)UDYNLINK_LOT_BASE_ADDR = (p_mod)->ram_base)

/* Option B: Host directly sets r9 (for --no-prologue modules or max performance).
 * Inline asm — Cortex-M only. */
#define UDYNLINK_SET_R9(p_mod) \
    __asm volatile ("mov r9, %0" :: "r"((p_mod)->ram_base) : "r9")

/* Option C: Universal macro from Task 1B — auto-detects module type at runtime.
 * Slightly more overhead (branch + potential asm) but works for ALL modules. */
#define UDYNLINK_PREPARE_CALL(p_mod)  /* already defined in udynlink.h */
```

#### 1b. Amortized Lookup Handle

```c
/* Opaque handle: resolve once, call many times. */
typedef struct {
    const udynlink_module_t *p_mod;
    uintptr_t addr;
    const char *name;   /* kept for debug / late validation */
} udynlink_func_t;

/* Resolve a symbol by name into a reusable handle.
 * Returns UDYNLINK_OK on success, or error code (e.g. UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL).
 * p_out is left zeroed on failure. */
udynlink_error_t udynlink_resolve_func(const udynlink_module_t *p_mod,
                                       const char *name,
                                       udynlink_func_t *p_out);
```

#### 1c. Call Macros (C11 `_Generic` for type safety)

```c
/* Two-phase macro for hot paths:
 *   udynlink_func_t handle;
 *   err = udynlink_resolve_func(&mod, "myfunc", &handle);
 *   if (err != UDYNLINK_OK) { ... }
 *   int result = UDYNLINK_CALL(&handle, int, (42));
 *
 * The _Generic ensures the cast target matches the usage context,
 * giving a compile-time error if the signature is grossly mismatched.
 */
#define UDYNLINK_CALL(p_func, ret_type, args) \
    /* expands to: set LOT base (universal), cast, call */

/* One-shot macro for convenience / cold paths.
 * Does lookup every time. Returns error code; result written to p_out_ret.
 */
#define UDYNLINK_CALL_MODULE_FUNC(p_mod, name, ret_type, args, p_out_ret) \
    /* expands to: lookup, set LOT base, cast, call, write result */
```

**Open detail**: The exact `_Generic` formulation needs to be validated against real ARM GCC. We will prototype before finalizing. If a specific toolchain lacks C11, a C99 fallback path (without `_Generic`) will be provided under `#ifdef __STDC_VERSION__`.

### Phase 2: C++ Idiomatic API (`udynlink/udynlink.hpp`)

**Design principles:**
- Full RAII for module lifecycle
- Template-based type safety for function calls
- Header-only, no `.cpp` file
- Error-code based (exceptions disabled, matching `-fno-exceptions` embedded convention)
- `udynlink::Module` constructor sets LOT base and **unconditionally** calls `udynlink_cpp_init()` (safe no-op for C modules, matches RAII convention)
- `udynlink::Context` RAII for efficient repeated calls from the same module in a tight loop

#### 2a. `udynlink::Func<R(Args...)>` Template

```cpp
namespace udynlink {

template<typename Sig>
class Func;

template<typename R, typename... Args>
class Func<R(Args...)> {
    const udynlink_module_t *p_mod_;
    uintptr_t addr_;
public:
    /* Construct from resolved handle. addr_ == 0 means "not found". */
    Func(const udynlink_module_t *p_mod, const char *name);

    /* Invoke: auto-sets LOT base (via UDYNLINK_PREPARE_CALL) then calls. */
    R operator()(Args... args) const;

    explicit operator bool() const { return addr_ != 0; }

    /* Access raw address if needed. */
    uintptr_t address() const { return addr_; }
};

} // namespace udynlink
```

#### 2b. `udynlink::Context` RAII Wrapper (Efficient LOT Base Management)

When calling multiple functions from the same module in a tight loop, per-call LOT base writes are redundant. `Context` binds to a module on construction, sets LOT base once, and restores it on destruction — enabling zero-overhead repeated calls.

```cpp
namespace udynlink {

class Context {
    const udynlink_module_t *p_mod_;
    uintptr_t prev_lot_base_;
public:
    /* Bind to a module: set LOT base, save previous value. */
    explicit Context(const udynlink_module_t *p_mod);

    /* Restore previous LOT base. */
    ~Context();

    /* Re-bind to a different module (mid-loop switch). */
    void rebind(const udynlink_module_t *p_mod);

    /* Access bound module. */
    const udynlink_module_t *module() const { return p_mod_; }
};

} // namespace udynlink
```

Usage example:
```cpp
udynlink::Module mod;
if (auto err = mod.load(mod_image_data); err != UDYNLINK_OK) { /* handle */ }

auto hello = mod.resolve<int(int)>("hello");
auto world = mod.resolve<void()>("world");

{
    udynlink::Context ctx(mod.handle());   // LOT base set once
    int r = hello(42);                     // no redundant LOT write
    world();                               // same module, zero overhead
}                                          // previous LOT base restored
```

**Thread safety note:** `Context` is NOT interrupt-safe. If an ISR calls into a different module while a `Context` is active, LOT base will be wrong. Use `Context` only in non-preemptive code paths, or disable interrupts around the block.

#### 2c. `udynlink::Module` RAII Wrapper

```cpp
namespace udynlink {

class Module {
    udynlink_module_t mod_;
    bool loaded_;
public:
    /* Load from memory-mapped image.
     * On success: sets LOT base, unconditionally calls udynlink_cpp_init()
     * (safe no-op for C modules). Returns error code; on failure mod_ is zeroed. */
    udynlink_error_t load(const void *base_addr,
                          void *load_addr, size_t load_size,
                          udynlink_load_mode_t mode);

    /* Convenience: auto-detect load mode (default COPY_ALL). */
    udynlink_error_t load(const void *base_addr);

    ~Module() { if (loaded_) unload(); }

    udynlink_error_t unload();

    bool is_loaded() const { return loaded_; }

    /* Resolve a typed function handle. */
    template<typename Sig>
    Func<Sig> resolve(const char *name) const;

    /* C++ init: exposed explicitly in case host wants to re-run after some operation.
     * Automatically called by load(); rarely needed manually. */
    void cpp_init();

    /* Access raw C handle if interop needed. */
    const udynlink_module_t *handle() const { return &mod_; }
    udynlink_module_t *handle() { return &mod_; }
};

} // namespace udynlink
```

Usage example:
```cpp
udynlink::Module mod;
if (auto err = mod.load(mod_image_data); err != UDYNLINK_OK) { /* handle */ }

auto hello = mod.resolve<int(int)>("hello");
if (!hello) { /* symbol not found */ }

int result = hello(42);   // LOT base auto-set, type-safe
// mod unloads automatically on scope exit
```

### Phase 3: Validation & Integration

- **Task 3.1**: Update CMake install targets to include new headers (`udynlink_call.h`, `udynlink.hpp`)
- **Task 3.2**: Write QEMU integration tests replacing boilerplate in `test-helloworld`, `test-helloworld-cpp`, `test-weak-symbols`
- **Task 3.3**: Add new test `test-call-ergonomics/` that exercises all C macros and C++ wrappers
- **Task 3.4**: Update `codemap.md`, `README.md`, and `AGENTS.md` with usage examples
- **Task 3.5**: Full test suite pass (`just ci`)

## Key Design Decisions (Post-User Review)

| Decision | Answer | Rationale |
|----------|--------|-----------|
| **C standard** | **C11** | `_Generic` enables compile-time type checking in macro expansions. If a specific embedded toolchain lacks C11, the user can fall back to the core API directly. |
| **LOT base strategy** | **Provide all three as separate macros** | `UDYNLINK_SET_LOT_BASE` (legacy), `UDYNLINK_SET_R9` (direct asm, max perf), `UDYNLINK_PREPARE_CALL` (universal, auto-detect). User picks the right one for their module type and performance constraints. |
| **C++ wrapper scope** | **Full `Module` + `Func` + `Context` API in a separate phase** | `Func<>` handles the call ergonomics. `Module` wraps load/init/unload in RAII, preventing leaks and auto-invoking cpp_init. `Context` amortizes LOT base writes for tight loops. |
| **Error handling** | **Error codes** | Embedded contexts vary; returning `udynlink_error_t` from resolve/load and `bool` from `Func` lets the host decide whether to assert or gracefully degrade. |
| **cpp_init in RAII** | **Yes, auto-invoked unconditionally in `Module::load()`** | `udynlink_cpp_init()` is a safe no-op for C modules (no `__init_array`). Matches RAII convention: the constructor (load) does all initialization. Host doesn't need to remember a separate step. |
| **Context prev_lot_base_** | **Save and restore on destruction** | Enables nested Contexts and safe return to host firmware state after a batch of module calls. |

## Task Breakdown

### Phase 1: C Convenience Layer
- **Task 1.1**: Implement `udynlink_func_t` handle + `udynlink_resolve_func()` in `udynlink/udynlink_call.h`
- **Task 1.2**: Implement `UDYNLINK_SET_LOT_BASE`, `UDYNLINK_SET_R9`, and `UDYNLINK_CALL` macros (building on existing `UDYNLINK_PREPARE_CALL` from Task 1B)
- **Task 1.3**: Implement `UDYNLINK_CALL_MODULE_FUNC` one-shot macro
- **Task 1.4**: Write QEMU integration tests for the new C macros (at least 2 test cases)

### Phase 2: C++ Idiomatic API
- **Task 2.1**: Design and implement `udynlink::Func<>` template in `udynlink/udynlink.hpp`
- **Task 2.2**: Design and implement `udynlink::Context` RAII wrapper for efficient LOT base management
- **Task 2.3**: Design and implement `udynlink::Module` RAII wrapper with auto `cpp_init()`
- **Task 2.4**: Write QEMU integration tests for C++ wrappers (extend `test-helloworld-cpp` or add `test-call-ergonomics-cpp`)

### Phase 3: Validation & Integration
- **Task 3.1**: Update CMake install targets to include new headers
- **Task 3.2**: Update documentation (`codemap.md`, `README.md`, `AGENTS.md`) with usage examples for all three macros and C++ API
- **Task 3.3**: Full test suite pass (`just ci`)

## Risks & Constraints

1. **Cast UB**: Casting `uintptr_t` to function pointer is not strictly standards-compliant. We mitigate with `union` punning in the macro implementation (also technically UB in C++ but widely supported on embedded ARM GCC). Document as platform abstraction.
2. **LOT base address is global mutable state**: Any wrapper that auto-sets it is not reentrant. If an interrupt fires during a module call and calls another module, LOT base will be wrong. We will document this limitation prominently in the header comments. `Context` adds a specific warning about interrupt safety.
3. **ABI compatibility**: The wrappers must not change `udynlink.h` or the module binary format. They are purely additive.
4. **Header-only**: All new code must be inline / macro / template. No new `.c` files or object code.
5. **Dependency on Task 1B**: This plan assumes `UDYNLINK_PREPARE_CALL()` and `UDYNLINK_ARCH_FLAG_NO_PROLOGUE` are already in `udynlink.h`. If Task 1B has not executed, Phase 1 macros can still be implemented using only `UDYNLINK_SET_LOT_BASE`, with a note to migrate to `UDYNLINK_PREPARE_CALL` later.

## Context Guide for Execution Agents

When implementing the tasks above, these files are the primary touchpoints:

| File | Role |
|------|------|
| `udynlink/udynlink.h` | Core public API. Must already contain `UDYNLINK_PREPARE_CALL` from Task 1B. Do not modify module structs or loader logic. |
| `udynlink/udynlink.c` | Core loader. Read-only for this plan. |
| `udynlink/udynlink_call.h` | **NEW** — C convenience layer: `udynlink_func_t`, `udynlink_resolve_func()`, macros. |
| `udynlink/udynlink.hpp` | **NEW** — C++ header-only wrapper: `udynlink::Func<>`, `udynlink::Context`, `udynlink::Module`. |
| `tests/qemu_host/src/test_utils.c` | Shared test utilities. May adopt new macros internally. |
| `tests/qemu_host/src/test_utils.h` | Declarations for test utilities. |
| `tests/test-helloworld/test_qemu.c` | Example host call site. Can be refactored to use new macros for demonstration. |
| `tests/test-helloworld-cpp/test_qemu.c` | C++ test host. Prime candidate for `udynlink::Module` RAII and `udynlink::Context`. |
| `tests/test_driver.py` | Test orchestrator. New tests need `test_data.py` + `test_qemu.c`. |
| `Justfile` | Test runner. `just test-mps2`, `just test-f429-single test-<name>`. |
| `CMakeLists.txt` | Install targets for new headers. |
| `docs/integrating-as-host.md` | Host integration guide. Document macro selection and C++ API. |
| `AGENTS.md` | Project conventions. Update with new header locations. |

### Critical Implementation Notes
1. **Prologue ABI**: The prologue loads `r9` from `UDYNLINK_LOT_BASE_ADDR`. For no-prologue modules, the host MUST set `r9` directly via `UDYNLINK_SET_R9` or `UDYNLINK_PREPARE_CALL`.
2. **`_Generic` portability**: ARM GCC 9+ supports C11 `_Generic`. If the user targets an older toolchain, provide a `#ifdef __STDC_VERSION__ >= 201112L` guard with a C99 fallback that skips `_Generic`.
3. **Error code handling**: `udynlink_resolve_func()` should return `UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL` when the symbol is not found. The caller is responsible for checking.
4. **`udynlink::Module::load()` auto-cpp_init**: Unconditionally call `udynlink_cpp_init()` — it is a no-op if no init array exists. This removes a manual step and matches RAII.
5. **`udynlink::Context` prev_lot_base_**: On construction, read `*(uint32_t*)UDYNLINK_LOT_BASE_ADDR` into `prev_lot_base_`, then write `p_mod->ram_base`. On destruction, restore `prev_lot_base_`. This supports nested Contexts.
6. **Tests run twice**: Every test is built and run with `-O0` and `-Os` (or `-O3`). Any change to the toolchain must not break either optimization level.
7. **Test isolation**: `test_driver.py` creates isolated `build_<platform>_<test>_<opt>_src` directories for each test. It copies all test source files there before compiling. New tests need a `test_data.py` and `test_qemu.c`.

## Execution Order & Dependencies

```
Phase 1 (C macros)
    │
    ├─ Task 1.1: udynlink_func_t + resolve_func()
    ├─ Task 1.2: SET_LOT_BASE / SET_R9 / CALL macros
    ├─ Task 1.3: CALL_MODULE_FUNC one-shot macro
    ├─ Task 1.4: integration tests
    │
Phase 2 (C++ API)  ← can start after Phase 1 headers are in place
    │
    ├─ Task 2.1: Func<> template
    ├─ Task 2.2: Context RAII wrapper
    ├─ Task 2.3: Module RAII wrapper
    ├─ Task 2.4: integration tests
    │
Phase 3 (Validation)
    │
    ├─ Task 3.1: CMake install targets
    ├─ Task 3.2: Documentation
    ├─ Task 3.3: just ci
```

**Recommended order:**
1. **Phase 1** first (additive, low risk, biggest immediate win).
2. **Phase 2** second (requires Phase 1 headers for best ergonomics but can be parallel if needed).
3. **Phase 3** last (integration and docs).

Each task is delegated to an `agent` subagent in a clean working copy, tested via `just ci`, and delivered as an atomic commit. Results are reviewed before the next task starts.

## Out of Scope (Deferred)

| Feature | Reason |
|---------|--------|
| **Lazy LOT base cache** | `Context` already solves the tight-loop amortization problem. A global "current module" cache adds state and reentrancy issues; `Context` is cleaner. |
| **Numeric symbol IDs** | Would eliminate string lookups entirely, but requires toolchain changes (see `micro-module-optimizations.md` Phase 2). Out of scope for ergonomics-only work. |
| **Thread safety / reentrancy** | LOT base is global mutable state; interrupts during module calls will corrupt it. A true fix requires per-context LOT base (out of scope). Document the limitation instead. |
| **`std::function`-like type erasure** | Adds vtable overhead and exceptions dependency; not suitable for `-fno-exceptions` embedded targets. |
| **JavaScript / Rust bindings** | Not relevant to this embedded C/C++ project. |

---

*Plan version: 2.0 (finalized, awaiting execution sign-off)*
*Date: 2026-05-27*
