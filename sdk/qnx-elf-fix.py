#!/usr/bin/env python3
"""qnx-elf-fix.py - make a linked executable loadable by QNX 6.3's ldqnx.

shle-nto-gcc runs this on every program it links. GNU ld can emit two
things the 2008-era QNX loader does not know:

  STB_GNU_UNIQUE  (binding 10) libstdc++.a defines some template statics
                  this way (std::string's _S_empty_rep_storage). ldqnx
                  treats the symbol as undefined: on 2026-10-25 hello8 died
                  at load with "unknown symbol: _ZNSs4_Rep20_S_empty_rep_
                  storageE". In an executable "unique" means nothing more
                  than global, so it becomes STB_GLOBAL.
  ELFOSABI_GNU    ld marks the file GNU/Linux when it has UNIQUE symbols;
                  set back to System V like the stock binaries.

Anything this cannot fix is an error (exit 1), so the build stops instead
of the car finding it: STT_GNU_IFUNC symbols, a PT_TLS segment (QNX 6.3
has no TLS).

usage: qnx-elf-fix.py <elf>      rewrites it in place, prints what it fixed
"""
import struct
import sys

STB_GLOBAL, STB_GNU_UNIQUE = 1, 10
STT_GNU_IFUNC = 10
SHT_SYMTAB, SHT_DYNSYM = 2, 11
PT_TLS = 7
EI_OSABI, ELFOSABI_SYSV, ELFOSABI_GNU = 7, 0, 3


def fix(data):
    """Fix data (a bytearray) in place; returns (notes, errors)."""
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        return [], ["not a 32-bit little-endian ELF"]
    notes, errors = [], []
    (e_phoff, e_shoff) = struct.unpack_from("<II", data, 28)
    (e_phentsize, e_phnum, e_shentsize, e_shnum) = struct.unpack_from("<HHHH", data, 42)

    for i in range(e_phnum):
        if struct.unpack_from("<I", data, e_phoff + i * e_phentsize)[0] == PT_TLS:
            errors.append("PT_TLS segment: thread_local / __thread, no TLS on QNX 6.3")

    shdrs = [struct.unpack_from("<IIIIIIIIII", data, e_shoff + i * e_shentsize)
             for i in range(e_shnum)]
    for _, sh_type, _, _, sh_offset, sh_size, sh_link, _, _, sh_entsize in shdrs:
        if sh_type not in (SHT_SYMTAB, SHT_DYNSYM) or sh_entsize == 0:
            continue
        kind = "dynsym" if sh_type == SHT_DYNSYM else "symtab"
        str_off = shdrs[sh_link][4]
        for off in range(sh_offset, sh_offset + sh_size, sh_entsize):
            st_name = struct.unpack_from("<I", data, off)[0]
            info = data[off + 12]
            bind, typ = info >> 4, info & 0xF
            if bind != STB_GNU_UNIQUE and typ != STT_GNU_IFUNC:
                continue
            end = data.index(b"\0", str_off + st_name)
            name = data[str_off + st_name:end].decode("ascii", "replace")
            if typ == STT_GNU_IFUNC:
                errors.append(f"{kind}: {name} is an IFUNC")
                continue
            data[off + 12] = (STB_GLOBAL << 4) | typ
            notes.append(f"{kind}: {name} UNIQUE -> GLOBAL")

    if data[EI_OSABI] == ELFOSABI_GNU:
        data[EI_OSABI] = ELFOSABI_SYSV
        notes.append("OS/ABI GNU -> System V")
    return notes, errors


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip().splitlines()[-1])
    path = sys.argv[1]
    with open(path, "rb") as f:
        data = bytearray(f.read())
    notes, errors = fix(data)
    for e in errors:
        print(f"qnx-elf-fix: {path}: {e}", file=sys.stderr)
    if errors:
        sys.exit(1)
    if notes:
        with open(path, "r+b") as f:
            f.write(data)
        for n in notes:
            print(f"qnx-elf-fix: {path}: {n}")


if __name__ == "__main__":
    main()
