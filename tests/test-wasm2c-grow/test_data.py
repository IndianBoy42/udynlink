# wasm2c dynamic memory: memory.grow drives wasm_rt_mem_realloc through the
# host's static pool (--malloc/--free hooks).  Proves heap-less viability and
# the realloc old-size contract: contents written before a grow must survive
# it (the D4 regression), and grows past max_pages must fail with -1.

test_data = {
    "desc": "wasm2c memory.grow via static-pool allocator hooks",
    "wasm": "grow.wat",
    "wasm_args": "--memory=dynamic --custom-page-size=1024 --malloc=wasm_pool_malloc --free=wasm_pool_free",
    "required": [
        r"^grow: 1 -> 2 pages, size 2$",
        r"^mem\[100\] = 12345678$",
        r"^content preserved after realloc$",
        r"^grow beyond max rejected$",
        r"^post-max grow rejected$",
    ],
}
