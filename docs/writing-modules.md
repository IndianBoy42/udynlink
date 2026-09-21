# Writing Loadable Modules

This guide is for module authors who want to write C or C++ code that can be loaded at runtime by the udynlink dynamic linker on ARM Cortex-M microcontrollers.

## Table of Contents

- [Overview](#overview)
- [Quick Start: Hello World Module](#quick-start-hello-world-module)
- [Module Code Structure](#module-code-structure)
- [Consuming Host Symbols](#consuming-host-symbols)
- [Cross-Module Calls](#cross-module-function-calls)
- [C++ Modules](#c-modules)
- [Data and Variables in Modules](#data-and-variables-in-modules)
- [Memory Regions and Section Placement](#memory-regions-and-section-placement)
- [Target Selection and Cross-Compilation](#target-selection-and-cross-compilation)
- [The mkmodule Command Reference](#the-mkmodule-command-reference)
- [Building and Distributing Modules](#building-and-distributing-modules)
- [Building Modules with CMake](#building-modules-with-cmake)
- [Common Patterns](#common-patterns)
- [Common Pitfalls and Troubleshooting](#common-pitfalls-and-troubleshooting)

## Overview

### What Is a Module?

A **module** is a position-independent binary blob produced from C or C++ source code. It can be loaded into RAM at any address and executed by the host firmware at runtime. The host does not need to know the module's contents at compile time; it discovers exported functions and resolves external symbols dynamically.

### The Module-Host Relationship

Your module runs in the host's address space and uses the host's memory allocator, debug output, and hardware abstractions. The host firmware must implement a small set of callbacks defined in [`udynlink_externals.h`](api-reference.md#udynlink_externals.h). At load time, udynlink resolves every undefined symbol in your module by asking the host, "Do you have a function or variable named `X`?" If the host says yes, the linker patches your module so that calls to `X` go to the host's implementation.

### What Modules Can Do

- Pure computation (math, algorithms, state machines)
- Call host-provided functions (`printf`, `malloc`, sensor drivers, etc.)
- Maintain global and static state
- Use C++ classes, templates, and namespaces

### What Modules Cannot Do

- **Direct hardware register access** unless the host explicitly maps MMIO into the module's address space or provides accessor functions.
- **C++ exceptions** — compiled with `-fno-exceptions`.
- **Run-Time Type Information (RTTI)** — compiled with `-fno-rtti`.
- **`atexit` / `__cxa_atexit`** — compiled with `-fno-use-cxa-atexit`.
- **System calls** — modules are linked with `-nostdlib`; there is no libc or system runtime.

## Quick Start: Hello World Module

### Step 1: Write `hello.c`

```c
#include <stdio.h>

int hello(int arg) {
    printf("Hello World! arg=%d\n", arg);
    return -arg;
}
```

`hello` is a non-static global function, so it becomes an **exported symbol** that the host can call. `printf` is not defined in the module, so it becomes an **external symbol** that the host must resolve at load time.

### Step 2: Compile with `mkmodule`

```bash
cd scripts
python3 mkmodule --gen-c-header --header-path /some/path hello.c
```

This produces:
- `hello.bin` — the binary module image.
- `/some/path/hello_module_data.h` — a C header with a `static const unsigned char hello_module_data[]` array for embedding at compile time.

### Step 3: Host Loads and Calls the Module

```c
#include "udynlink.h"
#include "hello_module_data.h"

udynlink_module_t mod;

void load_and_run(void) {
    // Load the module (auto-allocate RAM)
    if (udynlink_load_module(&mod, hello_module_data, NULL, 0,
                             UDYNLINK_LOAD_MODE_COPY_ALL) != UDYNLINK_OK) {
        return;
    }

    // Before calling ANY module function, prepare the call context
    UDYNLINK_PREPARE_CALL(&mod);

    // Look up the exported function
    udynlink_sym_t sym;
    if (udynlink_lookup_symbol(&mod, "hello", &sym) == NULL) {
        udynlink_unload_module(&mod);
        return;
    }

    // Call it
    int (*p_hello)(int) = (int (*)(int))sym.val;
    int result = p_hello(42);  // prints: Hello World! arg=42

    udynlink_unload_module(&mod);
}
```

For a deeper explanation of the load modes and the LOT, see [How It Works](how-it-works.md).

## Module Code Structure

### Exported Functions

Any non-static global function is **exported** and visible to the host:

```c
// Exported — host can look this up and call it.
int add(int a, int b) {
    return a + b;
}
```

### Internal Functions

`static` functions are invisible to the host. They are not wrapped, do not appear in the symbol table, and incur zero overhead for the host:

```c
// Internal — used only inside this module.
static int helper(int x) {
    return x * 2;
}

int public_func(int x) {
    return helper(x) + 1;
}
```

> **How are exported functions preserved?** The toolchain links with `--gc-sections`, which strips any function not reachable from an entry point or a `KEEP()` directive. Exported functions survive because `mkmodule` generates an assembly prologue wrapper for each one and places it in a special `.text_nogc` section. The linker script contains `KEEP(*(.text_nogc))`, so the wrappers — and the real function bodies they reference — are never garbage-collected. `static` functions, which get no wrapper, are eligible for removal if nothing calls them. With `--no-prologue`, the toolchain uses `-Wl,--undefined=<sym>` for each exported symbol instead. You do not need `__attribute__((used))` on exported functions; the wrapper mechanism already keeps them alive. For the full technical details, see [How It Works — Interaction with --gc-sections](how-it-works.md#interaction-with---gc-sections).

### Global Variables

Non-static global variables are also exported symbols. The host can look them up with `udynlink_lookup_symbol`:

```c
volatile int g_counter = 0;

int increment(void) {
    return ++g_counter;
}
```

### The `--public-symbols` Flag

By default, every non-static global function and variable is exported and gets an assembly wrapper (prologue) that sets up the LOT base register (`r9`). This increases code size. If you only need a few functions to be callable from the host, use `--public-symbols`:

```bash
python3 mkmodule --public-symbols add,subtract hello.c
```

Only `add` and `subtract` will be wrapped and exported. Everything else stays local, reducing binary size.

### Module Name

The module name is derived from the first source file by default:
- `hello.c` produces a module named `hello`.
- `sensor_driver.cpp` produces a module named `sensor_driver`.

You can override it with `--module-name my_module`.

## Consuming Host Symbols

### How It Works

When you call a function that is not defined in your module, the compiler leaves the call unresolved. At load time, udynlink walks the relocation table and asks the host to resolve each undefined symbol. The host's `udynlink_external_resolve_symbol` callback returns the address of the function or variable. If the host does not provide the symbol, loading fails with `UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL`.

### Declaration Patterns

**Pattern 1: `extern` declarations in your source file**

```c
extern int sensor_read(int channel);
extern void led_set(int pin, int state);

int read_temperature(void) {
    return sensor_read(0);  // channel 0 = temperature
}
```

**Pattern 2: Include a host-provided header (recommended)**

The host firmware can distribute a header that declares all services available to modules:

```c
// host_api.h — provided by the host firmware
#ifndef HOST_API_H
#define HOST_API_H

extern int sensor_read(int channel);
extern void led_set(int pin, int state);
extern void delay_ms(unsigned int ms);

#endif
```

Your module simply includes it:

```c
#include "host_api.h"

int read_and_blink(void) {
    int temp = sensor_read(0);
    led_set(1, 1);
    delay_ms(100);
    led_set(1, 0);
    return temp;
}
```

### What Happens If the Symbol Is Missing

If the host does not implement `sensor_read`, `udynlink_load_module` returns `UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL`. The module handle is invalid and must not be used.

### Cross-Module Function Calls

If your module needs to call functions in another module (e.g., a math library), declare them as `extern` just like host symbols. The host's dependency system will resolve them at load time:

```c
// mod_app.c — calls functions from mod_math
extern int math_add(int a, int b);
extern int math_mul(int a, int b);

int call_math(int a, int b) {
    int sum = math_add(a, b);
    int prod = math_mul(a, b);
    return sum | prod;
}
```

The other module (`mod_math.c`) simply exports the functions as non-static globals:

```c
int math_add(int a, int b) { return a + b; }
int math_mul(int a, int b) { return a * b; }
```

At load time, the host's `udynlink_external_resolve_symbol()` callback locates the target module and allocates a 10-byte stub (plus one shared 18-byte gateway per callee module) from the thunk pool. The stub loads the function address into `r12` (IP) and branches to the gateway, which switches `r9` to the callee module's LOT base before calling the function. From the module author's perspective, this is transparent — the call looks like a normal function call.

### Declaring Explicit Dependencies

To ensure a module is not loaded before its dependencies are available, use `UDYNLINK_REQUIRES`:

```c
#include "udynlink_deps_api.h"

UDYNLINK_REQUIRES(math);

extern int math_add(int a, int b);
```

`UDYNLINK_REQUIRES(math)` expands to an extern symbol named `.udynlink.mod.requires.math`. The host's dependency system recognizes this prefix and checks that a module named `math` is already loaded. If the dependency is missing, the load fails with `UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL`.

`udynlink_deps_api.h` is the **module-facing** half of the dependency system — the only udynlink header a module source ever needs (it is self-contained; see the [mkmodule `-I` flag](#the-mkmodule-command-reference) below for how module builds reach it). Hosts implement the runtime side via `udynlink_deps.h`; module sources must never include that one.

**Best practice:** Always use `UDYNLINK_REQUIRES` for every module you depend on. It documents the dependency for readers and enables the host to fail fast with a clear error message.

### Preallocating Cross-Module Thunk Exports

By default, cross-module calls allocate their trampolines (an 18-byte gateway + 10-byte stub per function) **lazily** from a shared dynamic thunk pool the first time another module imports the symbol. If a module knows which of its exports are most likely to be imported, it can instead **preallocate** the thunk space inside its own `.bss` and have the host generate the thunks at load time, so the dynamic pool is never touched for those exports.

In the exporting module:

```c
#include "udynlink_deps_api.h"

UDYNLINK_THUNK_GATEWAY();   /* exactly one per module */
UDYNLINK_THUNK_EXPORT(math_add);   /* one per export you expect to be imported */
```

`UDYNLINK_THUNK_GATEWAY()` reserves an 18-byte gateway slot and each `UDYNLINK_THUNK_EXPORT(fn)` a 10-byte stub slot, all in the module's `.bss` section `.bss.udynlink_thunk_pool` (kept alive under `--gc-sections` by `KEEP(*(.bss.udynlink_thunk_pool))` in `scripts/code_before_data.ld` — GCC's `__attribute__((retain))` is not honored by arm-none-eabi-gcc for variables). `udynlink_dep_load()` then calls `udynlink_dep_generate_thunks()` automatically after loading, which writes the gateway (patched with the module's `ram_base`) and one stub per declared export into those slots.

At resolve time, `udynlink_dep_resolve_func()` serves the pre-generated in-module thunk for a declared export; exports the module did not declare still fall back to the dynamic thunk pool. The macros live in `udynlink_deps_api.h`, which is self-contained (only `<stdint.h>`) so module sources can include it without dragging in any host-facing udynlink header.

**Relocation caveat:** because the thunks live inside the module's RAM, `udynlink_relocate_module()` invalidates their absolute immediates (gateway `ram_base`, stub function addresses). After relocating a module that declares thunk exports, the host must call `udynlink_dep_generate_thunks(&mgr, p_mod)` again; the `b.n` branches inside the slots survive the move unchanged.

### Common Host Symbols

Typical symbols a host firmware provides:

- `printf`, `sprintf`, `snprintf` — debug output
- `malloc`, `free` — dynamic memory
- `memcpy`, `memset`, `strlen` — standard string/memory operations
- Custom hardware abstractions: `sensor_read`, `adc_sample`, `pwm_set`, `spi_transfer`

## C++ Modules

### Supported Features

- Classes and objects
- Constructors and destructors
- Templates
- Namespaces
- Function overloading (for internal use; exported symbols need `extern "C"`)

### Restricted C++ Features

The toolchain automatically adds these flags for `.cpp` and `.cxx` files:

- `-fno-exceptions` — no `try`/`catch`/`throw`
- `-fno-rtti` — no `typeid` or `dynamic_cast`
- `-fno-use-cxa-atexit` — no static object destruction at exit
- `-fno-threadsafe-statics` — function-local `static` variables use a plain byte flag instead of the `__cxa_guard_*` ABI calls. Matches udynlink's "no thread safety" stance (the loader itself is not thread-safe). A host that *needs* interlocked first-time initialization for a specific module can opt back in per-module with `--build-flags=-fthreadsafe-statics`; see [Thread Safety — Thread-Safe Function-Local Statics](thread-safety.md#thread-safe-function-local-statics-__cxa_guard_).

### C++ ABI Symbols the Host Must Provide

Even with the flags above, GCC emits calls to a small set of C++ ABI symbols that the loader classifies as `external` and the host must resolve at load time — even when the call site is unreachable at runtime (for example, the deleting destructor of a class with `virtual ~T() = default;` that the module never `delete`s). Without a binding, `udynlink_load_module()` returns `UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL` on the first load mode and every subsequent one.

| Mangled name | Plain name | When referenced |
|---|---|---|
| `_ZdlPv`, `_ZdlPvj` | `operator delete(void*)`, `operator delete(void*, unsigned int)` | Any class with a `virtual` destructor (deleting destructor). Also reachable if the module actually executes `delete` through a base pointer. |
| `_ZdaPv`, `_ZdaPvj` | `operator delete[]` array forms | As above for array `delete[]`. |
| `_ZdlPvjSt11align_val_t`, `_ZdaPvjSt11align_val_t` | aligned `operator delete` / `delete[]` | Types with `alignas > 16`. |
| `_Znwj`, `_Znaj` | `operator new(unsigned int)`, `operator new[](unsigned int)` | Any `new T` / `new T[n]` expression the module actually executes. |
| `_ZnwjSt11align_val_t`, `_ZnajSt11align_val_t` | aligned `operator new` / `new[]` | `alignas > 16`. |
| `_ZnwjRKSt9nothrow_t`, `_ZnajRKSt9nothrow_t` | nothrow `operator new` / `new[]` | `new (std::nothrow) T`. |
| `__cxa_pure_virtual` | abstract-base vtable slot | Any class with a pure-virtual method (`= 0`) that GCC emits a vtable for. Called only by undefined behavior (dispatching a pure virtual during construction/destruction). |

The header `udynlink/udynlink_cpp_abi.h` ships weak defaults for all of them (`new`/`delete` forward to `udynlink_external_malloc`/`udynlink_external_free`; `__cxa_pure_virtual` loops forever) plus a resolver helper. The stubs have **neutral C names** (`udynlink_cpp_new`, `udynlink_cpp_delete`, ...) — udynlink never links against the host's symbol table by name, it only ever binds through `udynlink_external_resolve_symbol`. The stubs are therefore deliberately *not* named `_Znwj` / `_ZdlPv` at the C level; the resolver maps the mangled name the loader passes through to whichever address the host chooses. This keeps a C++ host's own libstdc++-provided `_Znwj` from silently overriding the module's `new` path at link time — the host stays in explicit, in-code control of which implementation a module sees.

Include the header and call the resolver from `udynlink_external_resolve_symbol`:

```c
#include "udynlink_cpp_abi.h"

uintptr_t udynlink_external_resolve_symbol(const udynlink_module_t *p_mod,
                                            const char *name) {
    (void)p_mod;
    uintptr_t a = udynlink_cpp_resolve_abi_symbol(name);
    if (a) return a;
    // ... host's normal symbols (printf, malloc, custom HAL, ...) ...
    return 0;
}
```

To change a stub's behavior (sized/aligned free, a log+abort on pure-virtual, routing `operator new` to the host's libstdc++ entry point, interposing for debugging), edit what `udynlink_cpp_resolve_abi_symbol()` returns for that name — or copy the resolver and substitute your own pointers. There is no link-time override to set up, and the stub's weak attribute is only there so the linker drops unused stubs. The `__cxa_guard_*` family is deliberately **not** shipped as a stub in this header — it is a synchronization primitive the host must implement; see [Thread Safety — Thread-Safe Function-Local Statics](thread-safety.md#thread-safe-function-local-statics-__cxa_guard_) for the opt-in build flag and a reference stub.

> **Do not define `__cxa_*` or `operator delete` inside a module.** `mkmodule` wraps every defined `STB_GLOBAL`/`STB_WEAK` function with a prologue that assumes `r9` is set up. If `__cxa_pure_virtual` is defined in the module, the vtable slot that references the un-wrapped name points at a wrapper expecting a stale `r9`, and dispatch through that slot (an undefined-behavior path that nonetheless must load cleanly) corrupts PIC state. Always provide these symbols on the host side — either through `udynlink_cpp_abi.h` or through the host's own C++ runtime.

### Global Constructors

If your module has global C++ objects with constructors, the host **must** call `udynlink_cpp_init()` after loading the module. The function sets the call context internally before invoking `__init_array`; you should still use `UDYNLINK_PREPARE_CALL()` before calling any other module function:

```c
udynlink_load_module(&mod, hello_cpp_bin, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
udynlink_cpp_init(&mod);   // Run __init_array (sets context internally)
UDYNLINK_PREPARE_CALL(&mod); // Prepare before other calls
```

If the host uses the C++ API (`udynlink.hpp`), `udynlink::Module::load()` calls `udynlink_cpp_init()` automatically:

```cpp
udynlink::Module mod;
mod.load(hello_cpp_bin);  // auto-calls udynlink_cpp_init()
```

The toolchain automatically compiles `cpp_init_fini.c` and links it into C++ modules. This file walks `__init_array` and `__preinit_array` to invoke all global constructors.

### C++ Module Example

```cpp
#include <stdio.h>

class Test {
public:
    Test(const char* arg) {
        printf("%s_constructor\n", arg);
    }
};

static Test constructor_test("static");

extern "C" int hello(int arg) {
    printf("Hello World! arg=%d\n", arg);
    return -arg;
}

extern "C" int test(void) {
    return (hello(0) == 0 && hello(10) == -10);
}
```

**Build:**

```bash
python3 mkmodule --gen-c-header mod_hello_cpp.cpp
```

**Host loading:**

```c
udynlink_module_t mod;
udynlink_load_module(&mod, mod_hello_cpp_module_data, NULL, 0,
                     UDYNLINK_LOAD_MODE_COPY_ALL);
UDYNLINK_PREPARE_CALL(&mod);
udynlink_cpp_init(&mod);

// Now safe to call exported functions
```

### Controlling C++ Symbol-Table Size

Heavily templated C++ modules emit a symbol table that can exceed the size of the module's code and data. Most of those symbols are name-mangled C++ internals (vtables, inline/template instantiations, internal helpers promoted to global by the compiler) that the host never looks up and the loader never needs to resolve by name. `mkmodule` ships four opt-in flags that demote these from the named pool to nameless internal entries without breaking any documented behavior (host `udynlink_lookup_symbol` on intended exports, load-time extern/weak resolution, cross-module deps). All four are pure `mkmodule` changes; the loader and binary format are untouched.

The default behavior with no flag set is byte-for-byte unchanged, so existing modules stay small. Pick the flag that matches your export convention.

| Flag | Demotes | When to use |
|------|---------|-------------|
| `--strip-hidden-syms` | Defined symbols with ELF visibility `STV_HIDDEN`/`STV_INTERNAL`. | The default C++ workflow. Pair with `-fvisibility=hidden -fvisibility-inlines-hidden` in `--build-flags` and mark real exports with `__attribute__((visibility("default")))`. Demotes vtables, inline instantiations, and any helper your compiler did not auto-statics. |
| `--strip-mangled-syms` | All defined `extern "C"`-not-marked symbols whose name starts with `_Z` (Itanium-mangled C++). Also skips the prologue wrapper for them, so the renamed name no longer carries the mangled name as a suffix. | When your module exposes only `extern "C"` entry points and you do not bother with visibility attributes. Demotes every C++ internal; keeps the `extern "C"` API. |
| `--strip-non-public-syms` | Every defined symbol not listed in `--public-symbols`. | When you already pass `--public-symbols <list>`. Closes the latent table-size gap (see `--public-symbols` above). |
| `--strip-weak-sym-names` | Every defined `STB_WEAK` symbol not in `--public-symbols`. | Vtables, inline instantiations, and any linkonce-odr definition emitted weak by GCC. Note: this loses the host-override path for those weak names — only use it when you do not need a host-side override of the symbol by name. |

Symbols listed in `--public-symbols` are never demoted, regardless of which flag is set. Dependency declarations (`.udynlink.mod.requires.*`), the module name, and unresolved externs are also never demoted.

Example — hidden-visibility workflow:

```bash
python3 mkmodule \
  --build-flags='-fvisibility=hidden -fvisibility-inlines-hidden' \
  --strip-hidden-syms \
  mod_cpp_filter.cpp
```

Example — `extern "C"`-only workflow:

```bash
python3 mkmodule --strip-mangled-syms mod_cpp_filter.cpp
```

#### How demotion works

Each flag demotes an eligible *defined* symbol from `exported`/`weak` to `internal` (nameless in the emitted symbol table). The symbol keeps its address, so relocations (`R_ARM_GOT_BREL`, `R_ARM_ABS32`) still resolve to the module's own value. Only the *name* is dropped from the binary. Consequences, all verified against the loader:

- `udynlink_lookup_symbol(p_mod, name, &sym)` returns `NULL` for a demoted name — the binary search of named entries no longer finds it.
- `udynlink_external_resolve_symbol(p_mod, name)` is never called for a demoted symbol — the loader only queries the host for `external`/`weak` entries.
- Intra-module `bl` calls, function pointers taken inside the module, and virtual dispatch via vtables **keep working** — these go through PC-relative calls or LOT/data relocations patched with the module's own value, none of which needs the name.

The four flags differ only in *which* defined symbols get demoted and what host/module capability is forfeited.

#### Dead-code elimination is separate

The toolchain already links with `--gc-sections` (paired with `-ffunction-sections -fdata-sections`), so **unused code and data are removed before the symbol table is even built**. GC roots are the `.text_nogc` section (`KEEP(*(.text_nogc))` in `scripts/code_before_data.ld`, which holds the prologue wrappers), `__init_array` (C++ global constructors), the entry point, and `-Wl,--undefined=` symbols. Anything unreachable from those roots is dropped at link time.

The bloat these four flags address is **live-but-unneeded names**: symbols whose code survives GC (because something references it) but whose name the host never needs. `--gc-sections` does not strip names from the symbol table — that is exactly the gap these flags close.

Only `--strip-mangled-syms` interacts with GC, as a side effect: it runs in `compile()` (before `link()`) and skips prologue-wrapper generation for mangled symbols. The wrapper normally lives in `.text_nogc` (a GC root) and references the renamed function body, keeping it alive. With the wrapper skipped, a mangled global that nothing else references becomes unreachable and `--gc-sections` drops its **body too** — a bonus code-size reduction beyond the name stripping. The other three flags run in `process()` (after linking) and only touch names, never code bytes.

#### Host and module impact per flag

| Flag | Module side | Host side | Forfeits |
|------|-------------|-----------|----------|
| `--strip-hidden-syms` | Adopt the visibility model: hide by default, mark exports `visibility("default")`. Requires `-fvisibility=hidden -fvisibility-inlines-hidden` in `--build-flags`. | No code change. Host still finds the `extern "C"` exports marked `default". | Nothing spec-valid: `STV_HIDDEN` symbols are non-interposable by ELF spec, so stripping their name violates no linker contract. |
| `--strip-mangled-syms` | Use `extern "C"` for every export (already the documented convention). No visibility attributes or `--public-symbols` list needed; works in default mode. | No code change. Host finds `extern "C"` exports; mangled lookups return `NULL`. | Ability to call a C++ method by mangled name from the host. Workaround: an `extern "C"` wrapper. |
| `--strip-non-public-syms` | Pass `--public-symbols <list>` and enumerate every intended export. | No code change. Host sees exactly the listed exports. | Lookups of non-listed globals now return `NULL` instead of a dangerous pointer to unwrapped code. This is a **latent-bug fix**: today `--public-symbols` narrows wrapping but not the table, so calling a non-listed global by name would jump to raw code with no `r9` prologue and corrupt PIC state. |
| `--strip-weak-sym-names` | None beyond passing the flag. Vtables and ODR-weak instantiations lose their names. | `udynlink_external_resolve_symbol` is no longer called for defined weaks. | Host-override of defined weak symbols by name (the loader's `WEAK` override path). The module's own default is used instead. **Silent regression** if you forget to list an override-eligible weak in `--public-symbols` — list such weaks explicitly to preserve the override. |

#### Choosing and combining flags

| Workflow | Flags | Notes |
|----------|-------|-------|
| Default C++ module | `--strip-hidden-syms` + `-fvisibility=hidden -fvisibility-inlines-hidden` | Spec-correct, predictable, no feature loss. The recommended starting point. |
| `extern "C"`-only module | `--strip-mangled-syms` | Zero author workflow change. Also shrinks `.text` (wrapper skip) alongside the table. |
| Strict export control | `--strip-non-public-syms` + `--public-symbols <list>` | Smallest table, full authorial intent. Also closes the unwrapped-export landmine. |
| Heavy-template legacy module | `--strip-mangled-syms` + `--strip-weak-sym-names` | Attacks vtables and ODR-weaks, the biggest bloat sources in templated C++. Avoid `--strip-weak-sym-names` if the host overrides weak hooks by name. |
| Maximum shrink | all four + `-fvisibility=hidden` + `--public-symbols` | Only for modules that need no host override of weaks. |

All four are safe to compose. `--public-symbols` exempts listed symbols from every flag. The end-to-end test `tests/test-cpp-symbol-filter` exercises `--strip-hidden-syms` on QEMU across all three load modes; `tests/test-cpp-symbol-filter-inlines` validates inline, static inline, and weak template instantiations under `--strip-mangled-syms --strip-weak-sym-names`; `tests/test-cpp-symbol-filter-weak-override-loss` confirms `--strip-weak-sym-names` does not break module loading; `tests/test-cpp-symbol-filter-init-fini` exercises C++ and C constructors under `--strip-mangled-syms`; `tests/test-cpp-symbol-filter-public-list` verifies `--public-symbols` + `--strip-non-public-syms` filtering; `tests/test-cpp-symbol-filter-composition` exercises all four flags combined with a public-symbols allowlist on QEMU; `tests/test-cpp-symbol-filter-hidden-mangled` and `tests/test-cpp-symbol-filter-hidden-weak` cover the remaining untested pair combinations; and `tests/test-cpp-symbol-emission-all` is the comprehensive QEMU integration test covering every GCC C++ symbol emission pattern (extern "C", classes, vtables, templates, inline, weak, hidden, constructors, namespace, destructors, template variables, static member data, multiple inheritance). `tests/py/test_symbol_filter.py` exercises all four flags against the parsed `.bin` symbol table, and `tests/py/test_cpp_symbol_emission_all.py` provides comprehensive parameterized tests for all 16 flag combinations across all emission patterns.

## Data and Variables in Modules

### Global Variables

Global variables work, but every access goes through the LOT (Linker Offset Table), which adds a small constant overhead compared to native code. For most applications this is negligible.

```c
volatile int g_counter = 50;

int increment(void) {
    g_counter += 10;
    return g_counter;
}
```

### Strings and String Literals

String literals are stored in `.rodata`, which is merged into `.text` during module construction. They work without any special handling.

```c
#include <stdio.h>

int test(void) {
    printf("Running test '%s'\n", "mod_three_files");
    return 1;
}
```

### Arrays

Arrays are relocated properly:

```c
static const int lookup_table[4] = {0, 10, 20, 30};

int get_value(int idx) {
    return lookup_table[idx & 3];
}
```

### Function Pointers

Function pointers to **module-internal** functions work correctly:

```c
static int square(volatile int *x) {
    return (*x) * (*x);
}

int double_val(volatile int *x) {
    return (*x) + (*x);
}

typedef int (*fptr_t)(volatile int*);

int test(void) {
    volatile fptr_t p = square;
    return p(&g) == 100;
}
```

Function pointers to **exported** functions also work, but be aware that taking the address of an exported function from inside the module yields the **wrapper address** (the assembly prologue that sets `r9`), not the raw function body. This is usually what you want for callbacks, but it means the wrapper runs every time the pointer is invoked.

Function pointers to **host** functions require care. If the host returns the address of a host function during symbol resolution, storing and calling it from a module works as long as the module sets `r9` correctly before the call. In practice, host callbacks are best invoked through named calls rather than stored pointers.

### `volatile` Variables

`volatile` is fully supported and is the correct way to interact with memory-mapped hardware if the host maps MMIO into the module's address space:

```c
volatile uint32_t *const TIMER_REG = (volatile uint32_t *)0x40000000;

void start_timer(void) {
    *TIMER_REG = 1;
}
```

### Section Placement

Without `--section`, the module image packs a single `.text`, a single
`.data` and a single `.bss` blob; the linker script folds the common
input-section variants into them (`.text*`/`.rodata*`/`.text_nogc` into
`.text`, `.data*`/`.sdata` into `.data`, `.bss*`/`.sbss`/`COMMON`/
`.bss.udynlink_thunk_pool` into `.bss`). A custom placement such as

```c
__attribute__((section(".fastdata"))) int x = 1;
```

fails the build with an error that names the offending section (`.fastdata`)
and the symbols stranded in it, because the packed image format has nowhere
to carry them. Place such objects in `.data`/`.bss` (or `.text` for code) —
or, when they genuinely belong in different memory, declare a placement
section as described in [Memory Regions and Section Placement](#memory-regions-and-section-placement).

### Data Alignment

Without `--section`, the loader guarantees only **4-byte alignment** for
module data: the RAM arena that holds `.data` and `.bss` starts at a
4-byte-aligned address and the sections are laid out in 4-byte steps.
`__attribute__((aligned(N)))` with `N > 4` on module variables is therefore
not honored at runtime — `mkmodule` prints a warning naming the required
alignment when it detects this, and the variable ends up at a merely
4-byte-aligned address. Code that dereferences such a variable with
alignment-sensitive instructions (`ldrd`/`strd`, floating-point loads, DMA
descriptors) can fault or corrupt data. The fix is a placement section:
`--section <name>` plus `UDYNLINK_SECTION_ALIGNED("<name>", N)` makes the
alignment real — the host places the whole section at a suitably aligned
address (see [Memory Regions and Section Placement](#memory-regions-and-section-placement)).
Alternatively, build with `--tag-on-align`: mkmodule then emits a section
table for the image (main `.text`/`.data`/`.bss` entries only) whenever one
of the main sections requires alignment above 4 in the linked ELF, so the
loader aligns each main section inside the RAM block and the alignment is
honored without a placement section. Such an image requires a loader ABI of
at least 3.1 — mkmodule raises `--udynlink-version` accordingly and prints
one line naming the triggering section and symbols instead of the untagged
warning above. Without the flag the image stays untagged.

## Memory Regions and Section Placement

Microcontrollers do not have one uniform RAM: there is tightly coupled
memory for time-critical code and data, DMA-capable SRAM, non-cacheable
regions for coherency, memory shared with other cores. A plain module gets
one host-provided RAM block for everything; **placement sections** let a
module state, per object or function, which kind of memory it needs. The
host stays in control: it resolves every section to a real address at load
time through its allocator callbacks (see [Integrating as a Host](integrating-as-host.md)).

Modules built **without** `--section` are unaffected: the image is
byte-identical to the plain format and takes the plain load path.

### Tagging Code and Data

Include `udynlink/udynlink_section.h` (self-contained; add the udynlink
include directory to mkmodule via `-I`) and tag objects or functions:

```c
#include "udynlink_section.h"

UDYNLINK_SECTION("dtcm") volatile int dtcm_counter = 7;            /* data  */
UDYNLINK_SECTION_ALIGNED("dma", 32) volatile unsigned char
    dma_buf[64];                                                    /* 32-byte-aligned DMA buffer */
UDYNLINK_SECTION("fastcode") int fast_scale(int v) { return v * 2; } /* code */
```

- `UDYNLINK_SECTION(name)` places the object/function into placement
  section `name`; `UDYNLINK_SECTION_ALIGNED(name, al)` additionally raises
  the alignment requirement to `al` bytes (power of two).
- Both work on objects **and** functions. Whether a section counts as code
  or data is derived from what it contains, not from the attribute.
- Zero-initialized objects (`.bss`-like) are carried as class `BSS`: the
  loader zeroes them, and the image carries no payload for them.
- Every tag used in the sources **must** be declared to mkmodule with
  `--section <name>`; an undeclared tag is a hard build error (an
  undeclared placement section would be silently dropped by
  `--gc-sections`).

### Declaring Sections on the Command Line

```bash
python3 mkmodule --section dtcm \
                 --section dma:align=32:flags=DMA,NOCACHE \
                 --section fastcode -I /path/to/udynlink hello.c
```

```
--section NAME[:align=N][:flags=F1,F2,...]     (repeatable)
```

| Part | Meaning |
|------|---------|
| `NAME` | `[A-Za-z_][A-Za-z0-9_]*` (it names a linker memory region; `main` is reserved). The tag used in `UDYNLINK_SECTION("NAME")`. |
| `align=N` | Alignment the host must honor when placing the section. Power of two, `>= 4`, default `4`. Content needing more (e.g. `UDYNLINK_SECTION_ALIGNED`) raises it — mkmodule warns when that happens. |
| `flags=...` | Comma-separated hint flags, below. Unknown names are rejected with the valid set. |

Hint flags (bit values as carried in the image; the loader never interprets
them — they are inputs to the host's placement policy):

| Flag | Bit | Meaning |
|------|-----|---------|
| `NOCACHE` | `0x01` | Host should map the memory non-cacheable (e.g. DMA coherency). |
| `DMA` | `0x02` | Must be reachable by the DMA controller (e.g. not DTCM/ITCM). |
| `SHARED` | `0x04` | May be shared with other modules or host code. |
| `host<N>` | `1 << (8+N)`, `N` in `0..7` | Host-private hint; passed through untouched. |

### Version Requirement

Sectioned images declare loader ABI **3.1**. Without an explicit
`--udynlink-version`, `--section` builds default to `3.1`; an explicitly
passed lower version is rejected (an old 3.0 loader would reject the image
anyway, by version fence). Untagged builds keep the `3.0` default.

### Guards

Some code models cannot survive placement, so `mkmodule` rejects them up
front or at link time:

- `--pc-rel` combined with `--section`: PC-relative data addressing bakes
  in the link-time placement, so the section could not be resolved to a
  different address at load time.
- `--no-long-calls` combined with `--section`: direct `bl` branches cannot
  cross placement sections. (The default `-mlong-calls` routes every call
  through the LOT, which is placement-agnostic.)
- Any direct branch (`R_ARM_THM_CALL`/`R_ARM_THM_JUMP24`) whose target
  lands in a different section than the call site is a hard error naming
  both sections and the symbol — a direct branch cannot be relocated to
  unrelated runtime addresses. Exported functions are safe automatically:
  their prologue wrappers are emitted into the same placement section as
  the function body.

### What the Host Does

At load time the loader asks the host to place each tagged section — one
`udynlink_external_malloc` call per section, named by section and carrying
the declared alignment and hint flags — then copies or zeroes the section
content at the returned address and validates the alignment. A host
returning `NULL` (or an under-aligned pointer) fails the load with a
section-specific error after freeing everything already allocated. The
main RAM block keeps today's single-allocation behavior (the callback
distinguishes it by a `NULL` section name). See
[Integrating as a Host](integrating-as-host.md) for the callback signature
and a pool example, and [API Reference](api-reference.md) for
`udynlink_get_section_info`/`udynlink_get_section_base`.

## Target Selection and Cross-Compilation

### Choosing the Right Target

The module's target must match the host MCU's core family and floating-point ABI. If the architecture tags do not match, `udynlink_load_module` returns `UDYNLINK_ERR_LOAD_ARCH_MISMATCH`.

### Default Target

```bash
python3 mkmodule hello.c          # defaults to --target cortex-m4
```

### Supported Targets

| Target | Core | FPU | Float ABI |
|--------|------|-----|-----------|
| `cortex-m0` | ARMv6-M | None | soft |
| `cortex-m0plus` | ARMv6-M | None | soft |
| `cortex-m3` | ARMv7-M | None | soft |
| `cortex-m4` | ARMv7E-M | None | soft |
| `cortex-m4f` | ARMv7E-M | fpv4-sp-d16 | hard |
| `cortex-m7` | ARMv7E-M | fpv5-d16 | hard |
| `cortex-m33` | ARMv8-M.Main | None | soft |
| `cortex-m55` | ARMv8.1-M.Main | auto | hard |
| `cortex-m85` | ARMv8.1-M.Main | auto | hard |

### Architecture Tag Compatibility

A module compiled for `cortex-m4f` (hard-float) will **not** load on a `cortex-m4` host (soft-float), even though both use the same core. The loader checks the `arch_tag` field in the module header at load time.

### Overriding the CPU

For non-standard or custom cores, override the raw `-mcpu` flag:

```bash
python3 mkmodule --mcpu cortex-m4f --target cortex-m4 hello.c
```

`--mcpu` always overrides the target database's default `mcpu`.

### Custom Compiler Flags

Pass extra flags to the compiler:

```bash
python3 mkmodule --build-flags="-Wall -Wextra -DENABLE_FAST_PATH" hello.c
```

## The mkmodule Command Reference

`mkmodule` is the module build tool. It compiles, links, and packages your source files into a udynlink binary module.

### Positional Arguments

One or more source files:

- `.c` — compiled as C
- `.cpp`, `.cxx` — compiled as C++ (automatic `-fno-exceptions -fno-rtti -fno-use-cxa-atexit`)

Leading `-Dmacro[=value]` definitions precede the source list, and one
leading `--` separator is accepted before it (wrapper scripts forward it
when shielding their own flags from their parser). Any other dash-prefixed
argument is rejected with an error naming it — unrecognized options used to
be handed to the compiler as input files, which failed with a confusing
"unrecognized command-line option". Compiler flags belong in
`--build-flags`/`-I`; they are never positional.

Source files are compiled with:
- `-fPIE -msingle-pic-base -mno-pic-data-is-text-relative`
- `-ffunction-sections -fdata-sections`
- `-mlong-calls`
- `-Os` (default; override with `-O <level>`)

### Options

| Option | Description |
|--------|-------------|
| `--target <name>` | Target from the database. Default: `cortex-m4`. |
| `--mcpu <cpu>` | Raw GCC `-mcpu` flag. Overrides the target's default. |
| `--public-symbols func1,func2` | Comma-separated list of symbols to export and wrap. If omitted, all global symbols are exported. |
| `-O <level>` | Optimization level: `0`, `s` (default, size), `2`, `3`, `z`. |
| `--bin-name <path>` | Custom output path for the `.bin` file. Default is derived from the first source file. |
| `--gen-c-header` | Generate a C header file containing the binary as a `static const unsigned char` array. |
| `--header-path <dir>` | Directory where the generated C header is written. Default: current directory. |
| `--mod-version <ver>` | Module ABI version in `major.minor` format. Default: `1.0`. |
| `--udynlink-version <ver>` | Minimum loader ABI version required. Default: `3.0` (`3.1` for `--section` builds without an explicit version; a lower explicit version together with `--section` is rejected). |
| `--build-flags <flags>` | Extra compiler flags prepended to the compile command. |
| `-I <dir>`, `--include-dir <dir>` | Add a directory to the module compile include path (repeatable). Module sources can then `#include` udynlink headers — e.g. `udynlink_deps_api.h` for `UDYNLINK_REQUIRES` / `UDYNLINK_THUNK_*`, `udynlink_section.h` for `UDYNLINK_SECTION*` — instead of pasting macros inline. Passed after `--build-flags` (so a conflicting `-I` there wins); applies to module sources only. |
| `--module-name <name>` | Explicit module name. Default is derived from the first source file name. |
| `--disasm` | Show disassembly of `.text` after linking. |
| `--section NAME[:align=N][:flags=F1,F2]` | Place tagged code/data into a dedicated host-placed memory region (repeatable). See [Memory Regions and Section Placement](#memory-regions-and-section-placement). Requires `--udynlink-version >= 3.1`; incompatible with `--pc-rel` and `--no-long-calls`. |
| `--pc-rel` | Allow pc-relative addressing. Not compatible with `--section` (data placement could not be resolved at load time). |
| `--no-long-calls` | Do not use `-mlong-calls`. Not compatible with `--section` (direct branches cannot cross placement sections). |
| `--stop-after-compile` | Stop after compiling source files to `.o`. |
| `--stop-after-link` | Stop after linking to `.elf`. |
| `--no-verbose` | Do not print executed commands. |
| `--no-debug` | Do not print debug output. |
| `--no-prologue` | Skip the assembly prologue/wrapper on exported functions. The host must use `UDYNLINK_PREPARE_CALL()` to set `r9` before every call. |
| `--lto` | Enable link-time optimization (GCC `-flto`). See [LTO Mode](#lto-mode). |
| `--workdir <dir>` | Directory for intermediate files (`*.o`, `*.elf`, `*.s`) and the default `.bin` output, keeping the source tree clean. Default: next to the source file. |
| `--strip-hidden-syms` | Demote defined symbols whose ELF visibility is `STV_HIDDEN`/`STV_INTERNAL` to nameless internal entries. Pair with `-fvisibility=hidden -fvisibility-inlines-hidden` in `--build-flags`. |
| `--strip-non-public-syms` | When `--public-symbols` is set, demote every defined symbol not in the list to nameless internal. No-op without `--public-symbols`. |
| `--strip-mangled-syms` | Demote Itanium-mangled (`_Z*`) defined symbols not in `--public-symbols` to nameless internal, and skip their prologue wrapping. |
| `--strip-weak-sym-names` | Demote all defined `STB_WEAK` symbols not in `--public-symbols` to nameless internal. Loses the host-override path for those weak symbols. |

### Environment Variables

| Variable | Description |
|----------|-------------|
| `UDYNLINK_WORKDIR` | Default value for `--workdir`. Set to route intermediate files out of the source tree. |
| `UDYNLINK_CC_PREFIX` | Compiler prefix. Default: `arm-none-eabi-`. |

### LTO Mode

`--lto` enables GCC link-time optimization: sources compile to *fat* LTO
objects (serialized IR plus real code) and the final link runs the LTO
plugin, letting GCC inline and constant-fold across translation units.
For multi-TU modules this typically shrinks the image meaningfully; for
single-TU modules the effect is small.

```bash
python3 mkmodule --lto source1.c source2.c
```

Implementation notes (why the pipeline differs from the default mode):

- binutils refuses `objcopy --redefine-sym` on LTO IR, so exported functions
  are split into wrapper/body via the linker's `--wrap` instead of object-level
  renaming: references to a wrapped function resolve to the `.text_nogc`
  prologue wrapper, which calls the body through `__real_<name>`. Cross-TU
  calls that survive inlining therefore go through the wrapper (identical
  semantics to the default mode).
- The link passes `-Wl,--export-dynamic`. Without it the plugin resolution
  marks module symbols internal and GCC internalizes exported data
  (`D g` becomes local `d g`) or folds reads of weak data away entirely —
  hosts could no longer find or override them.
- `-fno-section-anchors` keeps user-visible names on GOT relocations
  (whole-program data pooling would otherwise fold variables into
  `.LANCHORn` symbols; `UDYNLINK_REQUIRES()` declarations depend on their
  names surviving).
- A post-link `objcopy -W` pass restores `STB_WEAK` on weak symbols: LTO
  re-emits prevailing weak definitions as GLOBAL, which would otherwise
  switch them from the loader's weak (host-overridable) class to exported.

Constraints and caveats:

- **GCC-based toolchains only** (default `arm-none-eabi-` prefix). Clang
  LTO (`-flto=thin`) is not supported by this pipeline.
- Intermediate `.o` files are fat LTO objects (larger than default objects);
  compile time roughly doubles.
- Weak data whose every read GCC can prove constant is folded even with the
  countermeasures above if the module never references it in a non-constant
  way; mkmodule prints a warning if a weak symbol disappears entirely.
  Declare such overrides `volatile` or reference them through a volatile
  pointer, as the loader's weak-override contract is a runtime mechanism the
  compiler cannot see.
- Passing `-flto` via `--build-flags` without `--lto` is rejected: slim LTO
  objects hide function types from the discovery pass, which would silently
  break the wrapper mechanism.

## Building and Distributing Modules

### Output Files

After running `mkmodule`, you get:

- **`<name>.bin`** — the binary module image. This is the standalone artifact you can store in flash, SD card, or stream over a network.
- **`<name>_module_data.h`** — a C header with a byte array (only if `--gen-c-header` is used). Embed this in firmware at compile time if you do not have external storage.

### The C Header

```c
// hello_module_data.h — auto-generated
static const unsigned char hello_module_data[] = {
    0x55, 0x44, 0x4C, 0x4D,  // "UDLM"
    ...
};
```

The host passes `hello_module_data` directly to `udynlink_load_module` as the `base_addr` argument.

### Module Size Estimation and RAM Requirements

The RAM required to load a module depends on the load mode:

| Mode | RAM Used |
|------|----------|
| `UDYNLINK_LOAD_MODE_COPY_ALL` | header + relocations + symbol table + code + data + LOT + bss |
| `UDYNLINK_LOAD_MODE_COPY_TEXT_DATA` | code + data + LOT + bss (header stays at `base_addr`) |
| `UDYNLINK_LOAD_MODE_XIP` | data + LOT + bss only (code executes from flash) |

Use `udynlink_get_ram_requirements()` to compute the exact size before loading.

### Tips for Minimizing Module Size

1. **Use `--public-symbols`** to wrap only the functions the host needs. Every wrapped function adds an assembly prologue (~20 bytes).
2. **Use `static`** for internal functions and variables. They are stripped from the symbol table and do not get wrappers.
3. **Enable dead-code elimination**: the toolchain already links with `--gc-sections`, so unused functions are removed automatically.
4. **Avoid large global buffers** in `.bss` or `.data` if RAM is tight.

## Building Modules with CMake

When your host firmware is built with CMake and you consume udynlink via `add_subdirectory`, `FetchContent`, or `find_package`, you can build a loadable module as a CMake target with a single `udynlink_add_module()` call. This wraps `mkmodule` so you get correct rebuild ordering, generated-header consumption, and clean output in the build tree — no source-tree pollution.

### Signature

```cmake
udynlink_add_module(<name>
  SOURCES <src...>            # one or more C/C++ sources (required)
  [TARGET <target>]          # mkmodule --target (default: cortex-m4)
  [MCPU <cpu>]               # mkmodule --mcpu override
  [PUBLIC_SYMBOLS <a,b,...>] # mkmodule --public-symbols (comma list)
  [OPT_LEVEL <0|s|2|3|z>]    # -O (default: s)
  [MODULE_NAME <name>]       # --module-name (default: <name>)
  [BUILD_FLAGS <flags>]      # --build-flags (extra compiler flags)
  [MOD_VERSION <ver>]        # --mod-version (default: 1.0)
  [UDYNLINK_VERSION <ver>]   # --udynlink-version (default: 3.0)
  [NO_PROLOGUE]              # --no-prologue
  [PC_REL]                   # --pc-rel
  [NO_LONG_CALLS]            # --no-long-calls
  [DISASM]                   # --disasm
  [OUTPUT_DIR <dir>]         # where the .bin is written (default: ${CMAKE_CURRENT_BINARY_DIR})
  [GENERATE_HEADER]          # also emit <name>_module_data.h
  [HEADER_OUTPUT_DIR <dir>]  # header dir (default: OUTPUT_DIR)
  [DEPENDS <dep...>]         # extra build-graph deps (targets or files)
)
```

This creates two CMake targets:

- **`<name>`** — a custom target (part of `ALL`) that produces `${OUTPUT_DIR}/<name>.bin`. Intermediate files (`*.o`, `*.elf`, `*.s`) land under `${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/<name>.mkmodule`, never beside your sources.
- **`udynlink::module::<name>`** — an INTERFACE library. Linking your firmware against it pulls in the module's build ordering and (with `GENERATE_HEADER`) the directory containing `<name>_module_data.h` as an include directory.

The helper passes `-I <udynlink headers>` (`udynlink_INCLUDE_DIR` — the in-tree `udynlink/` for `add_subdirectory`/`FetchContent`, the installed `include/udynlink` for `find_package`) to every `mkmodule` invocation, so module sources can `#include "udynlink_deps_api.h"` directly. Extra include dirs for your own headers go through `BUILD_FLAGS` (e.g. `BUILD_FLAGS "-I${CMAKE_CURRENT_SOURCE_DIR}/include"`).

### Full Example (FetchContent)

```cmake
include(FetchContent)
FetchContent_Declare(udynlink
    GIT_REPOSITORY https://github.com/your-org/udynlink.git
    GIT_TAG        v0.1.0
)
FetchContent_MakeAvailable(udynlink)

# Build the module; generate mod_hello.bin and mod_hello_module_data.h
# in the build tree (no source-tree pollution).
udynlink_add_module(mod_hello
    SOURCES src/mod_hello.c
    TARGET cortex-m4
    GENERATE_HEADER)

add_executable(firmware src/main.c)
target_link_libraries(firmware PRIVATE
    udynlink::module::mod_hello   # build ordering + mod_hello_module_data.h include dir
    udynlink::udynlink)           # core linker runtime
```

The firmware target now `#include "mod_hello_module_data.h"` and pass `mod_hello_module_data` to `udynlink_load_module()`.

> **Rebuild granularity:** `mkmodule` lists your `.c`/`.cpp` sources as build dependencies, but it does not emit GCC `.d` depfiles, so headers `#include`d by a module source are **not** tracked transitively. Editing a header a module consumes does not automatically rebuild the module — `touch` the source or rebuild explicitly. This is a pre-existing `mkmodule` limitation, not a regression introduced by the CMake helper.

## Common Patterns

### Plugin Pattern

The module exports well-known lifecycle functions; the host calls them:

```c
// plugin.c
int plugin_init(void) {
    // Allocate resources, register callbacks
    return 0;
}

int plugin_run(void) {
    // Do work
    return 0;
}

int plugin_stop(void) {
    // Clean up
    return 0;
}
```

```bash
python3 mkmodule --public-symbols plugin_init,plugin_run,plugin_stop plugin.c
```

### Callback Pattern

The module registers callbacks with the host. The host stores the function pointer and invokes it later:

```c
// callback_module.c
extern void host_register_callback(const char *name, void (*cb)(int));

static void on_event(int code) {
    // Handle event
}

int init(void) {
    host_register_callback("sensor", on_event);
    return 0;
}
```

### Driver Pattern

The host defines a driver interface; the module implements it:

```c
// driver_module.c
extern void host_register_driver(const char *name,
    int (*read)(int reg),
    int (*write)(int reg, int val));

static int my_read(int reg) {
    return 0;
}

static int my_write(int reg, int val) {
    return 0;
}

int init(void) {
    host_register_driver("i2c_eeprom", my_read, my_write);
    return 0;
}
```

### Service Pattern

A module provides services consumed by the host or other modules via explicit symbol registration:

```c
// math_service.c
int svc_add(int a, int b) { return a + b; }
int svc_sub(int a, int b) { return a - b; }
```

The host loads the module, looks up the service functions, and passes them to consumers as function pointers. Alternatively, consumers can resolve service symbols at load time via the host's `udynlink_external_resolve_symbol()` callback.

## Common Pitfalls and Troubleshooting

### "Symbol not found" (`UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL`)

- You called a function without declaring it `extern`.
- The host does not implement the symbol in `udynlink_external_resolve_symbol`.
- Check the symbol name carefully — C++ names must be `extern "C"` to avoid mangling.

### "Architecture mismatch" (`UDYNLINK_ERR_LOAD_ARCH_MISMATCH`)

- The module was compiled for a different core or float ABI than the host. For example, a `cortex-m4f` (hard-float) module on a `cortex-m4` (soft-float) host.
- Rebuild with the correct `--target`.

### Crashes After Loading

- **Forgot to prepare the call context.** Before calling any module function, the host must call `UDYNLINK_PREPARE_CALL(&mod)` (or manually set `r9` to `mod.ram_base`).
- **Tried to call a function before `udynlink_load_module` returned.** Only call functions after a successful load.

### Module Works at `-O0` but Not `-Os`

- Likely undefined behavior in your C/C++ code (uninitialized variables, out-of-bounds access, strict aliasing violations).
- Missing `volatile` on memory-mapped hardware access.
- Add `-Wall -Wextra` with `--build-flags` and fix all warnings.

### C++ Constructors Not Running

- The host forgot to call `udynlink_cpp_init(&mod)` after loading. If using the C++ API, `udynlink::Module::load()` does this automatically.
- The call context must be prepared with `UDYNLINK_PREPARE_CALL(&mod)` **before** calling `udynlink_cpp_init`, because constructors may touch module data.

### Taking the Address of an Exported Function

Inside a module, `&my_exported_func` gives you the address of the **wrapper prologue**, not the raw function body. This is usually fine for callbacks, but if you need the raw address (for example, to compute a checksum over the function body), you cannot obtain it from within the module.

---

For host integration details, see the [Host Guide](integrating-as-host.md). For API reference, see [API Reference](api-reference.md). For the internals of relocation and the LOT, see [How It Works](how-it-works.md).
