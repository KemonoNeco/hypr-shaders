// "ascii": the asciivid look on the GPU, a port of quickshell/shaders/glsl/ascii.frag.
//
// Pass 1 (downsample): the framebuffer region under the window's character grid is reduced
//   to 2x2 texels per character cell (4 bilinear taps each, so ~64 source pixels/cell).
// Pass 2 (final): per cell, the 2x2 texels are averaged by one bilinear fetch at the cell
//   centre -> luminance -> contrast/gamma -> jitter -> glitches -> glyph slot -> atlas.
//
// Backdrop cells are aligned to the monitor's pixel grid so glyphs don't swim when the window
// moves; window-mode cells are aligned to the window origin.

#include "../Effect.hpp"
#include "../Log.hpp"

#include <cairo/cairo.h>
#include <fstream>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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
uniform vec2  uCell;         // cell size, framebuffer px
uniform vec2  uOrigin;       // framebuffer px of local cell (0,0)
uniform vec2  uCellOffset;   // global index of local cell (0,0), for stable noise
uniform vec2  uGrid;         // cells covering the box
uniform vec4  uFg;
uniform vec4  uBg;
uniform float uJitter;
uniform float uGlitch;
uniform float uTint;
uniform float uShade;
uniform float uContrast;
uniform float uGamma;
uniform float uInvert;

float hash(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.x + p.y) * p.z);
}

vec4 effect(vec2 fragCoord) {
    vec2 px = fragCoord - uOrigin;
    vec2 c = floor(px / uCell);
    vec2 local = fract(px / uCell);
    vec2 gc = c + uCellOffset;
    float tick = uTick;

    // Glitches, in the spirit of asciivid's DedSec passes: bursts raise the odds for a
    // step, rows tear sideways, blocks drop out or scramble.
    float burst = step(hash(vec3(tick, 7.0, 1.0)), uGlitch * 0.15);
    float g = uGlitch * (1.0 + 3.0 * burst);
    vec2 sc = c;
    float band = floor(gc.y / 3.0);
    if (hash(vec3(band, tick, 2.0)) < g * 0.06)
        sc.x = mod(sc.x + floor((hash(vec3(band, tick, 3.0)) - 0.5) * uGrid.x * 0.3), uGrid.x);
    vec2 block = floor(gc / vec2(8.0, 2.0));
    float blockRoll = hash(vec3(block, tick + 0.5));
    bool dropped = blockRoll < g * 0.015;
    bool scrambled = !dropped && blockRoll < g * 0.04;

    vec4 src = texture(uDown, (sc * 2.0 + 1.0) / uDownSize);
    float l = dot(src.rgb, vec3(0.2126, 0.7152, 0.0722));
    l = clamp((l - 0.5) * uContrast + 0.5, 0.0, 1.0);
    l = pow(l, uGamma);
    l = mix(l, 1.0 - l, uInvert);
    if (uJitter > 0.0)
        l = clamp(l + (hash(vec3(gc, tick)) - 0.5) * uJitter, 0.0, 1.0);
    if (scrambled)
        l = 0.35 + 0.65 * hash(vec3(gc, tick + 9.0));

    float slot = floor(l * (uSlots - 1.0) + 0.5);
    float ink = dropped ? 0.0 : texture(uAtlas, vec2((slot + local.x) / uSlots, local.y)).a;

    // Normalise the source colour so dark pixels still read through the glyph shading
    vec3 hue = src.rgb / max(max(src.r, max(src.g, src.b)), 0.004);
    vec3 base = mix(uFg.rgb, hue, uTint);
    base *= 1.0 - uShade + uShade * l;
    vec3 rgb = mix(uBg.rgb, base, ink);
    float a = mix(uBg.a, 1.0, ink * uFg.a);
    return vec4(rgb * a, a);
}
)GLSL";

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

        // pass 2: glyphs into the window box
        auto&      p      = *m_glyphProg;
        const bool BD     = f.mode == MODE_BACKDROP;
        const auto fg     = BD ? m_fgBackdrop : m_fgWindow;
        const auto tick   = m_fps > 0 ? std::floor(f.time * m_fps) : 0.0;
        glUseProgram(p.id());
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_down.tex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_atlasTex);
        glActiveTexture(GL_TEXTURE0);
        p.set1i("uDown", 0);
        p.set1i("uAtlas", 1);
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
        p.set1f("uGlitch", m_glitch);
        p.set1f("uTint", BD ? m_backdropTint : m_tint);
        p.set1f("uShade", m_shade);
        p.set1f("uContrast", m_contrast);
        p.set1f("uGamma", m_gamma);
        p.set1f("uInvert", m_invert);
        finalPass(f, p);
    }

    void destroyGL() override {
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
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
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
