# PoC: wasm2c-generated add(i32,i32)->i32 running as udynlink module

test_data = {
    "desc": "wasm2c add(i32,i32)->i32 via udynlink",
    "modules": [["mod_wasm2c_add.c", "wasm-rt-udynlink.c", "add.c"]]
}
