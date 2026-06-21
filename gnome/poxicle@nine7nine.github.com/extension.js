// Poxicle — GNOME Shell port of the KWin edge-particle effect.
//
// Tracks the active window, lays a transparent overlay over its frame, and draws
// each frame's particles on the GPU: a custom Clutter.Actor uploads the engine's
// vertex blob to a Cogl primitive and draws it in a single pass — no Cairo, no
// per-frame window-sized CPU surface (which made interactive resize stall). The
// simulation is the SAME C engine the KWin effect uses, reached through
// GObject-Introspection (gi://Poxicle) — no separate JavaScript port to keep in
// sync. Build/install the binding first (gnome/install.sh runs the engine's
// -Dintrospection target).

import GLib from 'gi://GLib';
import Gio from 'gi://Gio';
import Meta from 'gi://Meta';
import Clutter from 'gi://Clutter';
import Cogl from 'gi://Cogl';
import GObject from 'gi://GObject';
import Pox from 'gi://Poxicle';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const TICK_MS = 16;   // ~60fps; a vsync-locked frame clock is a later refinement.
const VERT_STRIDE = 12;   // poxicle_engine_tick_vertices: f32 x,y + u8 r,g,b,a

// poxicle-config's DE-neutral config — the same file the KWin effect reads. The
// engine resolves and applies it (Pox.Engine.apply_config); we only watch it.
const CONFIG_PATH =
    GLib.build_filenamev([GLib.get_user_config_dir(), 'poxicle', 'poxicle.conf']);

// Custom actor that draws one frame's particles straight on the GPU. The engine
// hands us a ready P2C4 triangle list (premultiplied colours); we wrap it in a
// Cogl attribute buffer and draw a single primitive in the actor's paint pass.
const PoxicleParticleActor = GObject.registerClass(
class PoxicleParticleActor extends Clutter.Actor {
    _init() {
        super._init({reactive: false});   // clicks pass through to the window
        this._bytes = null;
        this._nVerts = 0;
        this._pipeline = null;
        this._failed = false;
    }

    // This frame's GPU-ready vertex blob (from poxicle_engine_tick_vertices).
    setVertices(bytes, nVerts) {
        this._bytes = nVerts > 0 ? bytes : null;
        this._nVerts = nVerts;
        this.queue_redraw();
    }

    vfunc_paint(paintContext) {
        if (this._failed || !this._nVerts || !this._bytes)
            return;
        try {
            const fb = paintContext.get_framebuffer();
            const ctx = fb.get_context();
            if (!this._pipeline) {
                this._pipeline = Cogl.Pipeline.new(ctx);
                // Premultiplied "over" — the C side premultiplies rgb by alpha.
                this._pipeline.set_blend(
                    'RGBA = ADD(SRC_COLOR, DST_COLOR*(1-SRC_COLOR[A]))');
            }
            const data = this._bytes.get_data();   // Uint8Array, VERT_STRIDE/vertex
            const vbuf = Cogl.AttributeBuffer.new(ctx, data);
            const attrs = [
                Cogl.Attribute.new(vbuf, 'cogl_position_in',
                    VERT_STRIDE, 0, 2, Cogl.AttributeType.FLOAT),
                Cogl.Attribute.new(vbuf, 'cogl_color_in',
                    VERT_STRIDE, 8, 4, Cogl.AttributeType.UNSIGNED_BYTE),
            ];
            const prim = Cogl.Primitive.new_with_attributes(
                Cogl.VerticesMode.TRIANGLES, this._nVerts, attrs);
            prim.draw(fb, this._pipeline);
        } catch (e) {
            // Report once, then stay quiet — don't spam the shell log per frame.
            this._failed = true;
            logError(e, 'Poxicle: GPU paint failed; overlay disabled');
        }
    }
});

export default class PoxicleExtension extends Extension {
    enable() {
        this._win = null;
        this._winSignals = [];

        this._engine = new Pox.Engine();
        this._area = new PoxicleParticleActor();

        this._display = global.display;
        this._focusId = this._display.connect('notify::focus-window',
            () => this._retarget());

        // Live-reload when poxicle-config saves the neutral config file.
        this._cfgMonitor = Gio.File.new_for_path(CONFIG_PATH)
            .monitor_file(Gio.FileMonitorFlags.WATCH_MOVES, null);
        this._cfgMonitor.connect('changed', () => this._retarget());

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
        this._cfgMonitor?.cancel();
        this._cfgMonitor = null;
        this._untrack();
        this._area?.destroy();
        this._area = null;
        this._engine = null;
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

        // Resolve + apply the whole look (preset, its stored parameter edits, the
        // per-app override columns, palette, reverse) in the engine — parity with
        // the KWin effect. false => this window draws nothing.
        if (!this._engine.apply_config(win.get_wm_class()))
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
        // GPU-ready triangle list (P2C4); the engine expands + premultiplies in C.
        const bytes = this._engine.tick_vertices(TICK_MS / 1000);
        this._area.setVertices(bytes, (bytes.get_size() / VERT_STRIDE) | 0);
    }
}
