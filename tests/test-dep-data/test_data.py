test_data = {
    "desc": "Cross-module data symbol resolution (no thunk, direct address)",
    "modules": [
        ["mod_math.c"],
        ["mod_app.c"],
    ],
    "required": [
        r"^g_shared_counter = 42$",
        r"^cross-module data: OK$",
    ]
}
