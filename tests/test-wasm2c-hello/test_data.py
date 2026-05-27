# wasm2c linear memory + data segment test

test_data = {
    "desc": "wasm2c static linear memory with data segment via udynlink",
    "modules": [["-DWASM_RT_STATIC_MEMORY", "-DWASM_RT_INITIAL_PAGES=1", "mod_wasm2c_hello.c", "wasm-rt-udynlink.c", "hello.c"]]
}
