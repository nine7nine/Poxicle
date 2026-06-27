/* poxicleeffect.cpp — see poxicleeffect.h. */
#include "poxicleeffect.h"

#include <effect/effecthandler.h>
#include <effect/effectwindow.h>
#include <core/renderviewport.h>
#include <core/rect.h>
#include <core/output.h>          // KWin::LogicalOutput::geometry() (panel edge mask)
#include <core/renderbackend.h>   // KWin::OutputFrame (present-time source)
#include <scene/scene.h>   // KWin::RenderView

#include <algorithm>   // std::min/std::max for the ring-repaint thickness

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

// How long (logical ms) to hold a window's particles back after it starts
// un-minimizing / un-hiding — so the ring doesn't snap in at full frameGeometry
// over the still-animating window (Magic Lamp / Squash / Show-Desktop). The base
// comes from the active minimize animation's own duration (PoxConfig::
// minimizeAnimMs(), e.g. Magic Lamp's AnimationDuration), floored by the tunable
// PoxConfig::unminimizeGraceMs() (default 350), then scaled by the compositor's
// animationTimeFactor() so it tracks the user's animation-speed setting. This
// small tail is added on top so the ring never reappears a frame early.
constexpr int kRestoreTailMs = 60;

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

// Request a per-frame repaint of ONLY the ring this frame's particles occupy,
// not the whole window rect. `t` is the deepest any block reaches in from its
// nearest edge; four strips of that thickness contain every block (each block
// sits within `t` of an edge) and leave the window interior untouched. That
// interior is the cost: damaging it forces KWin to recomposite the whole stack
// behind a translucent (glass) window across the window's full area every frame
// — near-free for the thin ring, expensive for the full rect on a big display.
// A deep effect (centre burst / spray) grows `t` until the strips meet, falling
// back to a full-window band; it never under-damages, so no trails. Empty /
// invisible => no repaint, so an idle tracked window lets KWin sleep.
void requestRingRepaint(const QRectF &geom, const std::vector<PoxInstance> &insts)
{
    if (insts.empty() || geom.isEmpty())
        return;

    const qreal x0 = geom.x(), y0 = geom.y();
    const qreal x1 = x0 + geom.width(), y1 = y0 + geom.height();
    qreal t = 0.0;
    for (const PoxInstance &i : insts) {
        // Distance from each edge to the block's far side; the min is how thick
        // that block's nearest-edge strip must be to fully contain it.
        const qreal d = std::min(std::min(qreal(i.y + i.size) - y0, y1 - qreal(i.y)),
                                 std::min(qreal(i.x + i.size) - x0, x1 - qreal(i.x)));
        if (d > t)
            t = d;
    }
    if (t < 0.0)
        t = 0.0;
    t = std::min(t, std::max(geom.width(), geom.height()));   // cap at full band

    const qreal M = kBandMargin;
    KWin::effects->addRepaint(QRectF(x0 - M, y0 - M, geom.width() + 2 * M, t + M));    // top
    KWin::effects->addRepaint(QRectF(x0 - M, y1 - t, geom.width() + 2 * M, t + M));    // bottom
    KWin::effects->addRepaint(QRectF(x0 - M, y0 - M, t + M, geom.height() + 2 * M));   // left
    KWin::effects->addRepaint(QRectF(x1 - t, y0 - M, t + M, geom.height() + 2 * M));   // right
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
    // Un-hide (Show-Desktop off) restores every window at once — a global signal.
    connect(KWin::effects, &KWin::EffectsHandler::showingDesktopChanged,
            this, &PoxicleEffect::slotShowingDesktopChanged);

    // Attach to windows already open when the effect loads, and watch each one's
    // minimized state so we can hold its ring back while it animates back in.
    const auto windows = KWin::effects->stackingOrder();
    for (KWin::EffectWindow *w : windows) {
        connect(w, &KWin::EffectWindow::minimizedChanged,
                this, &PoxicleEffect::slotMinimizedChanged);
        maybeAttach(w);
    }

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
    // A fullscreen window (video, game, presentation) gets no ring — the particles
    // would draw over its edge-to-edge content. Checked here, the single per-frame
    // gate every draw/tick/repaint path runs through, so it covers per-app, the
    // focus overlay and external streams alike, and tracks the state live (the ring
    // clears the moment a window enters fullscreen and returns when it leaves).
    return w && w->isOnCurrentDesktop() && !w->isMinimized()
        && !w->isHiddenByShowDesktop() && !w->isFullScreen();
}

// A window started restoring from minimized — KWin flips isMinimized() to false at
// the START of the animation, while the warp still plays. Hold the ring back until
// the animation is done. Keyed on the window, so it works whether this window draws
// per-app, overlay or streamed particles, and survives a producer re-registering.
void PoxicleEffect::slotMinimizedChanged(KWin::EffectWindow *w)
{
    if (w && !w->isMinimized())
        armRestore(w);
}

// Show-Desktop turned off: every hidden window animates back in at once.
void PoxicleEffect::slotShowingDesktopChanged(bool showing)
{
    if (showing)
        return;
    for (const auto &kv : m_windows)
        armRestore(kv.first);
    for (const auto &kv : m_streams)
        armRestore(kv.second.window);
    armRestore(m_activeWindow);
}

// Begin (or extend) a restore hold on w, long enough to cover the active minimize
// animation. A non-affine warp (Magic Lamp) shows no transform we can release on,
// so we hold for the animation's configured length; an affine restore (Squash /
// Glide) is released early the moment its scale settles (see paintWindow). The
// floor (unminimizeGraceMs) covers effects we can't read a duration for.
void PoxicleEffect::armRestore(KWin::EffectWindow *w)
{
    if (!w || !eligible(w))
        return;
    const int base = std::max(m_config.minimizeAnimMs(), m_config.unminimizeGraceMs());
    const int ms = int(base * KWin::effects->animationTimeFactor()) + kRestoreTailMs;
    Restore &r = m_restore[w];
    r.until = m_lastPresent + std::chrono::milliseconds(ms);
    r.sawScale = false;
    KWin::effects->addRepaintFull();   // keep compositing so the hold can release
}

// True while w is inside a *timed* (non-affine) restore hold — i.e. the deadline
// has not passed and no foreign affine transform has been seen (those release via
// the scale guard instead). Used to suppress drawing + skip wasteful repaints.
bool PoxicleEffect::restoreHeld(KWin::EffectWindow *w) const
{
    const auto it = m_restore.find(w);
    return it != m_restore.end() && !it->second.sawScale
        && it->second.until.count() != 0 && m_lastPresent < it->second.until;
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

void PoxicleEffect::applyPanelEdgeMask(KWin::EffectWindow *w, PoxEngine *e) const
{
    // An edge gets the ring only if it's NOT flush against the screen border —
    // i.e. there's a gap between it and the matching screen edge. So a top panel
    // spanning the width lights only its bottom edge; a centred (non-spanning) top
    // panel adds its left/right edges; a floating panel (gap all round) gets all
    // four. No screen => fall back to the full ring.
    int top = 1, right = 1, bottom = 1, left = 1;
    if (KWin::LogicalOutput *o = w->screen()) {
        const QRectF pg = w->frameGeometry();
        const KWin::Rect sg = o->geometry();
        const qreal eps = 1.0;   // tolerate sub-pixel flushness
        const int sgL = sg.x(),               sgT = sg.y();
        const int sgR = sg.x() + sg.width(),  sgB = sg.y() + sg.height();
        top    = (pg.top()    > sgT + eps) ? 1 : 0;
        left   = (pg.left()   > sgL + eps) ? 1 : 0;
        right  = (pg.right()  < sgR - eps) ? 1 : 0;
        bottom = (pg.bottom() < sgB - eps) ? 1 : 0;
    }
    pox_engine_set_edge_mask(e, top, right, bottom, left);
}

void PoxicleEffect::maybeAttach(KWin::EffectWindow *w)
{
    if (!w || m_windows.count(w))
        return;
    // Normal windows are gated by eligible(); a dock (the desktop panel) is admitted
    // only via the Panel target. eligible() still rejects docks on purpose, so a
    // panel never becomes the active-overlay or a per-app window.
    const bool dock = w->isDock();
    if (!dock && !eligible(w))
        return;
    if (streamClaims(w))
        return;   // external-source window: the producer drives it, our rules don't

    const PoxResolved r = dock ? m_config.resolvePanel()
                               : m_config.resolve(w->windowClass());
    if (!r.enabled)
        return;   // preset "none" / no Panel target => this window draws nothing

    WinFx fx;
    fx.engine = pox_engine_new();
    pox_engine_set_seed(fx.engine, ++m_seedSeq);   // own clock: don't lockstep with other rings
    fx.isPanel = dock;
    fx.geom = w->frameGeometry();
    pox_engine_set_surface(fx.engine, int(fx.geom.width()), int(fx.geom.height()), 1);
    if (dock)
        // Panels keep SHARP corners (independent of the window corner-rounding
        // controls) and only ring their interior-facing edges.
        applyPanelEdgeMask(w, fx.engine);
    else
        pox_engine_set_corner_radius(fx.engine, m_config.cornerTop(), m_config.cornerBottom());
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
    if (w)
        connect(w, &KWin::EffectWindow::minimizedChanged,
                this, &PoxicleEffect::slotMinimizedChanged);
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
    m_restore.erase(w);
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
    pox_engine_set_seed(m_activeEngine, ++m_seedSeq);   // overlay runs on its own clock too
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
    pox_engine_set_corner_radius(m_activeEngine, m_config.cornerTop(), m_config.cornerBottom());
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

void PoxicleEffect::prePaintScreen(KWin::ScreenPrePaintData &data)
{
    // KWin dropped the presentTime argument; the frame's target pageflip time is
    // the present-time equivalent (a steady_clock time_point). Fall back to the
    // last value when no frame is attached so the sim simply doesn't re-advance.
    const std::chrono::milliseconds presentTime = data.frame
        ? std::chrono::duration_cast<std::chrono::milliseconds>(
              data.frame->targetPageflipTime().time_since_epoch())
        : m_lastPresent;

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

            if (!visible(w) || restoreHeld(w)) {
                fx.instances.clear();   // hidden, or held back mid un-minimize => don't draw
                continue;
            }

            // Poll geometry (cheap for the few tracked windows) instead of wiring
            // per-window EffectWindow signals — robust against move/resize/races.
            const QRectF g = w->frameGeometry();
            if (g.size() != fx.geom.size())
                pox_engine_set_surface(fx.engine, int(g.width()), int(g.height()), 1);
            // A panel moved or resized (e.g. edge changed, width mode toggled) can
            // flip which edges face the screen interior — recompute its mask.
            if (fx.isPanel && g != fx.geom)
                applyPanelEdgeMask(w, fx.engine);
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
        if (m_activeEngine && m_activeWindow && visible(m_activeWindow)
            && !restoreHeld(m_activeWindow)) {
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
            if (!s.window) {
                s.instances.clear();
                continue;
            }

            // Hold the ring back while the window animates back from minimized /
            // Show-Desktop. isMinimized() flips to false at the START of the restore
            // (the Magic Lamp warp still plays), and that warp is non-affine so the
            // scale guard in paintWindow() never fires for it — without the hold the
            // ring would snap in at full frameGeometry over the still-moving window.
            // The hold is armed from KWin's minimizedChanged / showingDesktopChanged
            // signals (slotMinimizedChanged / slotShowingDesktopChanged), keyed on
            // the window so it survives the producer re-registering its stream.
            if (!visible(s.window) || restoreHeld(s.window)) {
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

    KWin::effects->prePaintScreen(data);
}

void PoxicleEffect::prePaintWindow(KWin::RenderView *view, KWin::EffectWindow *w,
                                   KWin::WindowPrePaintData &data)
{
    // Our particles live in a ring OUTSIDE the window frame. KWin 6.7 removed the
    // per-window paint-region expansion (WindowPrePaintData::devicePaint); the only
    // way to escape the window's bounding-rect scissor is to mark the window
    // transformed, which disables that per-window clip for its paint pass so
    // paintWindow() can draw beyond the frame. We only do this when this window
    // actually has particles this frame, so an idle window keeps the cheap
    // region-limited paint path.
    auto it = m_windows.find(w);
    const bool perApp = (it != m_windows.end() && !it->second.instances.empty());
    const bool overlay = (w == m_activeWindow && !m_activeInstances.empty());
    ExtStream *st = streamFor(w);
    const bool ext = (st && !st->instances.empty());
    if (visible(w) && (perApp || overlay || ext))
        data.setTransformed();

    KWin::effects->prePaintWindow(view, w, data);
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

    // A FOREIGN AFFINE transform on this window — overview, desktop grid, Squash,
    // Glide, maximize — would leave our particles drawn at the settled frameGeometry,
    // a crisp rectangle floating where the window WILL land. Suppress until it
    // settles. Interactive move/resize is exempt: that is a pure translation we
    // follow via data.*Translation() below. We set PAINT_WINDOW_TRANSFORMED ourselves
    // (above, to paint outside the frame), so detect a foreign transform by a
    // non-identity scale, not that bit.
    const bool affine = (data.xScale() != 1.0 || data.yScale() != 1.0 || data.zScale() != 1.0)
                        && !w->isUserMove() && !w->isUserResize();

    // The restore hold (armed from minimizedChanged / showingDesktopChanged) covers
    // every particle layer — per-app, overlay and stream — for a window animating
    // back from minimized / Show-Desktop. An affine restore (Squash / Glide) is
    // released the instant its scale settles; a non-affine one (Magic Lamp shows no
    // scale) is held for the timed deadline. prePaintScreen already cleared the
    // timed-held instances; this also keeps the focus overlay off the window.
    if (auto rit = m_restore.find(w); rit != m_restore.end()) {
        if (affine) {
            rit->second.sawScale = true;   // affine restore: release on scale settle
            return;
        }
        if (rit->second.sawScale)
            m_restore.erase(rit);          // that affine restore has settled -> release
        else if (rit->second.until.count() != 0 && m_lastPresent < rit->second.until)
            return;                        // non-affine restore (Magic Lamp) still playing
    } else if (affine) {
        return;                            // foreign affine transform, no restore pending
    }

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
    // Keep frames coming while anything animates, but repaint ONLY the ring each
    // source's particles occupy this frame (requestRingRepaint) — not the whole
    // window rect. Damaging a translucent window's interior forces KWin to
    // recomposite the whole stack behind it; the ring is a thin frame, so this is
    // the dominant overdraw win on a big display. No particles / invisible => no
    // repaint requested, so KWin can sleep.
    if (m_active) {
        for (const auto &entry : m_windows) {
            if (visible(entry.first))
                requestRingRepaint(entry.second.geom, entry.second.instances);
        }
        // The active window may have no per-app rule (not in m_windows) yet still
        // carry the focus overlay — repaint its ring too.
        if (m_activeEngine && m_activeWindow && visible(m_activeWindow))
            requestRingRepaint(m_activeGeom, m_activeInstances);
        // External-source windows live outside m_windows; repaint their rings so
        // the streamed particles keep compositing while the producer is active.
        for (const auto &kv : m_streams) {
            const ExtStream &s = kv.second;
            if (s.window && visible(s.window))
                requestRingRepaint(s.geom, s.instances);
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
