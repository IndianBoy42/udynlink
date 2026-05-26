test_data = {
    "desc": "Circular dependency detection test",
    "modules": [
        ["mod_self_dep.c", "--depends", "mod_self_dep"],
    ],
    "required": [r"^circular dep detected ok$"]
}
