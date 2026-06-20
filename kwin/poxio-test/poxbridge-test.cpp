/* poxbridge-test.cpp — standalone external-source PRODUCER for the poxicle-kwin
 * compositor effect. No Chiguiro: it stands in for "a Wayland client that sims its
 * own particles and streams them to the effect", so the effect's external-source
 * path (org.ninez.PoxicleBridge + the memfd seqlock) can be exercised in a nested
 * KWin on its own.
 *
 * It maps a memfd, animates a ring of PoxInstance marching around a WxH box
 * (window-local coords), publishes each frame through the poxbridge seqlock, and
 * Registers the region against a TARGET window's pid. The effect binds the stream
 * to the window whose pid matches and draws the ring on it.
 *
 *   poxbridge-test <target-pid> [width] [height] [count]
 *
 * Find a target pid in the nested session with e.g. `pidof konsole`. Ctrl-C
 * Unregisters and exits.
 */
#include <sys/mman.h>   // memfd_create needs _GNU_SOURCE (set by the build's compiler flags)
#include <unistd.h>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <QCoreApplication>
#include <QTimer>
#include <QElapsedTimer>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>

#include <poxicle.h>      // PoxInstance, PoxShape
#include "poxbridge.h"    // PoxBridgeHeader + protocol

namespace {
volatile std::sig_atomic_t g_stop = 0;
void onSigint(int) { g_stop = 1; }

// Map t in [0,1) to a point marching clockwise around the [0,W]x[0,H] perimeter,
// inset by `m` so the ring sits just inside the window edge.
void perimeter(double t, double W, double H, double m, float &x, float &y)
{
    const double w = W - 2 * m, h = H - 2 * m;
    const double total = 2 * (w + h);
    double d = t * total;
    if (d < w)            { x = float(m + d);          y = float(m); }
    else if (d < w + h)   { x = float(W - m);          y = float(m + (d - w)); }
    else if (d < 2*w + h) { x = float(W - m - (d - (w + h))); y = float(H - m); }
    else                  { x = float(m);              y = float(H - m - (d - (2*w + h))); }
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <target-pid> [width] [height] [count]\n", argv[0]);
        return 2;
    }
    const int      targetPid = std::atoi(argv[1]);
    const uint32_t W     = argc > 2 ? uint32_t(std::atoi(argv[2])) : 800u;
    const uint32_t H     = argc > 3 ? uint32_t(std::atoi(argv[3])) : 500u;
    const uint32_t count = argc > 4 ? uint32_t(std::atoi(argv[4])) : 120u;
    const uint32_t capacity = count;

    if (targetPid <= 0) { std::fprintf(stderr, "bad pid\n"); return 2; }

    // ---- shared region (memfd) ----
    const uint64_t mapSize = pox_bridge_map_size(capacity, sizeof(PoxInstance));
    int fd = memfd_create("poxbridge", MFD_CLOEXEC);
    if (fd < 0) { std::perror("memfd_create"); return 1; }
    if (ftruncate(fd, off_t(mapSize)) != 0) { std::perror("ftruncate"); return 1; }

    void *base = mmap(nullptr, mapSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { std::perror("mmap"); return 1; }

    auto *hdr = static_cast<PoxBridgeHeader *>(base);
    std::memset(hdr, 0, sizeof(*hdr));
    hdr->magic     = POX_BRIDGE_MAGIC;
    hdr->version   = POX_BRIDGE_VERSION;
    hdr->capacity  = capacity;
    hdr->inst_size = uint32_t(sizeof(PoxInstance));
    hdr->width     = W;
    hdr->height    = H;
    hdr->seq       = 0;   // even: no frame yet
    auto *body = reinterpret_cast<PoxInstance *>(
        reinterpret_cast<char *>(hdr) + sizeof(PoxBridgeHeader));

    // ---- publish one frame through the seqlock (writer side of poxbridge.h) ----
    QElapsedTimer clock;
    clock.start();
    auto publish = [&]() {
        const double phase = clock.elapsed() / 4000.0;   // a lap every 4s
        for (uint32_t i = 0; i < count; ++i) {
            double t = double(i) / count + phase;
            t -= std::floor(t);
            float x, y;
            perimeter(t, W, H, 6.0, x, y);
            PoxInstance &p = body[i];
            p.x = x; p.y = y;
            p.size = 10.0f;
            p.angle = 0.0f;
            p.shape = POX_SHAPE_SQUARE;
            // fade brightness along the ring so motion is obvious
            const float a = 0.35f + 0.65f * float(0.5 + 0.5 * std::sin(t * 2 * M_PI));
            p.color = PoxColor{0.45f, 0.85f, 1.0f, a};
        }
        const uint32_t s = hdr->seq;                              // even
        __atomic_store_n(&hdr->seq, s + 1, __ATOMIC_RELAXED);     // odd: writing
        __atomic_thread_fence(__ATOMIC_RELEASE);
        hdr->count = count;
        __atomic_store_n(&hdr->seq, s + 2, __ATOMIC_RELEASE);     // even: published
    };
    publish();   // have a valid frame ready before the effect binds

    // ---- register with the effect ----
    QDBusInterface iface(QStringLiteral(POX_BRIDGE_SERVICE),
                         QStringLiteral(POX_BRIDGE_PATH),
                         QStringLiteral(POX_BRIDGE_IFACE),
                         QDBusConnection::sessionBus());
    if (!iface.isValid()) {
        std::fprintf(stderr, "poxbridge service not on the bus — is the effect loaded?\n");
        return 1;
    }
    QDBusReply<bool> reply = iface.call(QStringLiteral("Register"), targetPid,
                                        QVariant::fromValue(QDBusUnixFileDescriptor(fd)));
    if (!reply.isValid() || !reply.value()) {
        std::fprintf(stderr, "Register failed: %s\n",
                     qPrintable(reply.error().message()));
        return 1;
    }
    std::printf("registered stream for pid %d (%ux%u, %u particles)\n",
                targetPid, W, H, count);

    std::signal(SIGINT, onSigint);
    std::signal(SIGTERM, onSigint);

    // ~60 Hz: animate + publish; also poll the stop flag (SIGINT isn't safe to do
    // D-Bus from, so we tear down here on the main loop).
    QTimer timer;
    timer.setInterval(16);
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        if (g_stop) {
            iface.call(QStringLiteral("Unregister"), targetPid);
            QCoreApplication::quit();
            return;
        }
        publish();
    });
    timer.start();

    const int rc = app.exec();
    munmap(base, mapSize);
    close(fd);
    return rc;
}
