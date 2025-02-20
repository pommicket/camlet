project('camlet',
	'c',
	default_options: [
		'warning_level=3',
	]
)
cc = meson.get_compiler('c')
v4l2 = dependency('libv4l2')
sdl2 = dependency('SDL2')
sdl2_ttf = dependency('SDL2_ttf')
udev = dependency('libudev')
gl = dependency('GL')
sodium = dependency('libsodium')
fontconfig = dependency('fontconfig')
jpeg = dependency('libjpeg')
m_dep = cc.find_library('m', required: false)
if m_dep.found()
	add_project_link_arguments('-lm', language: 'c')
endif
if get_option('debug')
	debug_def = '-DDEBUG=1'
else
	debug_def = '-DDEBUG=0'
endif
executable('camlet', 'main.c', 'camera.c', '3rd_party/stb_image_write.c',
	dependencies: [v4l2, sdl2, sdl2_ttf, gl, udev, sodium, fontconfig, jpeg],
	c_args: ['-Wno-unused-function', '-Wno-format-truncation', '-Wshadow', debug_def])
