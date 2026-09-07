# wasm2c linear memory + data segment module, built through
# scripts/mkwasm2c-module (see test_driver.py)

test_data = {
    "desc": "wasm2c linear memory and data segment via udynlink, built by mkwasm2c-module",
    "wasm": "hello.wat",
    "required": [r"^hello: get_len\(\) = 12$", r"^hello: data\[0\] = 'h'$"],
}
