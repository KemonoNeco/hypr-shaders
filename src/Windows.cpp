#include "Windows.hpp"
#include "Log.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorState.hpp>

using namespace hs;

STagState hs::tagState(PHLWINDOW w) {
    STagState st;
    if (!w || !w->m_ruleApplicator)
        return st;
    for (std::string t : w->m_ruleApplicator->m_tagKeeper.getTags()) {
        if (t.ends_with('*')) // set by a window rule
            t.pop_back();
        if (t == "shader")
            st.window = true;
        else if (t == "shaderbg")
            st.backdrop = true;
        else if (t.starts_with("shader:") && t.size() > 7)
            st.name = t.substr(7);
    }
    if (st.name.empty())
        st.name = "ascii";
    if (!st.any())
        st.name.clear(); // keep "untagged" states comparable
    return st;
}

CBox hs::renderBox(PHLWINDOW w, PHLMONITOR mon) {
    const auto ws   = w->m_workspace;
    Vector2D   pos  = w->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    Vector2D   size = w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    if (ws && !w->m_pinned)
        pos += ws->m_renderOffset->value();
    pos += w->m_floatingOffset;
    pos -= mon->m_position;
    size = {std::max(size.x, 5.0), std::max(size.y, 5.0)};
    return CBox{pos, size}.scale(mon->m_scale).round();
}

CBox hs::toFB(const CBox& box, PHLMONITOR mon) {
    if (mon->m_transform == WL_OUTPUT_TRANSFORM_NORMAL)
        return box;
    CBox b = box;
    b.transform(Math::wlTransformToHyprutils(Math::invertTransform(mon->m_transform)), mon->m_transformedSize.x, mon->m_transformedSize.y);
    return b;
}

CBox hs::fromFB(const CBox& box, PHLMONITOR mon) {
    if (mon->m_transform == WL_OUTPUT_TRANSFORM_NORMAL)
        return box;
    CBox b = box;
    b.transform(Math::wlTransformToHyprutils(mon->m_transform), mon->m_pixelSize.x, mon->m_pixelSize.y);
    return b;
}

CBox hs::requiredRenderBox(PHLWINDOW w, PHLMONITOR mon, IEffect* e, eMode mode) {
    const CBox box = renderBox(w, mon);
    if (!e)
        return box;
    CBox req = fromFB(e->requiredBox(toFB(box, mon), mode, mon->m_scale), mon);
    if (mon->m_transform != WL_OUTPUT_TRANSFORM_NORMAL)
        req.expand(32); // grid axes differ from damage axes; be generous
    // union with the box itself, rounded outwards
    const double x1 = std::floor(std::min(req.x, box.x)), y1 = std::floor(std::min(req.y, box.y));
    const double x2 = std::ceil(std::max(req.x + req.w, box.x + box.w)), y2 = std::ceil(std::max(req.y + req.h, box.y + box.h));
    return CBox{x1, y1, x2 - x1, y2 - y1};
}

void hs::damageEffect(PHLWINDOW w, const STagState& st) {
    if (!w || !st.any())
        return;
    IEffect* e = findEffect(st.name);
    for (auto const& m : State::monitorState()->monitors()) {
        if (!g_pHyprRenderer->shouldRenderWindow(w, m))
            continue;
        if (st.window)
            m->addDamage(requiredRenderBox(w, m, e, MODE_WINDOW));
        if (st.backdrop)
            m->addDamage(requiredRenderBox(w, m, e, MODE_BACKDROP));
    }
}

// ---------------------------------------------------------------- decoration

CShaderDeco::CShaderDeco(PHLWINDOW w, eMode mode) : IHyprWindowDecoration(w), m_window(w), m_mode(mode) {
    m_lastTags = tagState(w);
}

SDecorationPositioningInfo CShaderDeco::getPositioningInfo() {
    SDecorationPositioningInfo info;
    info.policy         = DECORATION_POSITION_ABSOLUTE;
    info.desiredExtents = {{0, 0}, {0, 0}};
    info.edges          = 0;
    info.reserved       = false;
    return info;
}

void CShaderDeco::onPositioningReply(const SDecorationPositioningReply& reply) {
    ;
}

void CShaderDeco::draw(PHLMONITOR mon, float const& a) {
    const auto w = m_window.lock();
    if (!w)
        return;
    const auto st = tagState(w);
    if (!st.has(m_mode))
        return;

    float alpha = a;
    if (m_mode == MODE_BACKDROP) {
        // the backdrop replaces what's behind the window; only fades should make it
        // translucent, not the window's own (in)active opacity
        const float ACTIVE = w->alphaValue(Desktop::View::WINDOW_ALPHA_ACTIVE);
        if (!w->m_ruleApplicator->opaque().valueOrDefault() && ACTIVE > 0.001F)
            alpha = std::min(1.F, a / ACTIVE);
    }

    g_pHyprRenderer->addPassElement(makeUnique<CShaderPassElement>(CShaderPassElement::SData{
        .window = m_window,
        .mode   = m_mode,
        .effect = st.name,
        .alpha  = alpha,
    }));
}

eDecorationType CShaderDeco::getDecorationType() {
    return DECORATION_CUSTOM;
}

void CShaderDeco::updateWindow(PHLWINDOW w) {
    // called on rule/tag changes (Actions::tag -> propertiesChanged -> updateWindowDecos)
    const auto st = tagState(w);
    if (st == m_lastTags)
        return;
    damageEffect(w, m_lastTags);
    damageEffect(w, st);
    g_pHyprRenderer->damageWindow(w);
    m_lastTags = st;
}

void CShaderDeco::damageEntire() {
    if (const auto w = m_window.lock(); w)
        damageEffect(w, tagState(w));
}

eDecorationLayer CShaderDeco::getDecorationLayer() {
    // backdrop: before the surface is drawn (like blur); window: right after the surface
    return m_mode == MODE_BACKDROP ? DECORATION_LAYER_BOTTOM : DECORATION_LAYER_OVER;
}

uint64_t CShaderDeco::getDecorationFlags() {
    return DECORATION_NON_SOLID;
}

std::string CShaderDeco::getDisplayName() {
    return m_mode == MODE_BACKDROP ? DECO_NAME_BACKDROP : DECO_NAME_WINDOW;
}

// ---------------------------------------------------------------- pass element

std::optional<CBox> CShaderPassElement::boundingBox() {
    const auto w   = m_data.window.lock();
    const auto mon = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!w || !mon)
        return std::nullopt;
    // monitor-local logical coordinates
    return renderBox(w, mon).scale(1.0 / mon->m_scale);
}

std::vector<UP<IPassElement>> CShaderPassElement::draw() {
    try {
        auto&      rd  = g_pHyprRenderer->m_renderData;
        const auto w   = m_data.window.lock();
        const auto mon = rd.pMonitor.lock();
        const auto fb  = rd.currentFB;
        if (!w || !w->m_isMapped || !mon || !fb)
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

        CBox box = renderBox(w, mon);
        rd.renderModif.applyToBox(box);
        box.round();
        if (box.w < 1 || box.h < 1)
            return {};

        CRegion damage = rd.damage.copy().intersect(box);
        if (damage.empty())
            return {};

        SFrame f;
        f.mode            = m_data.mode;
        f.box             = toFB(box, mon);
        f.fbSize          = fb->m_size.x > 0 ? fb->m_size : mon->m_pixelSize;
        f.srcTex          = tex->m_texID;
        f.srcFBO          = glfb->getFBID();
        f.scale           = mon->m_scale;
        f.alpha           = std::clamp(m_data.alpha, 0.F, 1.F);
        f.radius          = std::max(0.F, w->rounding() * (float)mon->m_scale - 1.F);
        f.roundingPower   = w->roundingPower();
        f.time            = nowSeconds();
        f.damage          = damage;
        f.transformDamage = rd.transformDamage;

        SGLGuard guard;
        e->render(f);
    } catch (std::exception& ex) { log(ERR, "render failed: {}", ex.what()); }
    return {};
}
