#pragma once
// Small raw-GL helpers shared by every effect: program compilation, a unit quad,
// grow-only scratch framebuffers and a guard that restores the GL state Hyprland caches.

#include <GLES3/gl32.h>
#include <memory>
#include <string>
#include <unordered_map>

namespace Render {
    class CRenderContext;
}

namespace hs {

    // Render context of the pass element currently drawing (set by CShaderPassElement::draw),
    // falling back to the renderer's own. Hyprland's scissor/viewport helpers need it.
    extern Render::CRenderContext* g_drawCtx;
    Render::CRenderContext&        renderCtx();

    class CProgram {
      public:
        CProgram(GLuint id) : m_id(id) {}
        ~CProgram();
        CProgram(const CProgram&)            = delete;
        CProgram& operator=(const CProgram&) = delete;

        GLuint id() const {
            return m_id;
        }
        GLint loc(const char* name);

        void  set1f(const char* n, float v);
        void  set2f(const char* n, float a, float b);
        void  set4f(const char* n, float a, float b, float c, float d);
        void  set1i(const char* n, int v);
        void  setfv(const char* n, const float* v, int count); // count = 1..4 components

      private:
        GLuint                                 m_id = 0;
        std::unordered_map<std::string, GLint> m_locs;
    };

    // Vertex shader used by every pass: draws uRect (target pixels) of a target uTarget px big.
    // gl_FragCoord.xy therefore equals target pixel coordinates (y grows downwards in
    // Hyprland's work buffers), and vUV runs 0..1 across uRect.
    extern const char* QUAD_VERT;

    // compile + link; on failure returns nullptr and fills err
    std::unique_ptr<CProgram> compileProgram(const std::string& vert, const std::string& frag, std::string& err);

    struct SScratch {
        GLuint fbo = 0, tex = 0;
        int    w = 0, h = 0; // allocated size (grow-only)
        GLenum internalFormat = GL_RGBA8;
    };

    // ensure scratch is at least w x h; returns false on failure
    bool ensureScratch(SScratch& s, int w, int h, GLenum internalFormat = GL_RGBA8);
    void destroyScratch(SScratch& s);

    // shared unit quad VAO (attribute 0 = vec2 0..1)
    GLuint quadVAO();
    void   destroyShared();

    // Draws the quad into (0,0,w,h) of a scratch target. Program must already be in use.
    // Binds the scratch FBO, sets the viewport, disables blend + scissor.
    void drawToScratch(CProgram& prog, SScratch& s, int w, int h);

    // Saves/restores the bits of GL state that Hyprland either caches or relies on.
    struct SGLGuard {
        SGLGuard();
        ~SGLGuard();
        GLint program = 0, vao = 0, arrayBuffer = 0, activeTex = 0, readFB = 0, drawFB = 0;
        GLint viewport[4] = {0, 0, 0, 0};
        GLint tex[4]      = {0, 0, 0, 0};
    };
}
