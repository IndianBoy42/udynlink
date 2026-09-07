# wasm2c recoverable traps (K5 primary mechanism): the host registers a
# one-shot recovery point (wasm_rt_set_recovery) before each module call;
# wasm_rt_trap longjmps there instead of halting.  longjmp is resolved
# from the host at load time.  --stack-depth-limit turns runaway wasm
# recursion into a recoverable WASM_RT_TRAP_EXHAUSTION.

test_data = {
    "desc": "wasm2c recoverable traps: host-registered recovery point survives traps",
    "wasm": "trap.wat",
    "wasm_args": "--recoverable-traps --stack-depth-limit=64",
    "required": [
        r"^trap recovered: unreachable$",
        r"^trap recovered: div by zero$",
        r"^trap recovered: exhaustion$",
        r"^post-trap call OK: 42$",
    ],
}
