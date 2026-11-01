#pragma once
// Layer surfaces (bars, panels, docks): the backdrop effect drawn under them, the way
// "shaderbg" works for windows. Layers have no tags or decorations, so they are picked by
// namespace in config.json:
//
//   "surfaces": { "enabled": true, "shader": "ascii", "namespaces": ["^bar$", "-panel$"] }
//
// The effect is gated by the surface's own alpha, so a fullscreen transparent surface
// holding a small panel only gets it under the panel (and its curved joins), and a
// translucent background lets it show through.

#define WLR_USE_UNSTABLE
#include "Effect.hpp"

#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/Texture.hpp>

namespace hs {
    // install the renderLayer hook; false if Hyprland's symbol could not be found
    bool initLayers(void* handle);
    void exitLayers();

    // timer: animate visible matched layers; lowers nextIn to the next tick
    void tickLayers(double now, double& nextIn);
    // damage everything the effect covers on all matched layers
    void damageAllLayers();
    // render.pre: render-space boxes that must be redrawn whole when damage touches them
    std::vector<CBox> layerRequiredBoxes(PHLMONITOR mon);

    class CLayerPassElement : public IPassElement {
      public:
        struct SData {
            PHLLSREF          layer;
            std::string       effect;
            float             alpha = 1.F;
            CBox              box; // render space, before renderModif
            SP<Render::ITexture> mask;
        };

        CLayerPassElement(SData data) : m_data(std::move(data)) {}
        ~CLayerPassElement() override = default;

        std::vector<UP<IPassElement>> draw(Render::CRenderContext& ctx) override;
        bool                          needsLiveBlur(Render::CRenderContext& ctx) override {
            return false;
        }
        bool needsPrecomputeBlur(Render::CRenderContext& ctx) override {
            return false;
        }
        bool disableSimplification(Render::CRenderContext& ctx) override {
            return true;
        }
        std::optional<CBox> boundingBox(Render::CRenderContext& ctx) override;
        const char*         passName() override {
            return "CShaderLayerPassElement";
        }
        ePassElementType type() override {
            return EK_CUSTOM;
        }

      private:
        SData m_data;
    };
}
