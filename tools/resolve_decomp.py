#!/usr/bin/env python3
# Post-process a DumpDecomp.java output against its ELF (docs/rear-camera.md 12):
#   python3 -I tools/resolve_decomp.py <elf> <in.c> <out.c>
# - PTR_s_<label>_<addr>       -> "the full C string" the pool slot points to
#   (Ghidra truncates string labels, so log/trace format strings are otherwise
#   unsearchable)
# - (code *)PTR_FUN_<addr>     -> (code *)FUN_<target> (call through a pool slot)
# Segments come from the ELF program headers (PT_LOAD); works on binaries
# without section headers.
import re
import struct
import sys

elf_path, src, dst = sys.argv[1:4]
with open(elf_path, 'rb') as f:
    d = f.read()
if d[:4] != b'\x7fELF' or d[4] != 1 or d[5] != 1:
    sys.exit('need a 32-bit little-endian ELF')
e_phoff, = struct.unpack_from('<I', d, 0x1c)
e_phentsize, e_phnum = struct.unpack_from('<HH', d, 0x2a)
segs = []
text_lo, text_hi = None, None
for i in range(e_phnum):
    p_type, p_off, p_va, _, p_filesz, _, p_flags, _ = struct.unpack_from(
        '<8I', d, e_phoff + i * e_phentsize)
    if p_type == 1:  # PT_LOAD
        segs.append((p_va, p_off, p_filesz))
        if p_flags & 1:  # executable
            text_lo, text_hi = p_va, p_va + p_filesz


def rd(va, n):
    for base, off, sz in segs:
        if base <= va < base + sz:
            o = off + va - base
            return d[o:o + min(n, base + sz - va)]
    return None


def u32(va):
    b = rd(va, 4)
    return struct.unpack('<I', b)[0] if b and len(b) == 4 else None


def cstr(va):
    b = rd(va, 300)
    if not b:
        return None
    s = b.split(b'\0')[0].decode('latin1')
    return s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')


cache = {}


def sub_s(m):
    a = int(m.group(2), 16)
    if a not in cache:
        p = u32(a)
        cache[a] = cstr(p) if p else None
    s = cache[a]
    return f'"{s}"' if s is not None else m.group(0)


def sub_f(m):
    p = u32(int(m.group(1), 16))
    if p and text_lo is not None and text_lo <= p < text_hi:
        return f'(code *)FUN_{p:08x}'
    return m.group(0)


with open(src, encoding='latin1') as f, open(dst, 'w', encoding='latin1') as o:
    for line in f:
        line = re.sub(r'PTR_s_(\w*?)_([0-9a-f]{8})\b', sub_s, line)
        line = re.sub(r'\(code \*\)PTR_FUN_([0-9a-f]{8})', sub_f, line)
        o.write(line)
