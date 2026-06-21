/* poxicleeffect.h
 *
 * KWin (Plasma 6, Wayland) binary OpenGL effect that draws poxicle edge
 * particles per window. It reuses poxicle's simulation core and GLES renderer
 * through the bring-your-own-context path (pox_gl_render_mvp), driven by KWin's
 * own projection matrix. No poxicle-wl / subsurface here — KWin owns the surface
 * and the GL context.
 *
 * See poxicle/docs/kwin-effect-handoff.md for the full design + milestones.
 */
#pragma once

#include <effect/effect.h>

#include <chrono>
#include <unordered_map>
#include <vector>

#include <QRectF>
#include <QDBusUnixFileDescriptor>

#include <poxicle.h>
#include <poxicle-gl.h>

#include "poxbridge.h"
#include "poxconfig.h"

class PoxicleEffect : public KWin::Effect
{
    Q_OBJECT
    // Export the scriptable Register/Unregister/Wake slots under the interface
    // name the producer calls. MUST equal POX_BRIDGE_IFACE in poxbridge.h — a
    // literal because MOC processes Q_CLASSINFO and macro expansion there is
    // unreliable. Without this, QtDBus names the interface after the C++ class
    // and calls to org.ninez.PoxicleBridge fail with UnknownInterface.
    Q_CLASSINFO("D-Bus Interface", "org.ninez.PoxicleBridge")

public:
    PoxicleEffect();
    ~PoxicleEffect() override;

    void prePaintScreen(KWin::ScreenPrePaintData &data) override;
    void prePaintWindow(KWin::RenderView *view, KWin::EffectWindow *w,
                        KWin::WindowPrePaintData &data) override;
    void paintWindow(const KWin::RenderTarget &renderTarget,
                     const KWin::RenderViewport &viewport,
                     KWin::EffectWindow *w, int mask,
                     const KWin::Region &deviceRegion,
                     KWin::WindowPaintData &data) override;
    void postPaintScreen() override;
    bool isActive() const override;
    void reconfigure(ReconfigureFlags flags) override;

    // Late in the chain so our per-window draw lands over that window's own
    // content (windows stacked above are painted afterwards and occlude it).
    int requestedEffectChainPosition() const override { return 90; }

    // D-Bus org.ninez.PoxicleBridge — the producer side of external-source mode.
    // A Wayland client (Chiguiro) that runs its own sim hands the effect a memfd
    // (Register), then writes ready-to-draw instances into it each frame; the
    // effect positions + draws them on the client's window. Scriptable so they
    // are reachable over the session bus; see poxbridge.h for the protocol.
public Q_SLOTS:
    Q_SCRIPTABLE bool Register(int pid, const QDBusUnixFileDescriptor &shm);
    Q_SCRIPTABLE void Unregister(int pid);
    Q_SCRIPTABLE void Wake(int pid);   // un-park the repaint loop on idle->active

private Q_SLOTS:
    void slotWindowAdded(KWin::EffectWindow *w);
    void slotWindowClosed(KWin::EffectWindow *w);
    void slotWindowActivated(KWin::EffectWindow *w);

private:
    struct WinFx {
        PoxEngine *engine = nullptr;
        QRectF     geom;                       // last frameGeometry (logical coords)
        PoxColor   color { -1, -1, -1, -1 };   // per-app particle color; a<0 = none
        std::vector<PoxInstance> instances;    // this frame's particles, global logical coords
    };

    // One registered external-source stream (producer pid -> shared region). The
    // instances come from the mapped memfd, NOT a local engine; the effect just
    // offsets them by the bound window's rect and draws them.
    struct ExtStream {
        int                      fd = -1;       // dup'd memfd (owned; closed on drop)
        PoxBridgeHeader         *hdr = nullptr; // mmap base (read-only)
        size_t                   mapSize = 0;
        uint32_t                 capacity = 0;
        uint32_t                 lastSeq = 0;   // last seqlock value drawn
        int                      idle = 0;      // consecutive polls with no new frame
        KWin::EffectWindow      *window = nullptr;  // bound window (null = unbound)
        QRectF                   geom;          // bound window geometry (logical)
        std::vector<PoxInstance> instances;     // this frame, global logical coords
        bool                     wasMinimized = false;   // previous frame's isMinimized()
        std::chrono::milliseconds suppressUntil{0};      // hold particles until this present-time
        bool                     suppressed = false;     // this frame: inside the un-minimize grace
    };

    bool eligible(KWin::EffectWindow *w) const;   // a window type we ever decorate
    bool visible(KWin::EffectWindow *w) const;    // on current desktop, not minimized
    void maybeAttach(KWin::EffectWindow *w);
    void detach(KWin::EffectWindow *w);
    void rebuildActiveEngine();                   // (re)create/free the focus overlay engine
    void pointActiveAt(KWin::EffectWindow *w);    // aim the overlay at a window + re-arm it

    // External-source helpers.
    void        bindStreams();                    // bind unbound streams to a pid-matched window
    void        dropStream(int pid);              // unmap + close + erase a registered stream
    ExtStream  *streamFor(KWin::EffectWindow *w); // the stream bound to w, or null
    bool        streamClaims(KWin::EffectWindow *w) const;  // some stream owns w

    std::unordered_map<KWin::EffectWindow *, WinFx> m_windows;
    std::unordered_map<int, ExtStream>              m_streams;   // keyed by producer pid
    std::vector<PoxInstance>  m_scratch;    // one window's per-frame tick output
    std::vector<PoxInstance>  m_streamScratch;  // one stream's per-frame shm read
    PoxGL                    *m_gl = nullptr;
    PoxConfig                 m_config;
    std::chrono::milliseconds m_lastPresent{0};
    bool                      m_active = false;

    // Focus-following "active window" overlay: a single engine drawn on whichever
    // window currently has focus, ON TOP of that window's own per-app particles
    // (the two are independent layers). Null engine => no Active rule configured.
    KWin::EffectWindow       *m_activeWindow = nullptr;
    PoxEngine                *m_activeEngine = nullptr;
    PoxResolved               m_activeResolved;
    PoxColor                  m_activeColor { -1, -1, -1, -1 };
    std::vector<PoxInstance>  m_activeInstances;   // overlay particles, global logical coords
    QRectF                    m_activeGeom;        // active window's last geometry
};
