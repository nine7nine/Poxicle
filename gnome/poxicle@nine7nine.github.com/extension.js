// Poxicle — GNOME Shell port of the KWin edge-particle effect.
//
// Two roles, one C engine (gi://Poxicle — no JavaScript sim to keep in sync):
//
//  1. Ambient self-sim. Tracks the focused window, lays a transparent overlay
//     over its frame, and draws each frame's particles on the GPU: a custom
//     Clutter.Actor uploads the engine's vertex blob to a Cogl primitive and
//     draws it in a single pass — no Cairo, no per-frame window-sized CPU surface
//     (which made interactive resize stall).
//
//  2. Bridge receiver. Owns org.ninez.PoxicleBridge so a producer process that
//     runs its OWN sim (e.g. Chiguiro, which alone knows its terminal bell /
//     process / overscroll triggers) can stream ready-to-draw instances over a
//     shared memfd and have the Shell draw them at compositor level — far smoother
//     than the producer's in-app wl_subsurface, which Mutter throttles during
//     resize. Parity with the KWin effect's ExtStream: a streaming window draws
//     its own particles AND still gets the focus-following overlay on top (the
//     stream replaces only that window's own per-app look, never the overlay).
//
// Build/install the binding first (gnome/install.sh runs the engine's
// -Dintrospection target) so gi://Poxicle exposes attach_stream/read_stream_*.

import GLib from 'gi://GLib';
import Gio from 'gi://Gio';
import Meta from 'gi://Meta';
import Clutter from 'gi://Clutter';
import Cogl from 'gi://Cogl';
import GObject from 'gi://GObject';
import St from 'gi://St';
import Pox from 'gi://Poxicle';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const TICK_MS = 16;   // ~60fps; a vsync-locked frame clock is a later refinement.
const VERT_STRIDE = 12;   // poxicle_engine_tick_vertices: f32 x,y + u8 r,g,b,a

// Default time (logical ms) to hold a stream's particles back after its window
// starts un-minimizing, so the ring doesn't snap in at the settled frame_rect while
// Magic Lamp / Burn My Windows is still warping the window up from the panel. The
// live value is user-tunable via poxicle-config ([poxicle] UnminimizeGrace) and read
// from CONFIG_PATH; this is the fallback. Scaled at runtime by St.Settings so it
// tracks the user's animation speed and collapses to 0 when animations are off.
// Mirrors the KWin effect's PoxConfig::unminimizeGraceMs().
const UNMINIMIZE_GRACE_DEFAULT_MS = 350;

// poxicle-config's DE-neutral config — the same file the KWin effect reads. The
// engine resolves and applies it (Pox.Engine.apply_config); we only watch it.
const CONFIG_PATH =
    GLib.build_filenamev([GLib.get_user_config_dir(), 'poxicle', 'poxicle.conf']);

// Cross-process channel a producer uses to hand us its per-frame instances. The
// name/path/iface mirror kwin/src/poxbridge.h so the same producer talks to KWin
// or to us unchanged. Register carries the memfd as a Unix fd ('h').
const BRIDGE_NAME = 'org.ninez.PoxicleBridge';
const BRIDGE_PATH = '/PoxicleBridge';
const BRIDGE_IFACE = `
<node>
  <interface name="org.ninez.PoxicleBridge">
    <method name="Register">
      <arg type="i" name="pid" direction="in"/>
      <arg type="h" name="shm" direction="in"/>
      <arg type="b" name="ok" direction="out"/>
    </method>
    <method name="Unregister">
      <arg type="i" name="pid" direction="in"/>
    </method>
    <method name="Wake">
      <arg type="i" name="pid" direction="in"/>
    </method>
  </interface>
</node>`;

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

    // This frame's GPU-ready vertex blob (from tick_vertices / read_stream_vertices).
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

const nVertsOf = bytes => (bytes.get_size() / VERT_STRIDE) | 0;
const isNormal = win =>
    !!win && win.get_window_type() === Meta.WindowType.NORMAL;

export default class PoxicleExtension extends Extension {
    enable() {
        // --- ambient self-sim (focused window) ---
        this._win = null;
        this._winSignals = [];
        this._engine = new Pox.Engine();
        this._area = new PoxicleParticleActor();

        this._display = global.display;
        this._focusId = this._display.connect('notify::focus-window',
            () => this._retarget());

        // Live-reload when poxicle-config saves the neutral config file.
        this._readGrace();
        this._cfgMonitor = Gio.File.new_for_path(CONFIG_PATH)
            .monitor_file(Gio.FileMonitorFlags.WATCH_MOVES, null);
        this._cfgMonitor.connect('changed', () => {
            this._readGrace();
            this._retarget();
        });

        this._retarget();

        // --- bridge receiver (producer streams) ---
        this._streams = new Map();   // pid -> {pid, engine, actor, win, winSignals}
        this._dbus = Gio.DBusExportedObject.wrapJSObject(BRIDGE_IFACE, this);
        this._dbus.export(Gio.DBus.session, BRIDGE_PATH);
        this._nameId = Gio.bus_own_name(
            Gio.BusType.SESSION, BRIDGE_NAME, Gio.BusNameOwnerFlags.REPLACE,
            null, null, null);

        // One clock drives both roles.
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

        if (this._nameId) {
            Gio.bus_unown_name(this._nameId);
            this._nameId = 0;
        }
        this._dbus?.unexport();
        this._dbus = null;
        for (const pid of [...this._streams.keys()])
            this._dropStream(pid, /*retarget=*/ false);
        this._streams = null;

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

    // Read the user-tunable un-minimize grace from poxicle-config's neutral
    // config (the same file the KWin effect reads). Missing/invalid => default.
    _readGrace() {
        let ms = UNMINIMIZE_GRACE_DEFAULT_MS;
        try {
            const kf = new GLib.KeyFile();
            kf.load_from_file(CONFIG_PATH, GLib.KeyFileFlags.NONE);
            if (kf.has_group('poxicle') && kf.has_key('poxicle', 'UnminimizeGrace'))
                ms = kf.get_integer('poxicle', 'UnminimizeGrace');
        } catch (_e) {
            // no config yet / unreadable -> keep the default
        }
        this._graceMs = Math.max(0, Math.min(2000, ms));
    }

    // ---- ambient self-sim: active-window tracking ----

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
        if (!isNormal(win))
            return;
        // A streaming window (e.g. Chiguiro) still gets the focus-following overlay
        // on TOP of its own streamed particles — parity with the KWin effect, which
        // draws the active-window overlay additively over an ExtStream. We do NOT
        // skip it here just because the window has a stream.

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

    // ---- bridge receiver: D-Bus methods (called by the wrapped object) ----

    // Register carries a Unix fd, so it must be handled async to reach the
    // message's fd list (the plain wrapped-method path can't carry fds).
    RegisterAsync(params, invocation) {
        const [pid, handle] = params;
        let ok = false;
        try {
            const fdList = invocation.get_message().get_unix_fd_list();
            const fd = fdList ? fdList.get(handle) : -1;   // dup'd; we own it now
            ok = this._onRegister(pid, fd);
        } catch (e) {
            logError(e, 'Poxicle bridge: Register failed');
        }
        invocation.return_value(new GLib.Variant('(b)', [ok]));
    }

    Unregister(pid) {
        this._dropStream(pid);
    }

    Wake(_pid) {
        // The clock polls every stream each tick, so an idle->active edge needs no
        // explicit un-park here. Kept for protocol parity with the KWin effect.
    }

    // ---- bridge receiver: stream lifecycle ----

    _onRegister(pid, fd) {
        if (pid <= 0 || fd < 0) {
            if (fd >= 0)
                GLib.close(fd);   // attach_stream would have taken it; bail cleanly
            return false;
        }

        const engine = new Pox.Engine();
        if (!engine.attach_stream(fd))   // takes ownership of fd and closes it
            return false;

        this._dropStream(pid, /*retarget=*/ false);   // replace prior (producer respawn)

        const s = {pid, engine, actor: new PoxicleParticleActor(), win: null,
                   winSignals: [], wasMinimized: false, suppressUntil: 0};
        this._streams.set(pid, s);
        this._bindWindow(s);

        // Leave any focus-following overlay in place: a streaming window keeps the
        // active-window overlay on top of its stream (parity with the KWin effect).
        return true;
    }

    _dropStream(pid, retarget = true) {
        const s = this._streams?.get(pid);
        if (!s)
            return;
        for (const [obj, id] of s.winSignals)
            obj.disconnect(id);
        const parent = s.actor.get_parent();
        if (parent)
            parent.remove_child(s.actor);
        s.actor.destroy();
        s.engine.detach_stream();
        this._streams.delete(pid);
        // Refresh the focus overlay's target/placement now the stream is gone (the
        // overlay tracks focus independently, so this is just a defensive re-aim).
        if (retarget)
            this._retarget();
    }

    // Bind the stream to the first eligible window with a matching pid. The window
    // may not exist yet at Register time; _tick retries until it appears.
    _bindWindow(s) {
        if (s.win)
            return true;
        for (const a of global.get_window_actors()) {
            const w = a.meta_window;
            if (!w || w.get_pid() !== s.pid || !isNormal(w))
                continue;
            s.win = w;
            for (const sig of ['position-changed', 'size-changed'])
                s.winSignals.push([w, w.connect(sig, () => this._placeStream(s))]);
            s.winSignals.push([w, w.connect('unmanaged', () => this._dropStream(s.pid))]);
            global.window_group.add_child(s.actor);
            this._placeStream(s);
            return true;
        }
        return false;
    }

    _placeStream(s) {
        if (!s.win)
            return;
        const r = s.win.get_frame_rect();   // producer streams frame-local coords
        s.actor.set_position(r.x, r.y);
        s.actor.set_size(r.width, r.height);
    }

    // ---- frame loop ----

    _tick() {
        // Ambient self-sim for the focused (non-streaming) window.
        if (this._win && this._engine && this._area?.get_parent()) {
            const bytes = this._engine.tick_vertices(TICK_MS / 1000);
            this._area.setVertices(bytes, nVertsOf(bytes));
        }

        // Producer streams: draw the latest complete frame. null => no new frame
        // (keep the last); an empty blob => a frame with no particles (clears).
        if (!this._streams)
            return;
        const nowUs = GLib.get_monotonic_time();
        for (const s of this._streams.values()) {
            if (!s.win && !this._bindWindow(s))
                continue;

            // Our actor is a SIBLING of the window actor in window_group, so Mutter's
            // minimize-hide doesn't cover it — without this gate the ring would freeze
            // mid-air where the window was. Hide while minimized; on the un-minimize
            // edge arm a grace so it doesn't snap in at the settled frame_rect over a
            // still-animating window (Magic Lamp / Burn My Windows). Parity with the
            // KWin effect's stream path (visible() + kUnminimizeGraceMs).
            const minimizedNow = !!s.win.minimized;
            if (s.wasMinimized && !minimizedNow) {
                const st = St.Settings.get();
                const factor = st.enable_animations ? st.slow_down_factor : 0;
                s.suppressUntil = nowUs + this._graceMs * 1000 * factor;
            }
            s.wasMinimized = minimizedNow;

            if (minimizedNow || nowUs < s.suppressUntil) {
                s.actor.hide();
                continue;
            }
            if (!s.actor.visible) {
                s.actor.show();
                this._placeStream(s);   // re-aim at the settled frame_rect after restore
            }

            const bytes = s.engine.read_stream_vertices();
            if (bytes !== null)
                s.actor.setVertices(bytes, nVertsOf(bytes));
        }
    }
}
