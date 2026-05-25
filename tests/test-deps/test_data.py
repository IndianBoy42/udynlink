test_data = {
    "desc": "Module dependency tracking test",
    "modules": [
        ["mod_provider.c"],
        ["mod_consumer.c", "--depends", "mod_provider"],
    ],
    "required": [r"^dep ok$"]
}
