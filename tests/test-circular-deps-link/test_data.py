test_data = {
    "desc": "Circular dependency deferred link test",
    "modules": [
        ["mod_a.c", "--depends", "mod_b"],
        ["mod_b.c", "--depends", "mod_a"],
    ],
    "required": [
        r"^circular link ok$",
        r"^a_test=44$",
        r"^b_test=11$",
        r"^unload blocked a$",
        r"^unload blocked b$"
    ]
}
