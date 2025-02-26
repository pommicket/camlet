# camlet

<img src="camlet.png">

Camlet is a webcam application for Linux.
It features

- Proper handling of disconnecting/reconnecting devices
- Capture JPEG and PNG images and MP4 (H264+AAC) videos
- Full selection of resolutions available from camera
- Remembers settings across program launches

## Installation

You can find .deb installers on the [releases page on GitHub](https://github.com/pommicket/camlet/releases).
If you want automatic updates, you can also
[add my repository to apt](https://s.pommicket.com/deb/), and just do

```sh
sudo apt install camlet
```

If you are on a non-Debian-based Linux
distribution, you will have to build from source (see instructions below).

## Usage

- <kbd>F1</kbd> - show this help text
- <kbd>F2</kbd> - show debug info
- <kbd>Space</kbd> - take a picture or start/stop recording video
- <kbd>Escape</kbd> - open/close settings
- <kbd>Ctrl</kbd>+<kbd>f</kbd> - open picture directory
- <kbd>Tab</kbd> - switch between picture and video

## Building from source

camlet requires meson-build, a C compiler, and the development libraries
for SDL2, SDL2\_ttf, GL (headers only), v4l2, udev, sodium, jpeglib (from IJG), ogg, vorbis, theora, and fontconfig.

These can all be installed on Debian/Ubuntu with

```sh
sudo apt install clang meson libv4l-dev libudev-dev libsodium-dev libfontconfig-dev libgl-dev libsdl2-dev libsdl2-ttf-dev libjpeg-dev libvorbis-dev libogg-dev libvpx-dev
```

You can build the debug version of camlet with `make` (outputs `camlet.debug`), the release
version with `make release` (outputs `release/camlet`), and install it with
`make install`, or e.g. `INSTALL_PREFIX=~/.local make install` to customize the installation directory (default: `/usr/local`).
You can also build the .deb installer with `make camlet.deb`.

## Debugging

- Known issue: pulseaudio audio capturing is broken on some versions of SDL2.30:
  https://github.com/libsdl-org/SDL/issues/9706. This bug has been fixed now, so hopefully
  the fix will make it to your computer soon.

If you find a bug, please create an issue on GitHub; it will be helpful to have
your settings and log file, located in `~/.config/camlet`.

You can try deleting/renaming your settings file as a temporary solution, to reset camlet to its default settings.

## Version history

<table>
<tr><th>Version</th> <th>Description</th> <th>Date</th></tr>
<tr><td>0.0.0</td> <td>Initial release</td> <td>2025 Feb 26</td></tr>
<tr><td>0.0.1</td> <td>Fix crash switching to video mode with no camera connected</td> <td>2025 Feb 26</td></tr>
</table>

## License

0BSD
