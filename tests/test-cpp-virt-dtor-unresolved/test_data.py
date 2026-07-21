test_data = {
    "desc": "C++ module with virtual destructor + pure-virtual base loads and runs (host supplies C++ ABI stubs)",
    "modules": [["mod_virt_dtor.cpp"]],
    "required": [r"^test ok mode \d+$"],
}
