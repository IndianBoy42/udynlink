;; wasm2c dynamic-memory test: the module grows its linear memory past the
;; initial allocation, proving the realloc hook receives (and preserves) the
;; old buffer contents.  --memory=dynamic routes allocations through the
;; host-provided wasm_pool_malloc/wasm_pool_free static pool.
(module
  (memory $m 1 4)
  (func (export "grow") (param i32) (result i32) (memory.grow (local.get 0)))
  (func (export "size") (result i32) (memory.size))
  (func (export "poke") (param i32 i32) (i32.store (local.get 0) (local.get 1)))
  (func (export "peek") (param i32) (result i32) (i32.load (local.get 0))))
