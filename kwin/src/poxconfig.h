/* poxconfig.h
 *
 * Per-app particle configuration, a faithful port of Chiguiro's particle preset
 * + "App Glass" per-process override model (kgx-particle.h / kgx-settings.c),
 * keyed on the window's app-id (EffectWindow::windowClass()) instead of a
 * terminal process name, and dropping the terminal-only window glass color.
 *
 * Stored in kwinrc under [Effect-poxicle_kwin]:
 *   DefaultPreset = ambient
 *   Preset-<name> = speed;thickness;tail;pulseDepth;pulseSpeed;envAtk;envRel;
 *                   envCurve;releaseMode;shape;gap;thkAtk;thkRel;thkCurve;thkRelMode
 *   Rules         = StringList, each:
 *                   appId|preset|reverse|pColor|shape|gap|speed|thickness|tail|
 *                   atk|rel|relMode|thkAtk|thkRel|thkRelMode|palette
 *   Active        = one rule for the focus-following "active window" target, in
 *                   the same field order as a Rules entry but with NO leading
 *                   appId (preset|reverse|pColor|...). Empty/"none" => disabled.
 *
 * `palette` is the ambient/fireworks burst-colour set: a built-in palette index
 * (see pox_palette_count), or -1 to use the per-app `pColor` as a solid colour.
 * Absent (older configs) => 0, the historical "Muted" look.
 *
 * Override sentinels match Chiguiro: -1 for shape/gap/release-modes, 0 for the
 * numeric (percent/pixel) fields. Numeric override units: speed/tail/atk/rel/
 * thkAtk/thkRel are PERCENT (->/100 of the tunable), thickness is PIXELS.
 */
#pragma once

#include <QHash>
#include <QList>
#include <QString>

#include <poxicle.h>

struct PoxResolved {
    bool        enabled = true;  // false => don't draw on this window (preset "none")
    PoxTunables tunables;
    PoxColor    color;           // a < 0 => no per-app color set (engine default)
    int         reverse = 0;     // 0 forward, 1 reverse, 2 loop (alternate)
    PoxKind     kind = POX_KIND_AMBIENT;  // emission pattern, derived from preset name
    int         palette = 0;     // ambient/fireworks colours: built-in id, or -1 = use `color`
};

class PoxConfig
{
public:
    // (Re)read kwinrc [Effect-poxicle_kwin]. Presets seed from the built-in
    // defaults and are overridden by any stored values.
    void load();

    // Resolve styling for a window app-id (windowClass): the matching rule's
    // preset with its non-sentinel overrides applied, else the default preset.
    PoxResolved resolve(const QString &appId) const;

    // Resolve the focus-following "active window" target. enabled == false when
    // no Active rule is set or its preset is "none".
    PoxResolved resolveActive() const;

private:
    struct Rule {
        QString  appId;
        QString  preset;
        int      reverse = 0;     // 0 forward, 1 reverse, 2 loop (alternate)
        bool     hasColor = false;
        PoxColor color {};
        int      shape = -1, gap = -1, releaseMode = -1, thkReleaseMode = -1;
        int      speed = 0, thickness = 0, tail = 0, attack = 0, release = 0,
                 thkAttack = 0, thkRelease = 0;
        int      palette = 0;     // ambient/fireworks palette id, or -1 = use `color`
    };

    // Parse one packed rule, reading the preset field at `base` (1 for a Rules
    // entry whose field 0 is the appId, 0 for the appId-less Active entry).
    static Rule parseRule(const QStringList &fields, int base);
    // Build the resolved styling for an already-matched rule (preset tunables +
    // non-sentinel overrides + colour + kind). enabled == false for "none".
    PoxResolved resolveRule(const Rule &rule) const;

    QHash<QString, PoxTunables> m_presets;          // name -> tunables
    QList<Rule>                 m_rules;
    Rule                        m_active;           // focus-following "active window" target
    bool                        m_activeSet = false;
};
