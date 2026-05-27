(module
  (memory (export "memory") 1)
  (data (i32.const 0) "hello, world")
  (func (export "get_len") (result i32)
    i32.const 12
  )
  (func (export "get_char") (param i32) (result i32)
    local.get 0
    i32.load8_u
  )
)
