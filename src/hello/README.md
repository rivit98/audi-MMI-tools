# hello

The toolchain smoke test: the first native binary built for the MMI 3G+.
It writes `hello.txt` ("hello world") to the SD card and exits. Nothing
appears on screen.

- **Run:** `hello <sd_path>`. If the path is missing, it tries the usual SD
  mount points.
- **Output:** `hello.txt` on the card.

## On the car

**Works**
- A binary from this toolchain runs on the unit and exits 0.
- It writes to the SD card: `hello.txt` came back with "hello world" in it.
- The unit reports `QNX localhost 6.3.2 2009/10/30-15:33:18EDT HB_SH7785
  shle`.
