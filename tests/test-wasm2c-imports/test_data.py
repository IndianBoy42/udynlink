# wasm2c imports via the symbol contract, built through scripts/mkwasm2c-module.
# The module imports env.host_add; the host firmware implements w2c_env_host_add
# and the loader binds it at load time.  The test also proves the loader rejects
# an unresolved import instead of loading a broken module.

test_data = {
    "desc": "wasm2c imports via symbol contract: calc calls host w2c_env_host_add",
    "wasm": "calc.wat",
    "required": [
        r"^unresolved import rejected$",
        r"^calc\(20, 3\) = 64$",
        r"^env user round-trip OK$",
    ],
}
