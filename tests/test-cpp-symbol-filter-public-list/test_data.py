test_data = {
    "desc": "--public-symbols=a,c,test with --strip-non-public-syms leaves only a, c, and test addressable by the host",
    "mkmodule_args": "--public-symbols=a,c,test --strip-non-public-syms --strip-mangled-syms",
    "modules": [["mod_public_list.cpp"]],
    "required": [
        r"^a is exported$",
        r"^c is exported$",
        r"^b is NOT exported$",
        r"^d is NOT exported$",
    ],
}
