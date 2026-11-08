# Additions for the C++ cross compiler that builds HarfBuzz (C++): the same recipe as mcm-config.mak with C++
# enabled and its own prefix. Append it to that file: cat mcm-config.mak mcm-cxx.mak > config.mak (../Dockerfile
# does this). Pass the result to make as HB_CROSS=/opt/mipsel-n2008-cxx-musl-cross/bin/mipsel-linux-musl-
OUTPUT = /opt/mipsel-n2008-cxx-musl-cross
GCC_CONFIG := $(filter-out --enable-languages=%,$(GCC_CONFIG)) --enable-languages=c,c++
