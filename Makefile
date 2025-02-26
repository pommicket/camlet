INSTALL_PREFIX?=/usr/local

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
camlet.deb: release control
	rm -rf tmp
	mkdir -p tmp/camlet/usr/bin tmp/camlet/DEBIAN tmp/camlet/usr/share/applications tmp/camlet/usr/share/icons/hicolor/48x48/apps
	cp release/camlet tmp/camlet/usr/bin/
	cp camlet.desktop tmp/camlet/usr/share/applications/
	cp camlet.png tmp/camlet/usr/share/icons/hicolor/48x48/apps
	cp control tmp/camlet/DEBIAN/
	dpkg-deb --build tmp/camlet
	rm -rf tmp
install: release
	@[ -w $(INSTALL_PREFIX) ] || { echo "You need permission to write to $(INSTALL_PREFIX). Try running with sudo/as root." && exit 1; }
	mkdir -p $(INSTALL_PREFIX)/bin $(INSTALL_PREFIX)/share/applications $(INSTALL_PREFIX)/share/icons/hicolor/48x48/apps
	install -m 755 release/camlet $(INSTALL_PREFIX)/bin
	install -m 644 camlet.desktop $(INSTALL_PREFIX)/share/applications
	install -m 644 camlet.png $(INSTALL_PREFIX)/share/icons/hicolor/48x48/apps
uninstall:
	@[ -w $(INSTALL_PREFIX) ] || { echo "You need permission to write to $(INSTALL_PREFIX). Try running with sudo/as root." && exit 1; }
	rm -f $(INSTALL_PREFIX)/bin/camlet $(INSTALL_PREFIX)/share/applications/camlet.desktop $(INSTALL_PREFIX)/share/icons/hicolor/48x48/apps/camlet.png
