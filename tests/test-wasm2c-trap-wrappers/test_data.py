# wasm2c --wrappers-recover (K5 secondary mechanism): the generated export
# wrappers setjmp internally; a trapped wrapper returns the 0 sentinel and
# wasm_rt_last_trap() (exported) reports the code.  setjmp/longjmp are
# resolved from the host at load time.  --stack-depth-limit keeps the
# recursion test inside wasm trap territory.

test_data = {
    "desc": "wasm2c --wrappers-recover: trapped wrappers return the sentinel",
    "wasm": "trap.wat",
    "wasm_args": "--wrappers-recover --stack-depth-limit=64",
    "required": [
        r"^wrapper recovered: unreachable$",
        r"^wrapper recovered: div by zero$",
        r"^wrapper recovered: exhaustion$",
        r"^post-trap call OK: 42$",
    ],
}
