// config.js — read poxicle-config's DE-neutral config and resolve the look for a
// focused window. Same file/format the KWin effect reads:
// ~/.config/poxicle/poxicle.conf, [poxicle] group, GKeyFile ini.
//
//   Rules  = appId|preset|rev|color|shape|gap|speed|thk|tail|atk|rel|relMode|
//            tAtk|tRel|tRelMode|palette   (comma-separated lines)
//   Active = preset|rev|color|...|palette  (same, no leading appId)
//
// The configurator is the config UI on every desktop; this just consumes it.

import GLib from 'gi://GLib';

export const CONFIG_PATH =
    GLib.build_filenamev([GLib.get_user_config_dir(), 'poxicle', 'poxicle.conf']);
const GROUP = 'poxicle';

// Out-of-the-box look when no config file exists yet (before poxicle-config runs).
const DEFAULT_LOOK = {preset: 'ambient', reverse: 0, palette: 17};

function getStr(kf, key) {
    try {
        return kf.get_string(GROUP, key);
    } catch (_e) {
        return '';
    }
}

function intAt(f, i, dflt) {
    const v = f[i];
    if (v === undefined || v.trim() === '')
        return dflt;
    const n = parseInt(v, 10);
    return Number.isNaN(n) ? dflt : n;
}

// base=1 for a Rules line (appId at 0); base=0 for the Active line.
function ruleFrom(f, base) {
    const preset = (f[base] ?? '').trim() || 'ambient';
    return {
        preset,
        reverse: intAt(f, base + 1, 0),
        palette: intAt(f, base + 14, 0),
    };
}

// Resolve {preset, reverse, palette} for a focused window of WM class `wmClass`,
// or null when it should draw nothing ("none" / disabled). A matching per-app rule
// wins; otherwise the active-window target; otherwise the OOB default.
export function resolve(wmClass) {
    const kf = new GLib.KeyFile();
    try {
        kf.load_from_file(CONFIG_PATH, GLib.KeyFileFlags.NONE);
    } catch (_e) {
        return DEFAULT_LOOK;   // no config yet
    }

    let rule = null;
    const wc = (wmClass || '').trim().toLowerCase();
    const rulesStr = getStr(kf, 'Rules');
    if (wc && rulesStr) {
        for (const line of rulesStr.split(',')) {
            const f = line.split('|');
            if (f.length >= 2 && f[0] && f[0].trim().toLowerCase() === wc) {
                rule = ruleFrom(f, 1);
                break;
            }
        }
    }
    if (!rule) {
        const active = getStr(kf, 'Active');
        if (active)
            rule = ruleFrom(active.split('|'), 0);
    }
    if (!rule)
        return DEFAULT_LOOK;
    if (!rule.preset || rule.preset === 'none')
        return null;   // disabled for this window
    return rule;
}
