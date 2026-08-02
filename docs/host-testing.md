# Host-Side Testing of Modules

Testing module logic directly on your development machine — without an ARM
cross-compiler, without QEMU, and without the udynlink loader — is the fastest
way to iterate on module code. This guide explains what you can and cannot
test this way, and provides two integration paths:

1. **CMake helper** — [`udynlink_add_host_test`](#cmake-path) for projects that
   already consume udynlink via CMake (`FetchContent`, `add_subdirectory`, or
   `find_package`).
2. **Standalone template** — a copy-and-adapt [`Makefile` project](#vendored-path)
   for users who vendor the scripts or build without CMake.

- [Why Host Testing?](#why-host-testing)
- [What You Can and Cannot Test](#what-you-can-and-cannot-test)
- [The Core Workflow](#the-core-workflow)
- [Mocking Patterns](#mocking-patterns)
- [Cross-Module Tests](#cross-module-tests)
- [C++ Modules on Host](#c-modules-on-host)
- [Weak Symbols on Host](#weak-symbols-on-host)
- [Gotchas and Fidelity Gaps](#gotchas-and-fidelity-gaps)
- [CMake Path](#cmake-path)
- [Vendored Path](#vendored-path)
- [Choosing a Mocking Library](#choosing-a-mocking-library)

## Why Host Testing?

Every udynlink module is ordinary C or C++ source code. The position-independent
code model, the LOT/r9 mechanism, the assembly prologues, and the UDLM binary
format are all **toolchain-side concerns** — they live in `mkmodule` and the
loader, not in your module sources. This means the *logic* of a module —
algorithms, state machines, sensor processing, protocol parsing — can be compiled
and tested as a plain host binary with a standard C/C++ compiler.

Host tests give you:

- **Fast iteration**: no cross-compile, no QEMU boot, no flash cycle.
- **Standard tooling**: debuggers, sanitizers (ASan, UBSan), coverage,
  profilers — all available natively.
- **Mocking**: replace hardware abstractions with controllable stubs without
  touching module source code.

## What You Can and Cannot Test

| You **can** test on host | You **cannot** test on host |
|---|---|
| Module algorithm correctness | LOT/r9 relocation mechanics |
| Interaction with host symbols (via mocks) | Assembly prologue wrappers |
| Cross-module call **logic** (link together) | Cross-module **thunk dispatch** |
| Global/static state lifecycle | `udynlink_load_module` / `udynlink_unload_module` |
| C++ global constructor execution | ABI version / architecture tag checks |
| Weak-symbol override behavior (partial) | XIP vs COPY load modes |

> **Key insight:** Host tests do not exercise the udynlink loader at all.
> There is no `udynlink_module_t`, no `udynlink_load_module`, no
> `UDYNLINK_PREPARE_CALL`. You call module functions as ordinary C function
> calls. The loader's behavior is validated by the [QEMU integration test
> suite](testing.md); host tests validate your module's *logic*.

## The Core Workflow

A host test is three groups of source files compiled into one binary:

```
┌─────────────────────┐  ┌──────────────────┐  ┌─────────────────┐
│  Module sources      │  │  Mock sources     │  │  Test driver     │
│  (mod_foo.c, ...)   │  │  (mock_host.c)    │  │  (test_main.c)   │
│                     │  │                   │  │  - main()        │
│  - exported funcs    │  │  - fake sensor_*  │  │  - assertions    │
│  - extern host syms  │  │  - fake actuator_*│  │  - setup/teardown│
└─────────┬───────────┘  └────────┬──────────┘  └────────┬────────┘
          │                       │                       │
          └───────────────────────┼───────────────────────┘
                                  ▼
                    ┌──────────────────────────┐
                    │  Host test binary        │
                    │  (gcc / g++ / cc)        │
                    │  exits non-zero on fail  │
                    └──────────────────────────┘
```

1. **Module sources** — the exact same `.c` / `.cpp` files you pass to
   `mkmodule`. No changes. The `extern` declarations of host symbols
   (`sensor_read`, `printf`, etc.) become unresolved references that the mocks
   satisfy at link time.

2. **Mock sources** — C/C++ files providing implementations of every host
   symbol the module consumes. Mocks can be trivial (return a fixed value) or
   stateful (record call counts, capture arguments, return queued responses).

3. **Test driver** — a `main()` that sets up mock state, calls module
   functions, and asserts on the results.

### A Complete Example

Module (`module/mod_example.c`):

```c
#include <stdint.h>

extern int sensor_read(int channel);
extern void actuator_set(int value);

static int threshold = 100;

int regulate(int target) {
    int reading = sensor_read(0);
    if (reading > threshold) {
        actuator_set(0);
        return -1;
    }
    actuator_set(target - reading);
    return 0;
}
```

Mock (`mocks/mock_host.c`):

```c
#include "mock_host.h"

static int s_sensor_value = 0;
static int s_actuator_value = 0;
static int s_actuator_calls = 0;

void mock_sensor_set(int value) { s_sensor_value = value; }
void mock_actuator_reset(void) { s_actuator_value = 0; s_actuator_calls = 0; }
int  mock_actuator_get(void)   { return s_actuator_value; }
int  mock_actuator_calls(void) { return s_actuator_calls; }

/* Host symbols consumed by the module */
int sensor_read(int channel) { (void)channel; return s_sensor_value; }
void actuator_set(int value) { s_actuator_value = value; s_actuator_calls++; }
```

Test driver (`test/test_main.c`):

```c
#include <stdio.h>
#include "mock_host.h"

extern int regulate(int target);

#define ASSERT(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } \
} while (0)

int main(void) {
    mock_sensor_set(50);
    mock_actuator_reset();
    ASSERT(regulate(100) == 0);
    ASSERT(mock_actuator_get() == 50);
    ASSERT(mock_actuator_calls() == 1);

    mock_sensor_set(150);
    mock_actuator_reset();
    ASSERT(regulate(100) == -1);
    ASSERT(mock_actuator_get() == 0);

    printf("All host tests passed.\n");
    return 0;
}
```

## Mocking Patterns

### Pattern 1: Hand-written stateful mocks (recommended for most cases)

The example above shows this pattern. Mocks maintain internal state (captured
arguments, return values, call counts) that the test driver can set up and
inspect. This is the simplest approach, requires no external tooling, and works
with any build system.

### Pattern 2: Return-value queuing

Useful when a module calls a host function multiple times and you need different
return values per call:

```c
static int s_queue[16];
static int s_queue_len = 0;
static int s_queue_idx = 0;

void mock_sensor_queue(int value) {
    s_queue[s_queue_len++] = value;
}

int sensor_read(int channel) {
    (void)channel;
    return (s_queue_idx < s_queue_len) ? s_queue[s_queue_idx++] : 0;
}
```

### Pattern 3: Callback injection

When a module accepts function pointers from the host (the [callback
pattern](writing-modules.md#callback-pattern)), the test driver registers test
callbacks that record invocations:

```c
static int s_last_event_code = -1;

static void on_event_record(int code) { s_last_event_code = code; }

void test_callback_registration(void) {
    module_init(on_event_record);  /* module calls host_register_callback */
    module_trigger_event(42);
    ASSERT(s_last_event_code == 42);
}
```

### Pattern 4: Weak-symbol override

If a module declares `__attribute__((weak))` symbols, the host can provide a
strong definition to override them. On host this happens at **link time**: a
strong definition in your mock or test sources overrides the module's weak
definition. See [Weak Symbols on Host](#weak-symbols-on-host) for fidelity notes.

## Cross-Module Tests

When module A calls functions exported by module B (declared via
[`UDYNLINK_REQUIRES`](writing-modules.md#cross-module-function-calls)), you can
test the **call logic** on host by compiling both modules together:

```c
// mod_math.c
int math_add(int a, int b) { return a + b; }
```

```c
// mod_app.c
extern int math_add(int a, int b);
int call_math(int a, int b) { return math_add(a, b) * 2; }
```

On host, the linker resolves `math_add` directly to `mod_math`'s definition. No
thunks, no r9 switching — just a normal function call. This validates that
`mod_app` calls the right function with the right arguments.

> **What this does NOT test:** The udynlink thunk mechanism that makes
> cross-module calls work on target (10-byte stubs + 18-byte gateways that
> switch `r9` to the callee's LOT base). That is a loader concern validated by
> the [QEMU cross-module tests](testing.md#cross-module-test). Host
> cross-module tests validate **logic only**.

The `UDYNLINK_REQUIRES(math)` macro expands to an `extern` symbol with a special
section name (`.udynlink.mod.requires.math`). On host this symbol is **dormant**:
it exists in the object file but has no runtime effect. You do not need to
satisfy or handle it. Just compile the sources together.

## C++ Modules on Host

C++ module sources (`.cpp` / `.cxx`) work on host with one important difference:

| Aspect | On target | On host |
|---|---|---|
| Global constructors | `udynlink_cpp_init(&mod)` after load | Run automatically before `main()` |
| `-fno-exceptions` | Yes | **Apply manually** to match |
| `-fno-rtti` | Yes | **Apply manually** to match |
| `-fno-use-cxa-atexit` | Yes | **Apply manually** to match |
| `-fno-threadsafe-statics` | Yes | **Apply manually** to match |

On target, the host must call `udynlink_cpp_init()` to run `__init_array`
constructors. On host, the C runtime runs global constructors automatically
before `main()` — **do not call `udynlink_cpp_init`** (the module isn't loaded
via udynlink; there is no `udynlink_module_t`).

The C++ restriction flags (`-fno-exceptions -fno-rtti -fno-use-cxa-atexit -fno-threadsafe-statics`) are
critical: without them, host tests could accidentally use `try`/`catch` or
`dynamic_cast` — features the module cannot use on target. The [CMake
helper](#cmake-path) applies these automatically; the [Makefile
template](#vendored-path) includes them in `CXXFLAGS`.

```cpp
// mod_foo.cpp — tested on host
static int g_init_count = 0;

struct Tracker {
    Tracker() { g_init_count++; }
};

static Tracker g_tracker;  // constructor runs before main() on host

int get_init_count() { return g_init_count; }
```

```c
// test_main.c — or .cpp
#include <assert.h>
extern "C" int get_init_count();

int main() {
    assert(get_init_count() == 1);  // constructor ran
    return 0;
}
```

## Weak Symbols on Host

Modules can declare [`__attribute__((weak))` symbols](how-it-works.md#weak-symbol-support)
that the host overrides at load time. On host, weak-symbol override works
differently:

**On target:** The loader patches LOT/data relocations at load time via
`udynlink_external_resolve_symbol()`. For direct PC-relative calls (`bl
weak_func`), the override **does not take effect** — internal callers always
reach the module's own implementation.

**On host:** A strong definition in your mock or test sources overrides the
module's weak definition **for all call sites** at link time. This means:

- Indirect calls (function pointers, GOT loads): same behavior as target.
- **Direct calls**: on host, the strong override wins. On target, the module's
  own definition wins. This is a fidelity gap — if your module relies on the
  target's direct-call-to-weak behavior, be aware that host tests will see the
  overridden version.

For most use cases (data weak symbols, function-pointer-based hooks), host
behavior matches target behavior. The gap only matters for direct calls to weak
functions, which the [documentation](how-it-works.md#weak-symbol-support)
already notes as a limited scenario on target.

## Gotchas and Fidelity Gaps

### Memory-mapped I/O (MMIO)

Module code that dereferences hardware register addresses
(`*(volatile uint32_t*)0x40000000`) will segfault or access invalid memory on
host. **This is expected** — on target, these addresses correspond to
peripheral registers. On host they are unmapped.

**Fix:** Route all hardware access through host-provided accessor functions
(`extern void reg_write(uint32_t addr, uint32_t val)`) and mock those functions.
This is also [best practice for modularity](writing-modules.md#what-modules-cannot-do)
on target.

### `printf` and libc

On target, `printf` is a host symbol resolved by `udynlink_external_resolve_symbol`.
On host, `printf` is the standard libc function — it links automatically and
works. If your module uses `printf` for debug output, it will print to the test
process's stdout with no additional setup.

### `UDYNLINK_PREPARE_CALL` / `UDYNLINK_CALL`

These macros exist only on target (they set the `r9` LOT base register). On
host, there is no LOT and no `r9` — you call module functions as plain function
calls. **Do not include `udynlink_call.h` or `udynlink.h` in host test code.**
Host tests link module + mocks directly; there is no loader involvement.

### `UDYNLINK_REQUIRES` symbols

These emit `extern` symbols with section names like
`.udynlink.mod.requires.math`. On host they are **dormant** — the symbol exists
in the object file but has no runtime effect. You do not need to provide a
definition or mock for them.

### Unresolved symbols at link time

If the linker reports `undefined reference to 'foo'`, it means your module
calls a host symbol `foo` that you have not provided a mock for. Add a mock
implementation to your mock sources. This is the host equivalent of
`UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL` on target — same root cause, different stage.

## CMake Path

If you consume udynlink via CMake (`FetchContent`, `add_subdirectory`, or
`find_package`), use the `udynlink_add_host_test` helper:

```cmake
include(FetchContent)
FetchContent_Declare(udynlink
    GIT_REPOSITORY https://github.com/your-org/udynlink.git
    GIT_TAG        v0.1.0
)
FetchContent_MakeAvailable(udynlink)

enable_testing()

udynlink_add_host_test(test_regulate
    SOURCES      src/mod_example.c          # module sources
    MOCK_SOURCES  mocks/mock_host.c         # mock implementations
    TEST_SOURCES  tests/test_regulate.c     # test driver(s) with main()
)
```

This creates a `test_regulate` executable registered with CTest. Run tests with:

```bash
cmake --build build && ctest --test-dir build
```

### Signature

```cmake
udynlink_add_host_test(<name>
  SOURCES <src...>            # module + any non-mock, non-test sources
  [MOCK_SOURCES <src...>]     # mock implementations of host symbols
  [TEST_SOURCES <src...>]     # test driver(s) containing main()
  [INCLUDE_DIRS <dir...>]     # extra include directories
  [BUILD_FLAGS <flags>]       # extra compiler flags
  [LINK_LIBS <lib...>]        # extra link libraries
  [CXX_STANDARD <11|14|17|20>] # C++ standard (default: 17)
  [WORKING_DIRECTORY <dir>]   # CTest working directory
)
```

### What the helper does

- Creates an executable target `<name>` from all sources combined.
- Applies `-fno-exceptions -fno-rtti -fno-use-cxa-atexit` to C++ sources,
  matching the module toolchain's restricted C++ subset.
- Sets `C11` / `C++17` standards.
- Registers the binary with CTest.
- Adds `-Wall -Wextra` by default.

### Extending the target

The helper creates a standard CMake target, so you can extend it with normal
CMake commands after the call:

```cmake
udynlink_add_host_test(test_regulate
    SOURCES src/mod_example.c
    MOCK_SOURCES mocks/mock_host.c
    TEST_SOURCES tests/test_regulate.c)

# Add sanitizers for this test only:
target_compile_options(test_regulate PRIVATE -fsanitize=address,undefined)
target_link_options(test_regulate PRIVATE -fsanitize=address,undefined)
```

### Multiple tests

Call the helper once per test:

```cmake
udynlink_add_host_test(test_regulate
    SOURCES src/mod_example.c
    MOCK_SOURCES mocks/mock_host.c
    TEST_SOURCES tests/test_regulate.c)

udynlink_add_host_test(test_parser
    SOURCES src/mod_parser.c
    MOCK_SOURCES mocks/mock_host.c
    TEST_SOURCES tests/test_parser.c)
```

## Vendored Path

If you vendor the udynlink scripts (or build without CMake), use the
[template project](https://github.com/anshumanvichare/udynlink/tree/main/templates/host-test)
at `templates/host-test/` in the repository. Copy it and adapt:

```bash
cp -r templates/host-test my_module_tests
cd my_module_tests
# Edit Makefile: set MODULE_SRCS, MOCK_SRCS, TEST_SRCS
make test
```

The template provides:

- `Makefile` — builds module + mocks + test driver into one host binary.
- `module/mod_example.c` — example module consuming host symbols.
- `mocks/mock_host.h` / `mocks/mock_host.c` — example stateful mocks.
- `test/test_main.c` — example test driver with a minimal `ASSERT` macro.
- `README.md` — quick-start instructions.

The `Makefile` handles both `.c` and `.cpp` sources. To add a C++ module, drop
a `.cpp` file into `MODULE_SRCS`; the `CXXFLAGS` already include the restricted
C++ subset flags.

### Manual (no template)

If you do not want the template, the minimum is one `gcc` command:

```bash
gcc -Wall -Wextra -Isrc -Imocks \
    src/mod_example.c mocks/mock_host.c tests/test_main.c \
    -o test_host && ./test_host
```

For C++ modules, use `g++` and add the restriction flags:

```bash
g++ -Wall -Wextra -std=c++17 -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
    -Isrc -Imocks \
    src/mod_foo.cpp mocks/mock_host.c tests/test_main.cpp \
    -o test_host && ./test_host
```

## Choosing a Mocking Library

The examples in this guide use **hand-written mocks** — plain C functions that
implement the host symbols your module consumes. This is the recommended
starting point: zero dependencies, full control, and works with any build
system.

If your project already uses a mocking library, it drops in seamlessly. Mocking
libraries produce C source files (or linkable objects) that implement the host
symbol interface — add them to `MOCK_SOURCES` (CMake) or `MOCK_SRCS`
(Makefile), exactly as you would a hand-written mock.

| Library | Style | Integration |
|---------|-------|-------------|
| Hand-written (recommended) | Manual C functions | Add to `MOCK_SOURCES` / `MOCK_SRCS` |
| [CMock](https://github.com/ThrowTheSwitch/CMock) | Generates mocks from headers | Generate, then add output to `MOCK_SOURCES` |
| [fff (Fake Function Framework)](https://github.com/meekrosoft/fff) | Header-only, macro-based | Include `fff.h`, add to `INCLUDE_DIRS` |
| [cmocka](https://cmocka.org/) | Runtime, with assertions | Link via `LINK_LIBS`, use in `TEST_SOURCES` |

The udynlink test suite itself does not use any of these libraries; the QEMU
tests use hand-written mocks and golden-output matching. Your host tests are
independent of that pipeline.

---

For the full udynlink test suite (loader mechanics, relocation, ABI validation),
see [Testing Guide](testing.md). For writing modules, see [Writing
Modules](writing-modules.md).
