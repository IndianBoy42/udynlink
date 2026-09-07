# wasm2c add module, built through scripts/mkwasm2c-module (see test_driver.py)

test_data = {
    "desc": "wasm2c add(i32,i32) via udynlink, built by mkwasm2c-module",
    "wasm": "add.wat",
    "required": [r"^add: 30 \+ 70 = 100$"],
}
