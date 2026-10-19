<p align="center">
  <img src="docs/hero.png" alt="facelight: a soft ring of light around the screen, with a hole that follows the cursor" width="800">
</p>

<h1 align="center">facelight</h1>

<p align="center"><b>Your screen is the ring light.</b><br>
The macOS Edge Light, rebuilt for Linux and Wayland, in one tiny C binary.</p>

---

Bad lighting on video calls? You already own a big, bright, perfectly positioned light panel: your monitor.

**facelight** wraps your screen in a soft, warm halo the moment your camera turns on, and fades it away when the call ends. It lights your face the same way Apple's Edge Light does in macOS, using geometry recovered by reverse engineering Apple's own implementation ([how it works on macOS](docs/macos-internals.md)).

## Why you'll like it

- **Automatic.** Open Zoom, Meet, Teams, OBS or anything else that uses `/dev/video*`, and the light comes on. Close it and the light fades out. You don't have to configure anything.
- **Stays out of your way.** Clicks go straight through the ring. When your cursor nears the edge, a soft hole follows it, so the light never hides what you're pointing at.
- **Featherweight.** It's one ~50 KB C binary with no toolkit, no Electron and no Python.

  | State | CPU | Memory |
  |---|---|---|
  | Waiting for the camera | 0% | 2.6 MB |
  | Light on | 0.4% | 28 MB |
  | Light on, cursor sweeping the edge at 60 fps | ~5% | 28 MB |

  <sub>Measured on a single 1920×1080 output.</sub>
- **Faithful to the original.** It uses the same ring thickness, corner radius, glow falloff and two-layer cursor hole as the macOS original.
- **Multi-monitor and HiDPI.** You get one ring per output, rendered at the output's scale.
- **Omarchy-native.** An optional bar indicator sits next to Reminder, Night light and Do not disturb. One click toggles the light.

## Install

You need a wlroots-style compositor with `wlr-layer-shell` (Hyprland, Sway, river, niri, …), plus `wayland`, `wayland-protocols` and a C compiler.

```bash
git clone https://github.com/luisvzr/facelight
cd facelight
make install                                   # ~/.local/bin/facelight + systemd user unit
systemctl --user enable --now facelight.service
```

That's it. Turn your camera on.

### Omarchy bar indicator

```bash
./omarchy/install.sh
omarchy restart shell
```

This adds a 󰛨 toggle to the indicators strip next to the clock. Hover to reveal it; it stays lit while the ring is on. Clicking it turns the ring on or off by hand, and starts the daemon if needed.

<sub>Omarchy only loads indicators from the indicators plugin's own folder, so the script clones `omarchy.indicators` into `~/.config/omarchy/plugins/` (Omarchy's supported way to customize built-in widgets). From then on, that widget no longer picks up upstream changes.</sub>

## Usage

```
facelight [--on] [--width 0..1] [--color fff4e5] [--brightness 0..1] [--no-cursor-hole]
facelight toggle            # on/off by hand; a manual "off" lasts until the camera closes
facelight auto              # drop the manual override
facelight status [--follow] # {"alt": "on" | "off" | "stopped"}, for bars and scripts
```

| Option | Default | What it does |
|---|---|---|
| `--width` | `0.5` | Ring width (thickness = `w*90 + 10` pt, like macOS) |
| `--color` | `fff4e5` | Tint; the default is a warm white |
| `--brightness` | `0.95` | Ring opacity |
| `--no-cursor-hole` | off | Keep the ring solid under the cursor |
| `--on` | off | Start lit, without waiting for the camera |

To change the defaults for the service, run `systemctl --user edit facelight.service` and override `ExecStart`.

`status --follow` prints one JSON line per change, so it plugs straight into Waybar, eww or any other bar:

```jsonc
"custom/facelight": {
  "exec": "facelight status --follow",
  "return-type": "json",
  "format": "󰛨",
  "on-click": "facelight toggle"
}
```

## How it works

- **Camera detection:** inotify watches every `/dev/video*` for open and close events. On an event, facelight waits 1 s (browsers briefly open every camera while enumerating devices), then checks `/proc/*/fd` to see whether anyone still holds the camera. It never opens the camera itself.
- **Rendering:** each output gets an overlay `wlr-layer-shell` surface with an empty input region, backed by a double-buffered `wl_shm` pool. The ring is computed once as a signed-distance field of a rounded rectangle. While the cursor moves, facelight rewrites and damages only the 720×720 box around the hole.
- **Cursor:** on Hyprland, it reads the cursor position from the IPC socket (`cursorpos`). On other compositors the ring still works, just without the hole.

## Limitations

- **The ring shows up in screen shares and screenshots.** macOS hides it with `sharingType = .none`, and Wayland has no equivalent.
- **The cursor hole is Hyprland-only for now.** PRs for other compositors' IPC are welcome.
- **It doesn't measure your face's brightness.** macOS only turns the light on in low light; facelight turns it on whenever the camera is in use.

## Credits

The geometry comes from reverse engineering macOS 27's `RingLightHelper` with [rea](https://github.com/morluto/rea) and Ghidra. See [docs/macos-internals.md](docs/macos-internals.md). No Apple code or binaries are included in this repository.

MIT licensed.
