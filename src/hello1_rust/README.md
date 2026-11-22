# hello1_rust

[hello](../hello/README.md) in Rust, built by gccrs: writes `hello.txt`
("hello world") to the SD card and exits. It tests whether gccrs can make apps
for the unit ([docs/rust-gccrs.md](../../docs/rust-gccrs.md)).

- **Run:** `hello1_rust <sd_path>`
- **Build:** `make gccrs` once (the `mmi-gccrs` image, ~25 min), then
  `make hello1_rust` (from `dev/rivit_app/`). Once `build/hello1_rust` exists,
  `make sd` puts it in the card's `apps/`.
- **Source:** `main.rs` is `#![no_core]`. gccrs cannot compile `core`, so the
  app uses only raw pointers, arrays and libc through `extern "C"`.

## Verifies

- A gccrs-built binary loads and runs on the unit.
- Its libc calls work (`access`, `mkdir`, `fopen` ... `fsync`, `printf`).

## Results

- Built: ET_EXEC, NEEDED only `libc.so.2`, no gUSA sequences, 11,152 bytes.
- Not yet run on the car. Expected: exits 0, `var/hello.txt` contains
  "hello world", stdout `hello1_rust: wrote <sd>/var/hello.txt`.
