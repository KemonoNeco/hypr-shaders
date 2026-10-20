#pragma once
// Per-window glue: tag parsing, window boxes, the decoration that injects our pass element
// into Hyprland's render pass, and damage helpers.

#define WLR_USE_UNSTABLE
#include "Effect.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>

namespace hs {
    struct STagState {
        bool        window   = false; // "shader"
        bool        backdrop = false; // "shaderbg"
        std::string name;              // "shader:<name>", default "ascii"

        bool        any() const {
            return window || backdrop;
        }
        bool        has(eMode m) const {
            return m == MODE_WINDOW ? window : backdrop;
        }
        bool operator==(const STagState&) const = default;
    };

    STagState tagState(PHLWINDOW w);

    // window box in render space (monitor-local, scaled px, logical orientation)
    CBox renderBox(PHLWINDOW w, PHLMONITOR mon);
    // render space <-> framebuffer space (differs only on rotated/flipped monitors)
    CBox toFB(const CBox& box, PHLMONITOR mon);
    CBox fromFB(const CBox& box, PHLMONITOR mon);

    // render-space region an effect needs valid for this window/mode on this monitor
    CBox requiredRenderBox(PHLWINDOW w, PHLMONITOR mon, IEffect* e, eMode mode);

    // damage everything the effect touches for w (all monitors it is rendered on)
    void damageEffect(PHLWINDOW w, const STagState& st);

    class CShaderDeco : public IHyprWindowDecoration {
      public:
        CShaderDeco(PHLWINDOW w, eMode mode);
        ~CShaderDeco() override = default;

        SDecorationPositioningInfo getPositioningInfo() override;
        void                       onPositioningReply(const SDecorationPositioningReply& reply) override;
        void                       draw(PHLMONITOR, float const& a) override;
        eDecorationType            getDecorationType() override;
        void                       updateWindow(PHLWINDOW) override;
        void                       damageEntire() override;
        eDecorationLayer           getDecorationLayer() override;
        uint64_t                   getDecorationFlags() override;
        std::string                getDisplayName() override;

      private:
        PHLWINDOWREF m_window;
        eMode        m_mode;
        STagState    m_lastTags;
    };

    class CShaderPassElement : public IPassElement {
      public:
        struct SData {
            PHLWINDOWREF window;
            eMode        mode = MODE_WINDOW;
            std::string  effect;
            float        alpha = 1.F;
        };

        CShaderPassElement(SData data) : m_data(std::move(data)) {}
        ~CShaderPassElement() override = default;

        std::vector<UP<IPassElement>> draw() override;
        bool                          needsLiveBlur() override {
            return false;
        }
        bool needsPrecomputeBlur() override {
            return false;
        }
        // Our effect samples whole character cells / regions, so the stuff under the window
        // must be rendered even where an opaque window above would normally occlude it.
        bool disableSimplification() override {
            return true;
        }
        std::optional<CBox> boundingBox() override;
        const char*         passName() override {
            return "CShaderPassElement";
        }
        ePassElementType type() override {
            return EK_CUSTOM;
        }

      private:
        SData m_data;
    };

    inline constexpr const char* DECO_NAME_WINDOW   = "hypr-shaders (window)";
    inline constexpr const char* DECO_NAME_BACKDROP = "hypr-shaders (backdrop)";
}
