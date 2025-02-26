#!/bin/sh

# script for generating .deb control file

echo 'Package: camlet'
echo 'Version: '$(grep '#define VERSION' main.c | cut -d'"' -f2 | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' || exit 1)
cat <<EOF
Section: video
Priority: optional
Architecture: amd64
Essential: no
Maintainer: pommicket <pommicket@gmail.com>
Description: Take pictures and videos with a webcam
Depends: libsdl2-2.0-0 | libsdl2-dev, libv4l-0 | libv4l-dev, libudev1 | libudev-dev, libsodium23 | libsodium-dev, libfontconfig1 | libfontconfig-dev, libsdl2-ttf-2.0-0 | libsdl2-ttf-dev, libjpeg62-turbo | libjpeg-dev, libavcodec59 | libavcodec61 | libavcodec-dev, libavformat59 | libavformat61 | libavformat-dev
Homepage: https://github.com/pommicket/camlet
EOF
echo 'Installed-Size: '$(wc -c release/camlet camlet.png camlet.desktop | tail -n1 | cut -d' ' -f1 || exit 1)
