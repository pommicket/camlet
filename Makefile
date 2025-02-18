camlet.debug: meson.build main.c debug/setup
	meson compile -C debug
	ln -sf debug/camlet camlet.debug
	cp debug/compile_commands.json .
debug/setup:
	rm -rf debug
	meson setup debug
	touch debug/setup
release: release/camlet
release/camlet: meson.build main.c release/setup
	meson compile -C release
release/setup:
	rm -rf release
	meson setup release
	touch release/setup
