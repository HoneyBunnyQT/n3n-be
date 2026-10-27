#!/usr/bin/env python3
#
# Copyright (C) Honey Bunny QT
# SPDX-License-Identifier: AGPL-3.0-only
#
# The settings page of the NetworkManager plugin (nm/nm-n3n-be-editor.c),
# loaded as GNOME's Settings (GTK 4) or nm-connection-editor (GTK 3) load
# it, through the plugin: a connection imported from a file, shown, and
# taken back unchanged - every option as it was, also those the page does
# not show, the key with it, and the IPv4 setting's "only for resources on
# its network" following "Route all traffic through an exit peer".  Then
# the same with that unticked.  Needs a display (xvfb-run) and PyGObject
# with GTK and NM.
#
#   xvfb-run python3 tests/nm/editor.py 4.0|3.0 nm/ tests/nm/edge.conf \
#       [SCREENSHOT.png]

import os
import subprocess
import sys

import gi

GTK = sys.argv[1]
gi.require_version("Gtk", GTK)
gi.require_version("NM", "1.0")
from gi.repository import GLib, Gtk, NM  # noqa: E402

failed = False


def check(name, ok, detail):
    global failed
    print("%s %s: %s" % ("ok  " if ok else "FAIL", name, detail))
    failed |= not ok


def options(conn):
    vpn = conn.get_setting_vpn()
    return {k: vpn.get_data_item(k) for k in vpn.get_data_keys()
            if not k.endswith("-flags")}


def show(widget, seconds):
    """The page in a window, drawn for a while"""
    win = Gtk.Window(title="n3n-be")
    if GTK == "4.0":
        win.set_child(widget)
        win.present()
    else:
        win.add(widget)
        win.show_all()
    ctx = GLib.MainContext.default()
    end = GLib.get_monotonic_time() + seconds * 1000000
    while GLib.get_monotonic_time() < end:
        ctx.iteration(False)


def find(widget, kind, label=None):
    """The first widget of a kind (with that label) in the page"""
    if isinstance(widget, kind) and label in (None, widget.get_label()):
        return widget
    if GTK == "4.0":
        child = widget.get_first_child()
        while child:
            found = find(child, kind, label)
            if found:
                return found
            child = child.get_next_sibling()
    elif isinstance(widget, Gtk.Container):
        for child in widget.get_children():
            found = find(child, kind, label)
            if found:
                return found
    return None


if GTK == "4.0":
    Gtk.init()
plugin = NM.VpnEditorPlugin.load_from_file(
    os.path.abspath(sys.argv[2]) + "/libnm-vpn-plugin-n3n-be.so",
    "org.freedesktop.NetworkManager.n3n-be", 0, lambda *a: True, None)
conn = plugin.import_(sys.argv[3])
editor = plugin.get_editor(conn)
page = editor.get_widget()
show(page, 1)
if len(sys.argv) > 4:
    subprocess.run(["import", "-window", "root", sys.argv[4]], check=False)

taken = NM.SimpleConnection.new_clone(conn)
ok = editor.update_connection(taken)
check("taken", ok, "update_connection")
check("options as they were", options(taken) == options(conn), options(taken))
key = taken.get_setting_vpn().get_secret("community_key")
before = conn.get_setting_vpn().get_secret("community_key")
check("the key", key == before, key)
never = taken.get_setting_ip4_config().get_never_default()
check("all traffic: not only its network", not never,
      "never-default %s" % never)

# "Route all traffic through an exit peer" unticked
route_all = find(page, Gtk.CheckButton,
                 "_Route all traffic through an exit peer")
route_all.set_active(False)
taken = NM.SimpleConnection.new_clone(conn)
editor.update_connection(taken)
vpn = taken.get_setting_vpn()
never = taken.get_setting_ip4_config().get_never_default()
route = vpn.get_data_item("tuntap_route_all")
check("directly: only its network", never and route is None,
      "never-default %s, tuntap_route_all %s" % (never, route))
gateway = vpn.get_data_item("tuntap_gateway")
check("directly: the exit peer kept", gateway == "10.0.0.1", gateway)

sys.exit(1 if failed else 0)
