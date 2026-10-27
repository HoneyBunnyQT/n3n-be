// Copyright (C) Honey Bunny QT
// SPDX-License-Identifier: AGPL-3.0-only
//
// For tests/nm/shell.sh: a while after GNOME Shell started, opens the
// quick settings and the menu of n3n-be's toggle, and logs what they show -
// the toggle, and each row of the menu with its dot and its tick - as
// "n3n-be-test: ..." lines; then chooses the first exit in the menu, and
// "None: directly" again, and logs the toggle after each.

import GLib from 'gi://GLib';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const ORNAMENTS = ['', 'dot', 'check', 'hidden', 'no-dot'];

function say(text) {
    console.log(`n3n-be-test: ${text}`);
}

// the text of a menu item, and the dot's kind
function row(item) {
    const labels = item.get_children().filter(c => c instanceof St.Label);
    const dot = labels.find(l => l.has_style_class_name('n3n-be-dot'));
    const kind = dot ? ['exit', 'says', 'none'].find(k => dot.has_style_class_name(`n3n-be-dot-${k}`)) : '-';
    const text = labels.filter(l => l !== dot).map(l => l.text).join(' ') || item.label?.text || '';
    return `${kind} | ${text} | ${ORNAMENTS[item._ornament ?? 0] ?? ''}`;
}

export default class Driver extends Extension {
    enable() {
        this._timer = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 8, () => {
            const qs = Main.panel.statusArea.quickSettings;
            qs.menu.open(false);
            const toggle = qs.menu._grid.get_children().find(i => i.title === 'n3n-be');
            say(`toggle ${toggle ? 'there' : 'missing'}, visible ${toggle?.visible}, ` +
                `checked ${toggle?.checked}, subtitle ${toggle?.subtitle}`);
            toggle?.menu.open(false);
            // the menu again, once the exits' check is through; then all
            // traffic through the first exit, and directly again
            const rows = () => (toggle?.menu._getMenuItems() ?? []).flatMap(item =>
                item._getMenuItems?.() ?? [item]);
            const steps = [
                () => {
                    rows().forEach(i => say(`row ${row(i)}`));
                    // the menu as it is: for a screenshot
                    say('shown');
                },
                () => {},
                () => rows().find(i => row(i).startsWith('exit ') && i.reactive)?.activate(null),
                () => say(`chosen: checked ${toggle?.checked}, subtitle ${toggle?.subtitle}`),
                () => rows().find(i => row(i).includes('None: directly'))?.activate(null),
                () => say(`directly: checked ${toggle?.checked}, subtitle ${toggle?.subtitle}`),
            ];
            this._timer = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 8, () => {
                this._timer = GLib.timeout_add_seconds(GLib.PRIORITY_DEFAULT, 3, () => {
                    steps.shift()();
                    if (steps.length)
                        return GLib.SOURCE_CONTINUE;
                    say('done');
                    this._timer = 0;
                    return GLib.SOURCE_REMOVE;
                });
                steps.shift()();
                return GLib.SOURCE_REMOVE;
            });
            return GLib.SOURCE_REMOVE;
        });
    }

    disable() {
        if (this._timer)
            GLib.source_remove(this._timer);
        this._timer = 0;
    }
}
