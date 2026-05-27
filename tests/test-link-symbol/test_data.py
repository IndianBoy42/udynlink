test_data = {
    "desc": "Direct symbol patching via udynlink_link_symbol test",
    "modules": [
        ["mod_link_sym.c"],
    ],
    "required": [
        r"^link symbol ok$",
        r"^real_service called$",
        r"^mock_service called$"
    ]
}
