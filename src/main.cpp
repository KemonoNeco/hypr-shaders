// hypr-shaders: render tagged windows (tag "shader") or what is behind them (tag "shaderbg")
// through GPU shaders, inside the compositor. "shader:<name>" picks the effect (default ascii).

#define WLR_USE_UNSTABLE

#include "Effect.hpp"
#include "Log.hpp"
#include "Windows.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/render/Context.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <unordered_map>

using namespace hs;

static HANDLE PHANDLE = nullptr;

namespace {
    struct SWinState {
        PHLWINDOWREF window;
        STagState    tags;
        double       tick = -1;
    };

    std::unordered_map<Desktop::View::CWindow*, SWinState> g_windows;
    wl_event_source*                                       g_timer = nullptr;
    double                                                 g_lastPoll = -1;
    std::vector<Hyprutils::Signal::CHyprSignalListener>    g_listeners;

    void                                                   attach(PHLWINDOW w) {
        if (!w)
            return;
        bool hasWin = false, hasBg = false;
        for (auto& d : w->presentation().decorations()) {
            const auto n = d->getDisplayName();
            hasWin       = hasWin || n == DECO_NAME_WINDOW;
            hasBg        = hasBg || n == DECO_NAME_BACKDROP;
        }
        if (!hasBg)
            HyprlandAPI::addWindowDecoration(PHANDLE, w, makeShared<CShaderDeco>(w, MODE_BACKDROP));
        if (!hasWin)
            HyprlandAPI::addWindowDecoration(PHANDLE, w, makeShared<CShaderDeco>(w, MODE_WINDOW));
    }

    void damageAllTagged() {
        for (auto& w : Desktop::windowState()->windows()) {
            if (!w->mapped() || w->isHidden())
                continue;
            damageEffect(w, tagState(w));
        }
    }

    // Like blur: whatever damage touches a shaded window's region grows to the whole region, so
    // every character cell (and glitch tear source) is rendered completely this frame. Done
    // on the damage ring before the frame starts, so buffer-age bookkeeping stays correct.
    void onRenderPre(PHLMONITOR mon) {
        if (!mon)
            return;
        try {
            CRegion cur = mon->m_damage.getBufferDamage(1);
            if (cur.empty())
                return;

            struct SReq {
                CBox box;
                bool added = false;
            };
            std::vector<SReq> reqs;
            for (auto& w : Desktop::windowState()->windows()) {
                if (!w->mapped() || w->isHidden())
                    continue;
                const auto st = tagState(w);
                if (!st.any() || !g_pHyprRenderer->shouldRenderWindow(w, mon))
                    continue;
                IEffect* e = findEffect(st.name);
                if (st.backdrop)
                    reqs.push_back({requiredRenderBox(w, mon, e, MODE_BACKDROP)});
                if (st.window)
                    reqs.push_back({requiredRenderBox(w, mon, e, MODE_WINDOW)});
            }

            // two rounds: an expansion can reach into another shaded window
            for (int round = 0; round < 2; ++round) {
                bool grew = false;
                for (auto& r : reqs) {
                    if (r.added || cur.copy().intersect(r.box).empty())
                        continue;
                    mon->m_damage.damage(r.box);
                    cur.add(r.box);
                    r.added = true;
                    grew    = true;
                }
                if (!grew)
                    break;
            }
        } catch (std::exception& e) { log(ERR, "render.pre: {}", e.what()); }
    }

    int onTimer(void*) {
        const double now     = nowSeconds();
        double       nextIn  = 0.25;

        try {
            if (now - g_lastPoll >= 0.25) {
                g_lastPoll   = now;
                bool changed = false;
                if (settings().poll()) {
                    reconfigureEffects();
                    changed = true;
                }
                changed = pollEffects() || changed;
                if (changed)
                    damageAllTagged();
            }

            for (auto it = g_windows.begin(); it != g_windows.end();) {
                if (it->second.window.expired())
                    it = g_windows.erase(it);
                else
                    ++it;
            }

            for (auto& w : Desktop::windowState()->windows()) {
                if (!w->mapped())
                    continue;
                const auto st  = tagState(w);
                auto&      ws  = g_windows[w.get()];
                if (ws.window.lock() != w)
                    ws = SWinState{.window = w};

                if (st != ws.tags) {
                    // tag toggled without updateWindow() being called (or before it)
                    damageEffect(w, ws.tags);
                    damageEffect(w, st);
                    g_pHyprRenderer->damageWindow(w);
                    ws.tags = st;
                    ws.tick = -1;
                }
                if (!st.any() || w->isHidden())
                    continue;

                IEffect* e = findEffect(st.name);
                if (!e)
                    continue;
                const float fps = e->fps();
                if (fps <= 0)
                    continue;

                bool visible = false;
                for (auto const& m : State::monitorState()->monitors())
                    visible = visible || g_pHyprRenderer->shouldRenderWindow(w, m);
                if (!visible)
                    continue;

                const double tick = std::floor(now * fps);
                if (tick != ws.tick) {
                    ws.tick = tick;
                    damageEffect(w, st);
                }
                nextIn = std::min(nextIn, (tick + 1.0) / fps - now);
            }
        } catch (std::exception& e) { log(ERR, "timer: {}", e.what()); }

        wl_event_source_timer_update(g_timer, std::clamp((int)std::ceil(nextIn * 1000.0), 1, 250));
        return 0;
    }
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();
    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hypr-shaders] version mismatch: rebuild against the running Hyprland", CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
        throw std::runtime_error("[hypr-shaders] version mismatch");
    }

    if (!g_pHyprRenderer || g_pHyprRenderer->type() != Render::IHyprRenderer::RT_GL) {
        HyprlandAPI::addNotification(PHANDLE, "[hypr-shaders] needs the OpenGL renderer", CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
        throw std::runtime_error("[hypr-shaders] not GL");
    }

    nowSeconds(); // start the clock
    settings().poll();

    g_listeners.emplace_back(Event::bus()->m_events.window.open.listen([](PHLWINDOW w) { attach(w); }));
    g_listeners.emplace_back(Event::bus()->m_events.render.pre.listen([](PHLMONITOR m) { onRenderPre(m); }));

    for (auto& w : Desktop::windowState()->windows()) {
        if (!w->mapped())
            continue;
        attach(w);
    }

    g_timer = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, onTimer, nullptr);
    wl_event_source_timer_update(g_timer, 1);

    damageAllTagged();
    log(DEBUG, "loaded");
    return {"hypr-shaders", "Renders tagged windows (shader) or what is behind them (shaderbg) through GPU shaders, e.g. ASCII art", "kemononeco", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (g_timer) {
        wl_event_source_remove(g_timer);
        g_timer = nullptr;
    }
    g_listeners.clear();
    cancelRedraws();

    g_pHyprRenderer->context().m_pass.removeAllOfType("CShaderPassElement");

    // repaint tagged windows without the effect
    damageAllTagged();
    g_windows.clear();

    Render::GL::g_pHyprOpenGL->makeEGLCurrent();
    destroyEffects();
    destroyCopyProgram();
    destroyShared();
}
