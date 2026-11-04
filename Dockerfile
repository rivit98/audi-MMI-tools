# mmi-sdk - cross-compiler for the Audi MMI 3G+ (QNX 6.3.2, SH7785 / SH4 LE).
#
# Built by `make sdk` (see Makefile), which passes the inputs as narrow
# named build contexts so the 100 MB firmware tree is never sent to Docker:
#   qnx-libc-public  third_party/openqnx/trunk/lib/c/public
#   qnx-sys-public   third_party/openqnx/trunk/services/system/public
#   qnx-crt-sh       third_party/openqnx/trunk/lib/c/startup/sh
#   fwlibs           the device .so files to stub (staged by make)
# GLES 1.1 / EGL headers are the standard Khronos ones from Debian's
# libgles-dev/libegl-dev; build-sysroot.sh copies them into the sysroot.
# sid for gcc-16-sh4-linux-gnu: bookworm's GCC 12.2 miscompiles at -O1+
# (docs/gcc-sh4-treg-bug.md). The versioned package pins the major version;
# the canary in the Makefile catches a regression.
FROM debian:sid-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        gcc-16-sh4-linux-gnu g++-16-sh4-linux-gnu binutils-sh4-linux-gnu make python3 file \
        gcc g++ libc6-dev liblz4-dev \
        libgles-dev libegl-dev \
 && rm -rf /var/lib/apt/lists/*

COPY --from=qnx-libc-public . /tmp/in/qnx-libc-public/
COPY --from=qnx-sys-public  . /tmp/in/qnx-sys-public/
COPY --from=qnx-crt-sh      . /tmp/in/qnx-crt-sh/
COPY --from=fwlibs          . /tmp/in/fwlibs/
COPY sdk/ /opt/mmi-sdk/bin/

RUN chmod +x /opt/mmi-sdk/bin/shle-nto-gcc /opt/mmi-sdk/bin/build-sysroot.sh \
 && ln -s shle-nto-gcc /opt/mmi-sdk/bin/shle-nto-g++ \
 && /opt/mmi-sdk/bin/build-sysroot.sh /tmp/in /opt/mmi-sdk/sysroot \
 && rm -rf /tmp/in

ENV PATH=/opt/mmi-sdk/bin:$PATH
WORKDIR /src
