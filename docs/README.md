# udynlink Documentation

## Design Principles

All of udynlink is guided by seven principles. When evaluating a feature or change, check it against these:

| Principle | What it means |
|-----------|---------------|
| **Simplicity** | Minimal API surface: load, call, unload. No DSLs, no code generation, no macro magic. |
| **Unopinionated** | No imposed lifecycle, event loop, threading model, or memory strategy. |
| **Usage-agnostic** | Bootloaders, plugins, OTA patching, scripting FFI, LGPL compliance — all equally first-class. |
| **Flexible** | Three load modes, non-contiguous image loading, low-level relocation primitives, deferred symbols. |
| **Minimal overhead** | No heap allocation when the host provides a buffer. No hidden state. `udynlink_module_t` is 24 bytes. |
| **Zero-cost optional features** | Dependency system, hash resolution, call ergonomics, symbol cache — separate headers, zero cost if unused. |
| **Library, not framework** | You call udynlink; it never calls you back except through the five callbacks you implement. |

## Guides

| Guide | Description |
|-------|-------------|
| [How It Works](how-it-works.md) | Technical deep-dive: PIC model, LOT/r9 mechanism, relocations, binary format, ABI versioning |
| [Integrating as a Host](integrating-as-host.md) | Adding udynlink to your firmware, implementing callbacks, symbol tables, lifecycle, thread safety, dependency system, non-contiguous loading, C++ API |
| [Writing Modules](writing-modules.md) | Creating loadable C/C++ modules, consuming symbols, cross-module calls, mkmodule reference, dependency declarations |
| [Protobuf Modules](protobuf-modules.md) | Compiling `.proto` files into parse/write UDLM modules via `scripts/proto2module`; 1-module-per-struct overhead analysis |
| [API Reference](api-reference.md) | Complete reference for all public functions, structs, macros, callbacks, and C++ API |
| [Examples](examples.md) | Working code examples for every major feature |
| [Testing Guide](testing.md) | Running tests, adding test cases and platforms, debugging |
| [Host Testing](host-testing.md) | Testing module logic on your development machine without QEMU or ARM tools; mocking patterns and templates |
| [Host Sanitizer & Fuzz Testing](fuzzing.md) | Sanitizer (ASan+UBSan) and libFuzzer harnesses that exercise the udynlink loader itself on the host; seed corpus, crash triage |
| [Thread Safety](thread-safety.md) | Modules in multithreaded hosts (FreeRTOS/Zephyr): execution vs lifecycle, r9 preemption safety, same-module multi-thread rules, nested calls |

## Internal Notes

- [QEMU Testing Strategy](QEMU_TESTING_STRATEGY.md) — research notes on semihosting vs UART vs RTT for QEMU output
- [Mk RTOS Features for Porting](mk-rtos-features-for-porting.md) — analysis of Mk RTOS's hash table and dependency tracking for feature comparison
