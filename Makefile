camlet.debug: meson.build main.c debug/setup
	meson compile -C debug
	ln -sf debug/camlet camlet.debug
	cp debug/compile_commands.json .
debug/setup:
	rm -rf debug
	meson setup debug
	touch debug/setup
