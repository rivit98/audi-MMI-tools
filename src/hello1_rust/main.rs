// hello1_rust - hello (src/hello/hello.c) in Rust, compiled by gccrs: write
// "hello world" to var/hello.txt on the SD card and exit.
//
// gccrs cannot compile `core` (docs/rust-gccrs.md), so this crate is
// #![no_core]: it declares the few lang items the compiler needs itself and
// talks to QNX's libc.so.2 through `extern "C"` only. No slices, no
// formatting, no Option: just raw pointers, arrays and C strings.
//
// run.sh passes the SD card mount point as argv[1]. If it is missing we
// probe the usual MMI mount points (slot 1/2, partition t12/t11).
#![feature(no_core, lang_items, rustc_attrs, intrinsics)]
#![no_core]

#[lang = "sized"]
pub trait Sized {}

#[lang = "copy"]
pub trait Copy {}

// QNX 6.3.2 libc (int = i32, char = i8 on SH4, mode_t = u32).
pub enum FILE {}
extern "C" {
    fn access(path: *const i8, mode: i32) -> i32;
    fn mkdir(path: *const i8, mode: u32) -> i32;
    fn fopen(path: *const i8, mode: *const i8) -> *mut FILE;
    fn fputs(s: *const i8, f: *mut FILE) -> i32;
    fn fflush(f: *mut FILE) -> i32;
    fn fileno(f: *mut FILE) -> i32;
    fn fsync(fd: i32) -> i32;
    fn fclose(f: *mut FILE) -> i32;
    fn snprintf(buf: *mut i8, n: u32, fmt: *const i8, ...) -> i32;
    fn printf(fmt: *const i8, ...) -> i32;
    fn fprintf(f: *mut FILE, fmt: *const i8, ...) -> i32;
    fn strerror(e: i32) -> *const i8;
    fn __get_errno_ptr() -> *mut i32;
    // stderr is &_Stdout[2] in QNX's <stdio.h>; fdopen(2) is simpler here.
    fn fdopen(fd: i32, mode: *const i8) -> *mut FILE;
}

// Pointer arithmetic without core: the compiler's own intrinsic.
extern "rust-intrinsic" {
    fn offset<T>(p: *const T, n: isize) -> *const T;
}

const W_OK: i32 = 2;

// Builtin macros exist only where core declares them; without core, here.
#[rustc_builtin_macro]
macro_rules! concat {
    () => {};
}

// A NUL-terminated literal as a C string.
macro_rules! c {
    ($s:expr) => {
        concat!($s, "\0") as *const str as *const i8
    };
}

unsafe fn errstr() -> *const i8 {
    strerror(*__get_errno_ptr())
}

unsafe fn find_sd() -> *const i8 {
    let mounts: [*const i8; 4] = [
        c!("/mnt/sdcard10t12"),
        c!("/mnt/sdcard20t12"),
        c!("/mnt/sdcard10t11"),
        c!("/mnt/sdcard20t11"),
    ];
    let mut i: usize = 0;
    while i < 4 {
        if access(mounts[i], W_OK) == 0 {
            return mounts[i];
        }
        i = i + 1;
    }
    0 as *const i8
}

#[no_mangle]
pub unsafe extern "C" fn main(argc: i32, argv: *const *const i8) -> i32 {
    let err = fdopen(2, c!("w"));
    let sd = if argc > 1 { *offset(argv, 1) } else { find_sd() };
    let mut buf: [i8; 256] = [0; 256];
    let path = &mut buf as *mut [i8; 256] as *mut i8;

    if sd as usize == 0 {
        fprintf(err, c!("hello1_rust: no writable SD card found\n"));
        return 1;
    }
    snprintf(path, 256, c!("%s/var"), sd);
    mkdir(path, 0o777);
    snprintf(path, 256, c!("%s/var/hello.txt"), sd);

    let f = fopen(path, c!("w"));
    if f as usize == 0 {
        fprintf(err, c!("hello1_rust: fopen %s: %s\n"), path, errstr());
        return 1;
    }
    fputs(c!("hello world\n"), f);
    fflush(f);
    fsync(fileno(f));
    if fclose(f) != 0 {
        fprintf(err, c!("hello1_rust: fclose %s: %s\n"), path, errstr());
        return 1;
    }

    printf(c!("hello1_rust: wrote %s\n"), path);
    0
}
