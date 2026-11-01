// "ascii": the asciivid look on the GPU, a port of quickshell/shaders/glsl/ascii.frag.
//
// Pass 1 (downsample): the framebuffer region under the window's character grid is reduced
//   to 2x2 texels per character cell (4 bilinear taps each, so ~64 source pixels/cell).
// Pass 2 (final): per cell, the 2x2 texels are averaged by one bilinear fetch at the cell
//   centre -> luminance -> sharpen -> auto-levels -> contrast/gamma -> jitter -> glitches ->
//   glyph slot -> atlas, composited in linear light.
//
// Tone and glitches follow asciivid (tone.rs, grid.rs, glitch.rs): the downsampled grid is read
// back asynchronously (PBO + fence, a frame late) to build each window's auto-levels curve (a
// 1%-99% stretch blended with histogram equalization, eased between frames) and to decide
// whether light content gets inverted; glitch events are rolled per animation step on the CPU.
//
// Backdrop cells are aligned to the monitor's pixel grid so glyphs don't swim when the window
// moves; window-mode cells are aligned to the window origin.

#include "../Effect.hpp"
#include "../Log.hpp"

#include <array>
#include <cairo/cairo.h>
#include <fstream>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>

extern char** environ;

using namespace hs;
using json = nlohmann::json;

namespace {
    const char* DOWNSAMPLE_FRAG = R"GLSL(#version 300 es
precision highp float;
uniform sampler2D uSrc;
uniform vec2 uSrcSize;  // framebuffer px
uniform vec2 uOrigin;   // framebuffer px of the grid's top-left
uniform vec2 uSub;      // framebuffer px per output texel (half a cell)
uniform vec4 uClamp;    // framebuffer px: sample only inside (x0, y0, x1, y1)
out vec4 fragColor;
vec4 tap(vec2 p) {
    return texture(uSrc, clamp(p, uClamp.xy + 0.5, uClamp.zw - 0.5) / uSrcSize);
}
void main() {
    vec2 base = uOrigin + floor(gl_FragCoord.xy) * uSub;
    vec4 acc = tap(base + uSub * vec2(0.25, 0.25)) + tap(base + uSub * vec2(0.75, 0.25))
             + tap(base + uSub * vec2(0.25, 0.75)) + tap(base + uSub * vec2(0.75, 0.75));
    fragColor = acc * 0.25;
}
)GLSL";

    const char* ASCII_BODY = R"GLSL(
uniform sampler2D uDown;     // 2x2 texels per cell
uniform vec2  uDownSize;     // allocated size of uDown, texels
uniform sampler2D uAtlas;    // 256 glyph cells in one row; alpha = coverage
uniform float uSlots;
uniform sampler2D uLevels;   // 256x1 auto-levels tone curve (r)
uniform float uAutoLevels;
uniform vec2  uCell;         // cell size, framebuffer px
uniform vec2  uOrigin;       // framebuffer px of local cell (0,0)
uniform vec2  uCellOffset;   // global index of local cell (0,0), for stable noise
uniform vec2  uGrid;         // cells covering the box
uniform vec4  uFg;
uniform vec4  uBg;
uniform float uJitter;
uniform float uTint;
uniform float uShade;
uniform float uSaturation;
uniform float uBgFill;
uniform float uContrast;
uniform float uBrightness;
uniform float uGamma;
uniform float uInvert;
uniform float uSharpen;
// this step's glitches, rolled on the CPU like asciivid's Glitcher (cells / px)
uniform vec4  uScramble;     // x, y, w, h; w = 0: none
uniform vec4  uDrop;
uniform float uDropSolid;
uniform float uRoll;         // rows the grid jumps down
uniform vec3  uTears[7];     // y0, height, shift (px, relative to the grid)
uniform int   uTearCount;

const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

float hash(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.x + p.y) * p.z);
}

vec3 toLin(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

vec3 toSrgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

vec4 cellSrc(vec2 c) {
    c = clamp(c, vec2(0.0), uGrid - 1.0);
    return texture(uDown, (c * 2.0 + 1.0) / uDownSize);
}

bool inRect(vec2 c, vec4 r) {
    return r.z > 0.0 && all(greaterThanEqual(c, r.xy)) && all(lessThan(c, r.xy + r.zw));
}

vec4 effect(vec2 fragCoord) {
    vec2 px = fragCoord - uOrigin;
    vec2 gridPx = uGrid * uCell;

    // tears: bands of pixel rows slide sideways, wrapping around the grid
    for (int i = 0; i < 7; ++i) {
        if (i >= uTearCount)
            break;
        vec3 t = uTears[i];
        if (px.y >= t.x && px.y < t.x + t.y)
            px.x += t.z;
    }
    px.x = mod(px.x, gridPx.x);

    vec2 c = floor(px / uCell);
    vec2 local = fract(px / uCell);
    vec2 gc = c + uCellOffset;
    vec2 sc = vec2(c.x, mod(c.y - uRoll, uGrid.y)); // roll: the grid jumps down

    vec4 src = cellSrc(sc);
    float l = dot(src.rgb, LUMA);
    if (uSharpen > 0.0) {
        float n = dot(cellSrc(sc + vec2(1.0, 0.0)).rgb + cellSrc(sc - vec2(1.0, 0.0)).rgb
                    + cellSrc(sc + vec2(0.0, 1.0)).rgb + cellSrc(sc - vec2(0.0, 1.0)).rgb, LUMA) * 0.25;
        l = clamp(l + (l - n) * uSharpen, 0.0, 1.0);
    }
    if (uAutoLevels > 0.5)
        l = texture(uLevels, vec2((l * 255.0 + 0.5) / 256.0, 0.5)).r;
    l = clamp((l - 0.5) * uContrast + 0.5 + uBrightness, 0.0, 1.0);
    l = pow(l, uGamma);
    l = mix(l, 1.0 - l, uInvert);
    if (uJitter > 0.0)
        l = clamp(l + (hash(vec3(gc, uTick)) - 0.5) * uJitter, 0.0, 1.0);

    bool dropped = inRect(c, uDrop);
    bool solid = dropped && uDropSolid > 0.5;
    float lg = l;
    if (inRect(c, uScramble))
        lg = 0.35 + 0.65 * hash(vec3(gc, uTick + 9.0));
    if (solid)
        lg = 1.0;

    float slot = floor(lg * (uSlots - 1.0) + 0.5);
    float ink = dropped && !solid ? 0.0 : texture(uAtlas, vec2((slot + local.x) / uSlots, local.y)).a;

    // Normalise the source colour so dark pixels still read through the glyph shading
    vec3 hue = src.rgb / max(max(src.r, max(src.g, src.b)), 0.004);
    hue = clamp(mix(vec3(dot(hue, LUMA)), hue, uSaturation), 0.0, 1.0);
    vec3 base = mix(uFg.rgb, hue, uTint);
    base *= 1.0 - uShade + uShade * l;
    if (solid)
        base = uFg.rgb;

    // blend glyph coverage in linear light, like asciivid, so antialiased strokes keep weight
    float fill = dropped ? 0.0 : uBgFill * l;
    vec3 cellBg = mix(uBg.rgb, base, fill);
    vec3 rgb = toSrgb(mix(toLin(cellBg), toLin(base), ink));
    float a = mix(uBg.a, 1.0, max(ink * uFg.a, fill));
    return vec4(rgb * a, a);
}
)GLSL";

    // Client-memory texture uploads with tightly packed rows, whatever unpack state (or
    // bound unpack PBO) Hyprland left behind; restored afterwards.
    struct SUnpackGuard {
        GLint buffer = 0, alignment = 4, rowLength = 0, skipRows = 0, skipPixels = 0;
        SUnpackGuard() {
            glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &buffer);
            glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
            glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
            glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
            glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        }
        ~SUnpackGuard() {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
            glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
            glPixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
        }
    };

    constexpr int MAX_TEARS = 7;

    // asciivid's tone::level_curve: a 1%-99% percentile stretch blended with histogram
    // equalization by `eq`
    std::array<float, 256> levelCurve(const std::vector<float>& lum, float eq) {
        std::array<uint32_t, 256> hist{};
        for (float l : lum)
            hist[std::clamp((int)(l * 255.F), 0, 255)]++;
        const float            total = std::max<size_t>(lum.size(), 1);
        std::array<float, 256> cdf{};
        float                  acc = 0;
        for (int i = 0; i < 256; ++i) {
            acc += hist[i];
            cdf[i] = acc / total;
        }
        auto find = [&](float q) {
            for (int i = 0; i < 256; ++i)
                if (cdf[i] >= q)
                    return i / 255.F;
            return 1.F;
        };
        float lo = find(0.01F), hi = find(0.99F);
        // don't blow a near-flat frame up into noise
        const float mid = (lo + hi) / 2.F;
        lo              = std::max(0.F, std::min(lo, mid - 0.15F));
        hi              = std::min(1.F, std::max(hi, mid + 0.15F));
        const float cLo = cdf[(int)(lo * 255.F)], cHi = std::max(cdf[(int)(hi * 255.F)], 1e-6F);

        std::array<float, 256> curve{};
        for (int i = 0; i < 256; ++i) {
            const float l       = i / 255.F;
            const float stretch = (l - lo) / (hi - lo);
            const float equal   = (cdf[i] - cLo) / std::max(cHi - cLo, 1e-6F);
            curve[i]            = std::clamp(stretch + (equal - stretch) * eq, 0.F, 1.F);
        }
        return curve;
    }

    // splitmix64, seeded per (window, stream, step) so a step's glitches never depend on
    // which frames happened to be drawn
    struct SRng {
        uint64_t s;
        SRng(uint64_t key, uint64_t stream, int64_t step) : s(key * 0x9E3779B97F4A7C15ULL ^ (stream << 56) ^ (uint64_t)step * 0xD1B54A32D192ED03ULL) {}
        uint64_t next() {
            uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
            z          = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z          = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            return z ^ (z >> 31);
        }
        float f() {
            return (next() >> 40) / 16777216.F;
        }
        int range(int lo, int hi) { // inclusive
            return hi <= lo ? lo : lo + (int)(next() % (uint64_t)(hi - lo + 1));
        }
    };

    enum eStream : uint64_t {
        STREAM_BURST = 1,
        STREAM_CELLS = 2,
        STREAM_TEARS = 3,
    };

    // one animation step's glitches, as in asciivid's Glitcher (cells for rects, px for tears)
    struct SGlitch {
        std::array<float, 4>             scramble{}, drop{};
        bool                             solid = false;
        float                            roll  = 0;
        std::array<float, MAX_TEARS * 3> tears{};
        int                              tearCount = 0;
    };

    // longest burst, in steps; bursts last 3..MAX_BURST steps
    constexpr int MAX_BURST = 12;

    SGlitch       rollGlitch(float intensity, uint64_t key, int64_t step, int cols, int rows, float cellH) {
        SGlitch g;
        if (intensity <= 0 || cols < 1 || rows < 1)
            return g;

        bool burst = false;
        for (int d = 0; d < MAX_BURST && d <= step && !burst; ++d) {
            SRng r(key, STREAM_BURST, step - d);
            burst = r.f() < intensity * 0.03F && r.range(3, MAX_BURST) > d;
        }
        auto p    = [&](float base) { return std::min(1.F, base * intensity * (burst ? 4.F : 1.F)); };
        auto rect = [&](SRng& r, std::array<float, 4>& out) {
            const int w = std::min(r.range(cols / 12 + 1, cols / 2 + 1), cols);
            const int h = std::min(r.range(1, rows / 6 + 1), rows);
            out         = {(float)r.range(0, cols - w), (float)r.range(0, rows - h), (float)w, (float)h};
        };

        SRng cells(key, STREAM_CELLS, step);
        if (cells.f() < p(0.3F))
            rect(cells, g.scramble);
        if (cells.f() < p(0.4F)) {
            rect(cells, g.drop);
            g.solid = cells.f() < 0.3F;
        }
        if (cells.f() < p(0.05F))
            g.roll = cells.range(1, rows / 8 + 1);

        SRng      tears(key, STREAM_TEARS, step);
        const int H = (int)std::lround(rows * cellH);
        if (tears.f() < p(0.6F)) {
            g.tearCount = std::min(1 + tears.range(0, (int)(intensity * 6.F)), MAX_TEARS);
            for (int i = 0; i < g.tearCount; ++i) {
                const int height = std::min(tears.range(2, (int)(cellH * 6.F)), H);
                const int y0     = tears.range(0, H - height);
                // fraction of the largest shift; render() scales it by the grid width
                const float shift = tears.range(1, 1000) / 1000.F;
                g.tears[i * 3 + 0] = y0;
                g.tears[i * 3 + 1] = height;
                g.tears[i * 3 + 2] = tears.f() < 0.5F ? shift : -shift;
            }
        }
        return g;
    }

    struct SGrid {
        Vector2D origin;     // framebuffer px of local cell (0,0)
        Vector2D cellOffset; // global index of local cell (0,0)
        Vector2D cells;      // grid size
        Vector2D cell;       // cell px
    };
}

class CAsciiEffect : public IEffect {
  public:
    std::string name() const override {
        return "ascii";
    }

    void configure(const json& s) override {
        m_fps          = std::clamp(jnum(s, "fps", 12), 0.F, 240.F);
        m_jitter       = jnum(s, "jitter", 0);
        m_glitch       = jnum(s, "glitch", 0);
        m_shade        = jnum(s, "shade", 0.5);
        m_contrast     = jnum(s, "contrast", 1);
        m_gamma        = std::max(0.01F, jnum(s, "gamma", 1));
        m_tint         = jnum(s, "tint", 0);
        m_backdropTint = jnum(s, "backdropTint", 0);
        m_invert       = jnum(s, "invert", 0);
        m_autoInvert   = jnum(s, "autoInvert", 1) > 0.5F;
        m_autoLevels   = jnum(s, "autoLevels", 1) > 0.5F;
        m_equalize     = std::clamp(jnum(s, "equalize", 0.5), 0.F, 1.F);
        m_brightness   = jnum(s, "brightness", 0);
        m_sharpen      = std::clamp(jnum(s, "sharpen", 0), 0.F, 5.F);
        m_saturation   = jnum(s, "saturation", 1.5);
        m_bgFill       = std::clamp(jnum(s, "bgFill", 0), 0.F, 1.F);

        // colour roles (overridable per section; defaults per the bar's look)
        m_fgBackdropRole = jstr(s, "backdropFg", "primary");
        m_fgWindowRole   = jstr(s, "fg", "on_surface");
        m_bgRole         = jstr(s, "bg", "surface");
        m_fgBackdrop     = settings().color(m_fgBackdropRole, {0.6F, 0.85F, 1.F, 1.F});
        m_fgWindow       = settings().color(m_fgWindowRole, {0.85F, 0.9F, 0.9F, 1.F});
        m_bg             = settings().color(m_bgRole, {0.04F, 0.08F, 0.09F, 1.F});

        const int         fontSize = std::clamp((int)std::lround(jnum(s, "fontSize", 10)), 4, 128);
        const std::string charset  = jstr(s, "charset", "wd2");

        const bool        sizeChanged    = fontSize != m_fontSize;
        const bool        charsetChanged = !m_charset.empty() && charset != m_charset;
        m_fontSize                       = fontSize;
        m_charset                        = charset;
        m_atlasPath                      = xdgDir("XDG_CACHE_HOME", ".cache") + "/ascii/atlas-" + std::to_string(m_fontSize) + ".png";

        if (charsetChanged)
            generateAtlas(); // same file name, different ramp
        else if (sizeChanged || !m_atlasLoaded)
            loadOrGenerateAtlas();
    }

    float fps() const override {
        return (m_jitter > 0 || m_glitch > 0) ? m_fps : 0.F;
    }

    bool ready() const override {
        return m_atlasLoaded && !m_failed;
    }

    CBox requiredBox(const CBox& box, eMode mode, float scale) const override {
        if (mode == MODE_WINDOW)
            return box; // sampling is clamped to the window
        const auto g = grid(box, mode, scale);
        return CBox{g.origin, g.cells * g.cell};
    }

    bool poll() override {
        if (m_genPid > 0) {
            int        status = 0;
            const auto r      = waitpid(m_genPid, &status, WNOHANG);
            if (r == 0)
                return false; // still running
            if (r == m_genPid && !(WIFEXITED(status) && WEXITSTATUS(status) == 0))
                log(ERR, "asciivid --export-atlas failed for {}", m_atlasPath);
            m_genPid = -1;
            // r < 0 (ECHILD): Hyprland reaped it for us; just look at the files
            return loadAtlas();
        }
        // atlas regenerated externally (e.g. by the bar)? reload on mtime change
        struct stat st;
        if (m_atlasLoaded && stat(m_atlasPath.c_str(), &st) == 0 && st.st_mtim.tv_sec != m_atlasMtime)
            return loadAtlas();
        if (!m_atlasLoaded && m_genPid <= 0 && stat(m_atlasPath.c_str(), &st) == 0)
            return loadAtlas();
        return false;
    }

    void render(const SFrame& f) override {
        if (m_failed || !m_atlasLoaded)
            return;
        if (!ensureGL())
            return;
        if (m_atlasDirty) {
            uploadAtlas();
            m_atlasDirty = false;
        }
        if (!m_atlasTex)
            return;

        const auto g = grid(f.box, f.mode, f.scale);
        if (g.cells.x < 1 || g.cells.y < 1)
            return;

        // pass 1: downsample framebuffer -> 2x2 texels per cell
        const int dw = (int)g.cells.x * 2, dh = (int)g.cells.y * 2;
        if (!ensureScratch(m_down, dw, dh))
            return;
        glUseProgram(m_downProg->id());
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, f.srcTex);
        m_downProg->set1i("uSrc", 0);
        m_downProg->set2f("uSrcSize", f.fbSize.x, f.fbSize.y);
        m_downProg->set2f("uOrigin", g.origin.x, g.origin.y);
        m_downProg->set2f("uSub", g.cell.x / 2.0, g.cell.y / 2.0);
        // window mode: partial edge cells must not pick up the border/wallpaper outside the
        // window; backdrop: anything on the monitor is fair game
        if (f.mode == MODE_WINDOW)
            m_downProg->set4f("uClamp", f.box.x, f.box.y, f.box.x + f.box.w, f.box.y + f.box.h);
        else
            m_downProg->set4f("uClamp", 0, 0, f.fbSize.x, f.fbSize.y);
        drawToScratch(*m_downProg, m_down, dw, dh);

        const bool BD   = f.mode == MODE_BACKDROP;
        const auto fg   = BD ? m_fgBackdrop : m_fgWindow;
        const auto tint = BD ? m_backdropTint : m_tint;
        const auto tick = m_fps > 0 ? std::floor(f.time * m_fps) : 0.0;

        // auto-levels / auto-invert from last frame's readback of this window's grid
        // Unlike asciivid, light content is inverted in colour mode too: on a desktop the point
        // is that every shaded window keeps the theme's dark background.
        auto& lv    = m_levels[f.key];
        lv.lastUsed = f.time;
        if (m_autoLevels || (m_autoInvert && m_invert < 0.5F))
            updateLevels(lv, f, dw, dh);
        const bool invert = m_invert >= 0.5F || (m_autoInvert && lv.inverted);
        collectGarbage(f.time);

        const auto gl = rollGlitch(fps() > 0 ? m_glitch : 0.F, f.key, (int64_t)tick, (int)g.cells.x, (int)g.cells.y, (float)g.cell.y);
        auto       tears = gl.tears;
        const float gridW = g.cells.x * g.cell.x, maxShift = std::clamp(gridW * 0.25F * m_glitch, 4.F, std::max(4.F, gridW));
        for (int i = 0; i < gl.tearCount; ++i)
            tears[i * 3 + 2] = std::copysign(std::max(1.F, std::abs(tears[i * 3 + 2]) * maxShift), tears[i * 3 + 2]);

        // pass 2: glyphs into the window box
        auto& p = *m_glyphProg;
        glUseProgram(p.id());
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_down.tex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_atlasTex);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, lv.lut);
        glActiveTexture(GL_TEXTURE0);
        p.set1i("uDown", 0);
        p.set1i("uAtlas", 1);
        p.set1i("uLevels", 2);
        p.set1f("uAutoLevels", m_autoLevels && lv.lut && lv.hasCurve ? 1.F : 0.F);
        p.set2f("uDownSize", m_down.w, m_down.h);
        p.set1f("uSlots", m_slots);
        p.set2f("uCell", g.cell.x, g.cell.y);
        p.set2f("uOrigin", g.origin.x, g.origin.y);
        p.set2f("uCellOffset", g.cellOffset.x, g.cellOffset.y);
        p.set2f("uGrid", g.cells.x, g.cells.y);
        p.set4f("uFg", fg.r, fg.g, fg.b, fg.a);
        p.set4f("uBg", m_bg.r, m_bg.g, m_bg.b, m_bg.a);
        p.set1f("uTick", (float)tick);
        p.set1f("uJitter", m_jitter);
        p.set1f("uTint", tint);
        p.set1f("uShade", m_shade);
        p.set1f("uSaturation", m_saturation);
        p.set1f("uBgFill", m_bgFill);
        p.set1f("uContrast", m_contrast);
        p.set1f("uBrightness", m_brightness);
        p.set1f("uGamma", m_gamma);
        p.set1f("uInvert", invert ? 1.F : 0.F);
        p.set1f("uSharpen", m_sharpen);
        p.setfv("uScramble", gl.scramble.data(), 4);
        p.setfv("uDrop", gl.drop.data(), 4);
        p.set1f("uDropSolid", gl.solid ? 1.F : 0.F);
        p.set1f("uRoll", gl.roll);
        p.set1i("uTearCount", gl.tearCount);
        if (gl.tearCount > 0)
            glUniform3fv(p.loc("uTears"), gl.tearCount, tears.data());
        finalPass(f, p);
    }

    void destroyGL() override {
        for (auto& [_, lv] : m_levels)
            destroyLevels(lv);
        m_levels.clear();
        m_downProg.reset();
        m_glyphProg.reset();
        destroyScratch(m_down);
        if (m_atlasTex)
            glDeleteTextures(1, &m_atlasTex);
        m_atlasTex = 0;
        m_glReady  = false;
    }

    ~CAsciiEffect() override {
        if (m_genPid > 0)
            waitpid(m_genPid, nullptr, WNOHANG);
    }

  private:
    struct SLevels {
        GLuint                 pbo = 0, lut = 0;
        GLsync                 fence = nullptr;
        int                    w = 0, h = 0; // size of the pending readback, texels
        std::array<float, 256> curve{}, target{};
        bool                   hasCurve = false, hasTarget = false, inverted = false;
        bool                   selfRedraw = false; // this render was one we asked for
        double                 lastUsed   = 0;
    };

    // Collect the previous readback (if the GPU is done with it), ease the curve toward it,
    // then queue a readback of this frame's grid. Asks for extra frames while easing.
    void updateLevels(SLevels& lv, const SFrame& f, int dw, int dh) {
        const bool external = !lv.selfRedraw;
        lv.selfRedraw       = false;
        bool changed = false, pending = false;

        GLint prevPack = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPack);

        if (lv.fence) {
            const GLenum r = glClientWaitSync(lv.fence, 0, 0);
            if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED) {
                glDeleteSync(lv.fence);
                lv.fence = nullptr;
                changed  = collect(lv);
            } else if (r == GL_WAIT_FAILED) {
                glDeleteSync(lv.fence);
                lv.fence = nullptr;
            } else
                pending = true;
        }

        // a new readback when the content may have changed (any damage we didn't cause) or
        // when the last one moved the target
        if (!lv.fence && (external || changed || !lv.hasTarget)) {
            if (!lv.pbo)
                glGenBuffers(1, &lv.pbo);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, lv.pbo);
            glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)dw * dh * 4, nullptr, GL_STREAM_READ);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, m_down.fbo);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            lv.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            lv.w     = dw;
            lv.h     = dh;
            // static content needs one more frame to pick the result up; animated windows
            // get one from their timer anyway
            pending = pending || !(external && fps() > 0);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, prevPack);

        bool easing = false;
        if (lv.hasTarget) {
            float maxDiff = 0;
            for (int i = 0; i < 256; ++i) {
                const float next = lv.hasCurve ? lv.curve[i] + (lv.target[i] - lv.curve[i]) * 0.15F : lv.target[i];
                maxDiff          = std::max(maxDiff, std::abs(lv.target[i] - next));
                lv.curve[i]      = next;
            }
            lv.hasCurve = true;
            easing      = maxDiff > 0.5F / 255.F;
            uploadCurve(lv);
        }

        if (easing || changed || pending) {
            f.wantsRedraw = true;
            lv.selfRedraw = true;
        }
    }

    // reads the finished readback; returns true if the target curve or inversion changed
    bool collect(SLevels& lv) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, lv.pbo);
        const auto* px = (const unsigned char*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)lv.w * lv.h * 4, GL_MAP_READ_BIT);
        if (!px)
            return false;
        const int          cols = lv.w / 2, rows = lv.h / 2;
        std::vector<float> lum((size_t)cols * rows);
        float              border = 0;
        int                borderN = 0;
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                float l = 0;
                for (int k = 0; k < 4; ++k) {
                    const unsigned char* t = px + (((size_t)(r * 2 + k / 2) * lv.w) + (c * 2 + k % 2)) * 4;
                    l += (0.2126F * t[0] + 0.7152F * t[1] + 0.0722F * t[2]) / 255.F;
                }
                l /= 4.F;
                lum[(size_t)r * cols + c] = l;
                if (r == 0 || r == rows - 1 || c == 0 || c == cols - 1) {
                    border += l;
                    borderN++;
                }
            }
        }
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);

        // light background (mean of the outer ring of cells): invert so it stays dark, with
        // hysteresis so a page scrolling past doesn't flicker
        border /= std::max(borderN, 1);
        const bool inverted = lv.inverted ? border > 0.5F : border > 0.7F;

        const auto target  = levelCurve(lum, m_equalize);
        float      maxDiff = 0;
        for (int i = 0; i < 256; ++i)
            maxDiff = std::max(maxDiff, std::abs(target[i] - lv.target[i]));
        const bool changed = !lv.hasTarget || maxDiff > 1.F / 255.F || inverted != lv.inverted;
        lv.target          = target;
        lv.hasTarget       = true;
        lv.inverted        = inverted;
        return changed;
    }

    void uploadCurve(SLevels& lv) {
        std::array<unsigned char, 256> bytes;
        for (int i = 0; i < 256; ++i)
            bytes[i] = (unsigned char)std::lround(std::clamp(lv.curve[i], 0.F, 1.F) * 255.F);
        SUnpackGuard unpack;
        glActiveTexture(GL_TEXTURE0);
        if (!lv.lut) {
            glGenTextures(1, &lv.lut);
            glBindTexture(GL_TEXTURE_2D, lv.lut);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 256, 1, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else
            glBindTexture(GL_TEXTURE_2D, lv.lut);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RED, GL_UNSIGNED_BYTE, bytes.data());
    }

    void destroyLevels(SLevels& lv) {
        if (lv.fence)
            glDeleteSync(lv.fence);
        if (lv.pbo)
            glDeleteBuffers(1, &lv.pbo);
        if (lv.lut)
            glDeleteTextures(1, &lv.lut);
        lv = SLevels{};
    }

    // windows that stopped rendering (closed, untagged, off-screen) give their state back
    void collectGarbage(double now) {
        for (auto it = m_levels.begin(); it != m_levels.end();) {
            if (now - it->second.lastUsed > 10.0) {
                destroyLevels(it->second);
                it = m_levels.erase(it);
            } else
                ++it;
        }
    }

    SGrid grid(const CBox& box, eMode mode, float scale) const {
        SGrid g;
        g.cell = Vector2D{(double)m_cw, (double)m_ch} * std::max(scale, 0.1F);
        if (mode == MODE_BACKDROP) {
            // monitor-aligned: cells don't move when the window does
            const Vector2D first{std::floor(box.x / g.cell.x), std::floor(box.y / g.cell.y)};
            const Vector2D last{std::ceil((box.x + box.w) / g.cell.x), std::ceil((box.y + box.h) / g.cell.y)};
            g.origin     = first * g.cell;
            g.cellOffset = first;
            g.cells      = last - first;
        } else {
            g.origin     = box.pos();
            g.cellOffset = {};
            g.cells      = {std::ceil(box.w / g.cell.x), std::ceil(box.h / g.cell.y)};
        }
        return g;
    }

    bool ensureGL() {
        if (m_glReady)
            return true;
        std::string err;
        m_downProg = compileProgram(QUAD_VERT, DOWNSAMPLE_FRAG, err);
        if (m_downProg)
            m_glyphProg = compileProgram(QUAD_VERT, finalFragment(ASCII_BODY), err);
        if (!m_downProg || !m_glyphProg) {
            log(ERR, "ascii: shader compile failed: {}", err);
            m_failed = true;
            return false;
        }
        m_glReady = true;
        return true;
    }

    bool uploadAtlas() {
        if (m_pixels.empty())
            return m_atlasTex != 0;
        if (!m_atlasTex)
            glGenTextures(1, &m_atlasTex);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_atlasTex);
        SUnpackGuard unpack;
        // cairo ARGB32 is BGRA in memory on little endian; we only read .a, which is in place
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_pixW, m_pixH, 0, GL_RGBA, GL_UNSIGNED_BYTE, m_pixels.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_pixels.clear();
        m_pixels.shrink_to_fit();
        return true;
    }

    void loadOrGenerateAtlas() {
        struct stat st;
        if (stat(m_atlasPath.c_str(), &st) == 0 && loadAtlas())
            return;
        generateAtlas();
    }

    // CPU side only; the GL upload happens on the next render (EGL context guaranteed current)
    bool loadAtlas() {
        std::string jsonPath = m_atlasPath.substr(0, m_atlasPath.size() - 4) + ".json";
        int         cw = 0, ch = 0, slots = 256;
        try {
            std::ifstream f(jsonPath);
            if (!f.good())
                return false;
            json j = json::parse(f);
            cw     = j.value("cw", 0);
            ch     = j.value("ch", 0);
            slots  = j.value("slots", 256);
        } catch (std::exception& e) {
            log(WARN, "ascii: bad atlas sidecar {}: {}", jsonPath, e.what());
            return false;
        }
        if (cw <= 0 || ch <= 0 || slots <= 1)
            return false;

        cairo_surface_t* surf = cairo_image_surface_create_from_png(m_atlasPath.c_str());
        if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
            log(WARN, "ascii: can't read atlas {}", m_atlasPath);
            cairo_surface_destroy(surf);
            return false;
        }
        cairo_surface_flush(surf);
        const int w = cairo_image_surface_get_width(surf), h = cairo_image_surface_get_height(surf);
        const int stride = cairo_image_surface_get_stride(surf);
        const auto fmt   = cairo_image_surface_get_format(surf);
        if (fmt != CAIRO_FORMAT_ARGB32 && fmt != CAIRO_FORMAT_RGB24 && fmt != CAIRO_FORMAT_A8) {
            cairo_surface_destroy(surf);
            return false;
        }

        // repack to tight 4 bytes/px with alpha in byte 3
        m_pixels.assign((size_t)w * h * 4, 0);
        const unsigned char* data = cairo_image_surface_get_data(surf);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                unsigned char* dst = &m_pixels[((size_t)y * w + x) * 4];
                if (fmt == CAIRO_FORMAT_A8) {
                    dst[0] = dst[1] = dst[2] = dst[3] = data[(size_t)y * stride + x];
                } else {
                    const uint32_t px = *(const uint32_t*)(data + (size_t)y * stride + x * 4);
                    dst[0]            = (px >> 16) & 0xff;
                    dst[1]            = (px >> 8) & 0xff;
                    dst[2]            = px & 0xff;
                    dst[3]            = fmt == CAIRO_FORMAT_RGB24 ? 0xff : (px >> 24) & 0xff;
                }
            }
        }
        cairo_surface_destroy(surf);

        m_pixW  = w;
        m_pixH  = h;
        m_cw    = cw;
        m_ch    = ch;
        m_slots = slots;
        struct stat st;
        m_atlasMtime  = stat(m_atlasPath.c_str(), &st) == 0 ? st.st_mtim.tv_sec : 0;
        m_atlasLoaded = true;
        m_atlasDirty  = true; // uploaded on the next render
        log(DEBUG, "ascii: loaded atlas {} ({}x{}, cell {}x{})", m_atlasPath, w, h, cw, ch);
        return true;
    }

    void generateAtlas() {
        if (m_genPid > 0)
            return; // one at a time; poll() picks the result up
        const std::string script = "mkdir -p \"$(dirname \"$1\")\" && exec asciivid --export-atlas \"$1\" --font-size \"$2\" --charset \"$3\" >/dev/null 2>&1";
        const std::string size   = std::to_string(m_fontSize);
        const char*       argv[] = {"sh", "-c", script.c_str(), "sh", m_atlasPath.c_str(), size.c_str(), m_charset.c_str(), nullptr};
        pid_t             pid    = -1;
        if (posix_spawnp(&pid, "sh", nullptr, nullptr, (char* const*)argv, environ) != 0) {
            log(ERR, "ascii: couldn't spawn asciivid");
            return;
        }
        m_genPid = pid;
        log(DEBUG, "ascii: generating atlas {} (font {} charset {})", m_atlasPath, m_fontSize, m_charset);
    }

    // settings
    float                     m_fps = 12, m_jitter = 0, m_glitch = 0, m_shade = 0.5, m_contrast = 1, m_gamma = 1, m_tint = 0, m_backdropTint = 0, m_invert = 0;
    float                     m_equalize = 0.5, m_brightness = 0, m_sharpen = 0, m_saturation = 1.5, m_bgFill = 0;
    bool                      m_autoLevels = true, m_autoInvert = true;
    std::unordered_map<uintptr_t, SLevels> m_levels;
    std::string               m_fgBackdropRole, m_fgWindowRole, m_bgRole;
    SColor                    m_fgBackdrop, m_fgWindow, m_bg;
    int                       m_fontSize = 0;
    std::string               m_charset;

    // atlas
    std::string               m_atlasPath;
    std::vector<unsigned char> m_pixels;
    int                       m_pixW = 0, m_pixH = 0, m_cw = 6, m_ch = 11, m_slots = 256;
    time_t                    m_atlasMtime  = 0;
    bool                      m_atlasLoaded = false, m_atlasDirty = false;
    pid_t                     m_genPid      = -1;

    // GL
    bool                      m_glReady = false, m_failed = false;
    std::unique_ptr<CProgram> m_downProg, m_glyphProg;
    SScratch                  m_down;
    GLuint                    m_atlasTex = 0;
};

std::unique_ptr<IEffect> hs::makeAsciiEffect() {
    return std::make_unique<CAsciiEffect>();
}
