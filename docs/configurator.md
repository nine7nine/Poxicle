# poxicle — The GTK4 Configurator

The configurator (`configurator/`) is a standalone GTK4 / libadwaita app that edits poxicle's look — preset tunables, per-app rules, and the focus/panel targets — and writes the one DE-neutral config file both the KWin effect and the GNOME extension read.

## Table of Contents

1. [What the configurator is](#1-what-the-configurator-is)
2. [The window and its three pages](#2-the-window-and-its-three-pages)
3. [The Presets page](#3-the-presets-page)
4. [The Apps page](#4-the-apps-page)
5. [The Preferences page](#5-the-preferences-page)
6. [Custom cells](#6-custom-cells)
7. [How it links the engine](#7-how-it-links-the-engine)
8. [Design rules](#8-design-rules)

---

## 1. What the configurator is

`poxicle-config` is an `AdwApplication` with the app id `org.ninez.PoxicleConfig`. It is the **supported editor** for poxicle's configuration: it writes `~/.config/poxicle/poxicle.conf`, the neutral file the [renderers consume](configuration.gen.html). Because it stores plain key/value through `GKeyFile` rather than any KDE tooling, it runs under any desktop — the same binary configures the [KWin effect](kwin-effect.gen.html) and the [GNOME extension](gnome-extension.gen.html).

The app forces the dark colour scheme, paints itself in a translucent "App-Glass" style, and opens at 1200 × 900 — a size chosen so its densest page fits without a scrollbar. On launch it loads and applies the saved appearance, and saves surface as toasts.

## 2. The window and its three pages

The window is an `AdwApplicationWindow` titled "Poxicle Particles". Its body is an `AdwToastOverlay` over an `AdwViewStack` of three pages, switched by an `AdwViewSwitcher` in the body (the header bar carries a centred title):

| Page | Edits | Writes to |
| --- | --- | --- |
| **Presets** | the 19 built-in preset tunables | `Preset-<name>` keys |
| **Apps** | per-app rules + the Active and Panel targets | `Rules`, `Active`, `Panel`, `DefaultPreset` |
| **Preferences** | window appearance + a few effect-behaviour settings | `[Configurator]` group + `CornerTop`/`CornerBottom`/`UnminimizeGrace` |

All the values these pages read and write are described in [Configuration & Presets](configuration.gen.html); this page covers the UI that produces them.

## 3. The Presets page

The Presets page is the preset editor the KCM lacks: a grid of all **19 presets** (`ambient` through `fireflies`), one row each, with 15 columns exposing every `PoxTunables` field. Numeric fields are slim spin buttons (speed 0.1–8.0, thickness 2–100, tail 0.1–10.0, the pulse and envelope fractions, the thickness envelope); enum fields are flat "cycle" buttons that step through glyphs — the curve (`( / )`), release mode (`U R S …`), shape (`■ ● ◆ ▶`), and gap. Each row is tinted by a `.preset-<name>` CSS class so presets read at a glance.

A single **Apply** button saves every preset's 15 fields and then pings KWin to reconfigure. The seed defaults the page starts from are a verbatim copy of the engine's preset table, so an untouched preset writes back exactly the engine's built-in look.

## 4. The Apps page

The Apps page edits the per-app `Rules` list plus the two special targets. It is a `GtkListBox` of rows, each with 16 columns kept aligned by size groups: **App, Preset, Rev, Color, Shape, Gap, Spd, Thk, Tail, Atk, Rel, Rls, TAtk, TRel, TRls, Palette**.

- An ordinary row has a `GtkEntry` for the app id (matched as a case-insensitive substring of the window class), a preset dropdown, a reverse cycle (`▶ ◀ ◆` for forward/reverse/loop), a colour button opening a `GtkColorDialog`, and **dash-spins** for the numeric overrides that render `0` as "—" to signal *inherit*. The enum overrides start at `-1` (inherit) with a leading "—" glyph. The palette dropdown previews each built-in as a strip of coloured swatches, with row 0 = "Solid" (value `-1`, use the colour) and the rest mapping to palette ids.
- Two **un-removable** rows sit at the top: **★ Active window** (saved to `Active`, drawn on the focused window) and **▭ Panel** (saved to `Panel`, the desktop panel edges). They reuse the same column packing minus the app id.
- A top bar provides a "New app preset" default (saved as `DefaultPreset`), **Add app**, **Pick window**, and **Remove selected**. **Pick window** calls KWin's `queryWindowInfo` over D-Bus to grab a clicked window's class — a Wayland-safe way to fill the app id.

**Apply** collects the rows (dropping empty app ids), saves `Rules`, `Active`, `Panel` and the default preset, then reconfigures. The per-app override ranges are deliberately wide — speed to 800%, tail to 1000% — with `0` always meaning inherit.

## 5. The Preferences page

The Preferences page has no Apply button; every control applies live and self-persists. It is split in two:

- **Appearance** → the `[Configurator]` group: window opacity (0–100), glass colour, and accent colour. These drive the app's own glass styling and nothing the renderers read.
- **Effect settings** → the `[poxicle]` group: top corner radius, bottom corner radius (with tooltips noting KDE rounds top corners and GNOME rounds all four), and the un-minimize grace (0–2000 ms). Each save also pings KWin to reconfigure; the GNOME extension picks the change up through its file monitor.

## 6. Custom cells

`pox-cells.c` supplies the small reusable widgets and the styling that give the app its look. `pox_load_css` installs the static App-Glass stylesheet — a translucent window, transparent headerbar/toolbar/containers, redefined accent colours, slim borderless spin buttons, flat cycle buttons, opaque popovers, and the per-preset and per-column colour classes. `pox_apply_appearance` adds a second, higher-priority provider that overrides only the accent colours and the window glass from the saved `[Configurator]` values, so every rule referencing those colours re-resolves for free. The custom cell factories — `pox_spin_new` (with optional dash-as-zero), `pox_cycle_new` (glyph cycling), `pox_preset_new` (a dropdown over preset names), and `pox_palette_new` (a dropdown whose rows render swatch strips from the engine's palette colours) — are what the three pages are built from.

## 7. How it links the engine

The configurator's `meson.build` depends on `gtk4` and `libadwaita-1`, and compiles **`../src/poxicle.c` directly into the binary** — but only for the **palette catalogue**. It calls `pox_palette_count` / `pox_palette_name` / `pox_palette_colors` so the palette dropdown shows the exact names and colours the engine renders, with zero duplication. The GL backend is not linked; the configurator never draws particles itself.

Notably, the **preset seed table** is *not* pulled from the engine at runtime — it is a hand-maintained copy in the configurator's I/O layer, kept in step with the engine's defaults. The build is opt-in (`meson setup build -Dconfigurator=true`) and installs the binary, a `.desktop` entry, and an SVG icon (a clean particle square); the desktop file sets `StartupWMClass=org.ninez.PoxicleConfig` so the compositor maps the window to its icon. See [Build, Install & Packaging](build.gen.html).

## 8. Design rules

- **Write the neutral file.** The configurator stores plain `GKeyFile` under `~/.config/poxicle/`, so one app configures both renderers regardless of desktop.
- **Inherit by sentinel in the UI.** Dash-spins render `0` as "—" and enum cycles start at `-1`, surfacing the config's "inherit from preset" semantics directly in the widgets.
- **Pull catalogues from the engine.** Palette names and colours come from the linked engine, so the dropdown always matches what gets drawn.
- **Firewall UI styling.** The app's own glass settings live in a `[Configurator]` group the renderers never read.
- **Apply live.** Saves ping KWin to reconfigure, and the GNOME extension watches the file — edits land without a restart.
