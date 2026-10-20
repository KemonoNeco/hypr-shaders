// User shaders: ~/.config/hypr/shaders/<name>.frag (+ optional <name>.json).
//
// The .frag is a GLSL ES 3.00 *body* defining `vec4 effect(vec2 fragCoord)` (premultiplied
// output). Available to it:
//   vec4  source(vec2 uv)   the pixels under the window box this frame (uv 0..1, y down)
//   vec2  uSize             window box size, framebuffer px
//   vUV, uBox, uTime, uTick, uAlpha, uFg, uBg (palette colours), plus its own uniforms.
// The .json gives defaults: {"fps": 0, "fg": "primary", "bg": "surface",
//                            "uniforms": {"uAmount": 0.5, "uColor": [1, 0, 0, 1]}}
// and config.json["<name>"] overrides any of those keys (uniforms by name, at top level).
// Both files are hot-reloaded.

#include "../Effect.hpp"
#include "../Log.hpp"

#include <fstream>
#include <sstream>
#include <sys/stat.h>

using namespace hs;
using json = nlohmann::json;

namespace {
    const char* USER_PRELUDE = R"GLSL(
uniform sampler2D uSource;
uniform vec2 uSourceScale;
uniform vec2 uSize;
uniform vec4 uFg;
uniform vec4 uBg;
vec4 source(vec2 uv) { return texture(uSource, clamp(uv, 0.0, 1.0) * uSourceScale); }
)GLSL";

    std::string shaderDir() {
        return xdgDir("XDG_CONFIG_HOME", ".config") + "/hypr/shaders";
    }

    int64_t mtimeOf(const std::string& p) {
        struct stat st;
        if (stat(p.c_str(), &st) != 0)
            return -1;
        return (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
    }

    bool readFile(const std::string& p, std::string& out) {
        std::ifstream f(p);
        if (!f.good())
            return false;
        std::stringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    }
}

class CUserEffect : public IEffect {
  public:
    CUserEffect(std::string name) : m_name(std::move(name)) {
        m_fragPath = shaderDir() + "/" + m_name + ".frag";
        m_jsonPath = shaderDir() + "/" + m_name + ".json";
        reload();
    }

    std::string name() const override {
        return m_name;
    }

    void configure(const json& section) override {
        m_section = section.is_object() ? section : json::object();
        apply();
    }

    float fps() const override {
        return m_fps;
    }

    bool ready() const override {
        return !m_source.empty() && !m_failed;
    }

    bool poll() override {
        if (mtimeOf(m_fragPath) == m_fragMtime && mtimeOf(m_jsonPath) == m_jsonMtime)
            return false;
        reload();
        return true;
    }

    void render(const SFrame& f) override {
        if (m_failed || m_source.empty())
            return;
        if (!m_prog) {
            std::string err;
            m_prog = compileProgram(QUAD_VERT, finalFragment(USER_PRELUDE + std::string{"#line 1\n"} + m_source), err);
            if (!m_prog) {
                log(ERR, "shader {}: {}", m_name, err);
                m_failed = true; // until the file changes
                return;
            }
        }

        if (!copyRegion(f, f.box, m_copy))
            return;

        auto& p = *m_prog;
        glUseProgram(p.id());
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_copy.tex);
        p.set1i("uSource", 0);
        p.set2f("uSourceScale", std::ceil(f.box.w) / m_copy.w, std::ceil(f.box.h) / m_copy.h);
        p.set2f("uSize", f.box.w, f.box.h);
        p.set4f("uFg", m_fg.r, m_fg.g, m_fg.b, m_fg.a);
        p.set4f("uBg", m_bg.r, m_bg.g, m_bg.b, m_bg.a);
        p.set1f("uTick", m_fps > 0 ? (float)std::floor(f.time * m_fps) : 0.F);
        for (auto& [k, v] : m_uniforms)
            p.setfv(k.c_str(), v.data(), (int)v.size());
        finalPass(f, p);
    }

    void destroyGL() override {
        m_prog.reset();
        destroyScratch(m_copy);
    }

  private:
    void reload() {
        m_fragMtime = mtimeOf(m_fragPath);
        m_jsonMtime = mtimeOf(m_jsonPath);
        m_source.clear();
        readFile(m_fragPath, m_source);
        m_defaults = json::object();
        if (std::string js; readFile(m_jsonPath, js)) {
            try {
                m_defaults = json::parse(js);
            } catch (std::exception& e) { log(WARN, "shader {}: bad {}: {}", m_name, m_jsonPath, e.what()); }
        }
        m_prog.reset(); // recompiled lazily in render (GL context)
        m_failed = false;
        apply();
    }

    void apply() {
        auto pick = [&](const char* key) -> const json* {
            if (m_section.contains(key))
                return &m_section[key];
            if (m_defaults.contains(key))
                return &m_defaults[key];
            return nullptr;
        };
        const json* fps = pick("fps");
        m_fps           = fps && fps->is_number() ? std::clamp(fps->get<float>(), 0.F, 240.F) : 0.F;
        const json* fg  = pick("fg");
        const json* bg  = pick("bg");
        m_fg            = settings().color(fg && fg->is_string() ? fg->get<std::string>() : "primary", {1, 1, 1, 1});
        m_bg            = settings().color(bg && bg->is_string() ? bg->get<std::string>() : "surface", {0, 0, 0, 1});

        m_uniforms.clear();
        auto addUniform = [&](const std::string& k, const json& v) {
            std::vector<float> vals;
            if (v.is_number())
                vals.push_back(v.get<float>());
            else if (v.is_boolean())
                vals.push_back(v.get<bool>() ? 1.F : 0.F);
            else if (v.is_array() && v.size() >= 1 && v.size() <= 4) {
                for (auto& e : v)
                    vals.push_back(e.is_number() ? e.get<float>() : 0.F);
            } else if (v.is_string()) { // palette role or hex colour -> vec4
                const auto col = settings().color(v.get<std::string>(), {0, 0, 0, 1});
                vals           = {col.r, col.g, col.b, col.a};
            }
            if (!vals.empty())
                m_uniforms[k] = vals;
        };
        if (m_defaults.contains("uniforms") && m_defaults["uniforms"].is_object()) {
            for (auto& [k, v] : m_defaults["uniforms"].items()) {
                addUniform(k, m_section.contains(k) ? m_section[k] : v);
            }
        }
    }

    std::string                                        m_name, m_fragPath, m_jsonPath, m_source;
    int64_t                                            m_fragMtime = -1, m_jsonMtime = -1;
    json                                               m_defaults = json::object(), m_section = json::object();
    float                                              m_fps = 0;
    SColor                                             m_fg, m_bg;
    std::unordered_map<std::string, std::vector<float>> m_uniforms;
    std::unique_ptr<CProgram>                          m_prog;
    bool                                               m_failed = false;
    SScratch                                           m_copy;
};

std::unique_ptr<IEffect> hs::makeUserEffect(const std::string& name) {
    // names come from window tags; keep them to a safe file-name subset
    if (name.empty() || name.size() > 64)
        return nullptr;
    for (char c : name)
        if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.'))
            return nullptr;
    if (name.starts_with("."))
        return nullptr;
    struct stat st;
    if (stat((shaderDir() + "/" + name + ".frag").c_str(), &st) != 0)
        return nullptr;
    return std::make_unique<CUserEffect>(name);
}
