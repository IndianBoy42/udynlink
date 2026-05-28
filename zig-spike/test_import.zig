extern fn host_printf(fmt: [*:0]const u8, ...) void;
export fn greet() void {
    host_printf("hello");
}
