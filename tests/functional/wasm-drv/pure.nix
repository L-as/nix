derivation {
  name = "wasm-test-pure";
  system = "wasm";
  __contentAddressed = true;
  outputs = [ "out" ];
  builder = builtins.toFile "wasm-test-pure.wat" ''
    (module
      (import "" "log" (func $log (param i32 i32)))
      (import "" "fail" (func $fail))
      (import "" "mktemp" (func $mktemp (result i32)))
      (import "" "close" (func $close (param i32)))
      (import "" "read" (func $read (param i32 i32 i32 i32)))
      (import "" "write" (func $write (param i32 i32 i32 i32)))
      (import "" "size" (func $size (param i32) (result i32)))
      (import "" "resize" (func $resize (param i32 i32)))
      (import "" "add_dumb" (func $add_dumb (param i32 i32 i32 i32 i32 i32) (result i32)))
      (import "" "output" (func $output (param i32 i32 i32)))

      (memory (export "memory") 1)
      (data (i32.const 0) "Hello, World!\nHowdy!\nmywasmoutput")

      (func (export "build")
        (local i32)
        call $mktemp
        local.set 0
        i32.const 21
        i32.const 12
        i32.const 0
        i32.const 1
        i32.const 1
        local.get 0
        call $add_dumb
        local.set 0
        i32.const 27
        i32.const 3
        local.get 0
        call $output
        return
        )
    )
  '';
}
/*
derivation {
  name = "sh-hello";
  system = "x86_64-linux";
  builder = "/bin/sh";
  args = [ "-c" "echo" "hello" ];
}
*/
