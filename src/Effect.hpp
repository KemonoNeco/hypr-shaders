#pragma once
// The effect ("shader") interface and the shared machinery every effect builds on.
//
// An effect is asked to render one window per call. It gets the window box in framebuffer
// pixels, the framebuffer's colour texture (everything already drawn this frame: in
// backdrop mode that is what is *behind* the window, in window mode it includes the window
// itself), and draws the result back into the window box via finalPass(), which clips to
// the window's rounded corners, applies the window alpha and honours the frame's damage.
//
// Adding a shader = one class implementing IEffect, registered in Effects.cpp
// (or a .frag + .json dropped into ~/.config/hypr/shaders/, see UserEffect).

#include "GL.hpp"
#include "Settings.hpp"

#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Region.hpp>
#include <hyprutils/math/Vector2D.hpp>
#include <memory>
#include <string>
#include <vector>

namespace hs {
    using Hyprutils::Math::CBox;
    using Hyprutils::Math::CRegion;
    using Hyprutils::Math::Vector2D;

    enum eMode : uint8_t {
        MODE_WINDOW   = 0, // tag "shader": the window itself
        MODE_BACKDROP = 1, // tag "shaderbg": what is behind the (translucent) window
    };

    struct SFrame {
        eMode    mode = MODE_WINDOW;
        CBox     box;          // window box, framebuffer pixels (x right, y down)
        Vector2D fbSize;       // framebuffer size in pixels
        GLuint   srcTex = 0;   // colour texture of the framebuffer being drawn into. Never sample it
                               // in a pass that renders into that framebuffer (use copyRegion).
        GLuint   srcFBO = 0;   // the framebuffer object itself (for blits)
        float    scale  = 1.F; // monitor scale
        float    alpha  = 1.F; // window alpha (fade in/out)
        float    radius = 0.F, roundingPower = 2.F; // rounded corners, framebuffer pixels
        double   time   = 0;   // seconds since the plugin loaded
        CRegion  damage;       // render-space damage to draw (already intersected with the box)
        bool     transformDamage = true;
        uintptr_t key = 0;     // stable per window + mode, for effects that keep per-window state
        // set by the effect when it needs another frame without new damage (e.g. easing)
        mutable bool wantsRedraw = false;
    };

    // Shared header prepended to every final-pass fragment shader. The effect body must
    // define `vec4 effect(vec2 fragCoord)` returning a premultiplied colour; fragCoord is in
    // framebuffer pixels, vUV runs 0..1 across the window box (y down).
    extern const char* FINAL_HEADER;
    extern const char* FINAL_FOOTER;
    std::string        finalFragment(const std::string& body);

    // Draw prog (built from finalFragment) over frame.box into the frame's framebuffer.
    // The caller has bound its own textures/uniforms; this sets uBox, uRadius, uRoundingPower,
    // uAlpha, uTime, uRect, uTarget, binds the target and handles damage + scissoring.
    void finalPass(const SFrame& f, CProgram& prog);

    // Copy `region` (framebuffer pixels) of the frame's framebuffer into a scratch texture
    // at (0,0). Returns false on failure.
    bool copyRegion(const SFrame& f, const CBox& region, SScratch& dst);
    void destroyCopyProgram();

    class IEffect {
      public:
        virtual ~IEffect() = default;

        virtual std::string name() const = 0;

        // (re)read settings: called once after creation and whenever config.json or the
        // palette changes. `section` is config.json[name()] (null if absent).
        virtual void configure(const nlohmann::json& section) {}

        // animation rate in Hz; 0 = static (only redrawn on damage)
        virtual float fps() const {
            return 0.F;
        }

        // pixels (framebuffer space) that must be valid this frame for the box to render
        // correctly; damage touching the box is expanded to this. Default: the box itself.
        virtual CBox requiredBox(const CBox& box, eMode mode, float scale) const {
            return box;
        }

        // called periodically (~4 Hz) on the main thread, e.g. to pick up regenerated assets.
        // return true if the effect changed and tagged windows should be redrawn.
        virtual bool poll() {
            return false;
        }

        // false if the effect cannot render right now (missing asset, compile error...)
        virtual bool ready() const {
            return true;
        }

        virtual void render(const SFrame& f) = 0;

        // free GL objects (called with the EGL context current)
        virtual void destroyGL() {}
    };

    // registry
    IEffect*                  findEffect(const std::string& name); // creates on first use
    std::vector<IEffect*>     loadedEffects();
    void                      reconfigureEffects();
    bool                      pollEffects();
    void                      destroyEffects();

    // factory for built-ins, see Effects.cpp
    std::unique_ptr<IEffect> makeAsciiEffect();
    std::unique_ptr<IEffect> makeUserEffect(const std::string& name); // nullptr if no such file

    double                   nowSeconds(); // since plugin load
}
