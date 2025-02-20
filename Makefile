camlet.debug: meson.build *.[ch] 3rd_party/*.[ch] debug/setup
	meson compile -C debug
	ln -sf debug/camlet camlet.debug
	cp debug/compile_commands.json .
debug/setup:
	rm -rf debug
	meson setup debug
	touch debug/setup
release: release/camlet
release/camlet: meson.build *.[ch] 3rd_party/*.[ch] release/setup2
	meson compile -C release
release/setup2:
	rm -rf release
	meson setup --buildtype=release release
	touch release/setup2
