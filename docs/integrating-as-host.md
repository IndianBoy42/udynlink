# Integrating udynlink as a Host

This guide is for firmware developers who want to integrate the udynlink micro dynamic linker into their ARM Cortex-M project. The **host** is the firmware running on the MCU that uses udynlink to load, relocate, and execute binary modules at runtime.

## Table of Contents

- [Overview](#overview)
- [Integration Checklist](#integration-checklist)
- [Adding udynlink to Your Build](#adding-udynlink-to-your-build)
- [Implementing the External Callbacks](#implementing-the-external-callbacks)
- [The LOT Base Address Convention](#the-lot-base-address-convention)
- [Building and Using the Host Symbol Table](#building-and-using-the-host-symbol-table)
- [Hash-Based Symbol Resolution](#hash-based-symbol-resolution)
- [Module Lifecycle Management](#module-lifecycle-management)
- [Streaming I/O Loading](#streaming-io-loading)
- [Error Handling and Diagnostics](#error-handling-and-diagnostics)

---

## Overview

In the udynlink architecture, the **host** is the base firmware image running on your Cortex-M MCU. Modules are position-independent binary blobs compiled separately and loaded into RAM (or executed in place from flash) by the host at runtime.

The host has three core responsibilities:

1. **Provide memory** for the module's runtime data (LOT, `.data`, `.bss`) via `udynlink_external_malloc` and `udynlink_external_free`.
2. **Resolve symbols** that the module references but does not define. When a module calls `printf`, the host must provide the address of its own `printf` implementation.
3. **Set up the LOT base** before every call into module code. Module functions use `r9` as a base register to access their Linker Offset Table (LOT). The host must write the module's RAM base address to a fixed memory location before calling any module function.

The host-module contract is simple: the host exposes a set of symbols (functions and variables), and modules consume them. Modules can also export symbols for other modules to consume via the three-tier dependency resolution system.

---

## Integration Checklist

Follow this checklist to integrate udynlink into your firmware:

1. **Add libudynlink to your build** — as a CMake subdirectory, an installed package, or vendored source files.
2. **Define compile-time constants** — `UDYNLINK_MAX_HANDLES`, `UDYNLINK_HOST_ARCH_TAG`, and `UDYNLINK_LOT_BASE_ADDR`.
3. **Implement all external callbacks** — the 7 functions declared in `udynlink_externals.h`.
4. **Set up the LOT base address** before calling any module function (`udynlink_cpp_init()` sets it internally, so you only need to re-set it before other module calls).
5. **Build a host symbol table** — decide how your firmware will resolve symbols requested by modules.
6. **Write module loading/unloading code** — call `udynlink_load_module()`, manage handles, and call `udynlink_unload_module()` when done.
7. **(Optional) Set up hash-based symbol resolution** — use `scripts/mkhostsyms` for O(1) lookup when you export many symbols.
8. **(Optional) Implement streaming I/O** — if loading modules from SD card, SPI flash, or over a network.

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
- `udynlink/udynlink_hash.c`
- `udynlink/udynlink.h`
- `udynlink/udynlink_externals.h`
- `udynlink/udynlink_hash.h`

Add the `.c` files to your build and ensure the `udynlink/` directory is in your include path.

### Required Compile Definitions

You **must** define these before including `udynlink.h` or compiling `udynlink.c`:

| Definition | Purpose | Valid Values | Default |
|------------|---------|--------------|---------|
| `UDYNLINK_MAX_HANDLES` | Maximum number of simultaneously loaded modules | Any integer `> 0` | **None** — compilation fails if undefined |
| `UDYNLINK_HOST_ARCH_TAG` | Architecture tag of the host MCU | One of the `UDYNLINK_ARCH_TAG_*` constants | `UDYNLINK_ARCH_TAG_CORTEX_M4` |
| `UDYNLINK_LOT_BASE_ADDR` | Fixed RAM address where the LOT base is written | Any valid RAM address | `0x20000000` |

**`UDYNLINK_MAX_HANDLES`**

This controls the size of the internal module table. Every loaded module needs a handle. Set this to the maximum number of modules you expect to load simultaneously. There is no dynamic allocation of handles; the table is a fixed-size array.

```c
#define UDYNLINK_MAX_HANDLES 8
```

**Pitfall:** If you set this to 1, you can only load one module at a time. If you try to load a second module while the first is still loaded, you will get `UDYNLINK_ERR_LOAD_MAX_HANDLES_EXCEEDED`.

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

**`UDYNLINK_LOT_BASE_ADDR`**

This is the memory address that module prologues read to find their LOT base. It must be in RAM and must not overlap with module data. The default `0x20000000` is the start of SRAM on many STM32 parts. If your RAM starts elsewhere, override this.

**Pitfall:** Writing the LOT base to the wrong address will cause module functions to read garbage when accessing global data, leading to hard faults.

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
    UDYNLINK_MAX_HANDLES=8
    UDYNLINK_HOST_ARCH_TAG=UDYNLINK_ARCH_TAG_CORTEX_M4F
    UDYNLINK_LOT_BASE_ADDR=0x20000000
)

# Method 1: add_subdirectory
target_link_libraries(my_firmware PRIVATE udynlink)

# Or Method 3: add sources directly
target_sources(my_firmware PRIVATE
    udynlink/udynlink.c
    udynlink/udynlink_hash.c
)
```

### PlatformIO Note

A `library.json` exists in the repository, but the version field may be outdated. For PlatformIO projects, the most reliable approach is to vendor the source files (Method 3) and add them to `src_filter` or `build_src_filter`.

---

## Implementing the External Callbacks

These 7 functions form the integration contract between udynlink and your firmware. They are declared in `udynlink/udynlink_externals.h` and **must** be defined by your project. The linker will fail if any are missing.

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

**When it is called:** During `udynlink_load_module()` and `udynlink_load_module_from_stream()` when `load_addr` is `NULL` (auto-allocation mode).

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

**When it is called:** During relocation in `udynlink_load_module()` and `udynlink_load_module_from_stream()`, for each `UDYNLINK_SYM_TYPE_EXTERN` symbol that was not resolved by `udynlink_external_resolve_critical_symbol` and was not found in any dependency module.

**What it must do:** Look up `name` in the host firmware's exported API and return the 32-bit address of the symbol. Return `0` if the symbol is not found.

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
- Return exactly `0` for unresolved symbols. The loader treats any non-zero value as success.
- The returned address is written directly into the module's LOT or data section. Ensure it is the correct runtime address.
- This is the **fallback** resolver (tier 3). See [Three-Tier Resolution](how-it-works.md#three-tier-resolution-abi-20).

### f. `udynlink_external_resolve_critical_symbol`

```c
uint32_t udynlink_external_resolve_critical_symbol(const char *name);
```

**When it is called:** During relocation, for every `UDYNLINK_SYM_TYPE_EXTERN` symbol, **before** searching dependency modules and **before** calling `udynlink_external_resolve_symbol`.

**What it must do:** Resolve critical host symbols that modules absolutely need. This is the first tier of the three-tier resolution system. Return `0` if the symbol is not a critical one, allowing the loader to fall through to dependency search and then to `udynlink_external_resolve_symbol`.

**Minimal implementation:**

```c
uint32_t udynlink_external_resolve_critical_symbol(const char *name) {
    // Only resolve the most essential symbols here.
    // Everything else falls through to dependency modules or the fallback resolver.
    if (!strcmp(name, "udynlink_external_malloc"))
        return (uint32_t)(uintptr_t)&udynlink_external_malloc;
    if (!strcmp(name, "udynlink_external_free"))
        return (uint32_t)(uintptr_t)&udynlink_external_free;
    return 0;
}
```

**Common pitfalls:**
- If this function returns `0`, the loader continues to the next tier. If it returns a non-zero value, that address is used immediately and no further search occurs for that symbol.
- Do not resolve every symbol here unless you have a specific reason. The purpose of the three-tier system is to let dependency modules provide symbols without the host needing to re-export them.

### g. `udynlink_external_get_module_handle`

```c
udynlink_module_t *udynlink_external_get_module_handle(const char *module_name);
```

**When it is called:** During `udynlink_load_module()` and `udynlink_load_module_from_stream()` when validating dependencies. The loader iterates over the module's dependency list and calls this for each dependency name.

**What it must do:** Search your firmware's module table and return a pointer to the handle of the already-loaded module with the given name. Return `NULL` if no such module is loaded.

**Minimal implementation:**

```c
#define MAX_LOADED_MODULES 8

static udynlink_module_t *g_loaded_modules[MAX_LOADED_MODULES];
static int g_module_count = 0;

void host_register_module(udynlink_module_t *p_mod) {
    if (g_module_count < MAX_LOADED_MODULES) {
        g_loaded_modules[g_module_count++] = p_mod;
    }
}

udynlink_module_t *udynlink_external_get_module_handle(const char *module_name) {
    for (int i = 0; i < g_module_count; i++) {
        if (g_loaded_modules[i] == NULL) continue;
        const char *name = udynlink_get_module_name(g_loaded_modules[i]);
        if (name && !strcmp(name, module_name))
            return g_loaded_modules[i];
    }
    return NULL;
}
```

**Common pitfalls:**
- You must maintain your own list of loaded modules. udynlink does not provide a global registry.
- Return `NULL` if the dependency is missing; the loader will abort with `UDYNLINK_ERR_LOAD_MISSING_DEP`.
- The dependency name is the exact string provided to `mkmodule --depends <name>` when the module was built.

---

## The LOT Base Address Convention

The Linker Offset Table (LOT) is a per-module array of pointers that allows position-independent code to access global data and external functions. Module prologues load `r9` from a **fixed memory address** (`UDYNLINK_LOT_BASE_ADDR`) to find their LOT. This means the host must write the module's RAM base to this address before every call.

### The Critical Pattern

```c
uint32_t *mod_base = (uint32_t *)UDYNLINK_LOT_BASE_ADDR;
*mod_base = p_mod->ram_base;
```

This must happen **before every call to a module function**, including indirect calls through function pointers obtained from the module.

### For C++ Modules

If you load a C++ module, call `udynlink_cpp_init(p_mod)` **after** loading and **before** calling any module functions. The `udynlink_cpp_init()` function itself sets the LOT base internally before invoking `__init_array`, but you must set it again before calling any other module function afterward.

```c
udynlink_error_t err = udynlink_load_module(&mod, module_blob, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
if (err != UDYNLINK_OK) { /* handle error */ }

// Run C++ global constructors
udynlink_cpp_init(&mod);

// Set LOT base before calling module functions
uint32_t *mod_base = (uint32_t *)UDYNLINK_LOT_BASE_ADDR;
*mod_base = mod.ram_base;

int (*p_func)(void) = (int (*)(void))udynlink_get_symbol_value(&mod, "run");
int result = p_func();
```

### Choosing a LOT Base Address

- It must be in RAM (writable at runtime).
- It must not conflict with module data or your firmware's stack/heap.
- The default `0x20000000` is the very start of SRAM on STM32F1/F4/L4. If you place your vector table or other data there, choose a different address, such as `0x20000004` or `0x20000008`.
- Override via: `#define UDYNLINK_LOT_BASE_ADDR 0x20000000`

**Pitfall:** If you forget to set the LOT base, module functions will use a stale `r9` value, causing them to read/write the wrong memory region. This typically results in a hard fault or silent data corruption.

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

```c
udynlink_module_t mod;
memset(&mod, 0, sizeof(mod));

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
// Set LOT base before EVERY call
uint32_t *mod_base = (uint32_t *)UDYNLINK_LOT_BASE_ADDR;
*mod_base = mod.ram_base;

// Look up and call a function
udynlink_sym_t sym;
if (udynlink_lookup_symbol(&mod, "process_data", &sym) != NULL) {
    int (*process)(int) = (int (*)(int))(uintptr_t)sym.val;
    int result = process(42);
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
- Check `dep_refcount`. If another module depends on this one, it returns `UDYNLINK_ERR_MODULE_HAS_DEPENDENTS`.
- Decrement `dep_refcount` on all modules this module depends on.
- Free the auto-allocated RAM.
- Zero out the module handle.

### Dependency Tracking

If a module was built with `--depends provider`, the loader enforces that a module named `provider` is already loaded before the consumer can load.

**Rule:** Load dependencies before dependents. Unload in reverse order (consumers before providers).

```c
// Correct order
udynlink_module_t provider, consumer;
udynlink_load_module(&provider, provider_blob, NULL, 0, mode);
host_register_module(&provider);

udynlink_load_module(&consumer, consumer_blob, NULL, 0, mode);
host_register_module(&consumer);

// ... use consumer ...

// Unload in reverse
host_unregister_module(&consumer);
udynlink_unload_module(&consumer);

host_unregister_module(&provider);
udynlink_unload_module(&provider);
```

### Load Modes

| Mode | Behavior | RAM Needed | Use Case |
|------|----------|------------|----------|
| `UDYNLINK_LOAD_MODE_COPY_ALL` | Copy header, code, and data to RAM | Largest | Module blob is in a temporary buffer |
| `UDYNLINK_LOAD_MODE_COPY_TEXT_DATA` | Copy text and data sections to RAM; header stays at `base_addr` | Medium | Module is in flash, but code must run from RAM |
| `UDYNLINK_LOAD_MODE_XIP` | Copy only data to RAM; execute code from flash | Smallest | Module is in flash and supports XIP |

**Pitfall:** XIP mode requires that the module's code section is in an execute-in-place capable region (typically internal flash). The host must also ensure the flash is memory-mapped and readable at the `base_addr`.

---

## Streaming I/O Loading

Use streaming I/O when modules are stored on media that is not memory-mapped, such as SD cards, SPI flash, or external serial memory. The streaming loader reads the module incrementally through callbacks, avoiding the need to load the entire blob into RAM first.

### Implementing `udynlink_io_t`

You provide two callbacks:

```c
typedef int32_t (*udynlink_read_cb_t)(
    void *pv_ctx,      // Your context pointer
    void *buf,         // Destination buffer
    uint32_t num_bytes,// Bytes to read
    uint32_t offset    // Byte offset into the module image
);

typedef int32_t (*udynlink_get_size_cb_t)(
    void *pv_ctx       // Your context pointer
);
```

### Complete SD Card Example (FatFS-style)

```c
#include "udynlink.h"
#include "ff.h"  // FatFS

typedef struct {
    FIL fil;
} sd_ctx_t;

int32_t sd_read(void *pv_ctx, void *buf, uint32_t num_bytes, uint32_t offset) {
    sd_ctx_t *ctx = (sd_ctx_t *)pv_ctx;
    UINT br;
    FRESULT res = f_lseek(&ctx->fil, offset);
    if (res != FR_OK) return -1;
    res = f_read(&ctx->fil, buf, num_bytes, &br);
    if (res != FR_OK || br != num_bytes) return -1;
    return (int32_t)br;
}

int32_t sd_get_size(void *pv_ctx) {
    sd_ctx_t *ctx = (sd_ctx_t *)pv_ctx;
    return (int32_t)f_size(&ctx->fil);
}

udynlink_error_t load_module_from_sd(const char *path, udynlink_module_t *p_mod) {
    sd_ctx_t ctx;
    FRESULT res = f_open(&ctx.fil, path, FA_READ);
    if (res != FR_OK) return UDYNLINK_ERR_LOAD_IO_ERROR;

    udynlink_io_t io = {
        .read = sd_read,
        .get_size = sd_get_size,
        .pv_ctx = &ctx
    };

    // Allocate a work buffer on the stack or from a pool
    uint8_t work_buf[512];

    udynlink_error_t err = udynlink_load_module_from_stream(
        p_mod, &io,
        NULL, 0,                    // Auto-allocate RAM
        UDYNLINK_LOAD_MODE_COPY_ALL,
        work_buf, sizeof(work_buf)
    );

    f_close(&ctx.fil);
    return err;
}
```

### Work Buffer Sizing

The work buffer is a scratch area used by the streaming loader for temporary reads. The minimum size is 64 bytes (`UDYNLINK_STREAM_MIN_WORK_BUF_SIZE`), but larger buffers reduce the number of I/O callbacks.

To find the optimal size for a single-shot metadata read:

```c
uint32_t optimal_size = udynlink_get_stream_metadata_size(&io);
// This returns: byte offset to the code section (header + relocs + symtab + deps strtab + padding)
```

If you allocate a buffer of at least this size, the loader can read all metadata (header, relocations, symbol table) in a single `read()` call.

### Supported Modes for Streaming

Streaming supports `UDYNLINK_LOAD_MODE_COPY_ALL` and `UDYNLINK_LOAD_MODE_COPY_TEXT_DATA`.

`UDYNLINK_LOAD_MODE_XIP` is **not supported** and returns `UDYNLINK_ERR_LOAD_XIP_UNSUPPORTED`, because streaming implies the module is not in a memory-mapped execute region.

### RAM Pre-Calculation

Before loading, you can compute how much RAM the module will need:

```c
uint32_t ram_needed = udynlink_get_ram_requirements_stream(&io, UDYNLINK_LOAD_MODE_COPY_ALL);
if (ram_needed == 0) {
    // I/O error or invalid module
}
```

This is useful for pre-allocating a memory pool slot or checking free space before attempting the load.

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
| `UDYNLINK_ERR_LOAD_MAX_HANDLES_EXCEEDED` | Maximum handle count reached | Unload unused modules or increase `UDYNLINK_MAX_HANDLES` |
| `UDYNLINK_ERR_LOAD_INVALID_MODE` | Unknown load mode value | Check that you are passing a valid `udynlink_load_mode_t` |
| `UDYNLINK_ERR_LOAD_BAD_RELOCATION_TABLE` | Relocation data is malformed or points to an invalid symbol | The module is corrupted or was built with a buggy toolchain |
| `UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL` | An extern symbol could not be resolved by any tier | Ensure the symbol is exported by the host or by a loaded dependency module |
| `UDYNLINK_ERR_LOAD_DUPLICATE_NAME` | A module with the same name is already loaded | The eh2k fork allows multiple instances; this error may not trigger in current builds |
| `UDYNLINK_ERR_LOAD_VERSION_MISMATCH` | Module ABI version exceeds loader ABI version | Update the host's udynlink library or rebuild the module with `--udynlink-version` matching the host |
| `UDYNLINK_ERR_LOAD_ARCH_MISMATCH` | Module CPU/float ABI is incompatible with host | Rebuild the module with the correct `--target` or `--mcpu` |
| `UDYNLINK_ERR_LOAD_MISSING_DEP` | A dependency declared via `--depends` is not loaded | Load the dependency module first |
| `UDYNLINK_ERR_LOAD_IO_ERROR` | Streaming I/O read failed | Check SD card, SPI flash, or network connection |
| `UDYNLINK_ERR_MODULE_HAS_DEPENDENTS` | Cannot unload because module has active dependents | Unload dependent modules first |
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

**Problem:** Module loads successfully, but calling a module function causes a hard fault.

**Solution:** You forgot to set the LOT base before the call. Always write `p_mod->ram_base` to `*(uint32_t*)UDYNLINK_LOT_BASE_ADDR`.

**Problem:** `UDYNLINK_ERR_LOAD_ARCH_MISMATCH` on a Cortex-M4F module.

**Solution:** Your host `UDYNLINK_HOST_ARCH_TAG` is probably set to `UDYNLINK_ARCH_TAG_CORTEX_M4` (soft-float). Change it to `UDYNLINK_ARCH_TAG_CORTEX_M4F`.

**Problem:** `UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL` for `printf`.

**Solution:** Add `printf` (or your firmware's equivalent) to `udynlink_external_resolve_symbol`. If using newlib nano, the actual symbol may be `_printf` or `_write`.

**Problem:** Unloading a module returns `UDYNLINK_ERR_MODULE_HAS_DEPENDENTS`.

**Solution:** Another loaded module declared this one as a dependency via `--depends`. Unload the dependent module first.

**Problem:** Streaming load from SD card returns `UDYNLINK_ERR_LOAD_IO_ERROR`.

**Solution:** Check that your `read` callback correctly handles the `offset` parameter and returns exactly `num_bytes` on success. Verify that `get_size` returns the exact file size in bytes.

**Problem:** Module loads but global variables in the module have unexpected values.

**Solution:** Ensure you are using the correct `load_mode`. In `XIP` mode, only `.data` is copied; if the module blob itself is not in a memory-mapped flash region, the code section will be garbage.

---

## See Also

- [How It Works](how-it-works.md) — Position-independent code model and relocation details
- [API Reference](api-reference.md) — Complete reference for all public functions and data structures
- [Module Guide](writing-modules.md) — How to write and compile loadable modules
- [Testing](testing.md) — How to run the QEMU test suite
- [Examples](examples.md) — Example host firmware and module projects
