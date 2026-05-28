export fn hello(arg: i32) i32 {
    return arg + 42;
}
var counter: i32 = 0;
export fn get_counter() i32 {
    counter += 1;
    return counter;
}
export fn set_counter(val: i32) void {
    counter = val;
}
