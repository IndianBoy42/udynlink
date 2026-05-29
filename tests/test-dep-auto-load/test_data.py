test_data = {
    "desc": "Automatic dependency loading via udynlink_external_dep_load callback",
    "modules": [
        ["mod_math.c"],
        ["mod_app.c"],
    ],
    "required": [
        r"^dep count after auto-load = 2$",
        r"^call_math\(5,3\) = 15$",
        r"^call_math\(10,20\) = 222$",
        r"^auto-load: OK$",
    ]
}
