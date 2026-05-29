test_data = {
    "desc": "Stub deduplication: same function imported by two modules shares one stub",
    "modules": [
        ["mod_math.c"],
        ["mod_a.c"],
        ["mod_b.c"],
    ],
    "required": [
        r"^call_add_a\(3,4\) = 1007$",
        r"^call_add_b\(5,6\) = 2011$",
        r"^pool used = 10 \(1 stub shared by 2 modules\)$",
        r"^stub dedup: OK$",
    ]
}
