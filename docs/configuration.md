# poxicle — Configuration & Presets

poxicle's look is data: a catalogue of built-in presets and palettes baked into the engine, plus one DE-neutral config file that the configurator writes and every compositor backend reads — so a preset, a per-app rule, or a colour means the same thing under KWin and GNOME alike.

## Table of Contents

1. [Three consumers, one file](#1-three-consumers-one-file)
2. [The config file](#2-the-config-file)
3. [The preset catalogue](#3-the-preset-catalogue)
4. [Stored preset edits](#4-stored-preset-edits)
5. [Per-app rules, Active, and Panel](#5-per-app-rules-active-and-panel)
6. [Palettes](#6-palettes)
7. [Global keys: corners and grace](#7-global-keys-corners-and-grace)
8. [The configurator's own styling](#8-the-configurators-own-styling)
9. [Migration and live reload](#9-migration-and-live-reload)
10. [Design rules](#10-design-rules)

---

## 1. Three consumers, one file

Everything visual in poxicle reduces to plain values. The **preset** and **palette** tables are the single source of truth, compiled into the engine ([`src/poxicle.c`](simulation-core.gen.html)) and exposed through `pox_preset_*` / `pox_palette_*`, so the engine, the [configurator](configurator.gen.html), and every compositor backend share them with no private copies. A user's *choices* live in one DE-neutral file that three programs touch:

<div class="diagram-container">
<svg width="100%" viewBox="0 0 900 300" xmlns="http://www.w3.org/2000/svg">
  <style>
    .bg    { fill: #1a1b26; }
    .cfg   { fill: #2a2438; stroke: #e0af68; stroke-width: 1.5; }
    .write { fill: #1a2a1a; stroke: #9ece6a; stroke-width: 1.5; }
    .read  { fill: #16242b; stroke: #7dcfff; stroke-width: 1.5; }
    .lbl   { fill: #c0caf5; font-size: 11px; font-family: 'JetBrains Mono', monospace; }
    .lbl-sm{ fill: #c0caf5; font-size: 10px; font-family: 'JetBrains Mono', monospace; }
    .lbl-mut{ fill: #8c92b3; font-size: 9px;  font-family: 'JetBrains Mono', monospace; }
    .lbl-grn{ fill: #9ece6a; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-cy{ fill: #7dcfff; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .lbl-yel{ fill: #e0af68; font-size: 11px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
    .ln    { stroke: #7dcfff; stroke-width: 1.5; fill: none; }
    .lnw   { stroke: #9ece6a; stroke-width: 1.5; fill: none; }
    .title { fill: #7aa2f7; font-size: 14px; font-weight: bold; font-family: 'JetBrains Mono', monospace; }
  </style>
  <rect x="0" y="0" width="900" height="300" class="bg"/>
  <text x="450" y="26" text-anchor="middle" class="title">~/.config/poxicle/poxicle.conf</text>

  <rect x="350" y="120" width="200" height="64" class="cfg"/>
  <text x="450" y="146" text-anchor="middle" class="lbl-yel">poxicle.conf</text>
  <text x="450" y="164" text-anchor="middle" class="lbl-mut">GKeyFile / INI</text>
  <text x="450" y="177" text-anchor="middle" class="lbl-mut">[poxicle] + [Configurator]</text>

  <rect x="350" y="56" width="200" height="40" class="write"/>
  <text x="450" y="80" text-anchor="middle" class="lbl-grn">poxicle-config (writer)</text>
  <line x1="450" y1="96" x2="450" y2="120" class="lnw"/>

  <rect x="40"  y="210" width="240" height="56" class="read"/>
  <text x="160" y="234" text-anchor="middle" class="lbl-cy">KWin effect</text>
  <text x="160" y="251" text-anchor="middle" class="lbl-mut">poxconfig.cpp (KConfig)</text>

  <rect x="620" y="210" width="240" height="56" class="read"/>
  <text x="740" y="234" text-anchor="middle" class="lbl-cy">GNOME extension</text>
  <text x="740" y="251" text-anchor="middle" class="lbl-mut">apply_config() in C (GKeyFile)</text>

  <line x1="380" y1="184" x2="180" y2="210" class="ln"/>
  <line x1="520" y1="184" x2="720" y2="210" class="ln"/>
  <text x="450" y="240" text-anchor="middle" class="lbl-mut">readers parse the [poxicle] group; only the configurator reads [Configurator]</text>
</svg>
</div>

The file is **DE-neutral on purpose** — plain key/value, no KDE or GNOME tooling — so the configurator runs under any desktop and both renderers honour the same edits. The KWin effect parses it with `KConfig(SimpleConfig)`; the GNOME extension's [binding](gobject-binding.gen.html) parses it with `GKeyFile`. Both resolve a window's look the same way, field for field.

## 2. The config file

| Property | Value |
| --- | --- |
| Path | `$XDG_CONFIG_HOME/poxicle/poxicle.conf` (i.e. `~/.config/poxicle/poxicle.conf`) |
| Format | GKeyFile / INI; all values stored as strings |
| Groups | `[poxicle]` — read by both renderers; `[Configurator]` — the configurator's own window styling, ignored by the renderers |
| Floats | serialized C-locale (`.` decimal), so parsing is locale-independent |

The `[poxicle]` group holds the entire look. Its keys:

| Key | Shape | Default |
| --- | --- | --- |
| `Preset-<name>` | 15 `;`-separated tunable fields (one key per edited preset) | the engine seed if absent |
| `Rules` | comma-separated rule lines, each 16 `\|`-delimited fields | empty |
| `Active` | one `\|`-delimited rule, no appId (15 fields) — the focus-following target | absent → disabled |
| `Panel` | one `\|`-delimited rule, no appId — the desktop panel target | absent → disabled |
| `DefaultPreset` | preset name; the configurator's seed for a new app row | `ambient` |
| `UnminimizeGrace` | int ms, clamped 0–2000 | `350` |
| `CornerTop` / `CornerBottom` | int px | `0` (square) |

## 3. The preset catalogue

A **preset** is a name bound to seed tunables and an emission [kind](simulation-core.gen.html). The engine ships 19 selectable presets (plus `none`, an app *state* meaning "draw nothing," not a selectable look). The configurator lists them and the binding's `set_preset` applies them by name.

| Preset | Kind | Preset | Kind |
| --- | --- | --- | --- |
| `ambient` | auto-burst fan | `ripple` | pulse races both ways, collides |
| `corners` | arms from the corners | `charge` | implode to a point, fire out |
| `fireworks` | dense imploding fan | `spread` | shotgun volley fanning out |
| `ping-pong` | a bolt bouncing an edge | `radar` | bright wedge over a dim ring |
| `pulse-out` | symmetric outward pulse | `counter-spin` | two trails crossing |
| `rotate` | a lap-running arc | `snake` | a solid body crawling |
| `laser` | rapid-fire bolts | `breathe` | the whole outline glows |
| `tracer` | evenly-spaced bolts circling | `strobe` | the whole outline blinks |
| `comet` | one long-tailed traveler | `fireflies` | sparse blocks glimmering |
| `spinner` | a growing/shrinking arc | | |

`pox_kind_for_preset()` maps a name to its `PoxKind`; an unknown name falls back to `ambient` (the historical default), and a config can also name `scroll1` / `scroll2` to reach the looping `SCROLL` beam, which has no preset row of its own.

## 4. Stored preset edits

When a user tweaks a preset in the configurator, the edited tunables are stored under a `Preset-<name>` key as **15 `;`-separated fields**, in `PoxTunables` order:

```text
speed ; thickness ; tail_length ; pulse_depth ; pulse_speed ;
env_attack ; env_release ; env_curve ; release_mode ; shape ;
gap ; thk_attack ; thk_release ; thk_curve ; thk_release_mode
```

A reader seeds from the engine's built-in preset, then overlays the stored fields (an override needs all 15 fields to apply). This is why a preset edit looks identical under both renderers: the seed is shared engine data, and the delta is one shared string. See [Tunables and envelopes](simulation-core.gen.html#4-tunables-and-envelopes) for what each field controls.

## 5. Per-app rules, Active, and Panel

A **rule** binds a window class to a look. The `Rules` key is a comma-separated list; each entry is 16 `|`-delimited fields:

```text
appId | preset | rev | color | shape | gap | speed | thk | tail |
atk | rel | relMode | tAtk | tRel | tRelMode | palette
```

A renderer matches the **first** rule whose `appId` is a case-insensitive substring of the window's class. There is no global "all windows" default — an unmatched window draws nothing. After `appId` and `preset`, the remaining columns are **per-app overrides** layered on the resolved preset tunables:

- `rev` — `0` forward, `1` reverse, `2` loop (alternate direction each cycle).
- `color` — a `#rrggbb` tint, or empty.
- **Enum overrides** (`shape`, `gap`, `relMode`, `tRelMode`) — `-1` means *inherit from the preset*.
- **Numeric overrides** (`speed`, `thk`, `tail`, `atk`, `rel`, `tAtk`, `tRel`) — `0` means *inherit*. Non-zero values are **percent units** divided by 100, except `thk` (thickness), which is raw pixels.
- `palette` — a built-in palette id, or `-1` to use the rule's solid `color`. An absent field defaults to `0` (Muted) for backward compatibility.

Two special targets reuse the same packing without an appId:

- **`Active`** — the focus-following overlay. When no per-app rule matches the focused window, this look is drawn on whatever window currently has focus.
- **`Panel`** — the desktop panel ring (the KDE dock / GNOME top bar). It is packed exactly like `Active` but never reads the corner keys (panels keep sharp corners); the host derives which edges to light from panel-vs-screen geometry.

## 6. Palettes

A **palette** is a small ordered set of colours (up to `POX_PALETTE_MAX = 8`) that an emission kind samples — ambient/fireworks per burst, geometric kinds per segment or per loop cycle. The engine ships **25 built-ins**, ordered loosely by family so a dropdown reads in coherent groups, with ids append-only so a stored index keeps pointing at the same colours across releases:

| | | | | |
| --- | --- | --- | --- | --- |
| 0 Muted | 1 Sunset | 2 Ember | 3 Autumn | 4 Ocean |
| 5 Glacier | 6 Twilight | 7 Forest | 8 Moss | 9 Neon |
| 10 Pop | 11 Candy | 12 Rainbow | 13 Bloom | 14 Sorbet |
| 15 Mono Blue | 16 Mono Pink | 17 Verdant | 18 Meadow | 19 Orchid |
| 20 Spectra | 21 Marigold | 22 Aurora | 23 Nebula | 24 Sunfall |

Index `0` ("Muted") is the historical default, so an engine on the default palette looks unchanged. **Precedence** mirrors `applyPalette` across both renderers: when a rule sets a palette index, that palette is used; when the palette field is `-1`, a per-app `color` (if set) becomes a single-colour "Solid" palette, otherwise it falls back to Muted. With no config file at all, the out-of-box look is `ambient` on palette `17` (Verdant).

## 7. Global keys: corners and grace

Three keys apply across the resolved look:

- **`CornerTop` / `CornerBottom`** (px) round the window-ring corners. `CornerTop` rounds the two top corners (KDE's convention), `CornerBottom` rounds the bottom two (so GNOME, which rounds all four, sets both). The engine caps each at half the shorter side. The panel target ignores these.
- **`UnminimizeGrace`** (ms) is how long a ring is *held* through an un-minimize so it doesn't snap to the settled window rectangle while a Magic Lamp / Burn-My-Windows warp is still playing. Both renderers scale it by the user's animation-speed setting (and collapse it to zero when animations are off). The KWin effect additionally reads `kwinrc` for the configured Magic Lamp duration, since that warp is non-affine and the hold must cover its length.

## 8. The configurator's own styling

The `[Configurator]` group holds the configurator's window appearance and is read by nothing else:

| Key | Type | Default |
| --- | --- | --- |
| `Opacity` | int 0–100 | `90` |
| `GlassColor` | `#rrggbb` | `#14141a` |
| `AccentColor` | `#rrggbb` | `#3584e4` |

These reproduce the configurator's translucent "App-Glass" look. Firewalling them into a separate group keeps the renderers from ever seeing UI styling in the `[poxicle]` they parse.

## 9. Migration and live reload

On first run, if `poxicle.conf` is absent, the configurator performs a **one-time migration**: it copies every key from the legacy KDE location `~/.config/kwinrc [Effect-poxicle_kwin]` into the new neutral file, preserving the exact packed formats so existing setups carry over unchanged.

After a save, the two renderers pick up changes differently. The configurator pings KWin over D-Bus (`reconfigureEffect("poxicle_kwin")`) so the [effect](kwin-effect.gen.html) re-reads the file live; the [GNOME extension](gnome-extension.gen.html) instead watches the file with a `Gio.FileMonitor` and re-applies on every change. Either way the running ring updates without a restart.

## 10. Design rules

- **The look is data.** Presets, palettes and tunables are plain values in the engine; there is no per-look code outside it, so every consumer renders identically.
- **One neutral file, no DE tooling.** A GKeyFile under `~/.config/poxicle/` is written by the configurator and read by both renderers, so configuration is desktop-agnostic.
- **Inherit by sentinel.** Override columns use `-1` (enums) and `0` (numbers) to mean "take the preset's value," so a rule overrides only what it names.
- **Append-only catalogues.** Palette ids never shift, so a stored index keeps its colours across releases.
- **UI styling stays out of the look.** The configurator's glass settings live in their own group the renderers never read.
