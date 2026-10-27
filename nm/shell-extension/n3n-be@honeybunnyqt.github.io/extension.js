// Copyright (C) Honey Bunny QT
// SPDX-License-Identifier: AGPL-3.0-only
//
// n3n-be in GNOME Shell's quick settings: while an n3n-be VPN of
// NetworkManager is connected, a toggle "n3n-be" with its peers in the
// menu - the exits to the internet first, each with a dot, pink where this
// computer's check through it went through, blue where not (yet), the
// others with the room for one so that the names line up - and all
// traffic through one of them, or none, chosen there.  Opening the menu
// checks the exits that are due (as the tray app on Windows does); the
// toggle itself switches all traffic through the exit off, and on again.
//
// It asks NetworkManager's n3n-be service (nm/nm-n3n-be-service.c) on the
// system bus, interface org.n3n_be.Edge: the service holds the edge's
// management password, and asks polkit before it changes the exit.

import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import {QuickMenuToggle, SystemIndicator} from 'resource:///org/gnome/shell/ui/quickSettings.js';

const SERVICE = 'org.freedesktop.NetworkManager.n3n-be';
const PATH = '/org/n3n_be/Edge';
const IFACE = 'org.n3n_be.Edge';
// how often the toggle's subtitle is brought up to date, in seconds
const REFRESH = 10;
// how long an exit's check takes at most, in seconds (exit_probe.h)
const CHECK_WAIT = 6;

const DOT = '●';

// A method of the service at a bus name: its JSON, parsed (GNOME Shell
// has Gio.DBusConnection.call return a promise)
async function call(name, method, params = null) {
    const reply = await Gio.DBus.system.call(name, PATH, IFACE, method, params,
        new GLib.VariantType('(s)'), Gio.DBusCallFlags.NONE, 10000, null);
    return JSON.parse(reply.deepUnpack()[0]);
}

// The bus names of the services that run: one for each connection that is up
async function services() {
    const reply = await Gio.DBus.system.call('org.freedesktop.DBus', '/org/freedesktop/DBus',
        'org.freedesktop.DBus', 'ListNames', null, new GLib.VariantType('(as)'),
        Gio.DBusCallFlags.NONE, 5000, null);
    return reply.deepUnpack()[0].filter(n => n === SERVICE || n.startsWith(`${SERVICE}.`)).sort();
}

// The peers of an Info, as the menu lists them: exits first, then by name
function peers(info) {
    const list = [];
    for (const e of info.edges ?? []) {
        const addr = (e.ip4addr ?? '').split('/')[0];
        if (!addr || addr === '0.0.0.0' || list.some(p => p.addr === addr))
            continue;
        const exit = (e.exit ?? 0) > 0;
        list.push({
            addr,
            name: e.name || e.desc || '',
            exit,
            confirmed: exit && e.exit_check === 'ok',
        });
    }
    list.sort((a, b) => (b.exit - a.exit) || a.name.localeCompare(b.name) || a.addr.localeCompare(b.addr));
    return list;
}

// A menu item with a dot before its text: 'exit', 'says' or 'none'
function dotted(kind, text, params = {}) {
    const item = new PopupMenu.PopupBaseMenuItem(params);
    item.add_child(new St.Label({
        text: DOT,
        style_class: `n3n-be-dot n3n-be-dot-${kind}`,
        y_align: Clutter.ActorAlign.CENTER,
    }));
    item.label = new St.Label({text, y_expand: true, y_align: Clutter.ActorAlign.CENTER});
    item.add_child(item.label);
    item.label_actor = item.label;
    return item;
}

const N3nToggle = GObject.registerClass(
class N3nToggle extends QuickMenuToggle {
    _init() {
        super._init({
            title: 'n3n-be',
            iconName: 'network-vpn-symbolic',
            toggleMode: false,
        });
        this.menu.setHeader('network-vpn-symbolic', 'n3n-be', '');
        this._section = new PopupMenu.PopupMenuSection();
        this.menu.addMenuItem(this._section);
        // the connections, by their service's bus name: {info, peers}
        this._state = new Map();
        // the exit last chosen: for the toggle to switch it on again
        this._last = new Map();

        this.connect('clicked', () => this._toggled());
        this.menu.connect('open-state-changed', (menu, open) => {
            if (open)
                this._opened();
        });
    }

    // the services that run, and what they say
    async refresh() {
        let names = [];
        try {
            names = await services();
        } catch (e) {
            logError(e, 'n3n-be: the system bus');
        }
        const state = new Map();
        for (const name of names) {
            try {
                const info = await call(name, 'Info');
                state.set(name, {info, peers: peers(info)});
                if (info.info?.gateway)
                    this._last.set(name, info.info.gateway);
            } catch {
                // not connected (yet): not shown
            }
        }
        this._state = state;
        this._show();
    }

    _show() {
        this.visible = this._state.size > 0;
        const routed = [...this._state.values()].filter(s => s.info.info?.route_all);
        this.checked = routed.length > 0;

        // the subtitle: through which exit, or directly
        let subtitle = 'directly';
        if (routed.length) {
            const s = routed[0];
            const gw = s.info.info.gateway;
            const p = s.peers.find(q => q.addr === gw);
            subtitle = p?.name || gw;
        } else if (this._state.size === 0) {
            subtitle = '';
        }
        this.subtitle = subtitle;
        this.menu.setHeader('network-vpn-symbolic', 'n3n-be',
            [...this._state.values()].map(s => s.info.id).join(', '));

        this._section.removeAll();
        const several = this._state.size > 1;
        let first = true;
        for (const [name, s] of this._state) {
            if (!first)
                this._section.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
            first = false;
            if (several) {
                const head = new PopupMenu.PopupMenuItem(s.info.id, {reactive: false});
                this._section.addMenuItem(head);
            }
            this._showConnection(name, s);
        }
    }

    _showConnection(name, s) {
        const gateway = s.info.info?.gateway ?? '';
        const exits = s.peers.filter(p => p.exit);

        if (exits.length) {
            // what the dots mean: a menu has no tooltips
            for (const [kind, text] of [
                ['exit', 'An exit, confirmed by this computer'],
                ['says', 'Says it is an exit, not confirmed (yet)'],
            ]) {
                const legend = dotted(kind, text, {reactive: false});
                legend.label.add_style_class_name('n3n-be-legend');
                this._section.addMenuItem(legend);
            }
            this._section.addMenuItem(new PopupMenu.PopupSeparatorMenuItem('Route all traffic through'));
        }

        const direct = dotted('none', 'None: directly');
        direct.setOrnament(gateway ? PopupMenu.Ornament.NONE : PopupMenu.Ornament.CHECK);
        direct.connect('activate', () => this._choose(name, ''));
        this._section.addMenuItem(direct);

        for (const p of s.peers) {
            const kind = !p.exit ? 'none' : p.confirmed ? 'exit' : 'says';
            const label = p.name ? `${p.name} (${p.addr})` : p.addr;
            const item = dotted(kind, label);
            item.setOrnament(p.addr === gateway ? PopupMenu.Ornament.CHECK : PopupMenu.Ornament.NONE);
            item.connect('activate', () => this._choose(name, p.addr));
            this._section.addMenuItem(item);
        }
        if (!s.peers.length)
            this._section.addMenuItem(new PopupMenu.PopupMenuItem('no peers yet', {reactive: false}));

        if (s.info.web) {
            const web = new PopupMenu.PopupMenuItem('Open the web page');
            web.connect('activate', () => {
                try {
                    Gio.AppInfo.launch_default_for_uri(s.info.web, null);
                } catch (e) {
                    Main.notify('n3n-be', e.message);
                }
            });
            this._section.addMenuItem(web);
        }
    }

    // all traffic through an exit, or directly
    async _choose(name, addr) {
        try {
            await call(name, 'SetGateway', new GLib.Variant('(s)', [addr]));
        } catch (e) {
            Gio.DBusError.strip_remote_error(e);
            Main.notify('n3n-be', `Not changed: ${e.message}`);
        }
        await this.refresh();
    }

    // the toggle: off routes directly, on again through the exit before
    _toggled() {
        for (const [name, s] of this._state) {
            if (s.info.info?.route_all) {
                this._choose(name, '');
                return;
            }
        }
        for (const name of this._state.keys()) {
            const last = this._last.get(name);
            if (last) {
                this._choose(name, last);
                return;
            }
        }
        // none chosen yet: the menu, to choose one
        this.menu.open();
    }

    // the menu opens: the exits due are checked, and shown again once the
    // check is through
    async _opened() {
        for (const name of this._state.keys()) {
            try {
                await call(name, 'ProbeExits');
            } catch {
                // gone meanwhile
            }
        }
        if (this._checkTimer)
            GLib.source_remove(this._checkTimer);
        this._checkTimer = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, CHECK_WAIT, () => {
            this._checkTimer = 0;
            this.refresh();
            return GLib.SOURCE_REMOVE;
        });
    }

    destroy() {
        if (this._checkTimer)
            GLib.source_remove(this._checkTimer);
        this._checkTimer = 0;
        super.destroy();
    }
});

const N3nIndicator = GObject.registerClass(
class N3nIndicator extends SystemIndicator {
    _init() {
        super._init();
        this._toggle = new N3nToggle();
        this._toggle.visible = false;
        this.quickSettingsItems.push(this._toggle);

        // a service comes or goes: a connection went up or down
        this._signal = Gio.DBus.system.signal_subscribe('org.freedesktop.DBus',
            'org.freedesktop.DBus', 'NameOwnerChanged', '/org/freedesktop/DBus', null,
            Gio.DBusSignalFlags.NONE, (bus, sender, path, iface, signal, params) => {
                const [name] = params.deepUnpack();
                if (name === SERVICE || name.startsWith(`${SERVICE}.`))
                    this._soon();
            });
        this._timer = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, REFRESH, () => {
            this._toggle.refresh();
            return GLib.SOURCE_CONTINUE;
        });
        this._toggle.refresh();
    }

    // a service just come up answers once its edge is registered: a few
    // times in the first seconds
    _soon() {
        for (const s of [1, 3, 6]) {
            const id = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, s, () => {
                this._soonTimers = this._soonTimers.filter(t => t !== id);
                this._toggle.refresh();
                return GLib.SOURCE_REMOVE;
            });
            this._soonTimers = [...(this._soonTimers ?? []), id];
        }
    }

    destroy() {
        Gio.DBus.system.signal_unsubscribe(this._signal);
        GLib.source_remove(this._timer);
        for (const id of this._soonTimers ?? [])
            GLib.source_remove(id);
        this.quickSettingsItems.forEach(item => item.destroy());
        super.destroy();
    }
});

export default class N3nExtension extends Extension {
    enable() {
        this._indicator = new N3nIndicator();
        Main.panel.statusArea.quickSettings.addExternalIndicator(this._indicator);
    }

    disable() {
        this._indicator.destroy();
        this._indicator = null;
    }
}
