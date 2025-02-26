# camlet

Camlet is a webcam application for Linux.
It features

- Proper handling of disconnecting/reconnecting devices
- Capture JPEG and PNG images and MP4 (H264+AAC) videos
- Full selection of resolutions available from camera
- Remembers settings across program launches

# Debugging

If you find a bug, please create an issue on GitHub; it will be helpful to have
your settings and log file, located in `~/.config/camlet`.

You can try deleting/renaming your settings file as a temporary solution, to reset camlet to its default settings.

# Building from source

camlet requires meson-build, a C compiler, and the development libraries
for SDL2, SDL2\_ttf, GL (headers only), v4l2, udev, sodium, jpeglib (from IJG), avcodec, avformat, pulseaudio, and fontconfig.

These can all be installed on Debian/Ubuntu with

```sh
sudo apt install clang meson libv4l-dev libudev-dev libsodium-dev libfontconfig-dev libgl-dev libsdl2-dev libsdl2-ttf-dev libjpeg-dev libpulse-dev libavcodec-dev libavformat-dev
```

You can build the debug version of camlet with `make` (outputs `camlet.debug`), the release
version with `make release` (outputs `release/camlet`), and install it with
`make install`, or e.g. `INSTALL_PREFIX=~/.local make install` to customize the installation directory (default: `/usr/local`).
You can also build the .deb installer with `make camlet.deb`.
