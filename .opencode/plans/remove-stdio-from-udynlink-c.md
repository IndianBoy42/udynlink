# Plan: Remove `#include <stdio.h>` from `udynlink.c`

## Goal
Remove the `#include <stdio.h>` dependency from `udynlink/udynlink.c` (marked by a `// TODO: remove next` comment at line 6-7). This is an embedded MCU library and should not depend on stdio.

## Current State Analysis

### Source File: `udynlink/udynlink.c`
- **Line 6-7**: `// TODO: remove next` followed by `#include <stdio.h>`
- **Line 5**: Already includes `<stdarg.h>`, which provides `va_list` (the only C standard type that might be needed)
- **Debug output**: All output flows through `udynlink_external_vprintf` (declared in `udynlink_externals.h`), not stdio functions
- **Actual stdio usage in this file**: **None**. No `printf`, `snprintf`, `fprintf`, `stdout`, `stderr`, or any other stdio calls exist in `udynlink.c`.

### Why This Is Safe
The file includes `<stdio.h>` historically but no longer uses it. The `va_list` required by `udynlink_external_vprintf` comes from `<stdarg.h>`, which is already present. `udynlink.c` does not call any stdio functions directly.

### Impact on Host Implementations
The test host (`tests/qemu_host/src/main.c`) implements `udynlink_external_vprintf` by calling `vprintf`, which is a *host-side* implementation detail. Removing `#include <stdio.h>` from the core library does **not** affect host implementations that need stdio for their debug output.

## Proposed Change

1. **Remove the include and TODO comment** from `udynlink/udynlink.c`:
   ```c
   // TODO: remove next
   #include <stdio.h>
   ```
   This is a 2-line deletion.

2. **Verify compilation** by running the full test suite:
   ```bash
   cd tests
   python3 test_driver.py
   ```
   The test driver compiles each test case twice (`-O0` and `-Os`), builds the QEMU host firmware, and validates output.

## Open Questions for User

1. **Batch with other trivial cleanups?** `udynlink.h` has two known typos (`"ownershsip"` → `"ownership"`, `"correponding"` → `"corresponding"`). These are similarly trivial. Should we fix them in the same commit, or keep this change atomic and isolated?

2. **Are there downstream consumers** of this library that might have build systems that implicitly relied on `udynlink.c` transitively providing `<stdio.h>` via its include? (Unlikely for a self-contained embedded library, but worth confirming.)

## Execution Plan

| Step | Task | Agent | Notes |
|------|------|-------|-------|
| 1 | Delete `#include <stdio.h>` and TODO comment | `quick` | Trivial, no reasoning needed |
| 2 | Run `tests/test_driver.py` to verify all tests pass | `shell` | Must pass both `-O0` and `-Os` for all test cases |
| 3 | Review results | (human) | Confirm no regressions |

## Expected Deliverable
- A single commit removing the unused `#include <stdio.h>` (and optionally typo fixes)
- Clean test suite run confirming no compilation or runtime regressions
