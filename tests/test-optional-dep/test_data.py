test_data = {
    "desc": "Optional dependency graceful degradation test",
    "modules": [
        ["mod_consumer.c", "--depends", "mod_logging"],
        ["mod_logging.c"],
    ],
    "required": [
        r"^optional degraded ok$",
        r"^optional linked ok$",
        r"^consumer value=0$",
        r"^consumer value=123$"
    ]
}
