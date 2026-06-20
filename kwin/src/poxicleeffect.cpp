/* poxicleeffect.cpp — see poxicleeffect.h. */
#include "poxicleeffect.h"

#include <effect/effecthandler.h>
#include <effect/effectwindow.h>
#include <core/renderviewport.h>
#include <core/rect.h>
#include <scene/scene.h>   // KWin::RenderView

#include <QMatrix4x4>
#include <QDBusConnection>
#include <QDBusUnixFileDescriptor>

#include <epoxy/gl.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
// Edge-band half-thickness (logical px): covers the max particle block thickness
// plus burst reach. Used both to request the per-frame repaint and to expand each
// window's paint region so its particles render in the ring OUTSIDE the frame.
constexpr qreal kBandMargin = 48.0;

// Stop drawing / repainting an external stream after this many consecutive polls
// with no new frame, so a quiet producer lets KWin sleep (mirrors poxicle-wl's
// idle grace). A Wake() D-Bus call un-parks it.
constexpr int kStreamIdleGrace = 4;

// Base window (logical ms) to hold an external stream's particles back after its
// window starts un-minimizing, so the ring doesn't snap in at full frameGeometry
// over the still-animating window (Magic Lamp / Squash). Scaled at runtime by the
// compositor's animationTimeFactor() so it tracks the user's animation-speed
// setting and collapses to ~0 when animations are effectively instant. Slightly
// generous vs. the default minimize animation so the ring never appears mid-flight.
constexpr int kUnminimizeGraceMs = 350;

// Copy the latest COMPLETE frame from a producer's shared region into `out`,
// surface-local coords (the caller offsets by the window rect). Returns true only
// when a new, untorn frame was read; false when the producer is mid-write, has
// produced nothing new, or the region looks invalid. Uses the seqlock in the
// header (see poxbridge.h): odd = writing, even+advanced = a fresh frame.
bool readStreamFrame(PoxBridgeHeader *h, uint32_t capacity, uint32_t &lastSeq,
                     std::vector<PoxInstance> &out)
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t s1 = __atomic_load_n(&h->seq, __ATOMIC_ACQUIRE);
        if (s1 & 1u)            return false;   // producer mid-write -> keep last frame
        if (s1 == lastSeq)      return false;   // nothing new since we last drew
        uint32_t count = __atomic_load_n(&h->count, __ATOMIC_RELAXED);
        if (count > capacity) count = capacity;
        const PoxInstance *body = reinterpret_cast<const PoxInstance *>(
            reinterpret_cast<const char *>(h) + sizeof(PoxBridgeHeader));
        out.assign(body, body + count);
        // Re-read the seqlock: if it moved while we copied, the frame is torn.
        if (__atomic_load_n(&h->seq, __ATOMIC_ACQUIRE) == s1) {
            lastSeq = s1;
            return true;
        }
    }
    return false;   // producer churning faster than we can read; try next frame
}
}

PoxicleEffect::PoxicleEffect()
{
    m_scratch.resize(4096);
    m_config.load();

    // Create the renderer now — the framework guarantees a current GL context at
    // construction. Do it WITHOUT disturbing KWin's bound VAO / array buffer:
    // pox_gl_new sets up its own VAO, and KWin's scene renderer assumes the VAO
    // it bound stays bound (leaving it changed floods GL_INVALID_OPERATION
    // "no array object bound" from KWin's own draws).
    {
        GLint prevVao = 0, prevBuffer = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuffer);
        m_gl = pox_gl_new();
        glBindVertexArray(GLuint(prevVao));
        glBindBuffer(GL_ARRAY_BUFFER, GLuint(prevBuffer));
    }

    connect(KWin::effects, &KWin::EffectsHandler::windowAdded,
            this, &PoxicleEffect::slotWindowAdded);
    connect(KWin::effects, &KWin::EffectsHandler::windowClosed,
            this, &PoxicleEffect::slotWindowClosed);
    connect(KWin::effects, &KWin::EffectsHandler::windowDeleted,
            this, &PoxicleEffect::slotWindowClosed);   // also tear down when destroyed
    connect(KWin::effects, &KWin::EffectsHandler::windowActivated,
            this, &PoxicleEffect::slotWindowActivated);

    // Attach to windows already open when the effect loads.
    const auto windows = KWin::effects->stackingOrder();
    for (KWin::EffectWindow *w : windows)
        maybeAttach(w);

    // Seed the focus-following overlay against whatever is focused right now.
    KWin::EffectWindow *aw = KWin::effects->activeWindow();
    m_activeWindow = (aw && eligible(aw)) ? aw : nullptr;
    rebuildActiveEngine();

    // Expose the external-source bridge on the session bus so producers (e.g.
    // Chiguiro) can register their per-window instance stream. Registration is
    // best-effort: if the name is taken (a stale effect instance), external-source
    // mode is simply unavailable; the per-app + active-window paths still work.
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.registerService(QStringLiteral(POX_BRIDGE_SERVICE)))
        bus.registerObject(QStringLiteral(POX_BRIDGE_PATH), this,
                           QDBusConnection::ExportScriptableSlots);
}

PoxicleEffect::~PoxicleEffect()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.unregisterObject(QStringLiteral(POX_BRIDGE_PATH));
    bus.unregisterService(QStringLiteral(POX_BRIDGE_SERVICE));

    // Unmap + close every registered producer stream.
    for (auto &kv : m_streams) {
        if (kv.second.hdr)
            munmap(kv.second.hdr, kv.second.mapSize);
        if (kv.second.fd >= 0)
            close(kv.second.fd);
    }
    m_streams.clear();

    // KWin guarantees the GL context is current during destruction.
    for (auto &entry : m_windows) {
        if (entry.second.engine)
            pox_engine_free(entry.second.engine);
    }
    m_windows.clear();
    if (m_activeEngine)
        pox_engine_free(m_activeEngine);
    if (m_gl)
        pox_gl_free(m_gl);
}

bool PoxicleEffect::eligible(KWin::EffectWindow *w) const
{
    return w && w->isNormalWindow()
        && !w->isDesktop() && !w->isDock() && !w->isPopupWindow();
}

bool PoxicleEffect::visible(KWin::EffectWindow *w) const
{
    return w && w->isOnCurrentDesktop() && !w->isMinimized();
}

// Apply a resolved rule's burst/particle palette to an engine: a built-in
// palette id, or -1 ("Solid") to sample the per-app colour as a single-colour
// palette (falling back to the built-in default when no per-app colour is set).
// Every emission kind honours this now — ambient/fireworks bursts sample it per
// burst, and the geometric/scroll presets per segment or loop cycle; "Solid"
// collapses to the single per-app `color` everywhere.
static void applyPalette(PoxEngine *e, const PoxResolved &r)
{
    if (r.palette < 0) {
        if (r.color.a >= 0.0f)
            pox_engine_set_palette_colors(e, &r.color, 1);
        else
            pox_engine_set_palette(e, 0);
        return;
    }
    pox_engine_set_palette(e, r.palette);
}

void PoxicleEffect::maybeAttach(KWin::EffectWindow *w)
{
    if (!eligible(w) || m_windows.count(w))
        return;
    if (streamClaims(w))
        return;   // external-source window: the producer drives it, our rules don't

    const PoxResolved r = m_config.resolve(w->windowClass());
    if (!r.enabled)
        return;   // preset "none" => this window draws nothing

    WinFx fx;
    fx.engine = pox_engine_new();
    fx.geom = w->frameGeometry();
    pox_engine_set_surface(fx.engine, int(fx.geom.width()), int(fx.geom.height()), 1);
    pox_engine_set_tunables(fx.engine, &r.tunables);
    fx.color = r.color;
    // Drive the preset's actual motion (corners/rotate/ping-pong/pulse-out loop;
    // ambient/fireworks use the burst fan). Must follow set_tunables.
    pox_engine_set_kind(fx.engine, r.kind, r.reverse, r.color);
    applyPalette(fx.engine, r);
    m_windows.emplace(w, fx);

    KWin::effects->addRepaintFull();
}

void PoxicleEffect::detach(KWin::EffectWindow *w)
{
    auto it = m_windows.find(w);
    if (it == m_windows.end())
        return;
    // Repaint the band so the window's last particles are composited away when it
    // closes (otherwise stale particles linger where the window was).
    KWin::effects->addRepaint(
        it->second.geom.adjusted(-kBandMargin, -kBandMargin, kBandMargin, kBandMargin));
    if (it->second.engine)
        pox_engine_free(it->second.engine);
    m_windows.erase(it);
}

// ---- External-source mode (D-Bus org.ninez.PoxicleBridge) -------------------

bool PoxicleEffect::streamClaims(KWin::EffectWindow *w) const
{
    for (const auto &kv : m_streams)
        if (kv.second.window == w)
            return true;
    return false;
}

PoxicleEffect::ExtStream *PoxicleEffect::streamFor(KWin::EffectWindow *w)
{
    for (auto &kv : m_streams)
        if (kv.second.window == w)
            return &kv.second;
    return nullptr;
}

// Bind any registered-but-unbound stream to an eligible window whose pid matches
// the producer's. v1 matches on pid alone (single producer window); Phase 3 adds
// geometry disambiguation for multiple same-pid windows.
void PoxicleEffect::bindStreams()
{
    for (auto &kv : m_streams) {
        ExtStream &s = kv.second;
        if (s.window)
            continue;
        for (KWin::EffectWindow *w : KWin::effects->stackingOrder()) {
            if (!eligible(w) || int(w->pid()) != kv.first || streamClaims(w))
                continue;
            s.window = w;
            s.geom = w->frameGeometry();
            s.idle = 0;
            // External source takes precedence over the per-app engine on this
            // window — drop any owns-sim state so the two don't double-draw.
            detach(w);
            KWin::effects->addRepaintFull();
            break;
        }
    }
}

void PoxicleEffect::dropStream(int pid)
{
    auto it = m_streams.find(pid);
    if (it == m_streams.end())
        return;
    ExtStream &s = it->second;
    // Repaint the band so the producer's last particles are composited away.
    if (s.window && visible(s.window)) {
        KWin::effects->addRepaint(
            s.geom.adjusted(-kBandMargin, -kBandMargin, kBandMargin, kBandMargin));
    }
    if (s.hdr)
        munmap(s.hdr, s.mapSize);
    if (s.fd >= 0)
        close(s.fd);
    m_streams.erase(it);
}

bool PoxicleEffect::Register(int pid, const QDBusUnixFileDescriptor &shm)
{
    if (pid <= 0 || !shm.isValid())
        return false;

    // Keep our own fd: the QDBusUnixFileDescriptor closes its copy on return.
    // The producer sized the memfd, so fstat is the source of truth for the map.
    int fd = dup(shm.fileDescriptor());
    if (fd < 0)
        return false;

    struct stat st {};
    if (fstat(fd, &st) != 0 || st.st_size < (off_t) sizeof(PoxBridgeHeader)) {
        close(fd);
        return false;
    }
    const size_t mapSize = (size_t) st.st_size;

    void *base = mmap(nullptr, mapSize, PROT_READ, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        return false;
    }
    PoxBridgeHeader *hdr = static_cast<PoxBridgeHeader *>(base);
    const uint32_t capacity = hdr->capacity;
    // Reject a stale / wrong / undersized region before we ever read the body.
    if (hdr->magic != POX_BRIDGE_MAGIC || hdr->version != POX_BRIDGE_VERSION ||
        hdr->inst_size != sizeof(PoxInstance) ||
        pox_bridge_map_size(capacity, sizeof(PoxInstance)) > mapSize) {
        munmap(base, mapSize);
        close(fd);
        return false;
    }

    dropStream(pid);   // replace any prior registration (producer respawn)

    ExtStream s;
    s.fd = fd;
    s.hdr = hdr;
    s.mapSize = mapSize;
    s.capacity = capacity;
    m_streams.emplace(pid, std::move(s));

    bindStreams();
    return true;
}

void PoxicleEffect::Unregister(int pid)
{
    dropStream(pid);
}

void PoxicleEffect::Wake(int pid)
{
    auto it = m_streams.find(pid);
    if (it == m_streams.end())
        return;
    it->second.idle = 0;
    if (it->second.window && visible(it->second.window)) {
        KWin::effects->addRepaint(it->second.geom.adjusted(
            -kBandMargin, -kBandMargin, kBandMargin, kBandMargin));
    } else {
        // Registration may have raced window map — retry the bind, then nudge.
        bindStreams();
        KWin::effects->addRepaintFull();
    }
}

void PoxicleEffect::slotWindowAdded(KWin::EffectWindow *w)
{
    bindStreams();   // a stream registered before its window mapped binds here
    maybeAttach(w);  // skips w if a stream now claims it
}

void PoxicleEffect::slotWindowClosed(KWin::EffectWindow *w)
{
    if (w == m_activeWindow) {
        m_activeWindow = nullptr;
        m_activeInstances.clear();
    }
    if (ExtStream *s = streamFor(w)) {
        // The window is gone; unbind but keep the registration (the producer will
        // Unregister, or its stream can rebind to a new window). Clear the band.
        KWin::effects->addRepaint(
            s->geom.adjusted(-kBandMargin, -kBandMargin, kBandMargin, kBandMargin));
        s->window = nullptr;
        s->instances.clear();
    }
    detach(w);
}

// (Re)create the single focus-following overlay engine from the Active rule, or
// free it when no Active rule is set. Called at construction and on reconfigure.
void PoxicleEffect::rebuildActiveEngine()
{
    if (m_activeEngine) {
        pox_engine_free(m_activeEngine);
        m_activeEngine = nullptr;
    }
    m_activeInstances.clear();

    m_activeResolved = m_config.resolveActive();
    if (!m_activeResolved.enabled)
        return;   // no active-window poxicle configured

    m_activeEngine = pox_engine_new();
    m_activeColor = m_activeResolved.color;
    if (m_activeWindow)
        pointActiveAt(m_activeWindow);
}

// Aim the overlay at a window: size it to that window and (re)arm its motion with
// a focus burst, so the active-window poxicle "lands" on the newly focused window.
void PoxicleEffect::pointActiveAt(KWin::EffectWindow *w)
{
    if (!m_activeEngine || !w)
        return;
    const QRectF g = w->frameGeometry();
    m_activeGeom = g;
    pox_engine_set_surface(m_activeEngine, int(g.width()), int(g.height()), 1);
    pox_engine_set_tunables(m_activeEngine, &m_activeResolved.tunables);
    pox_engine_set_kind(m_activeEngine, m_activeResolved.kind,
                        m_activeResolved.reverse, m_activeResolved.color);
    applyPalette(m_activeEngine, m_activeResolved);
    const PoxColor c = m_activeColor.a >= 0.0f
        ? m_activeColor
        : PoxColor{0.55f, 0.78f, 1.0f, 1.0f};
    pox_engine_burst(m_activeEngine, 0.0f, c);
    KWin::effects->addRepaintFull();
}

void PoxicleEffect::slotWindowActivated(KWin::EffectWindow *w)
{
    KWin::EffectWindow *old = m_activeWindow;
    m_activeWindow = (w && eligible(w)) ? w : nullptr;

    // The overlay lives only on the focused window: clear it off the one that
    // just lost focus so its active-window particles don't linger.
    if (old && old != m_activeWindow && visible(old)) {
        const QRectF band = QRectF(old->frameGeometry())
            .adjusted(-kBandMargin, -kBandMargin, kBandMargin, kBandMargin);
        KWin::effects->addRepaint(band);
    }
    m_activeInstances.clear();
    if (m_activeEngine && m_activeWindow)
        pointActiveAt(m_activeWindow);

    // Per-app focus burst (unchanged): a window with its own rule still flares on
    // focus, independent of — and underneath — the active-window overlay.
    auto it = m_activeWindow ? m_windows.find(m_activeWindow) : m_windows.end();
    if (it != m_windows.end()) {
        const PoxColor c = it->second.color.a >= 0.0f
            ? it->second.color
            : PoxColor{0.55f, 0.78f, 1.0f, 1.0f};
        pox_engine_burst(it->second.engine, 0.0f, c);
        KWin::effects->addRepaintFull();
    }
}

void PoxicleEffect::prePaintScreen(KWin::ScreenPrePaintData &data,
                                   std::chrono::milliseconds presentTime)
{
    // Advance + collect once per frame. prePaintScreen runs once per output, so
    // gate on presentTime to avoid double-advancing the sim on multi-output frames.
    if (presentTime > m_lastPresent) {
        const double dt = m_lastPresent.count() == 0
            ? 0.0
            : (presentTime - m_lastPresent).count() / 1000.0;
        m_lastPresent = presentTime;

        m_active = false;

        for (auto &entry : m_windows) {
            KWin::EffectWindow *w = entry.first;
            WinFx &fx = entry.second;

            if (!visible(w)) {
                fx.instances.clear();   // off the current desktop / minimized => don't draw
                continue;
            }

            // Poll geometry (cheap for the few tracked windows) instead of wiring
            // per-window EffectWindow signals — robust against move/resize/races.
            const QRectF g = w->frameGeometry();
            if (g.size() != fx.geom.size())
                pox_engine_set_surface(fx.engine, int(g.width()), int(g.height()), 1);
            fx.geom = g;

            const size_t n = pox_engine_tick(fx.engine, dt,
                                             m_scratch.data(), m_scratch.size());

            // poxicle emits surface-local pixels; shift into global logical coords
            // so KWin's projection matrix lands them around this window on screen.
            // Stored per-window (not one shared list) so paintWindow() can draw each
            // window's particles in its own paint pass and get correct occlusion.
            const float ox = float(g.x());
            const float oy = float(g.y());
            fx.instances.clear();
            for (size_t i = 0; i < n; ++i) {
                PoxInstance inst = m_scratch[i];
                inst.x += ox;
                inst.y += oy;
                fx.instances.push_back(inst);
            }

            if (pox_engine_active(fx.engine))
                m_active = true;
        }

        // The focus-following overlay: tick it against the active window and store
        // its particles in that window's global coords. Drawn on TOP of the
        // window's own per-app particles in paintWindow() (independent layer).
        m_activeInstances.clear();
        if (m_activeEngine && m_activeWindow && visible(m_activeWindow)) {
            const QRectF g = m_activeWindow->frameGeometry();
            if (g.size() != m_activeGeom.size())
                pox_engine_set_surface(m_activeEngine, int(g.width()), int(g.height()), 1);
            m_activeGeom = g;

            const size_t n = pox_engine_tick(m_activeEngine, dt,
                                             m_scratch.data(), m_scratch.size());
            const float ox = float(g.x());
            const float oy = float(g.y());
            for (size_t i = 0; i < n; ++i) {
                PoxInstance inst = m_scratch[i];
                inst.x += ox;
                inst.y += oy;
                m_activeInstances.push_back(inst);
            }
            if (pox_engine_active(m_activeEngine))
                m_active = true;
        }

        // External-source streams: copy each bound producer's latest frame from
        // its shared region into the bound window's global coords. No engine tick
        // — the producer already ran the sim. A quiet producer parks (idle grace)
        // so KWin can sleep until Wake() or a new frame.
        for (auto &kv : m_streams) {
            ExtStream &s = kv.second;
            s.suppressed = false;
            if (!s.window) {
                s.instances.clear();
                continue;
            }

            // Un-minimize edge (minimized -> not): isMinimized() flips back to
            // false at the START of the restore, while the Magic Lamp / Squash
            // animation is still playing. The PAINT_WINDOW_TRANSFORMED guard in
            // paintWindow() does not fire for the lamp's non-affine vertex warp,
            // so without this the ring would snap in at full frameGeometry over the
            // still-animating window. Arm a short grace and hold it back instead;
            // scaled by animationTimeFactor() so it tracks the user's animation
            // speed (and is ~0 when animations are effectively instant).
            const bool minimizedNow = s.window->isMinimized();
            if (s.wasMinimized && !minimizedNow) {
                s.suppressUntil = presentTime + std::chrono::milliseconds(
                    int(kUnminimizeGraceMs * KWin::effects->animationTimeFactor()));
            }
            s.wasMinimized = minimizedNow;
            s.suppressed = presentTime < s.suppressUntil;

            if (!visible(s.window) || s.suppressed) {
                s.instances.clear();
                continue;
            }
            s.geom = s.window->frameGeometry();

            if (readStreamFrame(s.hdr, s.capacity, s.lastSeq, m_streamScratch)) {
                s.idle = 0;
                const float ox = float(s.geom.x());
                const float oy = float(s.geom.y());
                s.instances.clear();
                s.instances.reserve(m_streamScratch.size());
                for (PoxInstance inst : m_streamScratch) {
                    inst.x += ox;
                    inst.y += oy;
                    s.instances.push_back(inst);
                }
            } else if (++s.idle >= kStreamIdleGrace) {
                s.instances.clear();   // producer quiet -> stop drawing + repainting
            }

            if (!s.instances.empty())
                m_active = true;
        }
    }

    KWin::effects->prePaintScreen(data, presentTime);
}

void PoxicleEffect::prePaintWindow(KWin::RenderView *view, KWin::EffectWindow *w,
                                   KWin::WindowPrePaintData &data,
                                   std::chrono::milliseconds presentTime)
{
    // Our particles live in a ring OUTSIDE the window frame. Tell KWin this window
    // paints that larger band (in device coords) so the exterior ring isn't clipped
    // to the window rect when we draw it in paintWindow().
    auto it = m_windows.find(w);
    const bool perApp = (it != m_windows.end() && !it->second.instances.empty());
    const bool overlay = (w == m_activeWindow && !m_activeInstances.empty());
    ExtStream *st = streamFor(w);
    const bool ext = (st && !st->instances.empty());
    if (visible(w) && (perApp || overlay || ext)) {
        const QRectF base = perApp ? it->second.geom : (ext ? st->geom : m_activeGeom);
        const QRectF band = base.adjusted(-kBandMargin, -kBandMargin,
                                          kBandMargin, kBandMargin);
        data.devicePaint += view->mapToDeviceCoordinatesAligned(KWin::RectF(band));
    }

    KWin::effects->prePaintWindow(view, w, data, presentTime);
}

void PoxicleEffect::paintWindow(const KWin::RenderTarget &renderTarget,
                                const KWin::RenderViewport &viewport,
                                KWin::EffectWindow *w, int mask,
                                const KWin::Region &deviceRegion,
                                KWin::WindowPaintData &data)
{
    // Paint the window itself first, then this window's particles on top of it.
    // Windows stacked above are painted in later paintWindow() passes and composite
    // over these particles — so a window's particles are correctly occluded by the
    // windows in front of it. (The old paintScreen() path drew every window's
    // particles above the whole scene, so they floated over everything.)
    KWin::effects->paintWindow(renderTarget, viewport, w, mask, deviceRegion, data);

    // While another effect applies an AFFINE transform to this window — overview,
    // desktop grid, Squash — our particles would be drawn at the settled
    // frameGeometry, detached from the in-flight transform (a crisp rectangle of
    // particles floating where the window WILL land). Skip them until it settles.
    // Interactive move/resize is exempt: that is a pure translation we follow via
    // data.translation() below. NB: the Magic Lamp un-minimize warp is non-affine
    // and does NOT set this flag for us — that case is handled by the un-minimize
    // grace below (and in prePaintScreen), not here.
    if ((mask & PAINT_WINDOW_TRANSFORMED) && !w->isUserMove() && !w->isUserResize())
        return;

    // Hold every particle layer (per-app, overlay, and stream) for a streamed
    // window still inside its un-minimize grace — prePaintScreen tracks the
    // deadline and already clears the stream's own instances; this also keeps the
    // focus overlay off it until the restore animation settles.
    if (ExtStream *gs = streamFor(w); gs && gs->suppressed)
        return;

    auto it = m_windows.find(w);
    const bool perApp = (it != m_windows.end() && !it->second.instances.empty());
    const bool overlay = (w == m_activeWindow && !m_activeInstances.empty());
    ExtStream *st = streamFor(w);
    const bool ext = (st && !st->instances.empty());
    if ((!perApp && !overlay && !ext) || !m_gl || !visible(w))
        return;

    // poxicle-gl clobbers program / VAO / array buffer / blend and does not
    // restore — save & restore here so KWin's own rendering is undisturbed.
    GLint prevProgram = 0, prevVao = 0, prevBuffer = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuffer);
    const GLboolean prevBlend = glIsEnabled(GL_BLEND);
    GLint sRgb = 0, dRgb = 0, sAlpha = 0, dAlpha = 0;
    glGetIntegerv(GL_BLEND_SRC_RGB, &sRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &dRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &sAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dAlpha);

    // KWin's projectionMatrix maps logical -> clip (handles scale + output
    // transform); QMatrix4x4::constData() is column-major, as poxicle expects.
    QMatrix4x4 mvp = viewport.projectionMatrix();
    // Particles are positioned from the window's *committed* frameGeometry (polled
    // in prePaintScreen). During an interactive move KWin paints the window content
    // at frameGeometry + this per-frame translation while the committed geometry
    // lags a frame or two behind the drag — so without this the window slides ahead
    // and the particles trail it. data.translation() is in logical coords (same
    // space as projectionMatrix and our particle positions); applying it here makes
    // the particles undergo the exact transform KWin applies to the window. When the
    // window isn't being transformed this is identity, so it's a no-op at rest.
    mvp.translate(data.xTranslation(), data.yTranslation(), data.zTranslation());
    // Base layer first (per-app OR external-source — mutually exclusive per
    // window), then the active-window overlay on top. All use the same window
    // transform, so they stay glued together as the window moves.
    if (perApp)
        pox_gl_render_mvp(m_gl, it->second.instances.data(), it->second.instances.size(),
                          mvp.constData());
    if (ext)
        pox_gl_render_mvp(m_gl, st->instances.data(), st->instances.size(),
                          mvp.constData());
    if (overlay)
        pox_gl_render_mvp(m_gl, m_activeInstances.data(), m_activeInstances.size(),
                          mvp.constData());

    glUseProgram(GLuint(prevProgram));
    glBindVertexArray(GLuint(prevVao));
    glBindBuffer(GL_ARRAY_BUFFER, GLuint(prevBuffer));
    if (prevBlend)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);
    glBlendFuncSeparate(GLenum(sRgb), GLenum(dRgb), GLenum(sAlpha), GLenum(dAlpha));
}

void PoxicleEffect::postPaintScreen()
{
    // Keep frames coming while anything animates, but repaint ONLY each tracked
    // window's edge band — not the whole screen. KWin then recomposites a small
    // area per frame instead of the full panel (the overdraw cost that matters on
    // a big display). idle = no repaint requested, so KWin can sleep.
    if (m_active) {
        for (const auto &entry : m_windows) {
            if (!visible(entry.first))
                continue;
            const QRectF band = entry.second.geom.adjusted(-kBandMargin, -kBandMargin,
                                                           kBandMargin, kBandMargin);
            KWin::effects->addRepaint(band);
        }
        // The active window may have no per-app rule (not in m_windows) yet still
        // carry the overlay — repaint its band too.
        if (m_activeEngine && m_activeWindow && visible(m_activeWindow)) {
            const QRectF band = m_activeGeom.adjusted(-kBandMargin, -kBandMargin,
                                                      kBandMargin, kBandMargin);
            KWin::effects->addRepaint(band);
        }
        // External-source windows live outside m_windows; repaint their bands so
        // the streamed particles keep compositing while the producer is active.
        for (const auto &kv : m_streams) {
            const ExtStream &s = kv.second;
            if (s.window && visible(s.window) && !s.instances.empty()) {
                KWin::effects->addRepaint(s.geom.adjusted(
                    -kBandMargin, -kBandMargin, kBandMargin, kBandMargin));
            }
        }
    }
    KWin::effects->postPaintScreen();
}

bool PoxicleEffect::isActive() const
{
    return !m_windows.empty() || m_activeEngine != nullptr || !m_streams.empty();
}

void PoxicleEffect::reconfigure(ReconfigureFlags)
{
    m_config.load();
    // Re-evaluate every window against the new config — windows may now be
    // enabled/disabled or use a different preset, so tear down and re-attach.
    for (auto &entry : m_windows) {
        if (entry.second.engine)
            pox_engine_free(entry.second.engine);
    }
    m_windows.clear();
    const auto windows = KWin::effects->stackingOrder();
    for (KWin::EffectWindow *w : windows)
        maybeAttach(w);

    // Re-seed the focus-following overlay too (its Active rule may have changed).
    KWin::EffectWindow *aw = KWin::effects->activeWindow();
    m_activeWindow = (aw && eligible(aw)) ? aw : nullptr;
    rebuildActiveEngine();

    KWin::effects->addRepaintFull();
}

KWIN_EFFECT_FACTORY_SUPPORTED(PoxicleEffect, "metadata.json",
                              return KWin::effects->isOpenGLCompositing();)

#include "poxicleeffect.moc"
