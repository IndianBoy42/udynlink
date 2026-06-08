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

At load time, the host's `udynlink_external_resolve_symbol()` callback locates the target module and allocates a small thunk (28 bytes) that switches `r9` to the callee module's LOT base before calling the function. From the module author's perspective, this is transparent — the call looks like a normal function call.

### Declaring Explicit Dependencies

To ensure a module is not loaded before its dependencies are available, use `UDYNLINK_REQUIRES`:

```c
#include "udynlink_deps.h"

UDYNLINK_REQUIRES(math);

extern int math_add(int a, int b);
```

`UDYNLINK_REQUIRES(math)` expands to an extern symbol named `.udynlink.mod.requires.math`. The host's dependency system recognizes this prefix and checks that a module named `math` is already loaded. If the dependency is missing, the load fails with `UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL`.

**Best practice:** Always use `UDYNLINK_REQUIRES` for every module you depend on. It documents the dependency for readers and enables the host to fail fast with a clear error message.

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

### Unsupported Features

The toolchain automatically adds these flags for `.cpp` and `.cxx` files:

- `-fno-exceptions` — no `try`/`catch`/`throw`
- `-fno-rtti` — no `typeid` or `dynamic_cast`
- `-fno-use-cxa-atexit` — no static object destruction at exit

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
| `-O <level>` | Optimization level: `0`, `s` (default, size), `2`, `3`, `z`. |
| `--bin-name <path>` | Custom output path for the `.bin` file. Default is derived from the first source file. |
| `--gen-c-header` | Generate a C header file containing the binary as a `static const unsigned char` array. |
| `--header-path <dir>` | Directory where the generated C header is written. Default: current directory. |
| `--mod-version <ver>` | Module ABI version in `major.minor` format. Default: `1.0`. |
| `--udynlink-version <ver>` | Minimum loader ABI version required. Default: `3.0`. |
| `--build-flags <flags>` | Extra compiler flags prepended to the compile command. |
| `--module-name <name>` | Explicit module name. Default is derived from the first source file name. |
| `--disasm` | Show disassembly of `.text` after linking. |
| `--pc-rel` | Allow pc-relative addressing. |
| `--no-long-calls` | Do not use `-mlong-calls`. |
| `--stop-after-compile` | Stop after compiling source files to `.o`. |
| `--stop-after-link` | Stop after linking to `.elf`. |
| `--no-verbose` | Do not print executed commands. |
| `--no-debug` | Do not print debug output. |
| `--no-prologue` | Skip the assembly prologue/wrapper on exported functions. The host must use `UDYNLINK_PREPARE_CALL()` to set `r9` before every call. |

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
