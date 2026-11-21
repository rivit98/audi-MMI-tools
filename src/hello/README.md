# hello

Toolchain smoke test: writes `hello.txt` ("hello world") to the SD card and exits.

- **Run:** `hello <sd_path>`

## Verifies

- A binary from this toolchain loads and runs on the unit.
- An app can write to the SD card.

## Results

- **Works:** exits 0, `var/hello.txt` contains "hello world".
- The unit reports `QNX localhost 6.3.2 2009/10/30-15:33:18EDT HB_SH7785 shle`.

