;; wasm2c trap-policy test module: an unreachable bomb, an integer
;; division by zero, unbounded recursion (stack-depth limit turns it into
;; WASM_RT_TRAP_EXHAUSTION), and a well-behaved function proving the
;; device keeps running after recovered traps.
(module
  (func $rec (export "spiral") (param i32) (result i32)
    (if (result i32) (i32.eqz (local.get 0))
      (then (i32.const 0))
      (else (i32.add (local.get 0) (call $rec (i32.sub (local.get 0) (i32.const 1)))))))
  (func (export "bomb") (result i32) (unreachable))
  (func (export "divzero") (param i32 i32) (result i32)
    (i32.div_s (local.get 0) (local.get 1)))
  (func (export "safe") (result i32) (i32.const 42)))
