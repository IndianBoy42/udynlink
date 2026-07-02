# Host-Side Test Template

Compile and test udynlink module **logic** on your development machine — no ARM
cross-compiler, no QEMU, no udynlink loader required.

## Quick Start

```bash
make test
```

This builds `module/mod_example.c` + `mocks/mock_host.c` + `test/test_main.c`
into a `test_host` binary and runs it. The example passes if you see:

```
All host tests passed.
```

## What This Tests

Module **logic**: algorithms, state machines, interaction with host symbols
(via mocks). The module sources are the exact same files you pass to
`mkmodule` — no changes between target and host builds.

This does **not** test the udynlink loader (LOT/r9 relocations, prologue
wrappers, XIP/COPY modes, ABI checks). Those are validated by the QEMU
integration test suite. See [docs/host-testing.md](../../docs/host-testing.md)
for the full fidelity analysis.

## Adapting for Your Module

Edit the `Makefile` and update three lists:

```makefile
MODULE_SRCS = module/your_module.c
MOCK_SRCS = mocks/your_mocks.c
TEST_SRCS = test/your_test.c
```

### Adding a mock

1. Declare the host symbol `extern` in your module source (you already do
   this for `mkmodule`).

2. Implement it in a `mocks/*.c` file:

```c
// mocks/mock_host.c
int sensor_read(int channel) {
    return /* your controlled return value */;
}
```

3. (Optional) Add mock-control functions to `mocks/mock_host.h` so your test
   driver can configure and inspect mock state.

### C++ modules

Drop a `.cpp` file into `MODULE_SRCS`. The `CXXFLAGS` already include the
restricted C++ subset (`-fno-exceptions -fno-rtti -fno-use-cxa-atexit`) that
matches the module toolchain. Global constructors run automatically before
`main()` on host — do not call `udynlink_cpp_init()`.

### Cross-module tests

Add both modules' sources to `MODULE_SRCS`. The linker resolves cross-module
calls directly:

```makefile
MODULE_SRCS = module/mod_math.c module/mod_app.c
```

The `UDYNLINK_REQUIRES` macro emits a dormant symbol on host — no special
handling needed.

## Using CMake Instead

If your project uses CMake, prefer the `udynlink_add_host_test` helper:

```cmake
udynlink_add_host_test(test_regulate
    SOURCES      src/mod_example.c
    MOCK_SOURCES  mocks/mock_host.c
    TEST_SOURCES  tests/test_main.c)
```

See [docs/host-testing.md](../../docs/host-testing.md#cmake-path) for details.

## Files

```
templates/host-test/
├── Makefile                # build + test runner
├── README.md               # this file
├── module/
│   └── mod_example.c       # example module (regulate function)
├── mocks/
│   ├── mock_host.h         # mock control API
│   └── mock_host.c         # mock implementations of host symbols
└── test/
    └── test_main.c         # test driver with ASSERT macro
```
