test_data = {
    "desc": "C++ module exercising every GCC symbol emission pattern (functions, vtable, templates, inline, weak, init/fini) loads and runs correctly",
    "mkmodule_args": "",
    "modules": [["mod_all.cpp"]],
    # Each load mode must produce these prints.  3 load modes = 3 matches
    # of each regex (total_loads default = 3).
    "required": [r"^ctor1$", r"^ctor2$", r"^test ok mode \d+$"],
}
