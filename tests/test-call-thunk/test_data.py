test_data = {
    "desc": "Standalone call thunks via udynlink_thunk_make_call",
    "modules": [
        ["mod_math.c"],
    ],
    "required": [
        r"^thunked math_add\(1, 2\) = 3$",
        r"^thunked math_mul\(3, 4\) = 12$",
        r"^pool used = 20 \(2 stubs, 1 gateway\)$",
        r"^pool gateway_top = 494 \(1 gateway\)$",
        r"^stub dedup: math_add thunk reused, pool used = 20$",
        r"^call thunk: OK$",
    ]
}
