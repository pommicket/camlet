# camlet

Camlet is a picture-taking application for Linux.
It features

- Reasonably good performance
- Proper handling of disconnecting/reconnecting devices, including the ability to prioritize some cameras over others
- JPEG and PNG output
- Full selection of resolutions available from camera
- Remembers settings across program launches

# Building from source

camlet requires meson-build, a C compiler, and the development libraries
for SDL2, SDL2\_ttf, GL (headers only), v4l2, udev, sodium, jpeglib (from IJG), avcodec, avformat, pulseaudio, and fontconfig.

These can all be installed on Debian/Ubuntu with

```sh
sudo apt install clang meson libv4l-dev libudev-dev libsodium-dev libfontconfig-dev libgl-dev libsdl2-dev libsdl2-ttf-dev libjpeg-dev libpulse-dev libavcodec-dev libavformat-dev
```
