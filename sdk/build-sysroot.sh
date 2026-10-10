#!/bin/sh
# build-sysroot.sh - assemble the QNX 6.3 SH4 sysroot inside the SDK image.
#
#   $1  inputs dir with:  qnx-libc-public/  qnx-sys-public/  qnx-crt-sh/  fwlibs/
#   $2  output sysroot (e.g. /opt/mmi-sdk/sysroot)
set -eu

IN="$1"
OUT="$2"
CC=sh4-linux-gnu-gcc
ARCH="-m4 -ml -Wa,--isa=sh4"
HERE="$(cd "$(dirname "$0")" && pwd)"

mkdir -p "$OUT/usr/include" "$OUT/usr/lib" /tmp/stubs

# 1. Headers: openqnx libc public + kernel/system public (sys/neutrino.h,
#    sh/*.h, ...). Both have a sys/ dir; they merge without conflicts.
cp -r "$IN/qnx-libc-public/." "$OUT/usr/include/"
cp -r "$IN/qnx-sys-public/." "$OUT/usr/include/"

# openqnx is QNX 6.4.1, whose compiler_gnu.h gives _Int64t/_Uint64t 8-byte
# alignment. QNX 6.3.2 (what built the MMI's libraries) did not: its
# gf_surface_info_t, which holds a _Uint64t, is 68 bytes (STABS of
# qnx-audi). Drop the attribute so 64-bit types match the target ABI.
sed -i 's/ __attribute__((__aligned__(8)));/;/' \
    "$OUT/usr/include/sys/compiler_gnu.h"
grep -q '__aligned__(8)' "$OUT/usr/include/sys/compiler_gnu.h" && {
    echo "compiler_gnu.h: 64-bit alignment patch did not apply" >&2; exit 1; }

# 1b. Graphics headers: Khronos GLES 1.1 + EGL (from Debian's libgles-dev /
#     libegl-dev, compiled with EGL_NO_PLATFORM_SPECIFIC_TYPES so native
#     types are void*) and our reconstructed QNX gf headers (sdk/include).
cp -r /usr/include/GLES /usr/include/EGL /usr/include/KHR "$OUT/usr/include/"
cp -r "$HERE/include/." "$OUT/usr/include/"

# 2. Startup objects, assembled from openqnx lib/c/startup/sh.
#    crt1/crti/crtn are .S (need cpp), crtbegin/crtend are plain .s.
for f in crt1 crti crtn; do
    $CC $ARCH -c "$IN/qnx-crt-sh/$f.S" -o "$OUT/usr/lib/$f.o"
done
for f in crtbegin crtend; do
    $CC $ARCH -c "$IN/qnx-crt-sh/$f.s" -o "$OUT/usr/lib/$f.o"
done

# 3. Our init/fini array runners (see crtarray.c).
MMI_SYSROOT="$OUT" "$HERE/shle-nto-gcc" -O2 -c "$HERE/crtarray.c" \
    -o "$OUT/usr/lib/crtarray.o"

# 4. Link-time stubs for the MMI's real shared libraries.
#    lib<name>.so.N  ->  stub lib<name>.so  with SONAME lib<name>.so.N
for so in "$IN"/fwlibs/*.so.*; do
    base="$(basename "$so")"
    name="${base%%.so.*}"
    soname="$(python3 -I "$HERE/mkstubs.py" "$so" "/tmp/stubs/$name.S")"
    $CC $ARCH -shared -nostdlib -Wl,--hash-style=sysv \
        -Wl,-soname,"$soname" -o "$OUT/usr/lib/$name.so" "/tmp/stubs/$name.S"
    echo "stub: $name.so  (SONAME $soname)"
done

rm -rf /tmp/stubs
echo "sysroot ready: $OUT"
