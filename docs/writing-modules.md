# Writing Loadable Modules

This guide is for module authors who want to write C or C++ code that can be loaded at runtime by the udynlink dynamic linker on ARM Cortex-M microcontrollers.

## Table of Contents

- [Overview](#overview)
- [Quick Start: Hello World Module](#quick-start-hello-world-module)
- [Module Code Structure](#module-code-structure)
- [Consuming Host Symbols](#consuming-host-symbols)
- [Module Dependencies (Inter-Module Symbols)](#module-dependencies-inter-module-symbols)
- [C++ Modules](#c-modules)
- [Data and Variables in Modules](#data-and-variables-in-modules)
- [Target Selection and Cross-Compilation](#target-selection-and-cross-compilation)
- [The mkmodule Command Reference](#the-mkmodule-command-reference)
- [Building and Distributing Modules](#building-and-distributing-modules)
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
- Call functions exported by other loaded modules (with dependency tracking)

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

    // Before calling ANY module function, set the LOT base address
    *(uint32_t *)UDYNLINK_LOT_BASE_ADDR = mod.ram_base;

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

### Common Host Symbols

Typical symbols a host firmware provides:

- `printf`, `sprintf`, `snprintf` — debug output
- `malloc`, `free` — dynamic memory
- `memcpy`, `memset`, `strlen` — standard string/memory operations
- Custom hardware abstractions: `sensor_read`, `adc_sample`, `pwm_set`, `spi_transfer`

## Module Dependencies (Inter-Module Symbols)

Modules can consume symbols from other modules that are already loaded. This lets you build layered systems without the host firmware re-exporting every intermediate symbol.

### Declaring Dependencies

Tell `mkmodule` which modules your code needs:

```bash
python3 mkmodule --depends mod_provider consumer.c
```

This records `mod_provider` in the module header. The loader will refuse to load `consumer` unless `mod_provider` (or another module with the same name) is already loaded.

### At Runtime

Dependencies must be loaded **before** the dependent module:

```c
udynlink_module_t provider, consumer;

// Load provider first
udynlink_load_module(&provider, provider_bin, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);

// Then load consumer (succeeds because "mod_provider" is already loaded)
udynlink_load_module(&consumer, consumer_bin, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
```

### Consuming Symbols from Dependencies

Inside `consumer.c`, simply use the function name. The loader resolves it through the dependency chain:

```c
// consumer.c
#include <stdio.h>

// Defined in mod_provider, not in this file or the host.
extern int provider_add(int a, int b);

int test(void) {
    int result = provider_add(3, 4);
    printf("provider_add(3,4) = %d\n", result);
    return result == 7;
}
```

The loader resolves `provider_add` by searching:
1. Host critical symbols (`udynlink_external_resolve_critical_symbol`)
2. Already-loaded dependency modules (`udynlink_external_get_module_handle`)
3. Host fallback symbols (`udynlink_external_resolve_symbol`)

### Safe Unload

The loader tracks how many modules depend on each loaded module via `dep_refcount`. You cannot unload a dependency that still has active dependents:

```c
udynlink_unload_module(&consumer);   // OK
udynlink_unload_module(&provider);   // OK — no more dependents
```

If you try to unload `provider` while `consumer` is still loaded, the call returns `UDYNLINK_ERR_MODULE_HAS_DEPENDENTS`.

### Optional Dependencies

A module can declare an optional dependency and detect at runtime whether it is linked. If the host returns `UDYNLINK_DEP_DEFERRED` during load (or the dependency is never loaded), the extern symbol's LOT slot contains `0`. The module can check for this:

```c
extern void log_printf(const char *fmt, ...);

void module_init(void) {
    if (log_printf != NULL) {
        log_printf("module initialized\n");
    } else {
        // Degraded mode: no logging
    }
}
```

Build with `--depends logging` even though `logging` may not be loaded. The host controls whether the dependency is required or optional by returning `NULL` (fail) or `UDYNLINK_DEP_DEFERRED` (skip) from `udynlink_external_get_module_handle()`.

**Caveat:** If the host provides a fallback stub for `log_printf`, the LOT slot is non-zero and the module cannot detect absence using this pattern alone.

### Example: Provider and Consumer

**`provider.c`** — exports math utilities:

```c
int provider_add(int a, int b) {
    return a + b;
}

int provider_mul(int a, int b) {
    return a * b;
}
```

**`consumer.c`** — uses them:

```c
#include <stdio.h>

extern int provider_add(int a, int b);

int compute(int x) {
    return provider_add(x, x);
}

int test(void) {
    printf("dep ok\n");
    return 1;
}
```

**Build:**

```bash
python3 mkmodule provider.c
python3 mkmodule --depends provider consumer.c
```

## C++ Modules

### Supported Features

- Classes and objects
- Constructors and destructors
- Templates
- Namespaces
- Function overloading (for internal use; exported symbols need `extern "C"`)

### Unsupported Features

The toolchain automatically adds these flags for `.cpp` and `.cxx` files:

- `-fno-exceptions` — no `try`/`catch`/`throw`
- `-fno-rtti` — no `typeid` or `dynamic_cast`
- `-fno-use-cxa-atexit` — no static object destruction at exit

### Global Constructors

If your module has global C++ objects with constructors, the host **must** call `udynlink_cpp_init()` after loading the module. The function sets the LOT base internally before invoking `__init_array`; you must set it again before calling any other module function:

```c
udynlink_load_module(&mod, hello_cpp_bin, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
udynlink_cpp_init(&mod);   // Run __init_array (sets LOT base internally)
*(uint32_t *)UDYNLINK_LOT_BASE_ADDR = mod.ram_base; // Re-set before other calls
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
*(uint32_t *)UDYNLINK_LOT_BASE_ADDR = mod.ram_base;
udynlink_cpp_init(&mod);

// Now safe to call exported functions
```

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
| `--depends mod_a,mod_b` | Comma-separated list of dependency module names. The loader will enforce that these modules are already loaded. Circular dependencies are rejected by default but can be loaded via deferred dependency support (see [Host Guide](integrating-as-host.md#deferred-dependencies-and-symbols)). |
| `-O <level>` | Optimization level: `0`, `s` (default, size), `2`, `3`, `z`. |
| `--bin-name <path>` | Custom output path for the `.bin` file. Default is derived from the first source file. |
| `--gen-c-header` | Generate a C header file containing the binary as a `static const unsigned char` array. |
| `--header-path <dir>` | Directory where the generated C header is written. Default: current directory. |
| `--mod-version <ver>` | Module ABI version in `major.minor` format. Default: `1.0`. |
| `--udynlink-version <ver>` | Minimum loader ABI version required. Default: `2.0`. |
| `--lot-base <addr>` | LOT base address written into the assembly prologue. Default: `0x20000000`. |
| `--build-flags <flags>` | Extra compiler flags prepended to the compile command. |
| `--module-name <name>` | Explicit module name. Default is derived from the first source file name. |
| `--disasm` | Show disassembly of `.text` after linking. |
| `--pc-rel` | Allow pc-relative addressing. |
| `--no-long-calls` | Do not use `-mlong-calls`. |
| `--stop-after-compile` | Stop after compiling source files to `.o`. |
| `--stop-after-link` | Stop after linking to `.elf`. |
| `--no-verbose` | Do not print executed commands. |
| `--no-debug` | Do not print debug output. |

### Environment Variables

| Variable | Description |
|----------|-------------|
| `UDYNLINK_CC_PREFIX` | Compiler prefix. Default: `arm-none-eabi-`. |

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

A module provides services consumed by other modules via the dependency system:

```c
// math_service.c
int svc_add(int a, int b) { return a + b; }
int svc_sub(int a, int b) { return a - b; }
```

```bash
python3 mkmodule math_service.c
python3 mkmodule --depends math_service client.c
```

## Common Pitfalls and Troubleshooting

### "Symbol not found" (`UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL`)

- You called a function without declaring it `extern`.
- The host does not implement the symbol in `udynlink_external_resolve_symbol`.
- Check the symbol name carefully — C++ names must be `extern "C"` to avoid mangling.

### "Architecture mismatch" (`UDYNLINK_ERR_LOAD_ARCH_MISMATCH`)

- The module was compiled for a different core or float ABI than the host. For example, a `cortex-m4f` (hard-float) module on a `cortex-m4` (soft-float) host.
- Rebuild with the correct `--target`.

### Crashes After Loading

- **Forgot to set the LOT base address.** Before calling any module function, the host must write `mod.ram_base` to `*(uint32_t *)UDYNLINK_LOT_BASE_ADDR`.
- **Tried to call a function before `udynlink_load_module` returned.** Only call functions after a successful load.

### Module Works at `-O0` but Not `-Os`

- Likely undefined behavior in your C/C++ code (uninitialized variables, out-of-bounds access, strict aliasing violations).
- Missing `volatile` on memory-mapped hardware access.
- Add `-Wall -Wextra` with `--build-flags` and fix all warnings.

### C++ Constructors Not Running

- The host forgot to call `udynlink_cpp_init(&mod)` after loading.
- The LOT base address must be set **before** calling `udynlink_cpp_init`.

### Taking the Address of an Exported Function

Inside a module, `&my_exported_func` gives you the address of the **wrapper prologue**, not the raw function body. This is usually fine for callbacks, but if you need the raw address (for example, to compute a checksum over the function body), you cannot obtain it from within the module.

### Unloading a Module That Has Dependents

`udynlink_unload_module` returns `UDYNLINK_ERR_MODULE_HAS_DEPENDENTS` if another loaded module still declares this one as a dependency. Unload dependents first, then dependencies.

---

For host integration details, see the [Host Guide](integrating-as-host.md). For API reference, see [API Reference](api-reference.md). For the internals of relocation and the LOT, see [How It Works](how-it-works.md).
