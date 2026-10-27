#
# Copyright (C) Honey Bunny QT
# SPDX-License-Identifier: AGPL-3.0-only
#
# n3n-be for NetworkManager (./configure --with-network-manager), see
# n3n-nm.h: the VPN service, the plugin that imports, exports and loads the
# settings page, the settings pages of GTK 4 and GTK 3, the dialog for the
# key, and the GNOME Shell extension (shell-extension/).  Built against libnm, GLib and GTK, not against libn3n: the edge is
# a program of its own, which the service starts.
#

-include ../config.mak

# where the edge is, for the service
N3N_EDGE?=$(CONFIG_PREFIX)/sbin/n3n-be-edge
# where NetworkManager looks for the .name files of its VPN plugins
NM_VPNDIR?=$(shell pkg-config --variable=vpnservicedir libnm)
DBUS_SYSTEMDIR?=$(CONFIG_PREFIX)/share/dbus-1/system.d
# the GNOME Shell extension, for all users (each enables it)
EXTENSION=n3n-be@honeybunnyqt.github.io
EXTENSIONDIR?=$(CONFIG_PREFIX)/share/gnome-shell/extensions/$(EXTENSION)

NM_CFLAGS:=$(shell pkg-config --cflags libnm)
NM_LIBS:=$(shell pkg-config --libs libnm)
GTK4_CFLAGS=$(shell pkg-config --cflags gtk4 libnma-gtk4)
GTK4_LIBS=$(shell pkg-config --libs gtk4 libnma-gtk4)
GTK3_CFLAGS=$(shell pkg-config --cflags gtk+-3.0 libnma)
GTK3_LIBS=$(shell pkg-config --libs gtk+-3.0 libnma)

# GtkComboBoxText is deprecated in GTK 4.10, but there in 3 and 4 alike
CFLAGS_NM=$(filter-out -I%include,$(CFLAGS)) -I../src $(NM_CFLAGS) -Wno-deprecated-declarations
CFLAGS_LIB=$(CFLAGS_NM) -fPIC -fvisibility=hidden

PROGS=nm-n3n-be-service
LIBS_SO=libnm-vpn-plugin-n3n-be.so
ifeq ($(CONFIG_NM_GTK4),yes)
PROGS+=nm-n3n-be-auth-dialog
LIBS_SO+=libnm-gtk4-vpn-plugin-n3n-be-editor.so
endif
ifeq ($(CONFIG_NM_GTK3),yes)
LIBS_SO+=libnm-vpn-plugin-n3n-be-editor.so
endif

all: $(PROGS) $(LIBS_SO) nm-n3n-be-service.name tests-nm-conf nm-n3n-be-service.test

nm-n3n-be-service: nm-n3n-be-service.c n3n-nm-conf.c ../src/mgmt_client.c n3n-nm.h ../src/mgmt_client.h ../config.mak
	$(CC) $(CFLAGS_NM) -DN3N_EDGE='"$(N3N_EDGE)"' -DN3N_RUNDIR='"$(patsubst $(DESTDIR)%,%,$(CONFIG_RUNDIR))"' \
		-o $@ nm-n3n-be-service.c n3n-nm-conf.c ../src/mgmt_client.c $(LDFLAGS) $(NM_LIBS)

# the same, with the edge of this tree: for tests/nm/netns.sh
nm-n3n-be-service.test: nm-n3n-be-service.c n3n-nm-conf.c ../src/mgmt_client.c n3n-nm.h ../src/mgmt_client.h ../config.mak
	$(CC) $(CFLAGS_NM) -DN3N_EDGE='"$(abspath ../apps/n3n-be-edge)"' \
		-DN3N_RUNDIR='"$(patsubst $(DESTDIR)%,%,$(CONFIG_RUNDIR))"' \
		-o $@ nm-n3n-be-service.c n3n-nm-conf.c ../src/mgmt_client.c $(LDFLAGS) $(NM_LIBS)

nm-n3n-be-auth-dialog: nm-n3n-be-auth-dialog.c n3n-nm.h
	$(CC) $(CFLAGS_NM) $(GTK4_CFLAGS) -o $@ nm-n3n-be-auth-dialog.c $(LDFLAGS) $(NM_LIBS) $(GTK4_LIBS)

libnm-vpn-plugin-n3n-be.so: nm-n3n-be-plugin.c n3n-nm-conf.c n3n-nm.h
	$(CC) $(CFLAGS_LIB) -shared -o $@ nm-n3n-be-plugin.c n3n-nm-conf.c $(LDFLAGS) $(NM_LIBS) -ldl

libnm-gtk4-vpn-plugin-n3n-be-editor.so: nm-n3n-be-editor.c n3n-nm-conf.c n3n-nm.h
	$(CC) $(CFLAGS_LIB) $(GTK4_CFLAGS) -shared -o $@ nm-n3n-be-editor.c n3n-nm-conf.c $(LDFLAGS) $(NM_LIBS) $(GTK4_LIBS)

libnm-vpn-plugin-n3n-be-editor.so: nm-n3n-be-editor.c n3n-nm-conf.c n3n-nm.h
	$(CC) $(CFLAGS_LIB) $(GTK3_CFLAGS) -shared -o $@ nm-n3n-be-editor.c n3n-nm-conf.c $(LDFLAGS) $(NM_LIBS) $(GTK3_LIBS)

nm-n3n-be-service.name: nm-n3n-be-service.name.in ../config.mak
	sed -e 's|@LIBEXECDIR@|$(CONFIG_LIBEXECDIR)|g' -e 's|@PLUGINDIR@|$(CONFIG_NM_LIBDIR)|g' $< > $@

# the configuration's text, its import and export: see tests/tests-nm-conf.expected
tests-nm-conf: tests-nm-conf.c n3n-nm-conf.c n3n-nm.h
	$(CC) $(CFLAGS_NM) -o $@ tests-nm-conf.c n3n-nm-conf.c $(LDFLAGS) $(NM_LIBS)

install: all
	install -d $(DESTDIR)$(CONFIG_LIBEXECDIR) $(DESTDIR)$(CONFIG_NM_LIBDIR) $(DESTDIR)$(NM_VPNDIR) \
		$(DESTDIR)$(DBUS_SYSTEMDIR)
	install -m755 $(PROGS) $(DESTDIR)$(CONFIG_LIBEXECDIR)/
	install -m755 $(LIBS_SO) $(DESTDIR)$(CONFIG_NM_LIBDIR)/
	install -m644 nm-n3n-be-service.name $(DESTDIR)$(NM_VPNDIR)/
	install -m644 nm-n3n-be-service.conf $(DESTDIR)$(DBUS_SYSTEMDIR)/
	install -d $(DESTDIR)$(EXTENSIONDIR)
	install -m644 shell-extension/$(EXTENSION)/metadata.json shell-extension/$(EXTENSION)/extension.js \
		shell-extension/$(EXTENSION)/stylesheet.css $(DESTDIR)$(EXTENSIONDIR)/

clean:
	rm -f nm-n3n-be-service nm-n3n-be-service.test nm-n3n-be-auth-dialog *.so *.d nm-n3n-be-service.name tests-nm-conf

.PHONY: all install clean
