# Building from source

camlet requires meson-build, a C compiler, and the development libraries
for SDL2, SDL2\_ttf, GL (headers only), v4l2, udev, sodium, and fontconfig.

These can all be installed on Debian/Ubuntu with

```sh
sudo apt install clang meson libv4l-dev libudev-dev libsodium-dev libfontconfig-dev libgl-dev libsdl2-dev libsdl2-ttf-dev
```
