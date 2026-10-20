# hypr-shaders

A Hyprland plugin (0.56.x) that runs GPU shaders on tagged windows inside the compositor,
in real time. ASCII art is the first shader. More can be added as one C++ class or as a
`.frag` file.

Two modes, set with window tags:

| tag            | effect                                                                       |
|----------------|------------------------------------------------------------------------------|
| `shaderbg`     | **backdrop**: everything *behind* the window (wallpaper and the windows under it) goes through the shader inside the window's box. The window's own translucent surface draws on top. This works like Hyprland's blur. |
| `shader`       | **window**: the window itself (with anything showing through it) goes through the shader. |
| `shader:<name>`| picks the shader. With a mode tag but no name tag, `ascii` is used.            |

Tags set by window rules show with a trailing `*` (`shaderbg*`). Both forms work.

## Build

```sh
make            # -> hypr-shaders.so (builds against `pkg-config hyprland`, i.e. /usr/include/hyprland)
```

You need g++ (C++23), the Hyprland headers matching the Hyprland that is running, cairo
and nlohmann-json. The plugin refuses to load if the header hash doesn't match the
running Hyprland, so rebuild it after every Hyprland update.

The Makefile links to a temporary file and renames it into place. Overwriting a `.so`
that Hyprland has mapped can crash the compositor; a rename gives the new build a new
inode instead. To pick up a rebuild, unload the plugin and load it again.

## Load

```sh
hyprctl plugin load /path/to/hypr-shaders/hypr-shaders.so
hyprctl plugin unload /path/to/hypr-shaders/hypr-shaders.so
```

To load it at startup, add this to the Lua config (`hyprland.lua`):

```lua
hl.plugin.load("/path/to/hypr-shaders/hypr-shaders.so")
```

Window rules that go with it:

```lua
-- If a window rule sets the mode tag, put that rule BEFORE this one. Otherwise the
-- no_blur rule may not see the tag, and the ASCII gets blurred.
hl.window_rule({ name = "shader-backdrop", match = { tag = "shaderbg" }, no_blur = true })
```

For `shader` (window mode), the window must stay visible. Do **not** give it
`opacity = "0 override"`; that was only for the old Quickshell renderer. The plugin reads
the window's pixels and replaces them with the shader output.

Tag a window by hand:

```sh
hyprctl dispatch 'hl.dsp.window.tag({ tag = "+shaderbg", window = "address:0x..." })'
hyprctl dispatch 'hl.dsp.window.tag({ tag = "+shader:ascii", window = "address:0x..." })'
hyprctl dispatch 'hl.dsp.window.tag({ tag = "-shaderbg", window = "address:0x..." })'
```

## Settings, colours, atlas

- **Settings**: `~/.local/state/shaders/config.json` (honours `$XDG_STATE_HOME`). It has
  one section per shader. Each shader reads `config.json["<name>"]`. The file is polled
  by mtime every 250 ms, so changes made by the bar apply right away. Other keys
  (`auto`, ...) are ignored. If the file is missing, the plugin falls back to the old
  `~/.local/state/ascii/config.json` `live` section for `ascii`.

  ```json
  { "ascii": { "fontSize": 10, "charset": "wd2", "fps": 12, "jitter": 0.06, "glitch": 0.12,
               "shade": 0.5, "contrast": 1.3, "gamma": 1, "tint": 0, "backdropTint": 0 } }
  ```
  Optional `ascii` keys: `invert` (0/1), `fg` (window-mode glyph role, default
  `on_surface`), `backdropFg` (default `primary`), `bg` (default `surface`).
- **Colours**: `~/.local/state/theme/palette.json` `.colors` (Material roles, hex strings),
  also polled by mtime. Backdrop: glyphs `primary` on `surface`. Window: glyphs
  `on_surface` on `surface`. `tint` (window) and `backdropTint` (backdrop) blend the glyph
  colour toward the source pixel's normalised colour.
- **Glyph atlas**: `~/.cache/ascii/atlas-<fontSize>.png` plus its sidecar
  `atlas-<fontSize>.json` (`cw`, `ch`, `slots`). This is the strip written by
  `asciivid --export-atlas`: 256 cells, where cell *i* is the glyph for luminance *i* and
  alpha is coverage. If the atlas is missing, or `charset` changes, the plugin runs
  `asciivid --export-atlas <png> --font-size N --charset C` in the background. It does
  not block, and the new atlas is picked up when the process exits. Changes to the atlas
  file's mtime are also picked up.

## How it hooks rendering

Every window gets two `IHyprWindowDecoration`s:

- **backdrop** sits in `DECORATION_LAYER_BOTTOM`. Hyprland draws it before the window's
  surface, after everything below the window.
- **window** sits in `DECORATION_LAYER_OVER`, right after the surface and before popups.

When its tag is set, a decoration adds an `EK_CUSTOM` pass element (`CShaderPassElement`)
to the render pass. When the pass runs, the element's `draw()` reads the framebuffer that
is being rendered into (Hyprland's offscreen work buffer). For the backdrop, that buffer
holds exactly what is behind the window at this point in the frame, the same thing blur
samples. The effect then draws into the window box:

1. **ascii**: a downsample pass turns the region under the window's character grid into
   2×2 texels per cell, using 4 bilinear taps each, into a small scratch FBO.
2. A glyph pass draws over the window box. It is a port of
   `quickshell/shaders/glsl/ascii.frag`: luminance → contrast/gamma → jitter → glitches
   (row tears, block dropouts and scrambles, bursts) → glyph slot → atlas.

Backdrop cells line up with the monitor pixel grid, so glyphs don't swim when the window
moves, and their noise is seeded by global cell index. Window-mode cells line up with the
window origin, and their sampling stays inside the window.

The shared final pass (`finalPass` in `Effects.cpp`) does the following for every shader:

- clips to the window's rounded corners, using the same function as Hyprland's
  `rounding.glsl`, with radius `rounding*scale-1` and the window's `rounding_power`
- applies the window alpha. For backdrops, the window's own active/inactive opacity is
  divided out, so only fades make the backdrop translucent.
- scissors to the frame damage

The raw GL state that Hyprland caches is saved and restored around each draw: program,
VAO, buffers, textures, FBO bindings, viewport, scissor and blend.

**Damage.** On `render.pre`, before Hyprland turns the monitor's damage ring into frame
damage, any damage that touches a shaded window's region is grown to the whole region,
snapped to the cell grid. Every cell, and every source a glitch tear reads from, is then
rendered completely. This happens on the damage ring itself, so buffer-age bookkeeping
stays correct. Some cases this covers:

- Video wallpaper under the window: its damage gets expanded.
- A window behind it updating: same.
- The window moving: Hyprland damages the old and new boxes.

The pass element also returns `disableSimplification()` (as `CPreBlurElement` does), so
content under the window is fully drawn even where an opaque window above would normally
let Hyprland skip it.

**Animation.** A `wl_event_loop` timer wakes at each shader's `fps` tick boundary (default
12 Hz). On each tick, it damages the shaded regions of visible tagged windows whose shader
animates. `ascii` only animates when `jitter > 0` or `glitch > 0`. When nothing animates,
the timer runs at 4 Hz just to poll the config, palette and atlas files and to catch tag
changes.

**Tag changes** apply immediately in two ways:

- `hl.dsp.window.tag` calls the decorations' `updateWindow()`, which damages the window.
- The decoration's `draw()` re-reads the tags on every frame.

## Adding a shader

**C++:** add `src/effects/<Name>.cpp` implementing `hs::IEffect` (see `Effect.hpp`):

- `configure(json section)`
- `fps()`
- optional `requiredBox()`
- `render(SFrame)`

Then add a line to `findEffect()` in `Effects.cpp`. Build the final fragment with
`finalFragment(body)`, where the body defines `vec4 effect(vec2 fragCoord)` and returns
premultiplied colour. Call `finalPass(frame, program)`, which takes care of rounding,
alpha and damage. `frame.srcTex` is the framebuffer being drawn into, so only sample it
in your own scratch passes. Single-pass shaders call `copyRegion()` first.

**No compile:** put `~/.config/hypr/shaders/<name>.frag` (and optionally `<name>.json`) in
place, then tag a window `shader:<name>`. See `examples/shaders/crt.frag` and
`examples/shaders/crt.json`.

The `.frag` is a GLSL ES 3.00 body that defines `vec4 effect(vec2 fragCoord)`. It can use:

- `source(uv)`: the pixels under the window, with `uv` from 0..1 and y pointing down
- `vUV`
- `uSize`
- `uTime`, `uTick`, `uAlpha`
- `uFg`, `uBg`: palette colours, set by `fg`/`bg` roles in the `.json`
- its own uniforms, with defaults in `.json` under `"uniforms"`. Numbers, arrays of 2–4
  numbers, and palette roles or hex colours (which become a `vec4`) are supported.
  `config.json["<name>"]` can override any of them, and `fps`, by key.

Both files hot-reload. If a shader fails to compile, the error goes to the Hyprland log
and the window renders normally.

## Limitations

- Fullscreen windows get no effect: Hyprland doesn't draw decorations for them.
- Rotated or flipped monitors work, but backdrop glyphs follow the framebuffer
  orientation, so on a rotated output they are rotated too.
- Colours assume an sRGB/SDR work buffer. HDR outputs would need colour conversion.
- Windows that use window transformers (third-party plugins) render the backdrop from
  their offscreen pass rather than the main framebuffer.
- OpenGL renderer only. The plugin refuses to load on the Vulkan renderer.

## License

BSD-3-Clause, see [LICENSE](LICENSE).
