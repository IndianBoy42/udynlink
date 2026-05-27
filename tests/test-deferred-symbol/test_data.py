test_data = {
    "desc": "Deferred symbol resolution test",
    "modules": [
        ["mod_defer_sym.c"],
    ],
    "required": [
        r"^deferred load ok$",
        r"^deferred before=1$",
        r"^deferred after=2$",
        r"^late_init called$"
    ]
}
