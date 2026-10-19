# How Edge Light works on macOS

These are notes from reverse engineering the Edge Light in macOS 27.0.1 (build 26A434, Apple Silicon). Apple calls the feature **"Ring Light"** internally. facelight reproduces the numbers below.

Tools: [rea](https://github.com/morluto/rea) with Ghidra, plus `strings`, `nm` and `otool`. Addresses are only valid for that build. No Apple code is reproduced here; this file only describes behavior and constants.

## Who does what

1. **CMCapture** (the camera pipeline) decides when the light turns on, using `BWRingLightController` and `PTEffectRingLightEstimation`. It measures the lux on your face (`faceLuxLevel`, `effectEnableAtLux`, `effectDisableAtLux`) and turns it into an "effect strength". That strength sets the ring's width and a screen brightness floor (`screenNitsFloor`, through CoreBrightness `CBRingLight`).
2. **ControlCenter** stores the state under `global/videoeffects/ringlight-*` and sends it over XPC with `setRingLightState(isEnabled:width:tint:display:bundleIDs:)`.
3. **RingLightHelper** draws the halo. It's a sandboxed Swift/AppKit XPC service inside ControlCenter.app (`com.apple.controlcenter.ringlighthelper`). Its entitlements include `com.apple.private.SkyLight.event.monitor`, which lets it run a global event tap.

## The window

- Level `CGWindowLevelForKey(Dock) + 1`
- `ignoresMouseEvents = true`: clicks pass through
- `collectionBehavior = canJoinAllSpaces | stationary`
- `sharingType` comes from the `IncludeRingLightWindowInSharedDisplays` default, which is false. That's why **the halo never appears in screen shares or screenshots**.
- When it appears, the window fades its alpha from 0 to 1.

## The hole that follows the cursor

1. `CGEventTapCreate(kCGSessionEventTap, headInsert, listenOnly, …)` listens for mouseMoved, leftMouseDragged and rightMouseDragged.
2. The callback converts `CGEventGetLocation` to window coordinates. The position flows into the view, then into a `Mask` layer.
3. `Mask` is a `CALayer` subclass. When the position changes, it disables implicit animations, moves itself to the cursor, and hides itself when there is no position. The result tracks the cursor 1:1, with no lag.
4. The hole's shape comes from 2 black, concentric `BlurredShapeLayer`s with `cornerCurve = .continuous` and a gaussian blur filter:

   | Layer | Size | Blur | Corner radius |
   |---|---|---|---|
   | Inner | 300×300 pt | 60 | 120 |
   | Outer | 480×480 pt | 120 | 180 |

5. The `Mask` is set as the ring container's `mask`, so the halo fades softly around the cursor. There is no trail: it's just the 2 fixed layers.

## Ring geometry (`RingLightLayer.layoutSublayers`)

`w` is the normalized width, from 0 to 1.

- ring thickness = `w*90 + 10` pt
- outer corner radius = thickness + 114
- inner ring radius = outer radius − thickness/2
- shadows: inner `w*60+30`, outer `w*75+30`, highlights `w*55+25` and `w*65+25`, solid rings `thickness*0.2`
- it uses private CALayer APIs: `invertsShadow`, `punchoutShadow`, `shadowPathIsBounds` and `setInvertsMask:`

facelight collapses the stacked shadows into a single signed-distance falloff: a solid band plus a gaussian inner glow of width `w*60+30`. It approximates the blurred rounded squares of the cursor hole with radial gradients.
