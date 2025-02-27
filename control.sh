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
Depends: libsdl2-2.0-0, libv4l-0, libudev1, libsodium23, libfontconfig1, libsdl2-ttf-2.0-0, libjpeg62-turbo, libvpx7, libogg0, libvorbisenc2
Homepage: https://github.com/pommicket/camlet
EOF
echo 'Installed-Size: '$(expr $(wc -c release/camlet camlet.png camlet.desktop | tail -n1 | cut -d' ' -f1 || exit 1) / 1024)
