;; wasm2c imports test: the module calls the host through the env import
;; namespace (K2 symbol contract).  calc(20, 3) = 20*3 + host_add(3, 1) = 64.
(module
  (import "env" "host_add" (func $host_add (param i32 i32) (result i32)))
  (func (export "calc") (param i32 i32) (result i32)
    (i32.add (i32.mul (local.get 0) (i32.const 3))
             (call $host_add (local.get 1) (i32.const 1)))))
