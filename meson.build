project('camlet',
	'c',
	default_options: [
		'warning_level=4',
	]
)

libv4l2 = dependency('libv4l2')
sdl2 = dependency('SDL2')
if get_option('debug')
	debug_def = '-DDEBUG=1'
else
	debug_def = '-DDEBUG=0'
endif
executable('camlet', 'main.c', 'lib/stb_image_write.c', dependencies: [libv4l2, sdl2], c_args: ['-Wno-unused-function', '-Wno-format-truncation', '-Wshadow', debug_def])
