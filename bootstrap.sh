#!/bin/bash
autoreconf --install
PKG_CONFIG=arm-linux-pkg-config \
CFLAGS="-fno-omit-frame-pointer -funwind-tables -Wl,--no-merge-exidx-entries" \
./configure --host=arm-linux --with-cpu=neon --with-audio=alsa --with-cpu=neon --with-sysroot=/chroot --prefix= --disable-static --enable-shared
make -j8
make DESTDIR=/chroot install
