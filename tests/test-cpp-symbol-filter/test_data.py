test_data = {
    "desc": "C++ symbol-table filter strips hidden internals without breaking extern C exports",
    "mkmodule_args": "--strip-hidden-syms",
    "modules": [["mod_cpp_filter.cpp"]],
    "required": [r"^filtered \d+$"],
}
