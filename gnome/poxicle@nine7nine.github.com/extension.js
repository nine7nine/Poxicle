// Poxicle — GNOME Shell port of the KWin edge-particle effect.
//
// Tracks the active window, lays a transparent overlay over its frame, and draws
// each frame's particles with Cairo. The simulation is the SAME C engine the KWin
// effect uses, reached through GObject-Introspection (gi://Poxicle) — no separate
// JavaScript port to keep in sync. Build/install the binding first (gnome/install.sh
// runs the engine's -Dintrospection target).
//
// Config (enable / preset / palette / per-app) will move to GSettings + prefs.js;
// for now a default preset is applied to the focused window.

import GLib from 'gi://GLib';
import St from 'gi://St';
import Meta from 'gi://Meta';
import Pox from 'gi://Poxicle';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const TICK_MS = 16;                 // ~60fps; a vsync-locked frame clock is a later refinement.
const DEFAULT_PRESET = 'ambient';   // TODO: from GSettings (prefs.js).
const DEFAULT_PALETTE = 17;         // "Verdant".

// Packed PoxInstance layout from poxicle_engine_tick() — 36 bytes, little-endian:
const STRIDE = 36;
const OFF_X = 0, OFF_Y = 4, OFF_SIZE = 8, OFF_ANGLE = 12, OFF_SHAPE = 16,
    OFF_R = 20, OFF_G = 24, OFF_B = 28, OFF_A = 32;
const SHAPE_CIRCLE = 1, SHAPE_DIAMOND = 2, SHAPE_TRIANGLE = 3;

export default class PoxicleExtension extends Extension {
    enable() {
        this._win = null;
        this._winSignals = [];
        this._bytes = null;
        this._view = null;
        this._count = 0;

        this._engine = new Pox.Engine();
        this._engine.set_preset(DEFAULT_PRESET, 0);
        this._engine.set_palette(DEFAULT_PALETTE);

        // Non-reactive so clicks pass straight through to the window beneath.
        this._area = new St.DrawingArea({reactive: false});
        this._area.connect('repaint', area => this._repaint(area));

        this._display = global.display;
        this._focusId = this._display.connect('notify::focus-window',
            () => this._retarget());
        this._retarget();

        this._timer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, TICK_MS, () => {
            this._tick();
            return GLib.SOURCE_CONTINUE;
        });
    }

    disable() {
        if (this._timer) {
            GLib.source_remove(this._timer);
            this._timer = 0;
        }
        if (this._focusId) {
            this._display.disconnect(this._focusId);
            this._focusId = 0;
        }
        this._untrack();
        this._area?.destroy();
        this._area = null;
        this._engine = null;
        this._bytes = null;
        this._view = null;
        this._display = null;
    }

    // ---- active-window tracking ----

    _untrack() {
        for (const [obj, id] of this._winSignals)
            obj.disconnect(id);
        this._winSignals = [];
        const parent = this._area?.get_parent();
        if (parent)
            parent.remove_child(this._area);
        this._win = null;
    }

    _retarget() {
        this._untrack();
        const win = this._display?.focus_window;
        if (!win || win.get_window_type() !== Meta.WindowType.NORMAL)
            return;
        this._win = win;

        for (const sig of ['position-changed', 'size-changed'])
            this._winSignals.push([win, win.connect(sig, () => this._place())]);
        this._winSignals.push([win, win.connect('unmanaged', () => this._untrack())]);

        global.window_group.add_child(this._area);
        this._place();
    }

    _place() {
        if (!this._win || !this._area)
            return;
        const r = this._win.get_frame_rect();   // frame-local origin == engine origin
        this._area.set_position(r.x, r.y);
        this._area.set_size(r.width, r.height);
        this._engine.set_surface(r.width, r.height, 1);
    }

    // ---- frame loop ----

    _tick() {
        if (!this._win || !this._engine || !this._area)
            return;
        this._bytes = this._engine.tick(TICK_MS / 1000);   // GLib.Bytes of packed instances
        const data = this._bytes.get_data();               // Uint8Array (roots the bytes)
        this._view = new DataView(data.buffer, data.byteOffset, data.byteLength);
        this._count = (data.byteLength / STRIDE) | 0;
        this._area.queue_repaint();
    }

    // ---- render the engine's instances (read straight from the packed blob) ----

    _repaint(area) {
        const [w, h] = area.get_surface_size();
        if (w <= 0 || h <= 0 || !this._view || !this._count)
            return;
        const cr = area.get_context();   // St.DrawingArea hands us a cleared surface
        const dv = this._view;

        for (let i = 0, off = 0; i < this._count; i++, off += STRIDE) {
            let a = dv.getFloat32(off + OFF_A, true);
            if (a <= 0.003)
                continue;
            if (a > 1.0)
                a = 1.0;
            const x = dv.getFloat32(off + OFF_X, true);
            const y = dv.getFloat32(off + OFF_Y, true);
            const s = dv.getFloat32(off + OFF_SIZE, true);
            const shape = dv.getInt32(off + OFF_SHAPE, true);
            cr.setSourceRGBA(dv.getFloat32(off + OFF_R, true),
                dv.getFloat32(off + OFF_G, true),
                dv.getFloat32(off + OFF_B, true), a);

            switch (shape) {
                case SHAPE_CIRCLE:
                    cr.arc(x + s / 2, y + s / 2, s / 2, 0, 2 * Math.PI);
                    cr.fill();
                    break;
                case SHAPE_DIAMOND:
                    cr.moveTo(x + s / 2, y);
                    cr.lineTo(x + s, y + s / 2);
                    cr.lineTo(x + s / 2, y + s);
                    cr.lineTo(x, y + s / 2);
                    cr.closePath();
                    cr.fill();
                    break;
                case SHAPE_TRIANGLE:
                    cr.save();
                    cr.translate(x + s / 2, y + s / 2);
                    cr.rotate((dv.getFloat32(off + OFF_ANGLE, true) * Math.PI) / 180);
                    cr.moveTo(0, -s / 2);
                    cr.lineTo(s / 2, s / 2);
                    cr.lineTo(-s / 2, s / 2);
                    cr.closePath();
                    cr.fill();
                    cr.restore();
                    break;
                default:   // square
                    cr.rectangle(x, y, s, s);
                    cr.fill();
                    break;
            }
        }
        cr.$dispose();
    }
}
