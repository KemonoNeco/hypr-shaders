// Effect registry + the shared final pass.

#include "Effect.hpp"
#include "Log.hpp"

#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <chrono>
#include <map>

using namespace hs;

double hs::nowSeconds() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// ---------------------------------------------------------------- registry

namespace {
    struct SEntry {
        std::unique_ptr<IEffect> effect;
        uint64_t                 configuredGen = 0;
    };
    std::map<std::string, SEntry> g_effects;
    std::map<std::string, double> g_missing; // name -> time of last lookup failure
}

IEffect* hs::findEffect(const std::string& name) {
    if (auto it = g_effects.find(name); it != g_effects.end())
        return it->second.effect.get();

    // don't hit the filesystem every frame for an unknown name
    if (auto it = g_missing.find(name); it != g_missing.end() && nowSeconds() - it->second < 2.0)
        return nullptr;

    std::unique_ptr<IEffect> e;
    if (name == "ascii")
        e = makeAsciiEffect();
    else
        e = makeUserEffect(name);

    if (!e) {
        if (!g_missing.contains(name))
            log(WARN, "unknown shader \"{}\" (no built-in and no ~/.config/hypr/shaders/{}.frag)", name, name);
        g_missing[name] = nowSeconds();
        return nullptr;
    }
    g_missing.erase(name);

    e->configure(settings().section(name));
    auto* raw         = e.get();
    g_effects[name]   = SEntry{std::move(e), settings().generation()};
    return raw;
}

std::vector<IEffect*> hs::loadedEffects() {
    std::vector<IEffect*> v;
    for (auto& [_, e] : g_effects)
        v.push_back(e.effect.get());
    return v;
}

void hs::reconfigureEffects() {
    for (auto& [name, e] : g_effects) {
        if (e.configuredGen == settings().generation())
            continue;
        e.effect->configure(settings().section(name));
        e.configuredGen = settings().generation();
    }
}

bool hs::pollEffects() {
    bool any = false;
    for (auto& [_, e] : g_effects)
        any = e.effect->poll() || any;
    return any;
}

void hs::destroyEffects() {
    for (auto& [_, e] : g_effects)
        e.effect->destroyGL();
    g_effects.clear();
    g_missing.clear();
}

// ---------------------------------------------------------------- final pass

const char* hs::FINAL_HEADER = R"GLSL(#version 300 es
precision highp float;
uniform vec4  uBox;           // window box, framebuffer px (x, y, w, h)
uniform float uRadius;        // corner radius, px (0 = square)
uniform float uRoundingPower;
uniform float uAlpha;         // window alpha
uniform float uTime;          // seconds
uniform float uTick;          // animation step (floor(time * fps))
in vec2 vUV;                  // 0..1 across the box, y down
out vec4 fragColor;
)GLSL";

// Hyprland's rounding.glsl, so the clip matches the window surface exactly
const char* hs::FINAL_FOOTER = R"GLSL(
float hsDistanceWithRounding(vec2 coords, float power) {
    return pow(pow(coords.x, power) + pow(coords.y, power), 1.0 / power);
}

vec4 hsRounding(vec4 color, float radius, float power, vec2 topLeft, vec2 fullSize) {
    const float SMOOTHING = 3.1415926535897932384626433832795 / 5.34665792551;
    vec2 pixCoord = vec2(gl_FragCoord);
    pixCoord -= topLeft + fullSize * 0.5;
    pixCoord *= vec2(lessThan(pixCoord, vec2(0.0))) * -2.0 + 1.0;
    pixCoord -= fullSize * 0.5 - radius;
    pixCoord += vec2(1.0, 1.0) / fullSize;
    if (pixCoord.x + pixCoord.y > radius) {
        float dist = hsDistanceWithRounding(pixCoord, power);
        if (dist > radius + SMOOTHING)
            discard;
        float normalized = 1.0 - smoothstep(0.0, 1.0, (dist - radius + SMOOTHING) / (SMOOTHING * 2.0));
        color *= normalized;
    }
    return color;
}

void main() {
    vec4 c = effect(gl_FragCoord.xy);
    if (uRadius > 0.0)
        c = hsRounding(c, uRadius, uRoundingPower, uBox.xy, uBox.zw);
    fragColor = c * uAlpha;
}
)GLSL";

std::string hs::finalFragment(const std::string& body) {
    return std::string{FINAL_HEADER} + "#line 1\n" + body + FINAL_FOOTER;
}

void hs::finalPass(const SFrame& f, CProgram& prog) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f.srcFBO);
    Render::GL::g_pHyprOpenGL->setViewport(0, 0, f.fbSize.x, f.fbSize.y);
    g_pHyprRenderer->blend(true);

    prog.set4f("uRect", f.box.x, f.box.y, f.box.w, f.box.h);
    prog.set2f("uTarget", f.fbSize.x, f.fbSize.y);
    prog.set4f("uBox", f.box.x, f.box.y, f.box.w, f.box.h);
    prog.set1f("uRadius", f.radius);
    prog.set1f("uRoundingPower", f.roundingPower);
    prog.set1f("uAlpha", f.alpha);
    prog.set1f("uTime", (float)f.time);

    glBindVertexArray(quadVAO());
    f.damage.forEachRect([&f](const auto& RECT) {
        Render::GL::g_pHyprOpenGL->scissor(renderCtx(), &RECT, f.transformDamage);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    });
    Render::GL::g_pHyprOpenGL->scissor(renderCtx(), (const pixman_box32*)nullptr);
}

static std::unique_ptr<CProgram> g_copyProg;
static bool                      g_copyFailed = false;

bool hs::copyRegion(const SFrame& f, const CBox& region, SScratch& dst) {
    // A sampling pass rather than glBlitFramebuffer: the work buffer can be 10-bit or fp16,
    // which glBlitFramebuffer refuses to convert into an RGBA8 target.
    if (!g_copyProg && !g_copyFailed) {
        std::string err;
        g_copyProg = compileProgram(QUAD_VERT, R"GLSL(#version 300 es
precision highp float;
uniform sampler2D uSrc;
uniform vec2 uSrcSize;
uniform vec2 uOrigin;
out vec4 fragColor;
void main() {
    fragColor = texture(uSrc, (uOrigin + gl_FragCoord.xy) / uSrcSize);
}
)GLSL",
                                    err);
        if (!g_copyProg) {
            g_copyFailed = true;
            log(ERR, "copy shader failed: {}", err);
        }
    }
    if (!g_copyProg)
        return false;

    const int w = std::max(1, (int)std::ceil(region.w)), h = std::max(1, (int)std::ceil(region.h));
    if (!ensureScratch(dst, w, h))
        return false;

    glUseProgram(g_copyProg->id());
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, f.srcTex);
    g_copyProg->set1i("uSrc", 0);
    g_copyProg->set2f("uSrcSize", f.fbSize.x, f.fbSize.y);
    g_copyProg->set2f("uOrigin", std::floor(region.x), std::floor(region.y));
    drawToScratch(*g_copyProg, dst, w, h);
    return true;
}

void hs::destroyCopyProgram() {
    g_copyProg.reset();
    g_copyFailed = false;
}
