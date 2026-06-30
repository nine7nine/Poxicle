# Screenshots

Drop PNG screenshots here using the filenames below and they will appear in the
generated docs (add the matching `<figure class="screenshot">` block to the
page's `.md` source). Capture at a comfortable size; they are displayed at full
container width with a rounded border. Note that `grim` cannot screenshot on
KWin — use Spectacle or the GNOME screenshot tool.

| File | Page | Suggested shot |
| --- | --- | --- |
| `kwin-ring.png` | kwin-effect / architecture | A window with the poxicle edge ring drawn around it under the KWin effect |
| `gnome-ring.png` | gnome-extension | The same particle ring on the focused window under GNOME Shell |
| `panel-ring.png` | kwin-effect / gnome-extension | A desktop panel with the ring on its interior-facing edge only |
| `presets.png` | configuration / simulation-core | A montage of a few presets — ambient, comet, radar, fireworks |
| `config-presets.png` | configurator | The configurator's Presets page — the 19-row tunable grid |
| `config-apps.png` | configurator | The configurator's Apps page — per-app rules plus the Active and Panel targets |
| `demo.png` | renderer / wayland-backend | `poxicle-host` running: a plain window with the click-through overlay riding above it |

To add one to a page, insert a block like this into the relevant `.md` and
re-run `./md2html.sh <page>.md`:

```html
<figure class="screenshot">
  <img src="img/kwin-ring.png" alt="A poxicle edge ring around a KWin window">
  <figcaption>The poxicle edge ring drawn per window by the KWin Plasma 6 effect.</figcaption>
</figure>
```

Filenames are referenced from the `.md` sources; if you rename a file, update the
matching `<img src="img/...">` in that page and re-run `./md2html.sh <page>.md`.
