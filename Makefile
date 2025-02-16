camlet.debug: meson.build main.c
	@if [ '!' '-d' debug ]; then meson setup debug; fi
	meson compile -C debug
	ln -sf debug/camlet camlet.debug
	cp debug/compile_commands.json .
