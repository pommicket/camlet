project('camlet',
	'c',
	default_options: [
		'warning_level=4',
	]
)

libv4l2 = dependency('libv4l2')
executable('camlet', 'main.c', dependencies: [libv4l2], c_args: ['-Wno-unused-function'])
