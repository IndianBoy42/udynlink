# Integrating udynlink as a Host

This guide is for firmware developers who want to integrate the udynlink micro dynamic linker into their ARM Cortex-M project. The **host** is the firmware running on the MCU that uses udynlink to load, relocate, and execute binary modules at runtime.

## Table of Contents

- [Overview](#overview)
- [Integration Checklist](#integration-checklist)
- [Adding udynlink to Your Build](#adding-udynlink-to-your-build)
- [Implementing the External Callbacks](#implementing-the-external-callbacks)
- [Deferred Dependencies and Symbols](#deferred-dependencies-and-symbols)
- [The LOT Base Address Convention](#the-lot-base-address-convention)
- [Building and Using the Host Symbol Table](#building-and-using-the-host-symbol-table)
- [Hash-Based Symbol Resolution](#hash-based-symbol-resolution)
- [Module Lifecycle Management](#module-lifecycle-management)
- [Non-Contiguous Image Loading](#non-contiguous-image-loading)
- [Thread Safety and Concurrency](#thread-safety-and-concurrency)
- [Error Handling and Diagnostics](#error-handling-and-diagnostics)

---

## Overview

In the udynlink architecture, the **host** is the base firmware image running on your Cortex-M MCU. Modules are position-independent binary blobs compiled separately and loaded into RAM (or executed in place from flash) by the host at runtime.

The host has three core responsibilities:

1. **Provide memory** for the module's runtime data (LOT, `.data`, `.bss`) via `udynlink_external_malloc` and `udynlink_external_free`.
2. **Resolve symbols** that the module references but does not define. When a module calls `printf`, the host must provide the address of its own `printf` implementation.
3. **Set up the LOT base** before every call into module code. Module functions use `r9` as a base register to access their Linker Offset Table (LOT). The host must set `r9` to the module's RAM base address before calling any module function.

The host-module contract is simple: the host exposes a set of symbols (functions and variables), and modules consume them. The host can also defer individual symbols at load time and resolve them later using low-level primitives such as `udynlink_link_symbol()`, `udynlink_link_incremental()`, and `udynlink_relink_all()`.

---

## Integration Checklist

Follow this checklist to integrate udynlink into your firmware:

1. **Add libudynlink to your build** — as a CMake subdirectory, an installed package, or vendored source files.
2. **Define compile-time constants** — `UDYNLINK_HOST_ARCH_TAG`.
3. **Implement all external callbacks** — the 5 functions declared in `udynlink_externals.h`.
4. **Set up the LOT base** before calling any module function using `UDYNLINK_PREPARE_CALL()` (`udynlink_cpp_init()` sets it internally, so you only need to re-set it before other module calls).
5. **Build a host symbol table** — decide how your firmware will resolve symbols requested by modules.
6. **Write module loading/unloading code** — call `udynlink_load_module()`, manage handles, and call `udynlink_unload_module()` when done. **Remember to zero-initialize the module handle before the first load.**
7. **(Optional) Set up hash-based symbol resolution** — use `scripts/mkhostsyms` for O(1) lookup when you export many symbols.
8. **(Optional) Implement non-contiguous image loading** — if loading modules from SD card, SPI flash, decompressed buffers, or any source where the image sections are not contiguous in memory.

---

## Adding udynlink to Your Build

### Method 1: `add_subdirectory()` (In-Tree)

Clone the udynlink repository into your project and add it as a subdirectory:

```cmake
# In your top-level CMakeLists.txt
add_subdirectory(third_party/udynlink)

target_link_libraries(your_firmware PRIVATE udynlink)
```

The `udynlink` target is a static library that exposes its headers automatically. You only need to link against it.

### Method 2: `find_package(udynlink)` (Installed)

Build and install udynlink first:

```bash
cd udynlink
cmake -B build -S .
cmake --build build
cmake --install build --prefix /path/to/install
```

Then in your project:

```cmake
list(APPEND CMAKE_PREFIX_PATH /path/to/install)
find_package(udynlink REQUIRED)

target_link_libraries(your_firmware PRIVATE udynlink::udynlink)
```

### Method 3: Vendored (Add Source Files Directly)

For bare-metal projects without CMake, copy these files into your source tree:

- `udynlink/udynlink.c`
- `udynlink/udynlink.h`
- `udynlink/udynlink_externals.h`
- `udynlink/udynlink_hash.h`

Add the `.c` files to your build and ensure the `udynlink/` directory is in your include path.

### Required Compile Definitions

You **must** define this before including `udynlink.h` or compiling `udynlink.c`:

| Definition | Purpose | Valid Values | Default |
|------------|---------|--------------|---------|
| `UDYNLINK_HOST_ARCH_TAG` | Architecture tag of the host MCU | One of the `UDYNLINK_ARCH_TAG_*` constants | `UDYNLINK_ARCH_TAG_CORTEX_M4` |

**`UDYNLINK_HOST_ARCH_TAG`**

This is used at load time to validate that a module was compiled for a compatible CPU and float ABI. The architecture tag is a `uint16_t` encoded as:

- Bits `[3:0]` — core family ID
- Bit `4` — FPU present
- Bits `[6:5]` — float ABI (`00`=soft, `01`=softfp, `10`=hard)

Available constants:

```c
UDYNLINK_ARCH_TAG_CORTEX_M0      // 0x01
UDYNLINK_ARCH_TAG_CORTEX_M0PLUS  // 0x02
UDYNLINK_ARCH_TAG_CORTEX_M3      // 0x03
UDYNLINK_ARCH_TAG_CORTEX_M4      // 0x04
UDYNLINK_ARCH_TAG_CORTEX_M4F     // 0x54 (M4 + FPU + hard-float)
UDYNLINK_ARCH_TAG_CORTEX_M7      // 0x57
UDYNLINK_ARCH_TAG_CORTEX_M33     // 0x08
UDYNLINK_ARCH_TAG_CORTEX_M55     // 0x59
UDYNLINK_ARCH_TAG_CORTEX_M85     // 0x5A
```

**Pitfall:** If your firmware runs on a Cortex-M4 without FPU and you try to load a module compiled for `cortex-m4f` (hard-float), the loader will reject it with `UDYNLINK_ERR_LOAD_ARCH_MISMATCH`. Conversely, a soft-float module can run on a hard-float host.

### Complete CMake Snippet

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_firmware C)

# Set the ARM cross compiler
set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_C_COMPILER_WORKS TRUE)

add_executable(my_firmware
    src/main.c
    src/syscalls.c
    src/host_symbols.c
)

target_include_directories(my_firmware PRIVATE
    src
    udynlink  # if vendored
)

# Compile definitions for udynlink
target_compile_definitions(my_firmware PRIVATE
    UDYNLINK_HOST_ARCH_TAG=UDYNLINK_ARCH_TAG_CORTEX_M4F
)

# Method 1: add_subdirectory
target_link_libraries(my_firmware PRIVATE udynlink)

# Or Method 3: add sources directly
target_sources(my_firmware PRIVATE
    udynlink/udynlink.c
)
```

### PlatformIO Note

A `library.json` exists in the repository, but the version field may be outdated. For PlatformIO projects, the most reliable approach is to vendor the source files (Method 3) and add them to `src_filter` or `build_src_filter`.

---

## Implementing the External Callbacks

These 8 functions form the integration contract between udynlink and your firmware. They are declared in `udynlink/udynlink_externals.h` and **must** be defined by your project. The linker will fail if any are missing.

### a. `udynlink_external_is_pointer_in_ram`

```c
int udynlink_external_is_pointer_in_ram(const void *p);
```

**When it is called:** Currently declared as part of the host contract, but the core udynlink library does not invoke this function in the current release. It is reserved for future XIP mode validation.

**What it must do:** Return a non-zero value if `p` points into RAM, and 0 if it points into flash or any other memory region.

**Minimal implementation:**

```c
// Example for an STM32F4 with 128 KB SRAM at 0x20000000
int udynlink_external_is_pointer_in_ram(const void *p) {
    uint32_t addr = (uint32_t)(uintptr_t)p;
    return (addr >= 0x20000000 && addr < 0x20020000);
}
```

**Common pitfalls:**
- Do not return a constant `1`. In XIP mode, the loader needs to distinguish flash pointers from RAM pointers.
- Account for all RAM regions if your MCU has multiple SRAM banks (e.g., DTCM, SRAM1, SRAM2 on an STM32H7).

### b. `udynlink_external_malloc`

```c
void *udynlink_external_malloc(size_t size);
```

**When it is called:** During `udynlink_load_module()` and `udynlink_load_module_image()` when `load_addr` is `NULL` (auto-allocation mode).

**What it must do:** Allocate `size` bytes of RAM and return a pointer to it. The memory must be writable and readable by the module.

**Minimal implementation:**

```c
// Using your firmware's heap allocator
void *udynlink_external_malloc(size_t size) {
    return my_heap_alloc(size);
}
```

**Common pitfalls:**
- Do not use `malloc()` from the standard library unless your firmware actually links newlib with a working `sbrk`.
- Return `NULL` on failure; the loader will translate this into `UDYNLINK_ERR_LOAD_OUT_OF_MEMORY`.
- The allocated memory may need to be word-aligned. Most heap allocators return aligned pointers, but if yours does not, ensure 4-byte alignment.

### c. `udynlink_external_free`

```c
void udynlink_external_free(void *p);
```

**When it is called:** During `udynlink_unload_module()` to free RAM that was auto-allocated by `udynlink_external_malloc`. Also called during error cleanup in `udynlink_load_module()` if a mid-load failure occurs.

**What it must do:** Free the memory block previously returned by `udynlink_external_malloc`.

**Minimal implementation:**

```c
void udynlink_external_free(void *p) {
    my_heap_free(p);
}
```

**Common pitfalls:**
- Must handle `NULL` gracefully (like standard `free`).
- If the host provided `load_addr` (foreign RAM), `udynlink_unload_module()` will **not** call `free` for that module. It only frees auto-allocated memory.

### d. `udynlink_external_vprintf`

```c
void udynlink_external_vprintf(const char *s, va_list va);
```

**When it is called:** Whenever udynlink emits debug output, depending on the configured debug level. Called from `udynlink_debug()` inside `udynlink.c`.

**What it must do:** Format and output the string `s` with the variadic arguments in `va`. This is your debug logging hook.

**Minimal implementation:**

```c
#include <stdarg.h>
#include <stdio.h>

void udynlink_external_vprintf(const char *s, va_list va) {
    vprintf(s, va);
}
```

For bare-metal firmware without stdio:

```c
#include <stdarg.h>
#include <string.h>

static char log_buf[128];

void udynlink_external_vprintf(const char *s, va_list va) {
    vsnprintf(log_buf, sizeof(log_buf), s, va);
    uart_send_string(log_buf);  // Your UART output function
}
```

**Common pitfalls:**
- This function is called from within the loader. Keep it short and do not allocate memory here.
- If you set the debug level to `UDYNLINK_DEBUG_NONE`, this function is never called.

### e. `udynlink_external_resolve_symbol`

```c
uint32_t udynlink_external_resolve_symbol(const char *name);
```

**When it is called:** During relocation in `udynlink_load_module()` and `udynlink_load_module_image()`, for every `UDYNLINK_SYM_TYPE_EXTERN` symbol.

**What it must do:** Look up `name` in the host firmware's exported API and return the 32-bit address of the symbol. Return `0` if the symbol is not found. Return `UDYNLINK_SYM_DEFERRED` ((uint32_t)1) if the symbol is known but should not be resolved yet — the loader will write `0` to the relocation slot and continue loading.

**Minimal implementation (strcmp chain):**

```c
#include <string.h>
#include <stdint.h>

extern int my_printf(const char *fmt, ...);
extern void my_delay_ms(uint32_t ms);
extern uint32_t system_ticks;

uint32_t udynlink_external_resolve_symbol(const char *name) {
    if (!strcmp(name, "printf"))
        return (uint32_t)(uintptr_t)&my_printf;
    else if (!strcmp(name, "delay_ms"))
        return (uint32_t)(uintptr_t)&my_delay_ms;
    else if (!strcmp(name, "system_ticks"))
        return (uint32_t)(uintptr_t)&system_ticks;
    else
        return 0;
}
```

**Common pitfalls:**
- Return exactly `0` for unresolved symbols. The loader treats any non-zero value as success, **except** `UDYNLINK_SYM_DEFERRED`.
- The returned address is written directly into the module's LOT or data section. Ensure it is the correct runtime address.

---

## Deferred Symbols and Low-Level Linking

ABI v3.0 removes the built-in dependency system but retains low-level primitives for advanced use cases such as deferred symbol resolution, hot-patching, and dynamic symbol tables.

### `UDYNLINK_SYM_DEFERRED`

```c
#define UDYNLINK_SYM_DEFERRED ((uint32_t)1)
```

This sentinel is returned by `udynlink_external_resolve_symbol()`. Address `0x00000001` is not a valid code or data address on Cortex-M.

When the resolver returns this value:
- The loader writes `0` to the relocation slot.
- Loading **continues** (does not fail).
- The module can check `if (func_ptr != NULL)` at runtime.

Use cases:
- **Late-bound host symbols** — a module references a host function that will be registered after hardware initialization.
- **Optional features** — the module is designed to handle a NULL function pointer gracefully.

### Direct Symbol Patching: `udynlink_link_symbol()`

For host-mediated symbol injection without re-running the resolver:

```c
udynlink_error_t udynlink_link_symbol(udynlink_module_t *mod,
                                      const char *sym_name,
                                      uint32_t sym_addr);
```

This scans the module's relocation table for entries referencing `sym_name` and overwrites the slot directly with `sym_addr`. It does not update the symbol table. Use cases:
1. **Deferred host symbols** — the host knows the address now and wants to patch it directly.
2. **Hot-patching** — replace a module's extern reference with a different implementation at runtime (e.g., a mock for testing).
3. **Dynamic symbol tables** — the host maintains its own symbol table and pushes updates into loaded modules.

### Incremental and Full Relinking: `udynlink_link_incremental()` and `udynlink_relink_all()`

These functions re-resolve `EXTERN` relocation slots for a module that has already been loaded. They are useful when the host's symbol table changes after load time (e.g., new services are registered).

```c
udynlink_error_t udynlink_link_incremental(udynlink_module_t *p_mod);
udynlink_error_t udynlink_relink_all(udynlink_module_t *p_mod);
```

- `udynlink_link_incremental()` — only touches slots that are currently `0` (previously deferred). Fast, does not overwrite already-resolved symbols.
- `udynlink_relink_all()` — re-resolves every `EXTERN` slot from scratch using the current host resolver. Slower, but ensures the module picks up newly registered host symbols.

### Query Helper: `udynlink_is_symbol_resolved()`

```c
int udynlink_is_symbol_resolved(const udynlink_module_t *p_mod,
                                const char *sym_name);
```

Returns `1` if the symbol exists in the module's relocation table and has a non-zero value, `0` otherwise. A symbol that was deferred during load and has not yet been patched or relinked resolves to `0`.

---

## The LOT Base Address Convention

The Linker Offset Table (LOT) is a per-module array of pointers that allows position-independent code to access global data and external functions. In ABI v3.0, the host sets `r9` directly to the module's RAM base before every call. There is no fixed memory address and no global word.

### The Critical Pattern

```c
UDYNLINK_PREPARE_CALL(p_mod);
```

This macro expands to inline assembly that moves `p_mod->ram_base` into `r9`. It must be used **before every call to a module function**, including indirect calls through function pointers obtained from the module.

### Using the C Call Layer for Automatic Save/Restore

For ergonomics and safety, include `udynlink/udynlink_call.h` and use the provided macros. They save the caller's `r9`, set it to the module's base, call the function, and restore the original `r9`:

```c
#include "udynlink.h"
#include "udynlink_call.h"

udynlink_func_t h;
udynlink_error_t err = udynlink_resolve_func(p_mod, "run", &h);
if (err == UDYNLINK_OK) {
    int result = UDYNLINK_CALL(&h, int, (42));
}
```

`UDYNLINK_CALL` handles both prologued and `--no-prologue` modules because it always manages `r9` explicitly. For one-shot calls without creating a reusable handle:

```c
int result;
udynlink_error_t err = UDYNLINK_CALL_MODULE_FUNC(p_mod, "run", int, (42), &result);
```

### For C++ Modules

If you load a C++ module, call `udynlink_cpp_init(p_mod)` **after** loading and **before** calling any module functions. The `udynlink_cpp_init()` function itself sets `r9` internally before invoking `__init_array`, but you must set it again before calling any other module function afterward.

```c
udynlink_error_t err = udynlink_load_module(&mod, module_blob, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
if (err != UDYNLINK_OK) { /* handle error */ }

// Run C++ global constructors
udynlink_cpp_init(&mod);

// Set r9 before calling module functions
UDYNLINK_PREPARE_CALL(&mod);

int (*p_func)(void) = (int (*)(void))udynlink_get_symbol_value(&mod, "run");
int result = p_func();
```

**Pitfall:** If you forget to set `r9` before a module call, the function will use whatever value `r9` currently holds (usually the caller's or a previous module's base), causing it to read/write the wrong memory region. This typically results in a hard fault or silent data corruption.

### Calling Modules Built With `--no-prologue`

By default, every exported function gets a small assembly prologue that saves and restores the caller's `r9` around the real function body. When building micro-modules with a single export, this prologue is pure overhead. The `mkmodule` tool supports `--no-prologue` to skip it, and the module header advertises this via the `UDYNLINK_ARCH_FLAG_NO_PROLOGUE` bit in `arch_tag`.

For modules built with `--no-prologue`, the host must set `r9` directly because there is no wrapper to manage it. `UDYNLINK_PREPARE_CALL()` is sufficient, but `UDYNLINK_CALL()` is safer because it also saves and restores the caller's `r9` around the call:

```c
#include "udynlink.h"
#include "udynlink_call.h"

void call_module_func(udynlink_module_t *p_mod) {
    udynlink_func_t h;
    if (udynlink_resolve_func(p_mod, "run", &h) == UDYNLINK_OK) {
        int result = UDYNLINK_CALL(&h, int, ());
    }
}
```

**When to use `--no-prologue`:**

- Single-export micro-modules where the ~12-byte prologue is a significant fraction of total size.
- When the host is willing to manage `r9` directly via `UDYNLINK_PREPARE_CALL()` or `UDYNLINK_CALL()`.

**When NOT to use `--no-prologue`:**

- Modules with many exported functions (the per-function overhead is amortized).
- When the host code base is large and cannot easily adopt `UDYNLINK_PREPARE_CALL()` everywhere.

---

## Building and Using the Host Symbol Table

When a module references an external symbol (e.g., `printf`, `memcpy`, a custom host API), the loader must bind it to an actual address. There are three common patterns for doing this.

### Pattern 1: Simple `switch` / `strcmp` Chain

Best for: firmware with a small, stable API (fewer than ~20 symbols).

```c
#include <string.h>
#include <stdint.h>

extern void my_printf(const char *fmt, ...);
extern void my_delay_ms(uint32_t ms);
extern void *my_memcpy(void *dst, const void *src, size_t n);

uint32_t udynlink_external_resolve_symbol(const char *name) {
    if (!strcmp(name, "printf"))
        return (uint32_t)(uintptr_t)&my_printf;
    if (!strcmp(name, "delay_ms"))
        return (uint32_t)(uintptr_t)&my_delay_ms;
    if (!strcmp(name, "memcpy"))
        return (uint32_t)(uintptr_t)&my_memcpy;
    return 0;
}
```

**Tradeoff:** Simple to write and read, but lookup time is O(n) and the code grows with each symbol.

### Pattern 2: Static Array with Linear Search

Best for: firmware with a medium API (20-50 symbols) where you want the table to be data-driven.

```c
#include <string.h>
#include <stdint.h>
#include "udynlink_externals.h"

extern void my_printf(const char *fmt, ...);
extern void my_delay_ms(uint32_t ms);

static const struct {
    const char *name;
    void *addr;
} host_symbols[] = {
    UDYNLINK_SYMBOL(my_printf),
    UDYNLINK_SYMBOL(my_delay_ms),
    // Add more symbols here
    { NULL, NULL }
};

uint32_t udynlink_external_resolve_symbol(const char *name) {
    for (int i = 0; host_symbols[i].name != NULL; i++) {
        if (!strcmp(host_symbols[i].name, name))
            return (uint32_t)(uintptr_t)host_symbols[i].addr;
    }
    return 0;
}
```

The `UDYNLINK_SYMBOL(sym)` macro expands to `{ #sym, (void *)(uintptr_t)(sym) }`, saving you from typing symbol names twice.

**Tradeoff:** Still O(n), but the symbol list is easier to maintain as a table. Slightly more RAM usage for the string table.

### Pattern 3: Hash-Based Resolution

See [Hash-Based Symbol Resolution](#hash-based-symbol-resolution) below. Best for: firmware with a large API (50+ symbols).

### Pattern 4: Host-Side Symbol Cache

Best for: firmware loading many modules that resolve the same symbols, where you want to avoid repeated lookups without adding state to the loader core.

```c
#include "udynlink.h"
#include "udynlink_host_utils.h"

static udynlink_host_sym_cache_entry_t g_sym_cache[UDYNLINK_HOST_SYM_CACHE_SIZE];

static uintptr_t my_real_resolver(const char *name) {
    // Your existing resolution logic (strcmp chain, hash table, etc.)
    if (!strcmp(name, "printf"))
        return (uintptr_t)&my_printf;
    if (!strcmp(name, "delay_ms"))
        return (uintptr_t)&my_delay_ms;
    return 0;
}

uint32_t udynlink_external_resolve_symbol(const char *name) {
    return (uint32_t)udynlink_host_sym_cache_lookup(
        g_sym_cache, UDYNLINK_HOST_SYM_CACHE_SIZE, name, my_real_resolver);
}
```

**Tradeoff:** Zero overhead if unused. The cache is a simple direct-mapped table (default 16 entries) owned by the host. Collisions silently overwrite. Call `udynlink_host_sym_cache_invalidate()` if your symbol table changes at runtime.

---

## Hash-Based Symbol Resolution

If your firmware exports many symbols, linear search becomes slow. The `scripts/mkhostsyms` tool generates a **precomputed GNU hash table** with a Bloom filter, giving O(1) average-case lookup time.

### When to Use

- Your firmware exports more than ~50 symbols.
- You want the fastest possible module load time.
- You are willing to regenerate the hash table whenever your firmware ELF changes.

### How to Generate

Run `mkhostsyms` against your compiled host firmware ELF:

```bash
python3 scripts/mkhostsyms --elf build/my_firmware.elf --output src/host_syms.h
```

Optional flags:
- `--filter '<regex>'` — only include symbols matching the regex (e.g., `--filter '^my_api_'`)
- `--nbuckets <n>` — override the number of hash buckets
- `--bloom-size <n>` — override the Bloom filter size

The generated `host_syms.h` contains:
- Static arrays for the Bloom filter, buckets, hash values, symbol addresses, and string table.
- A pre-initialized `udynlink_hash_table_t g_host_sym_table` struct.

### How to Use at Runtime

Include the generated header and use `udynlink_resolve_hashed_symbol()`:

```c
#include "udynlink.h"
#include "udynlink_hash.h"
#include "host_syms.h"

uint32_t udynlink_external_resolve_symbol(const char *name) {
    void *addr = udynlink_resolve_hashed_symbol(&g_host_sym_table, name);
    if (addr != NULL)
        return (uint32_t)(uintptr_t)addr;

    // Fallback: try a small set of symbols not in the ELF (e.g. dynamically registered)
    if (!strcmp(name, "runtime_registered_func"))
        return (uint32_t)(uintptr_t)&runtime_registered_func;

    return 0;
}
```

### Complete Example

**Step 1:** Generate the header as part of your build:

```bash
# In your Makefile or build script
python3 $(UDYNLINK_DIR)/scripts/mkhostsyms \
    --elf $(BUILD_DIR)/my_firmware.elf \
    --output $(SRC_DIR)/host_syms.h
```

**Step 2:** Include and use it:

```c
// host_symbols.c
#include "udynlink.h"
#include "udynlink_hash.h"
#include "host_syms.h"
#include <string.h>

uint32_t udynlink_external_resolve_symbol(const char *name) {
    void *addr = udynlink_resolve_hashed_symbol(&g_host_sym_table, name);
    return addr ? (uint32_t)(uintptr_t)addr : 0;
}
```

**Performance tradeoffs:**

| Pattern | Lookup Time | RAM Overhead | Maintenance |
|---------|-------------|--------------|-------------|
| `strcmp` chain | O(n) | Minimal | Manual |
| Static array | O(n) | String table | Semi-manual |
| GNU hash table | O(1) avg | Bloom filter + buckets | Regenerate on every ELF change |

---

## Module Lifecycle Management

### Loading a Module

Before loading, you **must** zero-initialize the module handle. The loader does not guard against garbage values in `p_mod->p_ram` during error-path cleanup; an uninitialized handle can cause `udynlink_external_free()` to be called with a garbage pointer.

```c
udynlink_module_t mod;
memset(&mod, 0, sizeof(mod));   // REQUIRED before first load

udynlink_error_t err = udynlink_load_module(
    &mod,              // module handle (allocated by host)
    module_blob,       // pointer to module binary in flash or RAM
    NULL,              // load_addr: NULL = auto-allocate RAM
    0,                 // load_size: ignored when load_addr is NULL
    UDYNLINK_LOAD_MODE_COPY_ALL
);

if (err != UDYNLINK_OK) {
    const char *msg = udynlink_error_msg(&err);
    my_log("Failed to load module: %s", msg);
    return;
}

// Register in your module table so dependency resolution works
host_register_module(&mod);
```

### For C++ Modules

```c
// After successful load, run global constructors
udynlink_cpp_init(&mod);
```

### Calling Module Functions

```c
// Set r9 before EVERY call
UDYNLINK_PREPARE_CALL(&mod);

// Look up and call a function
udynlink_sym_t sym;
if (udynlink_lookup_symbol(&mod, "process_data", &sym) != NULL) {
    int (*process)(int) = (int (*)(int))(uintptr_t)sym.val;
    int result = process(42);
}
```

Or use the C call layer for automatic save/restore:

```c
#include "udynlink_call.h"

udynlink_func_t h;
if (udynlink_resolve_func(&mod, "process_data", &h) == UDYNLINK_OK) {
    int result = UDYNLINK_CALL(&h, int, (42));
}
```

### Unloading a Module

```c
// Unregister from your module table first
host_unregister_module(&mod);

udynlink_error_t err = udynlink_unload_module(&mod);
if (err != UDYNLINK_OK) {
    const char *msg = udynlink_error_msg(&err);
    my_log("Failed to unload module: %s", msg);
}
```

`udynlink_unload_module()` will:
- Free the auto-allocated RAM.
- Zero out the module handle.

### Load Modes

| Mode | Behavior | RAM Needed | Use Case |
|------|----------|------------|----------|
| `UDYNLINK_LOAD_MODE_COPY_ALL` | Copy header, code, and data to RAM | Largest | Module blob is in a temporary buffer |
| `UDYNLINK_LOAD_MODE_COPY_TEXT_DATA` | Copy text and data sections to RAM; header stays at `base_addr` | Medium | Module is in flash, but code must run from RAM |
| `UDYNLINK_LOAD_MODE_XIP` | Copy only data to RAM; execute code from flash | Smallest | Module is in flash and supports XIP |

**Pitfall:** XIP mode requires that the module's code section is in an execute-in-place capable region (typically internal flash). The host must also ensure the flash is memory-mapped and readable at the `base_addr`.

---

## Non-Contiguous Image Loading

Use non-contiguous image loading when modules are stored on media that is not memory-mapped, such as SD cards, SPI flash, decompressed buffers, or encrypted storage. The host assembles a `udynlink_module_image_t` descriptor with pointers to each section independently, then calls `udynlink_load_module_image()` which copies sections into RAM and applies relocations through the same canonical path as `udynlink_load_module()`.

### The `udynlink_module_image_t` Descriptor

```c
typedef struct {
    const udynlink_module_header_t *p_header;       // module header
    const uint32_t                *p_relocations; // relocation table
    const uint32_t                *p_symtab;        // symbol table base
    const uint8_t                 *p_code;          // .text section
    const uint8_t                 *p_data;          // .data section
} udynlink_module_image_t;
```

For a standard contiguous UDLM buffer, use the builder:

```c
udynlink_module_image_t image;
udynlink_image_from_memory(base_addr, &image);
```

### Loading from an SD Card (FatFS-style)

This example reads a module from an SD card into a single contiguous RAM buffer, then loads it via `udynlink_load_module()`. This is the simplest and most common pattern:

```c
#include "udynlink.h"
#include "ff.h"  // FatFS

udynlink_error_t load_module_from_sd(const char *path, udynlink_module_t *p_mod) {
    FIL fil;
    FRESULT res = f_open(&fil, path, FA_READ);
    if (res != FR_OK) return UDYNLINK_ERR_LOAD_IO_ERROR;

    // Get file size
    size_t file_size = f_size(&fil);

    // Allocate a buffer for the entire module image
    uint8_t *image_buf = (uint8_t *)udynlink_external_malloc(file_size);
    if (!image_buf) {
        f_close(&fil);
        return UDYNLINK_ERR_LOAD_OUT_OF_MEMORY;
    }

    UINT br;
    res = f_read(&fil, image_buf, file_size, &br);
    f_close(&fil);
    if (res != FR_OK || br != file_size) {
        udynlink_external_free(image_buf);
        return UDYNLINK_ERR_LOAD_IO_ERROR;
    }

    // Load from the contiguous buffer
    udynlink_error_t err = udynlink_load_module(
        p_mod, image_buf,
        NULL, 0,                    // Auto-allocate module RAM
        UDYNLINK_LOAD_MODE_COPY_ALL);

    if (err != UDYNLINK_OK) {
        udynlink_external_free(image_buf);
    }
    // image_buf can be freed after load if COPY_ALL was used
    return err;
}
```

### Custom Pipeline: Validate, Allocate, Copy, Relocate

For advanced use cases where you want to control every step (e.g., chunked I/O, memory pools, or section-at-a-time copying), use the low-level primitives directly:

```c
udynlink_error_t load_module_custom(const void *header_addr,
                                     const uint32_t *relocs,
                                     const uint32_t *symtab,
                                     const uint8_t *code,
                                     const uint8_t *data,
                                     size_t data_size,
                                     size_t bss_size,
                                     udynlink_module_t *p_mod) {
    // 1. Validate header
    const udynlink_module_header_t *hdr = header_addr;
    udynlink_error_t err = udynlink_validate_header(hdr);
    if (err != UDYNLINK_OK) return err;

    // 2. Compute RAM size and allocate
    size_t ram_size = udynlink_compute_ram_size(hdr, UDYNLINK_LOAD_MODE_COPY_TEXT_DATA);
    void *ram = udynlink_external_malloc(ram_size);
    if (!ram) return UDYNLINK_ERR_LOAD_OUT_OF_MEMORY;

    // 3. Set up module handle
    memset(p_mod, 0, sizeof(*p_mod));
    p_mod->p_header = hdr;
    p_mod->p_ram = ram;

    // 4. Copy code and data into RAM (skip LOT area at start)
    uint8_t *ram_code = (uint8_t *)ram + hdr->num_lot * sizeof(uint32_t);
    memcpy(ram_code, code, hdr->code_size);
    memcpy(ram_code + hdr->code_size, data, data_size);

    // 5. Zero BSS
    memset(ram_code + hdr->code_size + data_size, 0, bss_size);

    // 6. Apply relocations
    err = udynlink_load_apply_relocations(p_mod, hdr, relocs, symtab);
    if (err != UDYNLINK_OK) {
        udynlink_external_free(ram);
        memset(p_mod, 0, sizeof(*p_mod));
        return err;
    }

    return UDYNLINK_OK;
}
```

### Planning APIs

Before loading, you can inspect a module image without allocating RAM:

```c
// Read header from SD card (first 32 bytes)
udynlink_module_header_t header;
UINT br;
f_read(&fil, &header, sizeof(header), &br);

// Validate
udynlink_error_t err = udynlink_validate_header(&header);
if (err != UDYNLINK_OK) {
    printf("Invalid module\n");
}

// Compute RAM needed
size_t ram = udynlink_compute_ram_size(&header, UDYNLINK_LOAD_MODE_COPY_ALL);
printf("Module needs %zu bytes of RAM\n", ram);

// Get metadata size (header + relocs + symtab)
size_t meta = udynlink_get_image_metadata_size(&header);
printf("Metadata size: %zu bytes\n", meta);
```

### Supported Modes

All three load modes are supported via `udynlink_load_module_image()`:
- **COPY_ALL** — copies everything into a single contiguous RAM buffer.
- **COPY_TEXT_DATA** — copies code and data into RAM; metadata stays at the source pointers.
- **XIP** — copies only data to RAM; code stays at `image->p_code` (must be in executable flash).

For `COPY_TEXT_DATA` and `XIP`, the metadata (header, relocation table, symbol table) must remain accessible for post-load symbol lookups. If your source layout scatters these across different buffers, use `COPY_ALL`.

---

## Thread Safety and Concurrency

### No Internal Synchronization

udynlink performs **no thread safety checks or synchronization itself**. There are no locks, atomics, or interrupt-disabling wrappers inside the library. This is intentional: every byte of overhead matters on Cortex-M targets, and many simple use cases never need concurrency.

The following shared state is **not protected**:

| State | Where | Risk |
|-------|-------|------|
| `p_mod` fields (`p_header`, `p_ram`, `info`, `user_ctx`) | Written during load; read during symbol lookup and unload | A partially-initialized handle visible to another context will cause hard faults |
| `debug_level` static variable | Written by `udynlink_set_debug_level` from any context | Benign in practice (eventual consistency), but technically a data race |
| Host module registry (`g_loaded_modules` etc.) | Managed by host code | The host's own registry is equally unprotected; concurrent lookups while a module is being registered may find a partially-inserted entry |

### When You Need Protection

You need to add synchronization if **any** of these are true:

1. **Loading or unloading modules from an interrupt service routine (ISR).** On Cortex-M, a main-thread load can be preempted mid-way by an ISR that also calls `udynlink_load_module`. The ISR will see a half-initialized `p_mod`.

2. **Using an RTOS with multiple threads** that call `udynlink_load_module` or `udynlink_unload_module` concurrently. Even on a single-core MCU, preemptive context switches create the same window as ISR preemption.

3. **Calling `udynlink_set_debug_level` concurrently** with load/unload from a different priority level. In practice this is low-risk but technically a data race.

You do **not** need additional synchronization if:
- All load/unload operations happen from a single context (e.g., only from the main loop or only from a single RTOS task).
- Module loading is always complete before an ISR that uses those modules fires.
- You never load or unload modules inside an ISR.

### How to Add Thread Safety

#### Bare-Metal (Interrupt-Disable Wrapper)

The simplest approach on bare-metal Cortex-M: disable interrupts around the critical section.

```c
#include "udynlink.h"

// Wrap load/unload calls with interrupt protection
udynlink_error_t safe_load_module(udynlink_module_t *p_mod,
                                   const void *base_addr,
                                   void *load_addr,
                                   uint32_t load_size,
                                   udynlink_load_mode_t load_mode) {
    __disable_irq();
    udynlink_error_t err = udynlink_load_module(p_mod, base_addr,
                                                  load_addr, load_size, load_mode);
    __enable_irq();
    return err;
}

udynlink_error_t safe_unload_module(udynlink_module_t *p_mod) {
    __disable_irq();
    udynlink_error_t err = udynlink_unload_module(p_mod);
    __enable_irq();
    return err;
}
```

**Important:** `__disable_irq()` / `__enable_irq()` are CMSIS intrinsics. If you use ARM CC, they are available directly. For GCC, they are provided by `<cmsis_gcc.h>` or you can use inline assembly:

```c
static inline void __disable_irq(void) {
    __asm volatile ("cpsid i" ::: "memory");
}
static inline void __enable_irq(void) {
    __asm volatile ("cpsie i" ::: "memory");
}
```

**Caveat:** `udynlink_load_module` can take a non-trivial amount of time (it calls `udynlink_external_malloc`, iterates relocations, resolves symbols). Disabling interrupts for the entire duration may violate real-time deadlines. If this is a concern, consider the RTOS approach below or restrict module loading to an idle/task context.

#### RTOS (Mutex or Critical Section)

If you use an RTOS (FreeRTOS, Zephyr, etc.), protect load/unload with a mutex or a task-level critical section:

```c
// FreeRTOS example
#include "udynlink.h"
#include "FreeRTOS.h"
#include "semphr.h"

static SemaphoreHandle_t udynlink_mutex;

void udynlink_init(void) {
    udynlink_mutex = xSemaphoreCreateMutex();
}

udynlink_error_t safe_load_module(udynlink_module_t *p_mod,
                                   const void *base_addr,
                                   void *load_addr,
                                   uint32_t load_size,
                                   udynlink_load_mode_t load_mode) {
    xSemaphoreTake(udynlink_mutex, portMAX_DELAY);
    udynlink_error_t err = udynlink_load_module(p_mod, base_addr,
                                                  load_addr, load_size, load_mode);
    xSemaphoreGive(udynlink_mutex);
    return err;
}
```

**Note:** If any of your external callbacks (`udynlink_external_malloc`, `udynlink_external_resolve_symbol`, etc.) are also accessed from ISR context, you must also protect those. A mutex cannot be taken from ISR context; use a counting semaphore or `taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()` instead.

### The LOT Base (`r9`) and Concurrency

In ABI v3.0, `r9` is a register, not a memory word. This changes the concurrency story significantly:

- **r9 is a callee-saved register.** On Cortex-M, exception entry automatically saves r0–r3, r12, lr, pc, and xPSR. The compiler-generated ISR prologue pushes any additional callee-saved registers it uses (including r9 if needed). When the ISR returns, r9 is restored to its pre-exception value.
- **Module wrappers save and restore r9.** Every exported function wrapper does `push {r9, lr}` / `pop {r9, pc}` (or the v6-m equivalent), so r9 is correctly preserved across each module boundary crossing.

This means an ISR can safely call module B even while module A was preempted mid-execution. When the ISR returns, module A resumes with the correct r9 value.

**The concern is now the caller, not a shared memory word.** If the host uses `UDYNLINK_PREPARE_CALL()` (which sets `r9` directly) and then gets preempted before the module function is called, the ISR will see the modified `r9`. When the ISR returns, `r9` is restored to the value saved by the ISR prologue, **not** to the host's pre-`UDYNLINK_PREPARE_CALL` value. For this reason, `UDYNLINK_CALL()` (which saves `r9` in a local variable around the call) is safer than raw `UDYNLINK_PREPARE_CALL()` in reentrant contexts.

Guidelines:

- **For sequential (non-reentrant) use:** `UDYNLINK_PREPARE_CALL()` followed by the function call is sufficient.
- **For reentrant use (e.g., a host callback invoked by module A calls into module B):** Use `UDYNLINK_CALL()` or manually save/restore `r9` around the call. This guarantees that when B returns, the caller's original `r9` is restored regardless of any preemption that occurred during the call.
- **If module A must call its own wrappered entry point** after an ISR has modified `r9`: Re-set `r9` to A's base via `UDYNLINK_PREPARE_CALL(&mod_a)` before the call. Since module code normally calls internal (unwrappered) functions, this scenario is uncommon.

---

## Error Handling and Diagnostics

### Error Code Reference

| Error Code | Meaning | Recommended Host Action |
|------------|---------|------------------------|
| `UDYNLINK_OK` | Success | Continue normal operation |
| `UDYNLINK_ERR_LOAD_INVALID_SIGN` | Module signature does not match `UDLM` | Reject the module; it is either corrupted or not a udynlink module |
| `UDYNLINK_ERR_LOAD_RAM_LEN_LOW` | Provided `load_size` is smaller than required RAM | Increase the allocated RAM region or use auto-allocation (`load_addr = NULL`) |
| `UDYNLINK_ERR_LOAD_OUT_OF_MEMORY` | `udynlink_external_malloc` returned `NULL` | Free other modules or increase heap size |
| `UDYNLINK_ERR_LOAD_XIP_UNSUPPORTED` | XIP is not supported for this load configuration | Use `COPY_ALL` or `COPY_TEXT_DATA` instead |
| `UDYNLINK_ERR_LOAD_INVALID_MODE` | Unknown load mode value | Check that you are passing a valid `udynlink_load_mode_t` |
| `UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE` | Relocation data is malformed or points to an invalid symbol | The module is corrupted or was built with a buggy toolchain |
| `UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL` | An extern symbol could not be resolved by the host | Ensure the symbol is exported by the host via `udynlink_external_resolve_symbol` |
| `UDYNLINK_ERR_LOAD_DUPLICATE_NAME` | A module with the same name is already loaded | The eh2k fork allows multiple instances; this error may not trigger in current builds |
| `UDYNLINK_ERR_LOAD_VERSION_MISMATCH` | Module ABI version exceeds loader ABI version | Update the host's udynlink library or rebuild the module with `--udynlink-version` matching the host |
| `UDYNLINK_ERR_LOAD_ARCH_MISMATCH` | Module CPU/float ABI is incompatible with host | Rebuild the module with the correct `--target` or `--mcpu` |
| `UDYNLINK_ERR_LOAD_IO_ERROR` | Reserved (legacy streaming I/O error) | — |
| `UDYNLINK_ERR_INVALID_MODULE` | NULL or invalid module handle passed | Check your code for uninitialized handles |

### Human-Readable Error Messages

```c
udynlink_error_t err = udynlink_load_module(&mod, blob, NULL, 0, mode);
if (err != UDYNLINK_OK) {
    const char *msg = udynlink_error_msg(&err);
    my_log("udynlink error: %s", msg);
}
```

Note: `udynlink_error_msg` returns the raw enum name (e.g., `UDYNLINK_ERR_LOAD_OUT_OF_MEMORY`), not a descriptive sentence.

### Debug Levels

```c
udynlink_set_debug_level(UDYNLINK_DEBUG_NONE);    // No output
udynlink_set_debug_level(UDYNLINK_DEBUG_ERROR);    // Errors only
udynlink_set_debug_level(UDYNLINK_DEBUG_WARNING);  // Errors + warnings
udynlink_set_debug_level(UDYNLINK_DEBUG_INFO);     // Everything (very verbose)
```

At `UDYNLINK_DEBUG_INFO`, the loader prints the module name, RAM allocation details, every relocation applied, and symbol resolution steps. This is invaluable during integration but should be disabled in production to avoid semihosting or UART overhead.

### Common Integration Problems and Solutions

**Problem:** `udynlink_load_module()` crashes or calls `udynlink_external_free()` with an invalid pointer during error cleanup.

**Solution:** You forgot to zero-initialize the module handle before the first load. Always call `memset(p_mod, 0, sizeof(*p_mod))` before `udynlink_load_module()` or `udynlink_load_module_image()`. The loader reads `p_mod->p_ram` on the error path; if it contains stack garbage, it will pass that garbage to `udynlink_external_free()`.

**Problem:** Module loads successfully, but calling a module function causes a hard fault.

**Solution:** You forgot to set `r9` before the call. Always call `UDYNLINK_PREPARE_CALL(p_mod)` (or use `UDYNLINK_CALL()`) before invoking any module function.

**Problem:** `UDYNLINK_ERR_LOAD_ARCH_MISMATCH` on a Cortex-M4F module.

**Solution:** Your host `UDYNLINK_HOST_ARCH_TAG` is probably set to `UDYNLINK_ARCH_TAG_CORTEX_M4` (soft-float). Change it to `UDYNLINK_ARCH_TAG_CORTEX_M4F`.

**Problem:** `UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL` for `printf`.

**Solution:** Add `printf` (or your firmware's equivalent) to `udynlink_external_resolve_symbol`. If using newlib nano, the actual symbol may be `_printf` or `_write`.

**Problem:** Module loads but global variables in the module have unexpected values.

**Solution:** Ensure you are using the correct `load_mode`. In `XIP` mode, only `.data` is copied; if the module blob itself is not in a memory-mapped flash region, the code section will be garbage.

**Problem:** Deferred symbol is not detected by the module (`if (func != NULL)` is always true).

**Solution:** The host is providing a fallback stub for the deferred symbol via `udynlink_external_resolve_symbol`. Remove the stub, or return `UDYNLINK_SYM_DEFERRED` from the resolver so the LOT slot is initially `0`.

---

## See Also

- [How It Works](how-it-works.md) — Position-independent code model and relocation details
- [API Reference](api-reference.md) — Complete reference for all public functions and data structures
- [Module Guide](writing-modules.md) — How to write and compile loadable modules
- [Testing](testing.md) — How to run the QEMU test suite
- [Examples](examples.md) — Example host firmware and module projects
