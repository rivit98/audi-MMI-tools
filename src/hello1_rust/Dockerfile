# syntax=docker/dockerfile:1.7
# mmi-gccrs - gccrs (GCC's Rust front end) as an SH4 cross compiler, on top
# of mmi-sdk (`make sdk` first). Built by `make -C src/hello1_rust image`.
#
# Debian ships gccrs only for x86-64 (gccrs-16-x86-64-linux-gnu), so stage 1
# builds GCC 16.2's compilers (all-gcc: driver, cc1, crab1; no target
# libraries) for --target=sh4-linux-gnu. Same GCC version as mmi-sdk's
# sh4-linux-gnu-gcc-16, so its libgcc and crt objects fit what crab1 emits.
# crab1 links two small Rust helpers (libformat_parser, ffi-polonius), built
# with the host's cargo from crates vendored in the tarball (--offline).
FROM debian:sid-slim AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential libgmp-dev libmpfr-dev libmpc-dev zlib1g-dev \
        binutils-sh4-linux-gnu rustc cargo xz-utils file python3 \
 && rm -rf /var/lib/apt/lists/*

ARG GCC_VER=16.2.0
ADD --checksum=sha256:e6738e29597f733270731aa90600f37ffdc045079dfc27ec7e8192cc81085c3e \
    https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.xz /tmp/gcc.tar.xz

RUN mkdir /tmp/src /tmp/obj && tar xf /tmp/gcc.tar.xz -C /tmp/src --strip-components=1 \
 && cd /tmp/obj \
 && /tmp/src/configure --prefix=/opt/gccrs --target=sh4-linux-gnu \
        --program-prefix=sh4-linux-gnu- \
        --with-as=/usr/bin/sh4-linux-gnu-as --with-ld=/usr/bin/sh4-linux-gnu-ld \
        --enable-languages=c,rust --disable-bootstrap --disable-multilib \
        --disable-nls --disable-shared --disable-threads --without-headers \
        --with-cpu=sh4a --with-endian=little \
 && make -j"$(nproc)" all-gcc \
 && make install-gcc \
 && rm -rf /tmp/src /tmp/obj /tmp/gcc.tar.xz

FROM mmi-sdk
COPY --from=build /opt/gccrs /opt/gccrs
COPY shle-nto-gccrs /opt/mmi-sdk/bin/
RUN chmod +x /opt/mmi-sdk/bin/shle-nto-gccrs
