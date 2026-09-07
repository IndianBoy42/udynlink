# wasm2c recursive factorial module, built through scripts/mkwasm2c-module
# (see test_driver.py)

test_data = {
    "desc": "wasm2c recursive factorial via udynlink, built by mkwasm2c-module",
    "wasm": "fac.wat",
    "required": [r"^fac: 5! = 120$"],
}
