test_data = {
    "desc": "Cross-module function calls via thunks",
    "modules": [
        ["mod_math.c"],
        ["mod_app.c"],
    ],
    "required": [
        r"^eager thunk: OK$",
        r"^math_add\(1, 2\) = 3$",
        r"^math_mul\(3, 4\) = 12$",
        r"^call_math\(5, 3\): sum=8, prod=15$",
        r"^call_math\(10, 20\): sum=30, prod=200$",
        r"^pool used = 10$",
        r"^cross-module thunks: OK$",
    ]
}
