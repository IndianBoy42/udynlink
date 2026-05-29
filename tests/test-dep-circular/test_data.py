test_data = {
    "desc": "Circular dependency detection between two modules",
    "modules": [
        ["mod_a.c"],
        ["mod_b.c"],
    ],
    "total_loads": 1,
    "required": [
        r"^both modules loaded despite circular dep$",
        r"^circular dep deferred and patched: OK$",
    ]
}
