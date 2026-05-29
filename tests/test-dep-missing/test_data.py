test_data = {
    "desc": "Missing dependency causes load failure",
    "modules": [
        ["mod_missing.c"],
    ],
    "total_loads": 1,
    "required": [
        r"^missing dep rejected: OK$",
    ]
}
