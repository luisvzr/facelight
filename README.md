<p align="center">
  <img src="docs/hero.png" alt="facelight: a soft ring of light around the screen, with a hole that follows the cursor" width="800">
</p>

<h1 align="center">facelight</h1>

<p align="center"><b>The macOS Edge Light, made for Omarchy.</b><br>
Your screen lights your face on every call. It turns on with the camera, off when you hang up, and lives in your bar.</p>

<p align="center">
  <code>curl -fsSL https://raw.githubusercontent.com/luisvzr/facelight/main/install.sh | bash</code>
</p>

---

Mac users got Edge Light: the moment the camera turns on, the screen grows a soft, warm halo that lights your face like a ring light. On Omarchy you got… a dark face in a dark room.

**facelight brings it home.** It's built for Omarchy's Hyprland desktop, slots into the bar right next to Reminder and Night light, and installs the way Omarchy does: one line, done.

## What you get

**💡 It lights up by itself.** Join a Meet, Zoom or Teams call, or open OBS, and a warm ring fades in around your screen. Hang up and it fades out. You don't need a shortcut or a setting.

**🖱️ It never gets in your way.** Clicks go straight through the ring. Move the mouse toward the edge and a soft hole opens around the cursor and follows it, the same way it does on macOS.

**🧭 It lives in your bar.** Hover the indicators next to the clock and facelight sits there with the others. It glows while the ring is on, and one click turns it on or off by hand, for example to light a selfie or kill the light mid-call.

<p align="center">
  <img src="docs/omarchy-bar.png" alt="Omarchy bar indicators with the facelight bulb lit" width="700">
</p>

**🪶 It costs you nothing.** Omarchy is fast, and facelight keeps it that way. It's one ~50 KB C binary that talks to Wayland directly, with no GTK, no Electron and no Python.

| | CPU | Memory |
|---|---|---|
| Waiting for a call | **0%** | **2.6 MB** |
| Light on | 0.4% | 28 MB |
| Light on, cursor sweeping the edge | ~5% | 28 MB |

<sub>Measured on a 1920×1080 Omarchy desktop.</sub>

**🍎 Same look as the Mac.** The ring thickness, corner radius, glow falloff and two-layer cursor hole all come from reverse engineering Apple's own `RingLightHelper` ([see how it works on macOS](docs/macos-internals.md)).

## Install

```bash
curl -fsSL https://raw.githubusercontent.com/luisvzr/facelight/main/install.sh | bash
```

The installer:
- builds facelight, installing any missing build tools with `omarchy pkg add`
- turns on the background service
- adds the 💡 toggle to your bar

Run the same line again to update. To remove it:

```bash
bash ~/.local/share/facelight/install.sh --uninstall
```

<sub>About the bar: Omarchy only loads indicators from the indicators plugin's own folder, so the installer clones `omarchy.indicators` into `~/.config/omarchy/plugins/` (Omarchy's supported way to customize a built-in widget) and adds facelight to it. From then on, that widget no longer picks up upstream changes. To go back to the stock widget, change `<you>.indicators` back to `omarchy.indicators` in `~/.config/omarchy/shell.json`.</sub>

## Make it yours

```
facelight toggle            # on/off by hand (a manual "off" lasts until the camera closes)
facelight auto              # back to automatic
facelight status --follow   # {"alt": "on" | "off" | "stopped"}, one line per change
```

To change the look, run `systemctl --user edit facelight.service` and override `ExecStart`:

```ini
[Service]
ExecStart=
ExecStart=%h/.local/bin/facelight --width 0.8 --color ffe9cc
```

| Option | Default | What it does |
|---|---|---|
| `--width` | `0.5` | Ring width from 0 to 1 (thickness = `w*90 + 10` pt, like macOS) |
| `--color` | `fff4e5` | Tint. The default is a warm white; try `ffffff` for daylight or `ffd9a8` for candle |
| `--brightness` | `0.95` | Ring opacity |
| `--no-cursor-hole` | off | Keep the ring solid under the cursor |
| `--on` | off | Start lit, without waiting for the camera |

## Not on Omarchy?

facelight runs on any compositor with `wlr-layer-shell` (Hyprland, Sway, river, niri, …). Build it with `make install`, enable it with `systemctl --user enable --now facelight.service`, and wire `facelight status --follow` into your bar. Here's a Waybar example:

```jsonc
"custom/facelight": {
  "exec": "facelight status --follow",
  "return-type": "json",
  "format": "💡",
  "on-click": "facelight toggle"
}
```

The cursor hole uses Hyprland's IPC. On other compositors the ring works without it.

## Under the hood

- **Camera detection:** inotify watches `/dev/video*` for open and close events. After an event, facelight waits 1 s (browsers briefly open every camera while listing devices), then checks `/proc/*/fd` to see whether the camera is still in use. It never opens the camera itself.
- **Rendering:** each monitor gets an overlay `wlr-layer-shell` surface with an empty input region, backed by a double-buffered `wl_shm` pool. The ring is computed once as a signed-distance field. While the cursor moves, facelight rewrites only the 720×720 box around the hole.
- **Bar:** the daemon writes its state to `$XDG_RUNTIME_DIR/facelight/`, and `status --follow` streams it to the Omarchy indicator over inotify.

## Known limits

- **The ring shows up in screen shares and screenshots.** macOS hides it from capture, and Wayland has no equivalent.
- **It doesn't measure your lighting.** macOS only turns the light on in a dim room; facelight turns it on whenever the camera is in use.

## Credits

The geometry comes from reverse engineering macOS 27's `RingLightHelper` with [rea](https://github.com/morluto/rea) and Ghidra ([notes](docs/macos-internals.md)). No Apple code or binaries are included in this repository. Built for [Omarchy](https://omarchy.org).

MIT licensed.
