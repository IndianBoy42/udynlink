# Test for empty init-array sentinel symbol stripping

test_data = {
    "desc": "Strip empty init-array sentinel symbols",
    "modules": [["mod_c.c"], ["mod_cpp.cpp"]],
    "required": [r"^strip-init-array c ok$", r"^cpp constructor$", r"^strip-init-array cpp ok$"],
    "skip_platforms": ["stm32f429_discovery"],
}
