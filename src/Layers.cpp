#include "Layers.hpp"
#include "Log.hpp"
#include "Windows.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/rule/layerRule/LayerRuleApplicator.hpp>
#include <hyprland/src/desktop/state/LayerState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/Context.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>

#include <regex>
#include <unordered_map>

using namespace hs;

namespace {
    using FRenderLayer = void (*)(Render::IHyprRenderer*, Render::CRenderContext&, PHLLS, PHLMONITOR, const Time::steady_tp&, bool, bool, bool);

    CFunctionHook* g_hook = nullptr;

    struct SConfig {
        uint64_t                gen     = 0;
        bool                    enabled = false;
        std::string             shader  = "ascii";
        std::vector<std::regex> namespaces;
    } g_cfg;

    struct SLayerState {
        PHLLSREF layer;
        double   tick    = -1;
        bool     matched = false;
    };
    std::unordered_map<Desktop::View::CLayerSurface*, SLayerState> g_layers;

    const SConfig& cfg() {
        if (g_cfg.gen == settings().generation())
            return g_cfg;
        g_cfg     = SConfig{.gen = settings().generation()};
        const auto& s = settings().section("surfaces");
        if (!s.is_object())
            return g_cfg;
        g_cfg.enabled = s.value("enabled", false);
        g_cfg.shader  = jstr(s, "shader", "ascii");
        if (auto it = s.find("namespaces"); it != s.end() && it->is_array()) {
            for (auto& n : *it) {
                if (!n.is_string())
                    continue;
                try {
                    g_cfg.namespaces.emplace_back(n.get<std::string>());
                } catch (std::exception& e) { log(ERR, "surfaces: bad namespace regex {}: {}", n.dump(), e.what()); }
            }
        }
        return g_cfg;
    }

    bool matches(PHLLS ls) {
        const auto& c = cfg();
        if (!c.enabled || !ls)
            return false;
        for (auto& re : c.namespaces)
            if (std::regex_search(ls->m_namespace, re))
                return true;
        return false;
    }

    CBox layerBox(PHLLS ls, PHLMONITOR mon) {
        Vector2D pos  = ls->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) - mon->m_position;
        Vector2D size = ls->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        return CBox{pos, size}.scale(mon->m_scale).round();
    }

    CBox requiredBox(PHLLS ls, PHLMONITOR mon, IEffect* e) {
        const CBox box = layerBox(ls, mon);
        if (!e)
            return box;
        CBox req = fromFB(e->requiredBox(toFB(box, mon), MODE_BACKDROP, mon->m_scale), mon);
        if (mon->m_transform != WL_OUTPUT_TRANSFORM_NORMAL)
            req.expand(32);
        const double x1 = std::floor(std::min(req.x, box.x)), y1 = std::floor(std::min(req.y, box.y));
        const double x2 = std::ceil(std::max(req.x + req.w, box.x + box.w)), y2 = std::ceil(std::max(req.y + req.h, box.y + box.h));
        return CBox{x1, y1, x2 - x1, y2 - y1};
    }

    bool visible(PHLLS ls) {
        return ls && ls->mapped() && ls->alphaNonZero() && ls->m_monitor.lock();
    }

    void damageLayer(PHLLS ls) {
        const auto mon = ls->m_monitor.lock();
        if (!mon)
            return;
        mon->addDamage(requiredBox(ls, mon, findEffect(cfg().shader)));
    }

    // the main surface's texture as a GL_TEXTURE_2D (mask), or null
    SP<Render::ITexture> maskOf(PHLLS ls) {
        const auto wl = ls->wlSurface();
        if (!wl || !wl->resource())
            return nullptr;
        auto tex = wl->resource()->m_current.texture;
        if (!tex || !tex->m_texID)
            return nullptr;
        // RGBX has no coverage to gate on; EGLImage textures are not GL_TEXTURE_2D
        if (tex->m_type != Render::TEXTURE_RGBA || !dynamic_cast<Render::GL::CGLTexture*>(tex.get()))
            return nullptr;
        return tex;
    }

    void hkRenderLayer(Render::IHyprRenderer* self, Render::CRenderContext& ctx, PHLLS ls, PHLMONITOR mon, const Time::steady_tp& time, bool popups, bool lockscreen,
                       bool workspaceScene) {
        try {
            // mirrors renderLayer's own early-outs, so nothing is drawn for a skipped layer
            if (!popups && !lockscreen && ls && mon && !ctx.m_renderingSnapshot && matches(ls) && ls->mapped() && ls->alphaNonZero() &&
                (workspaceScene || ls->acceptsInput()) && ctx.shouldRenderContent(ls->m_ruleApplicator->noScreenShare().valueOrDefault()) &&
                !(ls->m_ruleApplicator->aboveLock().valueOrDefault() && g_pSessionLockManager->isSessionLocked())) {
                if (auto mask = maskOf(ls)) {
                    Render::IHyprRenderer::addPassElement(ctx,
                                                          makeUnique<CLayerPassElement>(CLayerPassElement::SData{
                                                              .layer  = ls,
                                                              .effect = cfg().shader,
                                                              .alpha  = ls->alpha()[Desktop::View::LS_ALPHA_FADE]->value(),
                                                              .box    = layerBox(ls, mon),
                                                              .mask   = mask,
                                                          }));
                }
            }
        } catch (std::exception& e) { log(ERR, "renderLayer hook: {}", e.what()); }
        ((FRenderLayer)g_hook->m_original)(self, ctx, ls, mon, time, popups, lockscreen, workspaceScene);
    }
}

bool hs::initLayers(void* handle) {
    for (auto& fn : HyprlandAPI::findFunctionsByName(handle, "renderLayer")) {
        if (!fn.demangled.contains("IHyprRenderer::renderLayer"))
            continue;
        g_hook = HyprlandAPI::createFunctionHook(handle, fn.address, (void*)&hkRenderLayer);
        if (g_hook && g_hook->hook())
            return true;
        log(ERR, "could not hook {}", fn.demangled);
        return false;
    }
    log(ERR, "IHyprRenderer::renderLayer not found");
    return false;
}

void hs::exitLayers() {
    // the hook itself is removed by Hyprland with the plugin
    for (auto& [_, st] : g_layers)
        if (const auto ls = st.layer.lock(); ls && st.matched)
            damageLayer(ls);
    g_layers.clear();
}

void hs::damageAllLayers() {
    for (auto& ls : Desktop::layerState()->layers())
        if (visible(ls) && (matches(ls) || g_layers[ls.get()].matched))
            damageLayer(ls);
}

void hs::tickLayers(double now, double& nextIn) {
    for (auto it = g_layers.begin(); it != g_layers.end();) {
        if (it->second.layer.expired())
            it = g_layers.erase(it);
        else
            ++it;
    }

    IEffect* e = nullptr;
    for (auto& ls : Desktop::layerState()->layers()) {
        auto& st = g_layers[ls.get()];
        if (st.layer.lock() != ls)
            st = SLayerState{.layer = ls};

        const bool m = matches(ls);
        if (m != st.matched) { // config changed: repaint with/without the effect
            st.matched = m;
            if (visible(ls))
                damageLayer(ls);
        }
        if (!m || !visible(ls))
            continue;

        if (!e && !(e = findEffect(cfg().shader)))
            return;
        const float fps = e->fps();
        if (fps <= 0)
            continue;
        const double tick = std::floor(now * fps);
        if (tick != st.tick) {
            st.tick = tick;
            damageLayer(ls);
        }
        nextIn = std::min(nextIn, (tick + 1.0) / fps - now);
    }
}

std::vector<CBox> hs::layerRequiredBoxes(PHLMONITOR mon) {
    std::vector<CBox> out;
    if (!cfg().enabled)
        return out;
    IEffect* e = findEffect(cfg().shader);
    for (auto& ls : Desktop::layerState()->layers())
        if (visible(ls) && ls->m_monitor.lock() == mon && matches(ls))
            out.push_back(requiredBox(ls, mon, e));
    return out;
}

// ---------------------------------------------------------------- pass element

std::optional<CBox> CLayerPassElement::boundingBox(Render::CRenderContext& ctx) {
    const auto mon = ctx.m_data.pMonitor.lock();
    if (!mon)
        return std::nullopt;
    return m_data.box.copy().scale(1.0 / mon->m_scale);
}

std::vector<UP<IPassElement>> CLayerPassElement::draw(Render::CRenderContext& ctx) {
    g_drawCtx = &ctx;
    try {
        auto&      rd  = ctx.m_data;
        const auto ls  = m_data.layer.lock();
        const auto mon = rd.pMonitor.lock();
        const auto fb  = rd.currentFB;
        if (!ls || !mon || !fb || !m_data.mask)
            return {};
        const auto tex = fb->getTexture();
        if (!tex || !tex->m_texID)
            return {};
        auto* glfb = dynamic_cast<Render::GL::CGLFramebuffer*>(fb.get());
        if (!glfb)
            return {};

        IEffect* e = findEffect(m_data.effect);
        if (!e || !e->ready())
            return {};

        CBox box = m_data.box;
        rd.renderModif.applyToBox(box);
        box.round();
        if (box.w < 1 || box.h < 1)
            return {};

        CRegion damage = rd.damage.copy().intersect(box);
        if (damage.empty())
            return {};

        SFrame f;
        f.mode            = MODE_BACKDROP;
        f.box             = toFB(box, mon);
        f.fbSize          = fb->m_size.x > 0 ? fb->m_size : mon->m_pixelSize;
        f.srcTex          = tex->m_texID;
        f.srcFBO          = glfb->getFBID();
        f.scale           = mon->m_scale;
        f.alpha           = std::clamp(m_data.alpha, 0.F, 1.F);
        f.time            = nowSeconds();
        f.damage          = damage;
        f.transformDamage = rd.transformDamage;
        f.key             = reinterpret_cast<uintptr_t>(ls.get()) * 2 + 1;
        f.maskTex         = m_data.mask->m_texID;

        SGLGuard guard;
        e->render(f);
    } catch (std::exception& ex) { log(ERR, "layer render failed: {}", ex.what()); }
    g_drawCtx = nullptr;
    return {};
}
