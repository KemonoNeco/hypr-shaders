#include "GL.hpp"
#include "Log.hpp"

#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

using namespace hs;

const char* hs::QUAD_VERT = R"GLSL(#version 300 es
precision highp float;
layout(location = 0) in vec2 aPos;
uniform vec4 uRect;   // target pixels: x, y, w, h
uniform vec2 uTarget; // target size in pixels
out vec2 vUV;
void main() {
    vUV = aPos;
    vec2 p = uRect.xy + aPos * uRect.zw;
    gl_Position = vec4(p / uTarget * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

CProgram::~CProgram() {
    if (m_id)
        glDeleteProgram(m_id);
}

GLint CProgram::loc(const char* name) {
    auto it = m_locs.find(name);
    if (it != m_locs.end())
        return it->second;
    GLint l = glGetUniformLocation(m_id, name);
    m_locs.emplace(name, l);
    return l;
}

void CProgram::set1f(const char* n, float v) {
    if (auto l = loc(n); l >= 0)
        glUniform1f(l, v);
}
void CProgram::set2f(const char* n, float a, float b) {
    if (auto l = loc(n); l >= 0)
        glUniform2f(l, a, b);
}
void CProgram::set4f(const char* n, float a, float b, float c, float d) {
    if (auto l = loc(n); l >= 0)
        glUniform4f(l, a, b, c, d);
}
void CProgram::set1i(const char* n, int v) {
    if (auto l = loc(n); l >= 0)
        glUniform1i(l, v);
}
void CProgram::setfv(const char* n, const float* v, int count) {
    auto l = loc(n);
    if (l < 0)
        return;
    switch (count) {
        case 1: glUniform1fv(l, 1, v); break;
        case 2: glUniform2fv(l, 1, v); break;
        case 3: glUniform3fv(l, 1, v); break;
        case 4: glUniform4fv(l, 1, v); break;
        default: break;
    }
}

static GLuint compileStage(GLenum type, const std::string& src, std::string& err) {
    GLuint      sh  = glCreateShader(type);
    const char* ptr = src.c_str();
    glShaderSource(sh, 1, &ptr, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::string log(std::max(len, 1), '\0');
        glGetShaderInfoLog(sh, len, nullptr, log.data());
        err += (type == GL_VERTEX_SHADER ? "vertex: " : "fragment: ") + log;
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

std::unique_ptr<CProgram> hs::compileProgram(const std::string& vert, const std::string& frag, std::string& err) {
    GLuint vs = compileStage(GL_VERTEX_SHADER, vert, err);
    if (!vs)
        return nullptr;
    GLuint fs = compileStage(GL_FRAGMENT_SHADER, frag, err);
    if (!fs) {
        glDeleteShader(vs);
        return nullptr;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    glLinkProgram(prog);
    glDetachShader(prog, vs);
    glDetachShader(prog, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log(std::max(len, 1), '\0');
        glGetProgramInfoLog(prog, len, nullptr, log.data());
        err += "link: " + log;
        glDeleteProgram(prog);
        return nullptr;
    }
    return std::make_unique<CProgram>(prog);
}

bool hs::ensureScratch(SScratch& s, int w, int h, GLenum internalFormat) {
    w = std::max(w, 1);
    h = std::max(h, 1);
    if (s.fbo && s.w >= w && s.h >= h && s.internalFormat == internalFormat)
        return true;

    // grow with some headroom so resizing windows doesn't reallocate every frame
    int nw = std::max(w, s.w), nh = std::max(h, s.h);
    if (s.fbo && (nw > s.w || nh > s.h)) {
        nw = std::max(nw, (int)(s.w * 1.25));
        nh = std::max(nh, (int)(s.h * 1.25));
    }

    GLint prevTex = 0, prevDraw = 0, prevRead = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);

    if (!s.tex)
        glGenTextures(1, &s.tex);
    glBindTexture(GL_TEXTURE_2D, s.tex);
    // texture storage is immutable with glTexStorage2D; use glTexImage2D so we can grow
    const GLenum fmt  = GL_RGBA;
    const GLenum type = internalFormat == GL_RGBA16F ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE;
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, nw, nh, 0, fmt, type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (!s.fbo)
        glGenFramebuffers(1, &s.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.tex, 0);
    const auto status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

    glBindTexture(GL_TEXTURE_2D, prevTex);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDraw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        log(ERR, "scratch framebuffer incomplete ({:#x}) at {}x{}", (unsigned)status, nw, nh);
        destroyScratch(s);
        return false;
    }

    s.w              = nw;
    s.h              = nh;
    s.internalFormat = internalFormat;
    return true;
}

void hs::destroyScratch(SScratch& s) {
    if (s.fbo)
        glDeleteFramebuffers(1, &s.fbo);
    if (s.tex)
        glDeleteTextures(1, &s.tex);
    s = {};
}

static GLuint g_quadVAO = 0, g_quadVBO = 0;

GLuint hs::quadVAO() {
    if (g_quadVAO)
        return g_quadVAO;

    static const float verts[] = {0.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1.f, 1.f};

    GLint              prevVAO = 0, prevBuf = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);

    glGenVertexArrays(1, &g_quadVAO);
    glGenBuffers(1, &g_quadVBO);
    glBindVertexArray(g_quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    glBindVertexArray(prevVAO);
    glBindBuffer(GL_ARRAY_BUFFER, prevBuf);
    return g_quadVAO;
}

void hs::destroyShared() {
    if (g_quadVBO)
        glDeleteBuffers(1, &g_quadVBO);
    if (g_quadVAO)
        glDeleteVertexArrays(1, &g_quadVAO);
    g_quadVAO = g_quadVBO = 0;
}

void hs::drawToScratch(CProgram& prog, SScratch& s, int w, int h) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.fbo);
    Render::GL::g_pHyprOpenGL->setViewport(0, 0, s.w, s.h);
    Render::GL::g_pHyprOpenGL->scissor(nullptr);
    g_pHyprRenderer->blend(false);

    prog.set4f("uRect", 0, 0, w, h);
    prog.set2f("uTarget", s.w, s.h);

    glBindVertexArray(quadVAO());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

SGLGuard::SGLGuard() {
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFB);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFB);
    glGetIntegerv(GL_VIEWPORT, viewport);
    for (int i = 0; i < 4; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex[i]);
    }
    glActiveTexture(activeTex);
}

SGLGuard::~SGLGuard() {
    for (int i = 0; i < 4; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, tex[i]);
    }
    glActiveTexture(activeTex);
    glUseProgram(program);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, arrayBuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFB);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFB);
    Render::GL::g_pHyprOpenGL->setViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    Render::GL::g_pHyprOpenGL->scissor(nullptr);
    g_pHyprRenderer->blend(true);
}
