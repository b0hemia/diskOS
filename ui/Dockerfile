# diskOS UI build environment - builds the pinned mipsel musl cross compiler from source, then builds `mq_ui`.
#
# The compiler is musl-cross-make at a fixed commit with toolchain/mcm-config.mak (GCC 11.2.0, musl 1.2.6,
# binutils 2.44), targeting the stock firmware's float ABI (MIPS32r2, FP64, NaN2008). musl-cross-make checks every
# downloaded source tarball against its own pinned hashes, so the toolchain cannot drift silently.
#
# A second build of the same recipe with toolchain/mcm-cxx.mak appended adds the C++ compiler HarfBuzz needs
# (HB_CROSS); the C compiler that builds the UI is unchanged.
#
# Build the image:   docker build -t diskos-ui-builder .        (from this ui/ directory; builds GCC twice: takes a while)
# Build the UI:       docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" diskos-ui-builder
#   The `-u` keeps the built mq_ui/*.o owned by you, not root. LVGL is vendored at ui/lvgl; FreeType 2.13.3 is vendored at ui/third_party.
#   The static mipsel `mq_ui` lands in the current directory, plus the helpers: make diskos-artdec diskos-bootprobe

FROM debian:12-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ca-certificates git wget xz-utils bzip2 patch rsync file \
    && rm -rf /var/lib/apt/lists/*

ARG MCM_URL=https://github.com/richfelker/musl-cross-make.git
ARG MCM_COMMIT=227df8b99103f9c59f6570babf892978e293082f
COPY toolchain/mcm-config.mak toolchain/mcm-cxx.mak /tmp/
RUN git clone "$MCM_URL" /tmp/mcm \
    && cd /tmp/mcm && git checkout --detach "$MCM_COMMIT" \
    && test "$(git rev-parse HEAD)" = "$MCM_COMMIT" \
    && cp /tmp/mcm-config.mak config.mak \
    && make -j"$(nproc)" && make install \
    && cd / && rm -rf /tmp/mcm
RUN git clone "$MCM_URL" /tmp/mcm \
    && cd /tmp/mcm && git checkout --detach "$MCM_COMMIT" \
    && test "$(git rev-parse HEAD)" = "$MCM_COMMIT" \
    && cat /tmp/mcm-config.mak /tmp/mcm-cxx.mak > config.mak \
    && make -j"$(nproc)" && make install \
    && cd / && rm -rf /tmp/mcm /tmp/mcm-config.mak /tmp/mcm-cxx.mak
ENV CROSS=/opt/mipsel-n2008-musl-cross/bin/mipsel-linux-musl-
ENV HB_CROSS=/opt/mipsel-n2008-cxx-musl-cross/bin/mipsel-linux-musl-

WORKDIR /src
# FreeType is built from the pruned in-tree sources with the same pinned compiler.
# Default: build mq_ui from the mounted ui/ tree. Fails clearly if the source isn't mounted.
CMD ["sh", "-c", "test -f Makefile || { echo 'mount the ui/ source at /src (see the Dockerfile header)'; exit 1; }; make CROSS=$CROSS HB_CROSS=$HB_CROSS && ls -l mq_ui"]
