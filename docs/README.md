# udynlink Documentation

| Guide | Description |
|-------|-------------|
| [How It Works](how-it-works.md) | Technical deep-dive: PIC model, LOT/r9 mechanism, relocations, binary format, ABI versioning |
| [Integrating as a Host](integrating-as-host.md) | Adding udynlink to your firmware, implementing callbacks, symbol tables, lifecycle, thread safety |
| [Writing Modules](writing-modules.md) | Creating loadable C/C++ modules, consuming symbols, dependencies, mkmodule reference |
| [API Reference](api-reference.md) | Complete reference for all public functions, structs, macros, and callbacks |
| [Examples](examples.md) | Working code examples for every major feature |
| [Testing Guide](testing.md) | Running tests, adding test cases and platforms, debugging |

## Internal Notes

- [QEMU Testing Strategy](QEMU_TESTING_STRATEGY.md) — research notes on semihosting vs UART vs RTT for QEMU output
- [Mk RTOS Features for Porting](mk-rtos-features-for-porting.md) — analysis of Mk RTOS's hash table and dependency tracking for feature comparison
