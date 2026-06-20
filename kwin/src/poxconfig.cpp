/* poxconfig.cpp — see poxconfig.h. */
#include "poxconfig.h"

#include <QColor>
#include <QStringList>

#include <KConfigGroup>
#include <KSharedConfig>

namespace {

// Build a PoxTunables from values in a fixed order (avoids designated-initializer
// ordering hazards). Order matches the packed Preset-<name> serialization.
PoxTunables mk(float speed, int thickness, float tail, float pulseDepth, float pulseSpeed,
               float envAtk, float envRel, int envCurve, int releaseMode, int shape, int gap,
               float thkAtk, float thkRel, int thkCurve, int thkReleaseMode)
{
    PoxTunables t;
    t.speed = speed;
    t.thickness = thickness;
    t.tail_length = tail;
    t.pulse_depth = pulseDepth;
    t.pulse_speed = pulseSpeed;
    t.env_attack = envAtk;
    t.env_release = envRel;
    t.env_curve = envCurve;
    t.release_mode = PoxReleaseMode(releaseMode);
    t.shape = PoxShape(shape);
    t.gap = gap;
    t.thk_attack = thkAtk;
    t.thk_release = thkRel;
    t.thk_curve = thkCurve;
    t.thk_release_mode = PoxReleaseMode(thkReleaseMode);
    return t;
}

struct Seed { const char *name; PoxTunables tun; };

// Ported verbatim from Chiguiro's gschema preset defaults (data/...gschema.xml.in).
// Fields: speed, thickness, tail, pulseDepth, pulseSpeed, envAtk, envRel, envCurve,
//         releaseMode, shape, gap, thkAtk, thkRel, thkCurve, thkRelMode
const Seed kSeeds[] = {
    { "none",      mk(0.7f,  6, 0.7f, 0.0f,  0.3f, 0.2f, 0.05f, 2, 1, 3, 0, 0.0f,  0.0f, 2, 0) },
    { "ambient",   mk(0.3f, 30, 0.9f, 1.0f,  2.1f, 0.2f, 0.5f,  1, 0, 0, 0, 0.5f,  0.0f, 3, 0) },
    { "corners",   mk(1.1f, 20, 0.9f, 0.5f,  0.8f, 0.2f, 0.3f,  2, 0, 0, 0, 0.0f,  0.0f, 2, 0) },
    { "fireworks", mk(1.4f, 20, 0.9f, 0.65f, 0.9f, 0.2f, 0.3f,  2, 1, 0, 0, 0.4f,  0.0f, 2, 0) },
    { "ping-pong", mk(0.5f, 20, 0.1f, 0.45f, 0.8f, 0.0f, 0.0f,  2, 0, 1, 0, 0.0f,  0.0f, 2, 0) },
    { "pulse-out", mk(1.0f, 20, 0.9f, 0.5f,  0.8f, 0.0f, 0.1f,  2, 1, 0, 0, 0.5f,  0.0f, 2, 0) },
    { "rotate",    mk(1.5f, 20, 0.9f, 0.5f,  0.8f, 0.2f, 0.3f,  2, 0, 0, 0, 0.15f, 0.3f, 2, 0) },
    { "laser",     mk(2.5f,  8, 0.4f, 0.0f,  0.5f, 0.0f, 0.1f,  2, 0, 0, 1, 0.0f,  0.0f, 2, 0) },
    { "tracer",    mk(1.2f, 12, 0.5f, 0.2f,  0.8f, 0.1f, 0.2f,  2, 0, 0, 0, 0.0f,  0.0f, 2, 0) },
    { "comet",     mk(0.8f, 16, 1.5f, 0.3f,  0.8f, 0.1f, 0.2f,  2, 0, 0, 1, 0.0f,  0.0f, 2, 0) },
    { "spinner",   mk(1.3f, 14, 0.9f, 0.2f,  0.8f, 0.1f, 0.2f,  2, 0, 0, 1, 0.0f,  0.0f, 2, 0) },
    { "ripple",    mk(1.0f, 14, 0.6f, 0.3f,  0.8f, 0.15f,0.3f,  2, 0, 1, 1, 0.0f,  0.0f, 2, 0) },
    { "charge",    mk(1.0f, 16, 0.6f, 0.3f,  0.8f, 0.15f,0.3f,  2, 0, 1, 1, 0.0f,  0.0f, 2, 0) },
};

int asInt(const QString &s, int dflt)
{
    bool ok = false;
    const int v = s.toInt(&ok);
    return ok ? v : dflt;
}

// Preset name -> emission pattern. The name selects the *motion*; the packed
// Preset-<name> tunables only modulate it.
PoxKind kindForPreset(const QString &name)
{
    if (name == QLatin1String("corners"))   return POX_KIND_CORNERS;
    if (name == QLatin1String("pulse-out"))  return POX_KIND_PULSE_OUT;
    if (name == QLatin1String("rotate"))     return POX_KIND_ROTATE;
    if (name == QLatin1String("ping-pong"))  return POX_KIND_PING_PONG;
    if (name == QLatin1String("fireworks"))  return POX_KIND_FIREWORKS;
    if (name == QLatin1String("laser"))      return POX_KIND_LASER;
    if (name == QLatin1String("tracer"))     return POX_KIND_TRACER;
    if (name == QLatin1String("comet"))      return POX_KIND_COMET;
    if (name == QLatin1String("spinner"))    return POX_KIND_SPINNER;
    if (name == QLatin1String("ripple"))     return POX_KIND_RIPPLE;
    if (name == QLatin1String("charge"))     return POX_KIND_CHARGE;
    if (name == QLatin1String("scroll1") || name == QLatin1String("scroll2"))
        return POX_KIND_SCROLL;
    return POX_KIND_AMBIENT;   // "ambient" + any unknown
}

float asFloat(const QString &s, float dflt)
{
    bool ok = false;
    const float v = s.toFloat(&ok);
    return ok ? v : dflt;
}

// Parse a packed "Preset-<name>" string into t (15 ';'-separated fields).
void parsePreset(const QString &packed, PoxTunables &t)
{
    const QStringList f = packed.split(QLatin1Char(';'));
    if (f.size() < 15)
        return;
    t.speed            = asFloat(f.at(0),  t.speed);
    t.thickness        = asInt(f.at(1),    t.thickness);
    t.tail_length      = asFloat(f.at(2),  t.tail_length);
    t.pulse_depth      = asFloat(f.at(3),  t.pulse_depth);
    t.pulse_speed      = asFloat(f.at(4),  t.pulse_speed);
    t.env_attack       = asFloat(f.at(5),  t.env_attack);
    t.env_release      = asFloat(f.at(6),  t.env_release);
    t.env_curve        = asInt(f.at(7),    t.env_curve);
    t.release_mode     = PoxReleaseMode(asInt(f.at(8),  t.release_mode));
    t.shape            = PoxShape(asInt(f.at(9),         t.shape));
    t.gap              = asInt(f.at(10),   t.gap);
    t.thk_attack       = asFloat(f.at(11), t.thk_attack);
    t.thk_release      = asFloat(f.at(12), t.thk_release);
    t.thk_curve        = asInt(f.at(13),   t.thk_curve);
    t.thk_release_mode = PoxReleaseMode(asInt(f.at(14), t.thk_release_mode));
}

} // namespace

void PoxConfig::load()
{
    KConfigGroup g = KSharedConfig::openConfig(QStringLiteral("kwinrc"))
                         ->group(QStringLiteral("Effect-poxicle_kwin"));

    // Presets: seed from the built-in defaults, then apply any stored overrides.
    m_presets.clear();
    for (const Seed &s : kSeeds) {
        PoxTunables t = s.tun;
        const QString packed = g.readEntry(QStringLiteral("Preset-") + QString::fromLatin1(s.name),
                                            QString());
        if (!packed.isEmpty())
            parsePreset(packed, t);
        m_presets.insert(QString::fromLatin1(s.name), t);
    }

    // Per-app rules.
    m_rules.clear();
    const QStringList rules = g.readEntry("Rules", QStringList());
    for (const QString &line : rules) {
        const QStringList f = line.split(QLatin1Char('|'));
        if (f.size() < 2 || f.at(0).isEmpty())
            continue;
        Rule r = parseRule(f, 1);       // field 0 is the appId
        r.appId = f.value(0).trimmed();
        m_rules.append(r);
    }

    // The focus-following "active window" target: one rule, no leading appId.
    m_activeSet = false;
    const QString activeLine = g.readEntry("Active", QString());
    if (!activeLine.isEmpty()) {
        const QStringList f = activeLine.split(QLatin1Char('|'));
        if (!f.value(0).trimmed().isEmpty()) {
            m_active = parseRule(f, 0);
            m_activeSet = true;
        }
    }
}

PoxConfig::Rule PoxConfig::parseRule(const QStringList &f, int base)
{
    Rule r;
    r.preset  = f.value(base + 0).trimmed();
    r.reverse = asInt(f.value(base + 1), 0);   // 0 fwd, 1 rev, 2 loop
    const QString pc = f.value(base + 2).trimmed();
    if (!pc.isEmpty()) {
        const QColor c(pc);
        if (c.isValid()) {
            r.hasColor = true;
            r.color = { float(c.redF()), float(c.greenF()), float(c.blueF()), 1.0f };
        }
    }
    r.shape          = asInt(f.value(base + 3),  -1);
    r.gap            = asInt(f.value(base + 4),  -1);
    r.speed          = asInt(f.value(base + 5),   0);
    r.thickness      = asInt(f.value(base + 6),   0);
    r.tail           = asInt(f.value(base + 7),   0);
    r.attack         = asInt(f.value(base + 8),   0);
    r.release        = asInt(f.value(base + 9),   0);
    r.releaseMode    = asInt(f.value(base + 10), -1);
    r.thkAttack      = asInt(f.value(base + 11),  0);
    r.thkRelease     = asInt(f.value(base + 12),  0);
    r.thkReleaseMode = asInt(f.value(base + 13), -1);
    r.palette        = asInt(f.value(base + 14),  0);   // absent => 0 (Muted), -1 => use color
    return r;
}

PoxResolved PoxConfig::resolveRule(const Rule &match) const
{
    PoxResolved out;
    out.color = { -1.0f, -1.0f, -1.0f, -1.0f };

    // A rule whose preset is "none" (or unknown) explicitly disables drawing.
    const QString presetName = match.preset;
    if (presetName.isEmpty() || presetName == QLatin1String("none")
        || !m_presets.contains(presetName)) {
        out.enabled = false;
        return out;
    }
    out.tunables = m_presets.value(presetName);
    out.reverse = match.reverse;
    out.kind = kindForPreset(presetName);
    out.palette = match.palette;
    if (match.hasColor)
        out.color = match.color;

    // Apply non-sentinel overrides. Numeric units mirror Chiguiro: percent for
    // speed/tail/attack/release/thk*, raw pixels for thickness.
    PoxTunables &t = out.tunables;
    if (match.shape          >= 0) t.shape = PoxShape(match.shape);
    if (match.gap            >= 0) t.gap = match.gap;
    if (match.releaseMode    >= 0) t.release_mode = PoxReleaseMode(match.releaseMode);
    if (match.thkReleaseMode >= 0) t.thk_release_mode = PoxReleaseMode(match.thkReleaseMode);
    if (match.speed      != 0) t.speed = match.speed / 100.0f;
    if (match.thickness  != 0) t.thickness = match.thickness;
    if (match.tail       != 0) t.tail_length = match.tail / 100.0f;
    if (match.attack     != 0) t.env_attack = match.attack / 100.0f;
    if (match.release    != 0) t.env_release = match.release / 100.0f;
    if (match.thkAttack  != 0) t.thk_attack = match.thkAttack / 100.0f;
    if (match.thkRelease != 0) t.thk_release = match.thkRelease / 100.0f;

    return out;
}

PoxResolved PoxConfig::resolve(const QString &appId) const
{
    // First rule whose appId is a (case-insensitive) substring of windowClass.
    // Only windows with a matching rule draw — there is no global "all windows"
    // default. An unmatched window draws nothing.
    for (const Rule &r : m_rules) {
        if (!r.appId.isEmpty() && appId.contains(r.appId, Qt::CaseInsensitive))
            return resolveRule(r);
    }
    PoxResolved out;
    out.enabled = false;
    out.color = { -1.0f, -1.0f, -1.0f, -1.0f };
    return out;
}

PoxResolved PoxConfig::resolveActive() const
{
    if (!m_activeSet) {
        PoxResolved out;
        out.enabled = false;
        out.color = { -1.0f, -1.0f, -1.0f, -1.0f };
        return out;
    }
    return resolveRule(m_active);
}
