// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_gles_renderer.h"

#ifdef ENABLE_GLES_3D

#include "color_utils.h"
#include "data_root_resolver.h"
#include "gcode_gl_fallback.h"
#include "gcode_projection.h"
#include "gcode_render_schedule.h"
#include "gcode_selection_style.h"
#include "lv_draw_buf_guard.h"
#include "runtime_config.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>

// GL backend selection: SDL_GL on desktop (LV_USE_SDL), EGL+GBM on embedded
#if LV_USE_SDL
#include <SDL.h>
#else
#include <EGL/egl.h>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>
#endif

// GLES2 function declarations and common headers (both paths)
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace helix {
namespace gcode {

namespace {

/// Matrix that turns a raw quantized int16 position back into millimetres:
/// `mm = q / scale_factor + min_bounds`. Composed into u_mvp / u_model_view so
/// the GPU can consume PackedVertex positions directly and the vertex shader
/// needs no knowledge of quantization. See PackedVertex in
/// include/gcode_geometry_builder.h.
glm::mat4 dequant_matrix(const QuantizationParams& q) {
    // A zero scale would produce a degenerate matrix and collapse the model to a
    // point; fall back to identity-scale rather than emit NaNs.
    const float inv = (q.scale_factor != 0.0f) ? (1.0f / q.scale_factor) : 1.0f;
    return glm::translate(glm::mat4(1.0f), q.min_bounds) *
           glm::scale(glm::mat4(1.0f), glm::vec3(inv));
}

} // namespace

// ============================================================
// RAII GL Handle Destructors
// ============================================================

GLBufferHandle::~GLBufferHandle() {
    if (id) {
        glDeleteBuffers(1, &id);
    }
}

GLFramebufferHandle::~GLFramebufferHandle() {
    if (id) {
        glDeleteFramebuffers(1, &id);
    }
}

GLRenderbufferHandle::~GLRenderbufferHandle() {
    if (id) {
        glDeleteRenderbuffers(1, &id);
    }
}

// ============================================================
// GL Error Checking
// ============================================================

/// Check for GL errors after significant GPU operations.
/// Returns true if no error, false on error (with spdlog output).
static inline bool check_gl_error(const char* operation) {
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        spdlog::error("[GCode GLES] GL error after {}: 0x{:04X}", operation, err);
        return false;
    }
    return true;
}

// ============================================================
// GL Context Save/Restore (RAII)
// ============================================================
// The LVGL display backend may have a GL context bound on this thread.
// We must save, bind ours, and restore on scope exit.

#if LV_USE_SDL

class SdlGlContextGuard {
  public:
    SdlGlContextGuard(void* our_window, void* our_context) {
        saved_context_ = SDL_GL_GetCurrentContext();
        saved_window_ = SDL_GL_GetCurrentWindow();

        int rc = SDL_GL_MakeCurrent(static_cast<SDL_Window*>(our_window),
                                    static_cast<SDL_GLContext>(our_context));
        if (rc != 0) {
            spdlog::error("[GCode GLES] SDL_GL_MakeCurrent failed: {}", SDL_GetError());
            // Restore previous context on failure
            if (saved_context_) {
                SDL_GL_MakeCurrent(saved_window_, saved_context_);
            }
        } else {
            ok_ = true;
            our_window_ = our_window;
        }
    }

    ~SdlGlContextGuard() {
        if (!ok_)
            return;
        // Restore previous context (LVGL's SDL renderer)
        if (saved_context_) {
            SDL_GL_MakeCurrent(saved_window_, saved_context_);
        } else {
            // No prior context — unbind ours
            SDL_GL_MakeCurrent(static_cast<SDL_Window*>(our_window_), nullptr);
        }
    }

    bool ok() const {
        return ok_;
    }

    SdlGlContextGuard(const SdlGlContextGuard&) = delete;
    SdlGlContextGuard& operator=(const SdlGlContextGuard&) = delete;

  private:
    SDL_GLContext saved_context_ = nullptr;
    SDL_Window* saved_window_ = nullptr;
    void* our_window_ = nullptr;
    bool ok_ = false;
};

#else // !LV_USE_SDL — EGL backend

class EglContextGuard {
  public:
    EglContextGuard(void* our_display, void* our_surface, void* our_context) {
        saved_display_ = eglGetCurrentDisplay();
        saved_context_ = eglGetCurrentContext();
        saved_draw_ = eglGetCurrentSurface(EGL_DRAW);
        saved_read_ = eglGetCurrentSurface(EGL_READ);

        // Release current context so we can bind ours
        if (saved_context_ != EGL_NO_CONTEXT) {
            eglMakeCurrent(saved_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }

        auto surface = our_surface ? static_cast<EGLSurface>(our_surface) : EGL_NO_SURFACE;
        ok_ = eglMakeCurrent(static_cast<EGLDisplay>(our_display), surface, surface,
                             static_cast<EGLContext>(our_context));
        if (!ok_) {
            spdlog::error("[GCode GLES] eglMakeCurrent failed: 0x{:X}", eglGetError());
            // Restore previous context on failure
            if (saved_context_ != EGL_NO_CONTEXT) {
                eglMakeCurrent(saved_display_, saved_draw_, saved_read_, saved_context_);
            }
        }
    }

    ~EglContextGuard() {
        if (!ok_)
            return;
        // Release our context
        auto display = eglGetCurrentDisplay();
        if (display != EGL_NO_DISPLAY) {
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        // Restore previous context (SDL's)
        if (saved_context_ != EGL_NO_CONTEXT) {
            eglMakeCurrent(saved_display_, saved_draw_, saved_read_, saved_context_);
        }
    }

    bool ok() const {
        return ok_;
    }

    EglContextGuard(const EglContextGuard&) = delete;
    EglContextGuard& operator=(const EglContextGuard&) = delete;

  private:
    EGLDisplay saved_display_ = EGL_NO_DISPLAY;
    EGLContext saved_context_ = EGL_NO_CONTEXT;
    EGLSurface saved_draw_ = EGL_NO_SURFACE;
    EGLSurface saved_read_ = EGL_NO_SURFACE;
    bool ok_ = false;
};

#endif // LV_USE_SDL

// ============================================================
// GLSL Shaders
// ============================================================

static const char* VERTEX_SHADER_DECLS = R"(
    // Per-pixel Phong shading with camera-following light.
    // Vertex format is packed (see PackedVertex):
    //   a_position : vec3 float
    //   a_color    : vec4 unorm8  (alpha unused)
    //   a_normal   : vec2 snorm8  (octahedral-encoded unit normal)
    uniform mat4 u_mvp;
    uniform mat4 u_model_view;
    uniform mat3 u_normal_matrix;
    uniform vec4 u_base_color;
    uniform float u_use_vertex_color;
    uniform float u_color_scale;

    attribute vec3 a_position;
    attribute vec4 a_color;
    attribute vec2 a_normal;

    varying vec3 v_normal;
    varying vec3 v_position;
    varying vec3 v_base_color;
)";

/// The octahedral normal decoder, kept as ONE piece of GLSL text and spliced into
/// every vertex shader that needs it (the lit pass and the selection shell). A
/// second copy could drift, and a shell extruded along a differently-decoded
/// normal would not wrap the surface it is meant to outline.
static const char* OCT_DECODE_GLSL = R"(
    // Decode an octahedral-encoded normal (Meyer et al., 2010).
    vec3 oct_decode(vec2 e) {
        vec3 n;
        n.x = e.x;
        n.y = e.y;
        n.z = 1.0 - abs(e.x) - abs(e.y);
        if (n.z < 0.0) {
            vec2 s = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
            n.xy = (1.0 - abs(n.yx)) * s;
        }
        return normalize(n);
    }
)";

static const char* VERTEX_SHADER_MAIN = R"(
    void main() {
        gl_Position = u_mvp * vec4(a_position, 1.0);
        vec3 normal = oct_decode(a_normal);
        // Stacked 0.2 mm layers are sub-pixel at preview sizes: a tube's bright top and dark
        // side alias into a moire, so walls shade as one continuous surface per direction.
        if (abs(normal.z) < 0.9) {
            normal = normalize(vec3(normal.x, normal.y, 0.0));
        }
        v_normal = normalize(u_normal_matrix * normal);
        v_position = (u_model_view * vec4(a_position, 1.0)).xyz;
        v_base_color = mix(u_base_color.rgb, a_color.rgb, u_use_vertex_color) * u_color_scale;
    }
)";

static const char* FRAGMENT_SHADER_SOURCE = R"(
    precision mediump float;
    varying vec3 v_normal;
    varying vec3 v_position;
    varying vec3 v_base_color;

    uniform vec3 u_light_dir[2];
    uniform vec3 u_light_color[2];
    uniform vec3 u_ambient;
    uniform float u_specular_intensity;
    uniform float u_specular_shininess;
    uniform float u_base_alpha;
    uniform float u_lift_strength;

    void main() {
        vec3 n = normalize(v_normal);
        vec3 view_dir = normalize(-v_position);

        // Diffuse from two lights
        vec3 diffuse = u_ambient;
        for (int i = 0; i < 2; i++) {
            float NdotL = max(dot(n, u_light_dir[i]), 0.0);
            diffuse += u_light_color[i] * NdotL;
        }

        // Blinn-Phong specular from both lights
        float spec = 0.0;
        for (int i = 0; i < 2; i++) {
            vec3 half_dir = normalize(u_light_dir[i] + view_dir);
            spec += pow(max(dot(n, half_dir), 0.0), u_specular_shininess);
        }

        // Headroom-proportional lift, the same idea as apply_shading() on the
        // 2D path and for the same reason. base * diffuse is a MULTIPLY: a black
        // filament has nothing to multiply, so all form collapses and the only
        // signal left is the specular term, which peaks around a quarter
        // intensity. The object reads as a flat silhouette.
        //
        // Adding light proportional to what the channel has left (1 - base)
        // gives a dark filament the range it lacks while a light one, having
        // almost no headroom, is left essentially as it was.
        float lit = clamp((diffuse.r + diffuse.g + diffuse.b) / 3.0, 0.0, 1.0);
        vec3 lift = (vec3(1.0) - v_base_color) * lit * u_lift_strength;

        vec3 color = v_base_color * diffuse + lift + vec3(spec * u_specular_intensity);
        gl_FragColor = vec4(color, u_base_alpha);
    }
)";

// ============================================================
// Lighting Constants
// ============================================================

// Fixed fill light direction (front-right)
static constexpr glm::vec3 LIGHT_FRONT_DIR{0.6985074f, 0.1397015f, 0.6985074f};

// ============================================================
// Construction / Destruction
// ============================================================

GCodeGLESRenderer::GCodeGLESRenderer() {
    // Selection cues from ui_xml/gcode_tokens.xml, resolved once here on the
    // main thread — same place and same reason as GCodeLayerRenderer's ctor.
    // The tokens are registered in the theme phase of startup, long before any
    // panel builds the viewer widget that owns this renderer, and the GL passes
    // consume the palette as uniforms rather than re-reading the registry.
    reset_colors();
    spdlog::debug("[GCode GLES] GCodeGLESRenderer created");
}

GCodeGLESRenderer::~GCodeGLESRenderer() {
    destroy_gl();

    // draw_buf_ is handed to the parallel render thread via dsc.src in
    // draw_cached_to_lvgl — same UAF pattern as GCodeLayerRenderer::cache_buf_
    // (#929).
    helix::safe_draw_buf_destroy(draw_buf_, "gles_draw_buf");

    spdlog::trace("[GCode GLES] GCodeGLESRenderer destroyed");
}

// ============================================================
// GL Initialization
// ============================================================

#if !LV_USE_SDL
// Try to set up EGL with a given display, returning true on success.
// On success, egl_display_, egl_context_, and optionally egl_surface_ are set.
bool GCodeGLESRenderer::try_egl_display(void* native_display, const char* label) {
    auto display = eglGetDisplay(static_cast<EGLNativeDisplayType>(native_display));
    if (!display || display == EGL_NO_DISPLAY) {
        spdlog::debug("[GCode GLES] {} — no display", label);
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(display, &major, &minor)) {
        spdlog::debug("[GCode GLES] {} — eglInitialize failed: 0x{:X}", label, eglGetError());
        return false;
    }
    spdlog::info("[GCode GLES] EGL {}.{} via {}", major, minor, label);

    eglBindAPI(EGL_OPENGL_ES_API);

    // Check surfaceless support
    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
    bool has_surfaceless =
        extensions && strstr(extensions, "EGL_KHR_surfaceless_context") != nullptr;

    // Choose config (try surfaceless first, then PBuffer)
    EGLConfig egl_config = nullptr;
    EGLint num_configs = 0;

    if (has_surfaceless) {
        EGLint attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE};
        eglChooseConfig(display, attribs, &egl_config, 1, &num_configs);
    }
    if (num_configs == 0) {
        EGLint attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE,
                            EGL_PBUFFER_BIT, EGL_NONE};
        eglChooseConfig(display, attribs, &egl_config, 1, &num_configs);
        has_surfaceless = false;
    }
    if (num_configs == 0) {
        spdlog::debug("[GCode GLES] {} — no suitable config", label);
        eglTerminate(display);
        return false;
    }

    // Create context
    EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    auto context = eglCreateContext(display, egl_config, EGL_NO_CONTEXT, ctx_attribs);
    if (context == EGL_NO_CONTEXT) {
        spdlog::debug("[GCode GLES] {} — context creation failed: 0x{:X}", label, eglGetError());
        eglTerminate(display);
        return false;
    }

    // Create PBuffer if needed
    EGLSurface surface = EGL_NO_SURFACE;
    if (!has_surfaceless) {
        EGLint pbuf_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        surface = eglCreatePbufferSurface(display, egl_config, pbuf_attribs);
        if (surface == EGL_NO_SURFACE) {
            spdlog::debug("[GCode GLES] {} — PBuffer creation failed: 0x{:X}", label,
                          eglGetError());
            eglDestroyContext(display, context);
            eglTerminate(display);
            return false;
        }
    }

    // Save the current EGL state (SDL may have a context bound on this thread)
    EGLDisplay saved_display = eglGetCurrentDisplay();
    EGLContext saved_context = eglGetCurrentContext();
    EGLSurface saved_draw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface saved_read = eglGetCurrentSurface(EGL_READ);
    bool had_previous_context = (saved_context != EGL_NO_CONTEXT);
    spdlog::debug("[GCode GLES] {} — prior EGL context: {} (display={})", label,
                  had_previous_context ? "yes" : "no", saved_display ? "valid" : "none");

    // Release the current context so we can bind ours
    if (had_previous_context) {
        eglMakeCurrent(saved_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    // Verify eglMakeCurrent actually works with our new context
    EGLSurface test_surface = (surface != EGL_NO_SURFACE) ? surface : EGL_NO_SURFACE;
    if (!eglMakeCurrent(display, test_surface, test_surface, context)) {
        spdlog::debug("[GCode GLES] {} — eglMakeCurrent failed: 0x{:X}", label, eglGetError());
        // Restore previous context
        if (had_previous_context)
            eglMakeCurrent(saved_display, saved_draw, saved_read, saved_context);
        if (surface != EGL_NO_SURFACE)
            eglDestroySurface(display, surface);
        eglDestroyContext(display, context);
        eglTerminate(display);
        return false;
    }

    // Release our context (compile_shaders will re-acquire it)
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    // Restore SDL's context
    if (had_previous_context) {
        eglMakeCurrent(saved_display, saved_draw, saved_read, saved_context);
    }

    // Success — store state
    egl_display_ = display;
    egl_context_ = context;
    egl_surface_ = (surface != EGL_NO_SURFACE) ? static_cast<void*>(surface) : nullptr;
    spdlog::info("[GCode GLES] Context ready via {} ({})", label,
                 has_surfaceless ? "surfaceless" : "PBuffer");
    return true;
}
#endif // !LV_USE_SDL

bool GCodeGLESRenderer::init_gl() {
    if (gl_initialized_)
        return true;
    if (gl_init_failed_)
        return false;

#if LV_USE_SDL
    // Desktop path: use SDL_GL_CreateContext with a hidden window.
    // This avoids SDL_Init(SDL_INIT_VIDEO) on Wayland+AMD poisoning EGL operations.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    auto* window =
        SDL_CreateWindow("helix-gles-offscreen", 0, 0, 1, 1, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) {
        spdlog::warn("[GCode GLES] SDL_CreateWindow failed: {}", SDL_GetError());
        gl_init_failed_ = true;
        return false;
    }

    auto gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        spdlog::warn("[GCode GLES] SDL_GL_CreateContext failed: {}", SDL_GetError());
        SDL_DestroyWindow(window);
        gl_init_failed_ = true;
        return false;
    }

    sdl_gl_window_ = window;
    sdl_gl_context_ = gl_ctx;

    spdlog::info("[GCode GLES] SDL GL context ready — GL_VERSION: {}, GL_RENDERER: {}",
                 reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                 reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    // Unbind our context (compile_shaders will re-acquire via guard)
    SDL_GL_MakeCurrent(window, nullptr);

#else  // !LV_USE_SDL — EGL backend
    // EGL initialization with fallback chain:
    // 1. GBM/DRM (Pi, embedded — surfaceless FBO rendering)
    // 2. Default EGL display (desktop Linux with X11/Wayland — PBuffer)
    bool egl_ok = false;

    // Path 1: Try GBM/DRM render nodes first (don't need DRM master, works alongside compositor)
    // Then try card nodes (needed on Pi where render nodes may not exist)
    static const char* DRM_DEVICES[] = {"/dev/dri/renderD128", "/dev/dri/renderD129",
                                        "/dev/dri/card1", "/dev/dri/card0", nullptr};
    for (int i = 0; DRM_DEVICES[i] && !egl_ok; ++i) {
        int fd = open(DRM_DEVICES[i], O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;

        auto* gbm = gbm_create_device(fd);
        if (!gbm) {
            close(fd);
            continue;
        }

        if (try_egl_display(gbm, DRM_DEVICES[i])) {
            drm_fd_ = fd;
            gbm_device_ = gbm;
            egl_ok = true;
        } else {
            gbm_device_destroy(gbm);
            close(fd);
        }
    }

    // Path 2: Default EGL display (Mesa on X11/Wayland)
    if (!egl_ok) {
        if (try_egl_display(EGL_DEFAULT_DISPLAY, "EGL_DEFAULT_DISPLAY")) {
            egl_ok = true;
        }
    }

    if (!egl_ok) {
        spdlog::warn("[GCode GLES] All EGL paths failed — GPU rendering unavailable");
        gl_init_failed_ = true;
        return false;
    }
#endif // LV_USE_SDL

    // Compile shaders (will acquire GL context internally via guard)
    if (!compile_shaders()) {
        gl_init_failed_ = true;
        destroy_gl();
        return false;
    }

    gl_initialized_ = true;
    return true;
}

static GLuint compile_shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    check_gl_error("glCompileShader");

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        spdlog::error("[GCode GLES] Shader compile error: {}", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool GCodeGLESRenderer::compile_shaders() {
#if LV_USE_SDL
    SdlGlContextGuard guard(sdl_gl_window_, sdl_gl_context_);
#else
    EglContextGuard guard(egl_display_, egl_surface_, egl_context_);
#endif
    if (!guard.ok())
        return false;

    const std::string vertex_src =
        std::string(VERTEX_SHADER_DECLS) + OCT_DECODE_GLSL + VERTEX_SHADER_MAIN;
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_src.c_str());
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FRAGMENT_SHADER_SOURCE);
    if (!vs || !fs) {
        if (vs)
            glDeleteShader(vs);
        if (fs)
            glDeleteShader(fs);
        return false;
    }

    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    check_gl_error("glLinkProgram");

    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
        spdlog::error("[GCode GLES] Program link error: {}", log);
        glDeleteProgram(program_);
        program_ = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!program_)
        return false;

    // Cache uniform/attribute locations
    u_mvp_ = glGetUniformLocation(program_, "u_mvp");
    u_normal_matrix_ = glGetUniformLocation(program_, "u_normal_matrix");
    u_light_dir_ = glGetUniformLocation(program_, "u_light_dir");
    u_light_color_ = glGetUniformLocation(program_, "u_light_color");
    u_ambient_ = glGetUniformLocation(program_, "u_ambient");
    u_base_color_ = glGetUniformLocation(program_, "u_base_color");
    u_specular_intensity_ = glGetUniformLocation(program_, "u_specular_intensity");
    u_specular_shininess_ = glGetUniformLocation(program_, "u_specular_shininess");
    u_model_view_ = glGetUniformLocation(program_, "u_model_view");
    u_base_alpha_ = glGetUniformLocation(program_, "u_base_alpha");
    u_lift_strength_ = glGetUniformLocation(program_, "u_lift_strength");
    a_position_ = glGetAttribLocation(program_, "a_position");
    a_normal_ = glGetAttribLocation(program_, "a_normal");
    a_color_ = glGetAttribLocation(program_, "a_color");
    u_use_vertex_color_ = glGetUniformLocation(program_, "u_use_vertex_color");
    u_color_scale_ = glGetUniformLocation(program_, "u_color_scale");

    if (a_position_ < 0 || a_normal_ < 0) {
        spdlog::error("[GCode GLES] Required attribute not found: a_position={}, a_normal={}",
                      a_position_, a_normal_);
        glDeleteProgram(program_);
        program_ = 0;
        return false;
    }

    spdlog::debug("[GCode GLES] Shaders compiled and linked (program={})", program_);
    return true;
}

bool GCodeGLESRenderer::create_fbo(int width, int height) {
    if (fbo_.id && fbo_width_ == width && fbo_height_ == height) {
        return true; // Already correct size
    }

    destroy_fbo();

    glGenFramebuffers(1, &fbo_.id);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_.id);
    if (!check_gl_error("glGenFramebuffers/glBindFramebuffer")) {
        destroy_fbo();
        return false;
    }

    // Color renderbuffer — use GL_RGBA8 (8 bits per channel) to match the
    // GL_RGBA/GL_UNSIGNED_BYTE format used by glReadPixels in blit_to_lvgl().
    // GL_RGBA4 would cause precision loss (4 bits stored, 8 bits read back).
    // GL_RGBA8 is available via OES_rgb8_rgba8 on GLES2 and natively on desktop GL.
    glGenRenderbuffers(1, &color_rbo_.id);
    glBindRenderbuffer(GL_RENDERBUFFER, color_rbo_.id);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, width, height);
    if (!check_gl_error("glRenderbufferStorage(color)")) {
        destroy_fbo();
        return false;
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color_rbo_.id);
    check_gl_error("glFramebufferRenderbuffer(color)");

    // Depth renderbuffer (16-bit)
    glGenRenderbuffers(1, &depth_rbo_.id);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rbo_.id);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);
    if (!check_gl_error("glRenderbufferStorage(depth)")) {
        destroy_fbo();
        return false;
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rbo_.id);
    check_gl_error("glFramebufferRenderbuffer(depth)");

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        spdlog::error("[GCode GLES] FBO incomplete: 0x{:X}", status);
        destroy_fbo();
        return false;
    }

    fbo_width_ = width;
    fbo_height_ = height;
    log_memory_report("fbo created");
    spdlog::debug("[GCode GLES] FBO created: {}x{}", width, height);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

void GCodeGLESRenderer::destroy_fbo() {
    // RAII handles call glDelete* in their destructors via move-assignment
    depth_rbo_ = GLRenderbufferHandle();
    color_rbo_ = GLRenderbufferHandle();
    fbo_ = GLFramebufferHandle();
    fbo_width_ = 0;
    fbo_height_ = 0;
}

void GCodeGLESRenderer::destroy_gl() {
    if (!gl_initialized_)
        return;

#if LV_USE_SDL
    // Make our context current for GL resource cleanup
    if (sdl_gl_window_ && sdl_gl_context_) {
        SDL_GLContext saved_ctx = SDL_GL_GetCurrentContext();
        SDL_Window* saved_win = SDL_GL_GetCurrentWindow();

        SDL_GL_MakeCurrent(static_cast<SDL_Window*>(sdl_gl_window_),
                           static_cast<SDL_GLContext>(sdl_gl_context_));

        free_vbos(layer_vbos_);
        destroy_fbo();

        if (program_) {
            glDeleteProgram(program_);
            program_ = 0;
        }
        if (line_program_) {
            glDeleteProgram(line_program_);
            line_program_ = 0;
        }
        if (line_vbo_) {
            glDeleteBuffers(1, &line_vbo_);
            line_vbo_ = 0;
        }
        if (shell_program_) {
            glDeleteProgram(shell_program_);
            shell_program_ = 0;
        }

        // Unbind before destroying
        SDL_GL_MakeCurrent(static_cast<SDL_Window*>(sdl_gl_window_), nullptr);

        SDL_GL_DeleteContext(static_cast<SDL_GLContext>(sdl_gl_context_));
        sdl_gl_context_ = nullptr;

        SDL_DestroyWindow(static_cast<SDL_Window*>(sdl_gl_window_));
        sdl_gl_window_ = nullptr;

        // Restore previous context
        if (saved_ctx) {
            SDL_GL_MakeCurrent(saved_win, saved_ctx);
        }
    }

#else  // !LV_USE_SDL — EGL backend
    // Save SDL's EGL state
    EGLDisplay saved_display = eglGetCurrentDisplay();
    EGLContext saved_context = eglGetCurrentContext();
    EGLSurface saved_draw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface saved_read = eglGetCurrentSurface(EGL_READ);

    // Make our context current for GL cleanup
    if (egl_display_ && egl_context_) {
        if (saved_context != EGL_NO_CONTEXT)
            eglMakeCurrent(saved_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglMakeCurrent(static_cast<EGLDisplay>(egl_display_), static_cast<EGLSurface>(egl_surface_),
                       static_cast<EGLSurface>(egl_surface_),
                       static_cast<EGLContext>(egl_context_));
    }

    free_vbos(layer_vbos_);
    destroy_fbo();

    if (program_) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    if (line_program_) {
        glDeleteProgram(line_program_);
        line_program_ = 0;
    }
    if (line_vbo_) {
        glDeleteBuffers(1, &line_vbo_);
        line_vbo_ = 0;
    }
    if (shell_program_) {
        glDeleteProgram(shell_program_);
        shell_program_ = 0;
    }

    if (egl_display_ && egl_context_) {
        eglMakeCurrent(static_cast<EGLDisplay>(egl_display_), EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        eglDestroyContext(static_cast<EGLDisplay>(egl_display_),
                          static_cast<EGLContext>(egl_context_));
        egl_context_ = nullptr;
    }

    if (egl_display_ && egl_surface_) {
        eglDestroySurface(static_cast<EGLDisplay>(egl_display_),
                          static_cast<EGLSurface>(egl_surface_));
        egl_surface_ = nullptr;
    }

    if (egl_display_) {
        eglTerminate(static_cast<EGLDisplay>(egl_display_));
        egl_display_ = nullptr;
    }

    // Restore SDL's EGL state
    if (saved_context != EGL_NO_CONTEXT) {
        eglMakeCurrent(saved_display, saved_draw, saved_read, saved_context);
    }

    if (gbm_device_) {
        gbm_device_destroy(static_cast<struct gbm_device*>(gbm_device_));
        gbm_device_ = nullptr;
    }

    if (drm_fd_ >= 0) {
        close(drm_fd_);
        drm_fd_ = -1;
    }
#endif // LV_USE_SDL

    gl_initialized_ = false;
    geometry_uploaded_ = false;
    upload_next_layer_ = 0;
    upload_total_layers_ = 0;
    spdlog::debug("[GCode GLES] GL resources destroyed");
}

// ============================================================
// Geometry Upload
// ============================================================

void GCodeGLESRenderer::upload_geometry(const RibbonGeometry& geom, std::vector<LayerVBO>& vbos) {
    // Lock palette during read to prevent data races with set_tool_color_overrides
    std::lock_guard<std::mutex> lock(palette_mutex_);

    free_vbos(vbos);

    if (geom.strips.empty() || geom.vertices.empty()) {
        return;
    }

    // Determine number of layers
    size_t num_layers = geom.layer_strip_ranges.empty() ? 1 : geom.layer_strip_ranges.size();

    vbos.resize(num_layers);

    constexpr size_t VERTEX_STRIDE = PackedVertex::stride();

    // Reuse upload buffer across layers (sized to largest layer)
    std::vector<uint8_t> buf;

    for (size_t layer = 0; layer < num_layers; ++layer) {
        size_t first_strip = 0;
        size_t strip_count = geom.strips.size();

        if (!geom.layer_strip_ranges.empty()) {
            auto [fs, sc] = geom.layer_strip_ranges[layer];
            first_strip = fs;
            strip_count = sc;
        }

        if (strip_count == 0) {
            vbos[layer].vbo = GLBufferHandle();
            vbos[layer].vertex_count = 0;
            continue;
        }

        // Use pre-computed buffers if available (prepared on background thread)
        if (!geom.prepared_buffers.empty() && layer < geom.prepared_buffers.size() &&
            geom.prepared_buffers[layer].vertex_count > 0) {
            const auto& prepared = geom.prepared_buffers[layer];
            GLBufferHandle vbo_handle;
            glGenBuffers(1, &vbo_handle.id);
            glBindBuffer(GL_ARRAY_BUFFER, vbo_handle.id);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(prepared.vertex_count * VERTEX_STRIDE),
                         prepared.data.data(), GL_STATIC_DRAW);
            bool buf_ok = check_gl_error("glBufferData (prepared)");
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            if (!buf_ok) {
                spdlog::error("[GCode GLES] VBO creation failed for layer {} (prepared)", layer);
                vbos[layer].vbo = GLBufferHandle();
                vbos[layer].vertex_count = 0;
                continue;
            }

            vbos[layer].vbo = std::move(vbo_handle);
            vbos[layer].vertex_count = prepared.vertex_count;
            continue;
        }

        // Each strip = 4 vertices → 2 triangles → 6 vertices (for GL_TRIANGLES)
        size_t total_verts = strip_count * 6;
        size_t buf_bytes = total_verts * VERTEX_STRIDE;
        if (buf.size() < buf_bytes) {
            buf.resize(buf_bytes);
        }

        geom.expand_strips(first_strip, strip_count, reinterpret_cast<PackedVertex*>(buf.data()));

        GLBufferHandle vbo_handle;
        glGenBuffers(1, &vbo_handle.id);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_handle.id);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(total_verts * VERTEX_STRIDE),
                     buf.data(), GL_STATIC_DRAW);
        bool buf_ok = check_gl_error("glBufferData");
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        if (!buf_ok) {
            spdlog::error("[GCode GLES] VBO creation failed for layer {}", layer);
            vbos[layer].vbo = GLBufferHandle();
            vbos[layer].vertex_count = 0;
            continue;
        }

        vbos[layer].vbo = std::move(vbo_handle);
        vbos[layer].vertex_count = total_verts;
    }

    spdlog::debug("[GCode GLES] Uploaded {} layers, {} total strips to VBOs", num_layers,
                  geom.strips.size());
}

bool GCodeGLESRenderer::upload_geometry_chunk(const RibbonGeometry& geom,
                                              std::vector<LayerVBO>& vbos, size_t& next_layer,
                                              size_t total_layers) {
    // Time budget: 8ms per frame for uploads
    constexpr auto TIME_BUDGET = std::chrono::milliseconds(8);
    auto start = std::chrono::steady_clock::now();

    // Lock palette during read to prevent data races with set_tool_color_overrides
    std::lock_guard<std::mutex> lock(palette_mutex_);

    constexpr size_t VERTEX_STRIDE = PackedVertex::stride();
    // Reuse CPU buffer for layers that don't have prepared data
    std::vector<uint8_t> buf;

    while (next_layer < total_layers) {
        size_t layer = next_layer;

        size_t first_strip = 0;
        size_t strip_count = geom.strips.size();

        if (!geom.layer_strip_ranges.empty()) {
            auto [fs, sc] = geom.layer_strip_ranges[layer];
            first_strip = fs;
            strip_count = sc;
        }

        if (strip_count == 0) {
            vbos[layer].vbo = GLBufferHandle();
            vbos[layer].vertex_count = 0;
            ++next_layer;
            continue;
        }

        // Use pre-computed buffers if available (prepared on background thread)
        if (!geom.prepared_buffers.empty() && layer < geom.prepared_buffers.size() &&
            geom.prepared_buffers[layer].vertex_count > 0) {
            const auto& prepared = geom.prepared_buffers[layer];
            GLBufferHandle vbo_handle;
            glGenBuffers(1, &vbo_handle.id);
            glBindBuffer(GL_ARRAY_BUFFER, vbo_handle.id);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(prepared.vertex_count * VERTEX_STRIDE),
                         prepared.data.data(), GL_STATIC_DRAW);
            bool buf_ok = check_gl_error("glBufferData (prepared)");
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            if (!buf_ok) {
                spdlog::error("[GCode GLES] VBO creation failed for layer {} (prepared)", layer);
                vbos[layer].vbo = GLBufferHandle();
                vbos[layer].vertex_count = 0;
            } else {
                vbos[layer].vbo = std::move(vbo_handle);
                vbos[layer].vertex_count = prepared.vertex_count;
            }
        } else {
            // CPU fallback: expand strips inline (for color re-upload case)
            size_t total_verts = strip_count * 6;
            size_t buf_bytes = total_verts * VERTEX_STRIDE;
            if (buf.size() < buf_bytes) {
                buf.resize(buf_bytes);
            }

            geom.expand_strips(first_strip, strip_count,
                               reinterpret_cast<PackedVertex*>(buf.data()));

            GLBufferHandle vbo_handle;
            glGenBuffers(1, &vbo_handle.id);
            glBindBuffer(GL_ARRAY_BUFFER, vbo_handle.id);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(total_verts * VERTEX_STRIDE),
                         buf.data(), GL_STATIC_DRAW);
            bool buf_ok = check_gl_error("glBufferData");
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            if (!buf_ok) {
                spdlog::error("[GCode GLES] VBO creation failed for layer {}", layer);
                vbos[layer].vbo = GLBufferHandle();
                vbos[layer].vertex_count = 0;
            } else {
                vbos[layer].vbo = std::move(vbo_handle);
                vbos[layer].vertex_count = total_verts;
            }
        }

        ++next_layer;

        // Check time budget (check every layer, glBufferData can be slow)
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed >= TIME_BUDGET) {
            break;
        }
    }

    bool done = (next_layer >= total_layers);
    if (done) {
        spdlog::debug("[GCode GLES] Incremental upload complete: all {} layers uploaded",
                      total_layers);
    }
    return done;
}

void GCodeGLESRenderer::free_vbos(std::vector<LayerVBO>& vbos) {
    // RAII handles (GLBufferHandle) call glDeleteBuffers in their destructors
    vbos.clear();
}

// ============================================================
// Main Render Entry Point
// ============================================================

void GCodeGLESRenderer::render(lv_layer_t* layer, const ParsedGCodeFile& gcode,
                               const GCodeCamera& camera, const lv_area_t* widget_coords) {
    // Initialize GL on first render
    if (!gl_initialized_) {
        if (!init_gl()) {
            return; // GPU not available
        }
    }

    // Sticky bail: once a fatal GL error or a denylisted GPU has disabled the
    // GPU path this session, never issue another draw. The viewer polls
    // render_failed() and falls back to the pure-CPU 2D renderer.
    if (gl_render_failed_)
        return;

    // No geometry loaded
    if (!geometry_)
        return;

        // Acquire our GL context (saves and restores LVGL's)
#if LV_USE_SDL
    SdlGlContextGuard guard(sdl_gl_window_, sdl_gl_context_);
#else
    EglContextGuard guard(egl_display_, egl_surface_, egl_context_);
#endif
    if (!guard.ok())
        return;

    // Layer 1 — proactive GL_RENDERER denylist (issues #966 / #1084 / #1085).
    // Evaluated exactly once, now that the GL context is current so GL_RENDERER
    // is valid. Known-bad Mali/Panfrost GPUs SIGSEGV inside glDrawArrays, which
    // the reactive glGetError() guard cannot catch — bail before the first draw.
    if (!gpu_checked_) {
        gpu_checked_ = true;
        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        spdlog::info("[GCode GLES] GL_RENDERER: {}", renderer ? renderer : "(null)");
        if (gl_renderer_is_denylisted(renderer)) {
            spdlog::error("[GCode GLES] GPU '{}' is denylisted (known to fault inside the driver) "
                          "— disabling GPU rendering, falling back to 2D",
                          renderer ? renderer : "(null)");
            gl_render_failed_ = true;
            cancel_job();
            return;
        }
    }

    // Incremental VBO upload: upload a time-budgeted batch of layers per frame
    if (!geometry_uploaded_ && geometry_) {
        // Initialize incremental upload on first frame
        if (upload_total_layers_ == 0) {
            free_vbos(layer_vbos_); // Free old VBOs inside GL context
            size_t num_layers =
                geometry_->layer_strip_ranges.empty() ? 1 : geometry_->layer_strip_ranges.size();
            layer_vbos_.resize(num_layers);
            upload_total_layers_ = num_layers;
            spdlog::info("[GCode GLES] Starting incremental VBO upload: {} layers", num_layers);
        }

        bool done = upload_geometry_chunk(*geometry_, layer_vbos_, upload_next_layer_,
                                          upload_total_layers_);
        if (done) {
            geometry_uploaded_ = true;
            upload_next_layer_ = 0;
            upload_total_layers_ = 0;
            uploaded_triangles_ = 0;
            for (const auto& vbo : layer_vbos_) {
                uploaded_triangles_ += vbo.vertex_count / 3;
            }
            // Free pre-computed interleaved buffers — all data is now in GPU VBOs.
            // The CPU fallback path (for tool color re-upload) re-expands from
            // the compact vertices/strips directly, so these aren't needed.
            size_t freed = 0;
            for (auto& pb : geometry_->prepared_buffers) {
                freed += pb.data.capacity(); // data is std::vector<uint8_t> — capacity is bytes
                pb.data.clear();
                pb.data.shrink_to_fit();
            }
            geometry_->prepared_buffers.clear();
            geometry_->prepared_buffers.shrink_to_fit();
            if (freed > 0) {
                spdlog::info("[GCode GLES] Freed {} MB of upload buffers after VBO upload",
                             freed / (1024 * 1024));
            }
            // Defer first GPU render by a few frames to avoid blocking panel animations
            render_defer_frames_ = 3;
        } else {
            // Still uploading -- show cached buffer if available, otherwise skip frame
            if (draw_buf_) {
                draw_cached_to_lvgl(layer, widget_coords);
            }
            return;
        }
    }

    // If deferring, draw the cached buffer and count down.
    // If no cached buffer exists, skip the defer entirely — nothing to show.
    if (render_defer_frames_ > 0) {
        if (draw_buf_) {
            render_defer_frames_--;
            draw_cached_to_lvgl(layer, widget_coords);
            return;
        }
        render_defer_frames_ = 0;
    }
    // Finger down: draw the whole frame now at whatever detail the measured GPU
    // rate affords, instead of handing out slices. cancel_job() inside
    // render_moving clears have_complete_image_, so the release restarts a
    // still job even for a tap that never moved the camera.
    if (interaction_mode_) {
        arm_gpu_guard();
        render_moving(layer, gcode, camera, widget_coords);
        clear_gpu_guard();
        return;
    }

    // Build current render state for frame-skip check
    const CachedRenderState current_state = snapshot_state(camera);
    render_schedule::JobInputs in;
    in.scene_changed = frame_dirty_ || !current_state.same_scene(job_scene_);
    in.have_complete_image = have_complete_image_ && draw_buf_;
    in.job_running = job_.active;
    in.job_incremental = job_.incremental;
    in.selection_active = selection_.any_highlighted();
    in.job_progress = job_.progress_layer;
    in.new_progress = progress_layer_;
    switch (render_schedule::decide_job(in)) {
    case render_schedule::JobAction::Keep:
        break;
    case render_schedule::JobAction::Restart:
        start_job(false, current_state);
        break;
    case render_schedule::JobAction::Incremental:
        start_job(true, current_state);
        break;
    case render_schedule::JobAction::Extend:
        job_.solid_end = std::min(progress_layer_, job_.solid_end_limit);
        job_.progress_layer = progress_layer_;
        job_.phase = JobPhase::Solid;
        break;
    }
    frame_dirty_ = false;

    if (!job_.active) {
        // draw_cached_to_lvgl skips glReadPixels: it just blits the existing draw_buf_.
        draw_cached_to_lvgl(layer, widget_coords);
        return;
    }

    // Layer 2 — crash-loop breaker. Arm the guard file immediately before the
    // real GPU draw; if the driver hard-faults inside run_slice the process
    // dies with the file still present and the next startup promotes it to a
    // persistent block. Cleared right after the first successful blit.
    arm_gpu_guard();

    const bool done = run_slice(gcode, camera);
    if (done) {
        // Read pixels from FBO and blit to LVGL
        blit_to_lvgl(layer, widget_coords);
        have_complete_image_ = true;
    } else {
        // Mid-refinement: keep showing the last finished image rather than a
        // partially drawn one.
        draw_cached_to_lvgl(layer, widget_coords);
    }
    clear_gpu_guard();

    // guard destructor restores LVGL's GL context
}

// ============================================================
// GPU crash-loop breaker (Layer 2, issues #966 / #1084 / #1085)
// ============================================================

void GCodeGLESRenderer::arm_gpu_guard() {
    if (gpu_guard_armed_)
        return;
    gpu_guard_armed_ = true;

    const std::string path = helix::writable_path("gpu_3d_guard");
    std::ofstream guard(path, std::ios::out | std::ios::trunc);
    if (guard.is_open()) {
        guard << "1";
        spdlog::debug("[GCode GLES] Armed GPU crash-loop guard: {}", path);
    } else {
        spdlog::warn("[GCode GLES] Could not write GPU crash-loop guard: {}", path);
    }
}

void GCodeGLESRenderer::clear_gpu_guard() {
    if (!gpu_guard_armed_ || gpu_guard_cleared_)
        return;
    gpu_guard_cleared_ = true;

    const std::string path = helix::writable_path("gpu_3d_guard");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
        spdlog::warn("[GCode GLES] Could not remove GPU crash-loop guard {}: {}", path,
                     ec.message());
    } else {
        spdlog::debug("[GCode GLES] Cleared GPU crash-loop guard after first successful frame");
    }
}

// ============================================================
// FBO Rendering
// ============================================================

/// Layer ranges for the solid and ghost passes. ghosting is true when
/// 0 <= progress_layer_ < max_layer.
void GCodeGLESRenderer::pass_ranges(int& draw_start, int& draw_end, int& solid_end,
                                    int& ghost_start, bool& ghosting) const {
    const int max_layer = static_cast<int>(layer_vbos_.size()) - 1;
    draw_start = (layer_start_ >= 0) ? layer_start_ : 0;
    draw_end = (layer_end_ >= 0) ? std::min(layer_end_, max_layer) : max_layer;
    ghosting = progress_layer_ >= 0 && progress_layer_ < max_layer;
    solid_end = ghosting ? std::min(progress_layer_, draw_end) : draw_end;
    ghost_start = ghosting ? std::max(progress_layer_ + 1, draw_start) : draw_end + 1;
}

bool GCodeGLESRenderer::setup_frame(const GCodeCamera& camera, float scale, bool clear,
                                    glm::mat4& mvp, glm::mat4& mvp_dequant) {
    int render_w = std::max(1, static_cast<int>(viewport_width_ * scale));
    int render_h = std::max(1, static_cast<int>(viewport_height_ * scale));

    // Create/resize FBO
    if (!create_fbo(render_w, render_h)) {
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_.id);
    glViewport(0, 0, render_w, render_h);

    if (clear) {
        // Neutral gray background - light and dark filaments both contrast well
        glClearColor(BACKGROUND_GRAY, BACKGROUND_GRAY, BACKGROUND_GRAY_BLUE, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    glEnable(GL_DEPTH_TEST);

    // Alpha is not part of the picture: readback converts RGBA to RGB and throws
    // the alpha byte away. So it is reserved as the selection tag channel, and
    // masking it off here is what guarantees the tag means only one thing. Ghost
    // layers blend with GL_ONE_MINUS_SRC_ALPHA, which would otherwise leave
    // arbitrary alpha behind and put stray rim pixels on unselected geometry.
    // Everything stays at the cleared 255 until render_selection_tag() unmasks.
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);

    // Select active geometry
    active_geometry_ = geometry_.get();

    if (!active_geometry_ || layer_vbos_.empty()) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }

    // Use shader program
    glUseProgram(program_);

    mvp = build_mvp(camera);

    // Normal matrix (inverse transpose of upper-left 3x3 of model-view).
    glm::mat4 model = glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(0, 0, 1));
    glm::mat4 view = camera.get_view_matrix();
    glm::mat3 normal_mat = glm::transpose(glm::inverse(glm::mat3(view * model)));

    // Vertex positions arrive as raw quantized int16. Dequantization is affine
    // (mm = q / scale_factor + min_bounds), so it composes into the matrices we
    // already upload instead of costing a uniform and a shader edit. Applied on
    // the right so it runs first, before model/view/projection.
    //
    // Normals are unaffected: they are a separate octahedral attribute, and this
    // transform is a uniform positive scale plus a translation, which cannot
    // skew a normal or flip winding.
    const glm::mat4 dequant = dequant_matrix(active_geometry_->quantization);

    // Set uniforms
    mvp_dequant = mvp * dequant;
    glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, glm::value_ptr(mvp_dequant));
    glUniformMatrix3fv(u_normal_matrix_, 1, GL_FALSE, glm::value_ptr(normal_mat));

    glm::mat4 model_view = view * model * dequant;
    glUniformMatrix4fv(u_model_view_, 1, GL_FALSE, glm::value_ptr(model_view));

    // Light 0: Camera-following directional light (tracks camera position)
    glm::vec3 cam_pos = camera.get_camera_position();
    glm::vec3 cam_target = camera.get_target();
    glm::vec3 cam_light_world = glm::normalize(cam_pos - cam_target);

    // Light 1: Fixed fill light from front-right (prevents black shadows)
    // Both transformed to view space (normals are in view space via u_normal_matrix)
    glm::mat3 view_model_rot = glm::mat3(view * model);
    glm::vec3 light_dirs[2] = {glm::normalize(view_model_rot * cam_light_world),
                               glm::normalize(view_model_rot * LIGHT_FRONT_DIR)};
    glm::vec3 light_colors[2] = {glm::vec3(CAMERA_LIGHT_INTENSITY), // Camera light: primary
                                 glm::vec3(FILL_LIGHT_INTENSITY)};  // Fill light: subtle
    glUniform3fv(u_light_dir_, 2, glm::value_ptr(light_dirs[0]));
    glUniform3fv(u_light_color_, 2, glm::value_ptr(light_colors[0]));

    glm::vec3 ambient{AMBIENT_INTENSITY};
    glUniform3fv(u_ambient_, 1, glm::value_ptr(ambient));

    // Material
    glUniform1f(u_specular_intensity_, specular_intensity_);
    glUniform1f(u_specular_shininess_, specular_shininess_);
    // Matches depth_shading::LIFT_STRENGTH on the 2D path, so the same model in
    // the same filament reads the same way in either renderer.
    glUniform1f(u_lift_strength_, helix::gcode::depth_shading::LIFT_STRENGTH);

    // Per-vertex color mode: use vertex colors when geometry has a color palette.
    // With per-tool AMS overrides, the palette is updated in-place so vertex colors
    // always reflect the correct AMS slot colors. Only fall back to uniform color
    // when palette has a single-tool override (legacy path).
    bool has_palette = active_geometry_ && !active_geometry_->color_palette.empty();
    bool has_vertex_colors = has_palette && !palette_.has_override;
    glUniform1f(u_use_vertex_color_, has_vertex_colors ? 1.0f : 0.0f);
    return true;
}

void GCodeGLESRenderer::render_moving(lv_layer_t* layer, const ParsedGCodeFile& gcode,
                                      const GCodeCamera& camera, const lv_area_t* widget_coords) {
    cancel_job();
    const render_schedule::MovingPlan plan =
        render_schedule::plan_moving(uploaded_triangles_, gpu_rate_tris_per_ms_);
    glm::mat4 mvp, mvp_dequant;
    if (!setup_frame(camera, plan.half_resolution ? 0.5f : 1.0f, true, mvp, mvp_dequant)) {
        return;
    }

    int draw_start, draw_end, solid_end, ghost_start;
    bool ghosting;
    pass_ranges(draw_start, draw_end, solid_end, ghost_start, ghosting);

    const size_t before = triangles_rendered_;
    const auto t0 = std::chrono::steady_clock::now();

    if (ghosting) {
        if (draw_start <= solid_end) {
            draw_layers(layer_vbos_, draw_start, solid_end, 1.0f, 1.0f, plan.stride);
        }
        if (ghost_start <= draw_end) {
            constexpr float GHOST_LIGHTEN_SCALE = 4.0f;
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            draw_layers(layer_vbos_, ghost_start, draw_end, GHOST_LIGHTEN_SCALE,
                        ghost_opacity_ / 255.0f, plan.stride);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }
    } else {
        draw_layers(layer_vbos_, draw_start, draw_end, 1.0f, 1.0f, plan.stride);
    }

    glUseProgram(0);
    render_brackets_3d(gcode, mvp);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glFinish();
    const float ms =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    gpu_rate_tris_per_ms_ =
        render_schedule::update_rate(gpu_rate_tris_per_ms_, triangles_rendered_ - before, ms);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    blit_to_lvgl(layer, widget_coords);
    spdlog::trace("[GCode GLES] Moving frame: stride {}, {} res, {:.1f}ms", plan.stride,
                  plan.half_resolution ? "half" : "full", ms);
}

int GCodeGLESRenderer::draw_layers(const std::vector<LayerVBO>& vbos, int layer_start,
                                   int layer_end, float color_scale, float alpha, int stride,
                                   size_t max_triangles) {
    // Set uniforms for this draw batch
    glUniform4fv(u_base_color_, 1, glm::value_ptr(filament_color_));
    glUniform1f(u_color_scale_, color_scale);
    glUniform1f(u_base_alpha_, alpha);

    constexpr size_t STRIDE = PackedVertex::stride();

    // Enable vertex attributes once before the loop (a_position_ and a_normal_
    // are validated >= 0 during compile_shaders)
    glEnableVertexAttribArray(static_cast<GLuint>(a_position_));
    glEnableVertexAttribArray(static_cast<GLuint>(a_normal_));
    if (a_color_ >= 0) {
        glEnableVertexAttribArray(static_cast<GLuint>(a_color_));
    }

    size_t submitted = 0;
    int layer = layer_start;
    for (; layer <= layer_end; layer += stride) {
        if (layer < 0 || layer >= static_cast<int>(vbos.size()))
            continue;
        const auto& lv = vbos[static_cast<size_t>(layer)];
        if (!lv.vbo || lv.vertex_count == 0)
            continue;

        glBindBuffer(GL_ARRAY_BUFFER, lv.vbo);

        // Position: quantized int16, NOT normalized — the raw integer reaches the
        // shader and u_mvp / u_model_view carry the dequantization.
        glVertexAttribPointer(static_cast<GLuint>(a_position_), 3, GL_SHORT, GL_FALSE,
                              static_cast<GLsizei>(STRIDE),
                              reinterpret_cast<void*>(PackedVertex::position_offset()));

        // Normal: int8[2] octahedral, decoded in the vertex shader.
        glVertexAttribPointer(static_cast<GLuint>(a_normal_), 2, GL_BYTE, GL_TRUE,
                              static_cast<GLsizei>(STRIDE),
                              reinterpret_cast<void*>(PackedVertex::normal_offset()));

        if (a_color_ >= 0) {
            // Color: RGBA8 unorm; alpha byte is present but ignored by the shader.
            glVertexAttribPointer(static_cast<GLuint>(a_color_), 4, GL_UNSIGNED_BYTE, GL_TRUE,
                                  static_cast<GLsizei>(STRIDE),
                                  reinterpret_cast<void*>(PackedVertex::color_offset()));
        }

        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(lv.vertex_count));
        triangles_rendered_ += lv.vertex_count / 3;
        submitted += lv.vertex_count / 3;
        if (submitted >= max_triangles) {
            layer += stride;
            break;
        }
    }

    glDisableVertexAttribArray(static_cast<GLuint>(a_position_));
    glDisableVertexAttribArray(static_cast<GLuint>(a_normal_));
    if (a_color_ >= 0) {
        glDisableVertexAttribArray(static_cast<GLuint>(a_color_));
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // One glGetError() per draw batch — NOT per primitive (a per-glDrawArrays
    // check would stall the GPU pipeline). On constrained Mali/Panfrost GPUs
    // (e.g. Allwinner CB1) the driver can fault under memory pressure with the
    // phong shader + ribbon VBOs; a fatal error here means we must abandon GPU
    // rendering. The viewer polls render_failed() after render() and falls back
    // to the pure-CPU 2D renderer for the rest of the session.
    GLenum draw_err = glGetError();
    if (gl_draw_error_is_fatal(draw_err)) {
        spdlog::error("[GCode GLES] Fatal GL error after glDrawArrays: 0x{:04X} — disabling GPU "
                      "rendering, falling back to 2D",
                      draw_err);
        gl_render_failed_ = true;
        cancel_job();
    }
    return layer;
}

// ============================================================
// Time-sliced refinement
// ============================================================

void GCodeGLESRenderer::start_job(bool incremental, const CachedRenderState& scene) {
    const int previous_progress = job_.progress_layer;
    int draw_start, draw_end, solid_end, ghost_start;
    bool ghosting;
    pass_ranges(draw_start, draw_end, solid_end, ghost_start, ghosting);
    job_ = RefineJob{};
    job_.active = true;
    job_.incremental = incremental;
    job_.phase = JobPhase::Solid;
    job_.solid_end_limit = draw_end;
    job_.progress_layer = progress_layer_;
    job_.started = std::chrono::steady_clock::now();
    if (incremental) {
        job_.first_slice = false;
        job_.solid_start = std::max(previous_progress + 1, draw_start);
        job_.solid_end = solid_end;
        job_.ghost_start = 0;
        job_.ghost_end = -1;
    } else {
        job_.solid_start = draw_start;
        job_.solid_end = ghosting ? solid_end : draw_end;
        job_.ghost_start = ghost_start;
        job_.ghost_end = ghosting ? draw_end : -1;
        have_complete_image_ = false;
    }
    job_.next_layer = job_.solid_start;
    job_scene_ = scene;
}

bool GCodeGLESRenderer::run_slice(const ParsedGCodeFile& gcode, const GCodeCamera& camera) {
    glm::mat4 mvp, mvp_dequant;
    if (!setup_frame(camera, kStillSupersample, job_.first_slice, mvp, mvp_dequant)) {
        cancel_job();
        return false;
    }
    job_.first_slice = false;
    const size_t quota = render_schedule::slice_quota(gpu_rate_tris_per_ms_);
    const size_t before = triangles_rendered_;
    const auto t0 = std::chrono::steady_clock::now();
    while (job_.phase != JobPhase::Done && triangles_rendered_ - before < quota) {
        const size_t left = quota - (triangles_rendered_ - before);
        if (job_.phase == JobPhase::Solid) {
            if (job_.next_layer > job_.solid_end) {
                job_.phase =
                    job_.ghost_start <= job_.ghost_end ? JobPhase::Ghost : JobPhase::Overlays;
                job_.next_layer = job_.ghost_start;
                continue;
            }
            job_.next_layer =
                draw_layers(layer_vbos_, job_.next_layer, job_.solid_end, 1.0f, 1.0f, 1, left);
        } else if (job_.phase == JobPhase::Ghost) {
            if (job_.next_layer > job_.ghost_end) {
                job_.phase = JobPhase::Overlays;
                continue;
            }
            constexpr float GHOST_LIGHTEN_SCALE = 4.0f;
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            job_.next_layer = draw_layers(layer_vbos_, job_.next_layer, job_.ghost_end,
                                          GHOST_LIGHTEN_SCALE, ghost_opacity_ / 255.0f, 1, left);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        } else { // Overlays: the selection tag and brackets a moving frame skips
            glUseProgram(0);
            // The tag covers exactly the solid layers (solid_end is the
            // progress layer while ghosting, the last drawn layer otherwise).
            // An incremental job drew onto a finished image that already
            // carries its tag.
            if (!job_.incremental) {
                render_selection_tag(gcode, mvp_dequant, job_.solid_start, job_.solid_end);
            }
            render_brackets_3d(gcode, mvp);
            job_.phase = JobPhase::Done;
        }
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glFinish();
    const float ms =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    gpu_rate_tris_per_ms_ =
        render_schedule::update_rate(gpu_rate_tris_per_ms_, triangles_rendered_ - before, ms);
    job_.slices++;
    job_.max_slice_ms = std::max(job_.max_slice_ms, ms);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (job_.phase != JobPhase::Done) {
        return false;
    }
    job_.active = false;
    spdlog::debug(
        "[GCode GLES] Refine done: {} slices, max slice {:.1f}ms, {:.0f}ms wall, "
        "rate {:.0f} tris/ms{}",
        job_.slices, job_.max_slice_ms,
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - job_.started)
            .count(),
        gpu_rate_tris_per_ms_, job_.incremental ? " (incremental)" : "");
    return true;
}

void GCodeGLESRenderer::cancel_job() {
    job_ = RefineJob{};
    have_complete_image_ = false;
}

// ============================================================
// LVGL Output
// ============================================================

void GCodeGLESRenderer::draw_cached_to_lvgl(lv_layer_t* layer, const lv_area_t* widget_coords) {
    // Fast path: draw the existing draw_buf_ without GPU readback.
    // Used when the frame hasn't changed (frame-skip) or during render deferral.
    if (!draw_buf_ || !draw_buf_->data)
        return;

    lv_draw_image_dsc_t img_dsc;
    lv_draw_image_dsc_init(&img_dsc);
    img_dsc.src = draw_buf_;

    lv_area_t area = *widget_coords;
    lv_draw_image(layer, &img_dsc, &area);
}

void GCodeGLESRenderer::blit_to_lvgl(lv_layer_t* layer, const lv_area_t* widget_coords) {
    int widget_w = lv_area_get_width(widget_coords);
    int widget_h = lv_area_get_height(widget_coords);

    // Create or recreate draw buffer at widget size
    if (!draw_buf_ || draw_buf_width_ != widget_w || draw_buf_height_ != widget_h) {
        // May still be in flight to the parallel render thread (#929 cluster).
        helix::safe_draw_buf_destroy(draw_buf_, "gles_draw_buf");
        draw_buf_ = lv_draw_buf_create(static_cast<uint32_t>(widget_w),
                                       static_cast<uint32_t>(widget_h), LV_COLOR_FORMAT_RGB888, 0);
        if (!draw_buf_) {
            spdlog::error("[GCode GLES] Failed to create draw buffer");
            return;
        }
        draw_buf_width_ = widget_w;
        draw_buf_height_ = widget_h;
    }

    if (!fbo_.id)
        return;

    // Read pixels from FBO
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_.id);

    // Read RGBA from GPU (matches GL_RGBA8_OES renderbuffer format)
    // Reuse persistent readback buffer to avoid per-frame allocation
    size_t readback_size = static_cast<size_t>(fbo_width_ * fbo_height_ * 4);
    if (readback_buf_.size() != readback_size) {
        readback_buf_.resize(readback_size);
    }
    glReadPixels(0, 0, fbo_width_, fbo_height_, GL_RGBA, GL_UNSIGNED_BYTE, readback_buf_.data());
    check_gl_error("glReadPixels");

    // The white silhouette, derived from the tag render_selection_tag() left in
    // the alpha byte. Byte 3 is alpha in the readback's RGBA and in the
    // rasterizer's ARGB8888 alike, so the tag scan reads the same offset either
    // way — but the rim COLOUR is an XML token and need not be grey, so the
    // readback's channel order has to be named. glReadPixels above asked for
    // GL_RGBA, which puts red at byte 0 where an ARGB8888 buffer puts blue.
    if (selection_.any_highlighted()) {
        // The rim is sized for the widget the frame is displayed at, then
        // scaled into readback pixels: a supersampled still strokes its rim at
        // 2x so the box filter lands it back at the same on-screen width a
        // widget-sized frame would have shown.
        const float readback_scale =
            static_cast<float>(fbo_width_) / static_cast<float>(std::max(1, widget_w));
        const int rim =
            static_cast<int>(std::lround(selection::outline_width_px(widget_w) * readback_scale));
        const RasterTarget rt{readback_buf_.data(), static_cast<size_t>(fbo_width_) * 4, fbo_width_,
                              fbo_height_};
        size_t tagged = 0;
        if (spdlog::should_log(spdlog::level::trace)) {
            for (size_t i = 3; i < readback_buf_.size(); i += 4) {
                tagged += (readback_buf_[i] == helix::gcode::kSelectedAlpha);
            }
        }
        helix::gcode::stroke_selection_rim(rt, rim, rim, sel_palette_.outline,
                                           helix::gcode::ChannelOrder::Rgba);
        spdlog::trace("[GCode GLES] Selection rim: {} tagged px of {}, rim {}px", tagged,
                      static_cast<size_t>(fbo_width_) * fbo_height_, rim);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Convert GL RGBA → LVGL RGB888 (BGR byte order), flip Y, and scale if needed
    if (!draw_buf_->data) {
        spdlog::error("[GCode GLES] draw_buf_ data is null");
        return;
    }
    auto* dest = static_cast<uint8_t*>(draw_buf_->data);
    const auto* src = readback_buf_.data();
    bool needs_scale = (fbo_width_ != widget_w || fbo_height_ != widget_h);
    // Use actual draw buffer stride (aligned to LV_DRAW_BUF_STRIDE_ALIGN)
    uint32_t dst_stride = draw_buf_->header.stride;

    // A 2x supersampled frame comes down through a 2x2 box filter: the extra
    // samples are the whole point of rendering big, and nearest sampling would
    // throw three of every four away. Any other size ratio (the half-res
    // moving FBO upscaled, or a same-size frame) takes the paths below.
    if (fbo_width_ == widget_w * 2 && fbo_height_ == widget_h * 2) {
        const auto avg4 = [](unsigned a, unsigned b, unsigned c, unsigned d) {
            return static_cast<uint8_t>((a + b + c + d + 2) / 4);
        };
        for (int dy = 0; dy < widget_h; ++dy) {
            // Y-flip on the source, same as the loops below.
            const uint8_t* row_a =
                src + static_cast<size_t>(fbo_height_ - 1 - (dy * 2)) * fbo_width_ * 4;
            const uint8_t* row_b =
                src + static_cast<size_t>(fbo_height_ - 1 - (dy * 2 + 1)) * fbo_width_ * 4;
            auto* dst_row = dest + static_cast<size_t>(dy) * dst_stride;
            for (int dx = 0; dx < widget_w; ++dx) {
                const size_t s0 = static_cast<size_t>(dx * 2) * 4;
                const size_t s1 = s0 + 4;
                const size_t di = static_cast<size_t>(dx) * 3;
                dst_row[di + 0] = avg4(row_a[s0 + 2], row_a[s1 + 2], row_b[s0 + 2], row_b[s1 + 2]);
                dst_row[di + 1] = avg4(row_a[s0 + 1], row_a[s1 + 1], row_b[s0 + 1], row_b[s1 + 1]);
                dst_row[di + 2] = avg4(row_a[s0 + 0], row_a[s1 + 0], row_b[s0 + 0], row_b[s1 + 0]);
            }
        }
        lv_draw_image_dsc_t ssaa_dsc;
        lv_draw_image_dsc_init(&ssaa_dsc);
        ssaa_dsc.src = draw_buf_;
        lv_area_t ssaa_area = *widget_coords;
        lv_draw_image(layer, &ssaa_dsc, &ssaa_area);
        return;
    }

    // Row-based conversion: RGBA→BGR with Y-flip
    for (int dy = 0; dy < widget_h; ++dy) {
        int sy = needs_scale ? (dy * fbo_height_ / widget_h) : dy;
        int gl_row = fbo_height_ - 1 - sy;
        const auto* src_row = src + static_cast<size_t>(gl_row * fbo_width_) * 4;
        auto* dst_row = dest + static_cast<size_t>(dy) * dst_stride;

        if (needs_scale) {
            for (int dx = 0; dx < widget_w; ++dx) {
                int sx = dx * fbo_width_ / widget_w;
                size_t si = static_cast<size_t>(sx) * 4;
                size_t di = static_cast<size_t>(dx) * 3;
                dst_row[di + 0] = src_row[si + 2]; // B
                dst_row[di + 1] = src_row[si + 1]; // G
                dst_row[di + 2] = src_row[si + 0]; // R
            }
        } else {
            // No scaling: convert entire row RGBA→BGR
            for (int dx = 0; dx < widget_w; ++dx) {
                size_t si = static_cast<size_t>(dx) * 4;
                size_t di = static_cast<size_t>(dx) * 3;
                dst_row[di + 0] = src_row[si + 2]; // B
                dst_row[di + 1] = src_row[si + 1]; // G
                dst_row[di + 2] = src_row[si + 0]; // R
            }
        }
    }

    // Draw to LVGL layer
    lv_draw_image_dsc_t img_dsc;
    lv_draw_image_dsc_init(&img_dsc);
    img_dsc.src = draw_buf_;

    lv_area_t area = *widget_coords;
    lv_draw_image(layer, &img_dsc, &area);
}

// ============================================================
// CachedRenderState
// ============================================================

bool GCodeGLESRenderer::CachedRenderState::same_scene(const CachedRenderState& o) const {
    // Epsilon comparisons: tighter for angles, looser for zoom/distance
    auto near_angle = [](float a, float b) { return std::abs(a - b) < ANGLE_EPSILON; };
    auto near_zoom = [](float a, float b) { return std::abs(a - b) < ZOOM_EPSILON; };
    return near_angle(azimuth, o.azimuth) && near_angle(elevation, o.elevation) &&
           near_zoom(distance, o.distance) && near_zoom(zoom_level, o.zoom_level) &&
           near_angle(target.x, o.target.x) && near_angle(target.y, o.target.y) &&
           near_angle(target.z, o.target.z) && layer_start == o.layer_start &&
           layer_end == o.layer_end && highlight_count == o.highlight_count &&
           highlight_set_hash == o.highlight_set_hash && exclude_count == o.exclude_count &&
           filament_color == o.filament_color && ghost_opacity == o.ghost_opacity &&
           content_offset_y == o.content_offset_y && viewport_width == o.viewport_width &&
           viewport_height == o.viewport_height;
}

bool GCodeGLESRenderer::CachedRenderState::operator==(const CachedRenderState& o) const {
    return same_scene(o) && progress_layer == o.progress_layer;
}

GCodeGLESRenderer::CachedRenderState
GCodeGLESRenderer::snapshot_state(const GCodeCamera& camera) const {
    CachedRenderState s;
    s.azimuth = camera.get_azimuth();
    s.elevation = camera.get_elevation();
    s.distance = camera.get_distance();
    s.zoom_level = camera.get_zoom_level();
    s.target = camera.get_target();
    s.progress_layer = progress_layer_;
    s.layer_start = layer_start_;
    s.layer_end = layer_end_;
    s.highlight_count = selection_.highlighted().size();
    s.highlight_set_hash = selection_.highlighted_hash();
    s.exclude_count = selection_.excluded().size();
    s.filament_color = filament_color_;
    s.ghost_opacity = ghost_opacity_;
    s.content_offset_y = content_offset_y_percent_;
    s.viewport_width = viewport_width_;
    s.viewport_height = viewport_height_;
    return s;
}

// ============================================================
// Configuration Methods
// ============================================================

void GCodeGLESRenderer::set_viewport_size(int width, int height) {
    if (width == viewport_width_ && height == viewport_height_)
        return;
    viewport_width_ = width;
    viewport_height_ = height;
    frame_dirty_ = true;
}

void GCodeGLESRenderer::set_interaction_mode(bool interacting) {
    if (interaction_mode_ == interacting)
        return;
    interaction_mode_ = interacting;
}

void GCodeGLESRenderer::set_filament_color(const std::string& hex_color) {
    // The sscanf("#%02x%02x%02x") this replaced happened to be the only site in
    // the tree that read an 8-digit #RRGGBBAA token correctly - it stopped after
    // six digits. It accepted and silently truncated anything longer, though,
    // and its size()<7 guard documented a rule it did not enforce. Route it
    // through the shared parser so "correct" stops being an accident.
    uint32_t rgb = 0;
    if (!helix::parse_hex_color(hex_color.c_str(), rgb)) {
        return;
    }
    const float r = static_cast<float>((rgb >> 16) & 0xFF);
    const float g = static_cast<float>((rgb >> 8) & 0xFF);
    const float b = static_cast<float>(rgb & 0xFF);
    filament_color_ = glm::vec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    frame_dirty_ = true;
}

void GCodeGLESRenderer::set_extrusion_color(lv_color_t color) {
    filament_color_ =
        glm::vec4(color.red / 255.0f, color.green / 255.0f, color.blue / 255.0f, 1.0f);
    palette_.has_override = true;
    palette_.override_color = color;
    frame_dirty_ = true;
    spdlog::debug("[GCode GLES] set_extrusion_color: R={} G={} B={} → ({:.2f},{:.2f},{:.2f})",
                  color.red, color.green, color.blue, filament_color_.r, filament_color_.g,
                  filament_color_.b);
}

void GCodeGLESRenderer::set_tool_color_overrides(const std::vector<uint32_t>& ams_colors) {
    if (!geometry_ || ams_colors.empty()) {
        return;
    }

    // Lock palette during modification to prevent data races with render path
    std::lock_guard<std::mutex> lock(palette_mutex_);

    // The loop below overwrites the baked palette IN PLACE - the vertex data
    // indexes into it, so there is nowhere else to put an override. Snapshot it
    // the first time, because that copy becomes the only surviving record of
    // what the slicer said and clear_tool_color_overrides() restores from it.
    // At most 256 uint32_t (RibbonGeometry caps the palette), so ~1KB: cheaper
    // than re-deriving the file palette and re-running the tool->palette map on
    // the way back out, and it cannot disagree with what was actually baked.
    if (baked_color_palette_.empty()) {
        baked_color_palette_ = geometry_->color_palette;
    }

    // Replace palette entries using tool→palette mapping from geometry build
    bool changed = false;
    for (size_t tool = 0; tool < ams_colors.size(); ++tool) {
        auto it = geometry_->tool_palette_map.find(static_cast<uint8_t>(tool));
        if (it == geometry_->tool_palette_map.end()) {
            continue;
        }
        uint8_t palette_idx = it->second;
        if (palette_idx < geometry_->color_palette.size() &&
            geometry_->color_palette[palette_idx] != ams_colors[tool]) {
            geometry_->color_palette[palette_idx] = ams_colors[tool];
            changed = true;
        }
    }

    if (changed) {
        // Per-tool overrides replace palette entries baked into vertex data,
        // so clear any single-color override that would bypass vertex colors.
        palette_.has_override = false;
        // Patch only the color bytes in the prepared buffers in place. Avoids
        // discarding the ~100MB pack the background thread just produced and
        // re-expanding it on the foreground thread — only the RGBA8 lanes
        // need to change.
        if (geometry_) {
            geometry_->patch_prepared_buffer_colors();
        }
        // Force VBO re-upload to push the new colors to the GPU
        // (old VBOs freed inside render() where GL context is active)
        cancel_job();
        geometry_uploaded_ = false;
        upload_next_layer_ = 0;
        upload_total_layers_ = 0;
        frame_dirty_ = true;
        spdlog::debug("[GCode GLES] Applied {} tool color overrides, triggering VBO re-upload",
                      ams_colors.size());
    }
}

void GCodeGLESRenderer::clear_tool_color_overrides() {
    if (!geometry_) {
        return;
    }

    std::lock_guard<std::mutex> lock(palette_mutex_);

    // Nothing was ever overridden on this geometry, so the palette it was built
    // with is still the palette it has. Note this is NOT the same test as
    // "ams_colors is empty" on the way in: that one means "no information, leave
    // the slicer palette alone", which is why it must stay a no-op there.
    if (baked_color_palette_.empty()) {
        return;
    }

    const bool changed = geometry_->color_palette != baked_color_palette_;
    geometry_->color_palette = baked_color_palette_;
    baked_color_palette_.clear();

    if (!changed) {
        return;
    }

    // Same repaint route as applying an override: patch only the RGBA8 lanes in
    // the prepared buffers rather than re-expanding the whole pack, then force
    // the VBOs back up so the GPU sees the restored colors.
    geometry_->patch_prepared_buffer_colors();
    cancel_job();
    geometry_uploaded_ = false;
    upload_next_layer_ = 0;
    upload_total_layers_ = 0;
    frame_dirty_ = true;
    spdlog::debug("[GCode GLES] Retracted tool color overrides, restored {}-entry baked palette",
                  geometry_->color_palette.size());
}

void GCodeGLESRenderer::set_simplification_tolerance(float /*tolerance_mm*/) {
    // Simplification is applied during geometry build, not at render time
}

void GCodeGLESRenderer::set_specular(float intensity, float shininess) {
    specular_intensity_ = std::clamp(intensity, MIN_SPECULAR_INTENSITY, MAX_SPECULAR_INTENSITY);
    specular_shininess_ = std::clamp(shininess, MIN_SPECULAR_SHININESS, MAX_SPECULAR_SHININESS);
    frame_dirty_ = true;
}

void GCodeGLESRenderer::set_debug_face_colors(bool enable) {
    debug_face_colors_ = enable;
    frame_dirty_ = true;
}

void GCodeGLESRenderer::set_show_travels(bool show) {
    show_travels_ = show;
    frame_dirty_ = true;
}

void GCodeGLESRenderer::set_show_extrusions(bool show) {
    show_extrusions_ = show;
    frame_dirty_ = true;
}

void GCodeGLESRenderer::set_layer_range(int start, int end) {
    layer_start_ = start;
    layer_end_ = end;
    frame_dirty_ = true;
}

void GCodeGLESRenderer::set_highlighted_object(const std::string& name) {
    std::unordered_set<std::string> objects;
    if (!name.empty())
        objects.insert(name);
    set_highlighted_objects(objects);
}

void GCodeGLESRenderer::set_highlighted_objects(const std::unordered_set<std::string>& names) {
    // SelectionState memoizes the set hash, so the per-frame cached-state build
    // does not iterate every name on each LVGL invalidation tick.
    if (selection_.set_highlighted(names) != InvalidationScope::Nothing) {
        frame_dirty_ = true;
    }
}

void GCodeGLESRenderer::set_excluded_objects(const std::unordered_set<std::string>& names) {
    if (selection_.set_excluded(names) != InvalidationScope::Nothing) {
        frame_dirty_ = true;
    }
}

void GCodeGLESRenderer::set_global_opacity(lv_opa_t opacity) {
    global_opacity_ = opacity;
    frame_dirty_ = true;
}

void GCodeGLESRenderer::reset_colors() {
    // Main thread; the GL passes below consume these as uniforms.
    sel_palette_ = selection::palette_from_theme();
    palette_.has_override = false;
    filament_color_ = DEFAULT_FILAMENT_COLOR;
    frame_dirty_ = true;
}

void GCodeGLESRenderer::clear_cached_frame() {
    // Free the cached draw buffer so stale frames aren't blitted during render deferral
    // Frees the SAME draw_buf_ the destructor guards as a #929 hazard, but this
    // site had no lv_draw_wait_for_finish() at all: a deferred-render clear could
    // free the buffer out from under an in-flight draw task. The shared helper
    // makes that impossible to get wrong again.
    if (draw_buf_) {
        helix::safe_draw_buf_destroy(draw_buf_, "gles_draw_buf");
        draw_buf_width_ = 0;
        draw_buf_height_ = 0;
    }
    render_defer_frames_ = 0;
}

RenderingOptions GCodeGLESRenderer::get_options() const {
    RenderingOptions opts;
    opts.show_extrusions = show_extrusions_;
    opts.show_travels = show_travels_;
    opts.layer_start = layer_start_;
    opts.layer_end = layer_end_;
    opts.highlighted_object = highlighted_object_;
    return opts;
}

// ============================================================
// Ghost / Print Progress
// ============================================================

void GCodeGLESRenderer::set_print_progress_layer(int current_layer) {
    // No frame_dirty_: print progress goes through render_schedule::decide_job,
    // which decides between extending, an incremental pass, or a restart.
    progress_layer_ = current_layer;
}

void GCodeGLESRenderer::set_ghost_opacity(lv_opa_t opacity) {
    ghost_opacity_ = opacity;
    frame_dirty_ = true;
}

void GCodeGLESRenderer::set_content_offset_y(float offset_percent) {
    const float clamped = std::clamp(offset_percent, -1.0f, 1.0f);
    if (std::abs(clamped - content_offset_y_percent_) < 1e-4f) {
        return;
    }
    content_offset_y_percent_ = clamped;
    frame_dirty_ = true;
}

void GCodeGLESRenderer::set_ghost_render_mode(GhostRenderMode mode) {
    ghost_render_mode_ = mode;
    frame_dirty_ = true;
}

int GCodeGLESRenderer::get_max_layer_index() const {
    if (geometry_)
        return static_cast<int>(geometry_->max_layer_index);
    return 0;
}

// ============================================================
// Geometry Release
// ============================================================

void GCodeGLESRenderer::release_geometry() {
    cancel_job();
    size_t freed = geometry_ ? geometry_->memory_usage() : 0;

    // Free GPU VBOs (requires GL context)
    if (gl_initialized_ && !layer_vbos_.empty()) {
#if LV_USE_SDL
        SdlGlContextGuard guard(sdl_gl_window_, sdl_gl_context_);
#else
        EglContextGuard guard(egl_display_, egl_surface_, egl_context_);
#endif
        if (guard.ok()) {
            free_vbos(layer_vbos_);
        }
    }

    // Free CPU geometry
    geometry_.reset();
    {
        // Nothing left to restore the baked palette onto.
        std::lock_guard<std::mutex> lock(palette_mutex_);
        baked_color_palette_.clear();
    }
    active_geometry_ = nullptr;
    current_filename_.clear();
    geometry_uploaded_ = false;
    upload_next_layer_ = 0;
    upload_total_layers_ = 0;

    // Free readback buffer
    freed += readback_buf_.capacity();
    readback_buf_.clear();
    readback_buf_.shrink_to_fit();

    if (freed > 0) {
        spdlog::info("[GCode GLES] Released geometry: {} MB freed", freed / (1024 * 1024));
    }
}

// ============================================================
// Geometry Loading
// ============================================================

void GCodeGLESRenderer::set_prebuilt_geometry(std::unique_ptr<RibbonGeometry> geometry,
                                              const std::string& filename) {
    cancel_job();
    geometry_ = std::move(geometry);
    {
        // The snapshot describes the palette of the geometry just replaced.
        // Kept, it would be restored onto a different file's palette.
        std::lock_guard<std::mutex> lock(palette_mutex_);
        baked_color_palette_.clear();
    }
    current_filename_ = filename;
    geometry_uploaded_ = false;
    upload_next_layer_ = 0;
    upload_total_layers_ = 0;
    frame_dirty_ = true;
    spdlog::debug("[GCode GLES] Geometry set: {} strips, {} vertices",
                  geometry_ ? geometry_->strips.size() : 0,
                  geometry_ ? geometry_->vertices.size() : 0);
}

void GCodeGLESRenderer::set_prebuilt_coarse_geometry(std::unique_ptr<RibbonGeometry> /*geometry*/) {
    // Coarse LOD no longer used — GPU handles full geometry at full speed
}

// ============================================================
// Statistics
// ============================================================

size_t GCodeGLESRenderer::get_geometry_color_count() const {
    if (geometry_)
        return geometry_->color_palette.size();
    return 0;
}

helix::gcode::RenderMemoryReport GCodeGLESRenderer::memory_report() const {
    helix::gcode::RenderMemoryReport r;

    size_t geometry = 0;
    if (geometry_) {
        geometry += geometry_->vertices.size() * sizeof(RibbonVertex);
        geometry += geometry_->strips.size() * sizeof(TriangleStrip);
        geometry += geometry_->strip_color_index.size() * sizeof(uint8_t);
    }
    r.add("geometry", geometry);

    // RGB888, no alpha: blit_to_lvgl drops the alpha byte on the way in, which
    // is what freed it up to carry the selection tag.
    r.add("draw_buf", draw_buf_ ? static_cast<size_t>(draw_buf_width_) *
                                      static_cast<size_t>(draw_buf_height_) * 3
                                : 0);

    // Was missing from the old accounting entirely. It is a full RGBA copy of
    // the framebuffer and it is resident for the life of the renderer.
    r.add("readback", readback_buf_.size());

    size_t vbo = 0;
    for (const auto& lv : layer_vbos_) {
        if (lv.vbo) {
            vbo += lv.vertex_count * PackedVertex::stride();
        }
    }
    r.add("vbo_gpu", vbo);

    // Colour RBO (RGBA8, 4 bytes) + depth RBO (16-bit, 2 bytes). Lives in VRAM,
    // reported alongside host heap because on the SoCs we ship to it is the same
    // physical memory.
    r.add("fbo_gpu",
          fbo_.id ? static_cast<size_t>(fbo_width_) * static_cast<size_t>(fbo_height_) * 6 : 0);
    return r;
}

void GCodeGLESRenderer::log_memory_report(const char* when) const {
    if (!spdlog::should_log(spdlog::level::debug)) {
        return;
    }
    spdlog::debug("[GCode GLES] memory after {}: {}", when, memory_report().format());
}

size_t GCodeGLESRenderer::get_triangle_count() const {
    if (geometry_)
        return geometry_->extrusion_triangle_count;
    return 0;
}

glm::mat4 GCodeGLESRenderer::build_mvp(const GCodeCamera& camera) const {
    // Model: -90° CW around Z to match slicer thumbnail orientation.
    glm::mat4 model = glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(0, 0, 1));
    glm::mat4 proj = camera.get_projection_matrix();
    // NDC Y range is [-1, 1]; multiply by 2 so the percent maps to a full shift.
    if (std::abs(content_offset_y_percent_) > 0.001f) {
        proj[3][1] += -content_offset_y_percent_ * 2.0f;
    }
    return proj * camera.get_view_matrix() * model;
}

// ============================================================
// Selection Brackets (3D, GPU-side — drawn inside the FBO)
// ============================================================

static const char* LINE_VERTEX_SHADER = R"(
    uniform mat4 u_mvp;
    attribute vec3 a_position;
    void main() {
        gl_Position = u_mvp * vec4(a_position, 1.0);
    }
)";

static const char* LINE_FRAGMENT_SHADER = R"(
    precision mediump float;
    uniform vec4 u_color;
    void main() {
        gl_FragColor = u_color;
    }
)";

bool GCodeGLESRenderer::init_line_program() {
    if (line_program_)
        return true;

    GLuint vs = compile_shader(GL_VERTEX_SHADER, LINE_VERTEX_SHADER);
    if (!vs)
        return false;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, LINE_FRAGMENT_SHADER);
    if (!fs) {
        glDeleteShader(vs);
        return false;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLchar log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        spdlog::error("[GCode GLES] Line program link failed: {}", log);
        glDeleteProgram(prog);
        return false;
    }

    line_program_ = prog;
    line_u_mvp_ = glGetUniformLocation(prog, "u_mvp");
    line_u_color_ = glGetUniformLocation(prog, "u_color");
    line_a_position_ = glGetAttribLocation(prog, "a_position");

    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    line_vbo_ = vbo;
    return true;
}

void GCodeGLESRenderer::render_brackets_3d(const ParsedGCodeFile& gcode, const glm::mat4& mvp) {
    if (!selection_.any_highlighted())
        return;
    if (!line_program_ && !init_line_program())
        return;

    // Collect line endpoints (one vec3 per vertex, two verts per line).
    // 8 corners * 3 axes * 2 verts = 48 vertices per object.
    std::vector<glm::vec3> verts;
    verts.reserve(selection_.highlighted().size() * 48);

    for (const auto& name : selection_.highlighted()) {
        auto it = gcode.objects.find(name);
        if (it == gcode.objects.end())
            continue;
        const AABB& bbox = it->second.bounding_box;
        if (bbox.is_empty())
            continue;

        // Run the bbox through the same quantization the geometry pipeline
        // applied to vertices, so brackets sit on the rendered surface rather
        // than offset by a fraction of a quantization step.
        glm::vec3 bmin = bbox.min;
        glm::vec3 bmax = bbox.max;
        if (geometry_) {
            const auto& quant = geometry_->quantization;
            bmin = quant.dequantize_vec3(quant.quantize_vec3(bbox.min));
            bmax = quant.dequantize_vec3(quant.quantize_vec3(bbox.max));
        }

        // Shared sizing, so 2D and 3D bracket proportions cannot drift apart.
        // Measured on the quantized box, not the raw one.
        const AABB quantized{bmin, bmax};
        const float bracket_len = selection::bracket_arm_length(quantized);
        if (bracket_len == 0.0f)
            continue;

        quantized.for_each_bracket_arm(bracket_len,
                                       [&](const glm::vec3& origin, const glm::vec3& tip) {
                                           verts.push_back(origin);
                                           verts.push_back(tip);
                                       });
    }

    if (verts.empty())
        return;

    // Draw on top of existing geometry. Brackets are unlit and uniform color.
    glDisable(GL_DEPTH_TEST);
    glUseProgram(line_program_);
    glUniformMatrix4fv(line_u_mvp_, 1, GL_FALSE, glm::value_ptr(mvp));
    // Silver, fully opaque, from the same constant the 2D path draws.
    const glm::vec4 bracket_rgba = selection::to_vec4(sel_palette_.bracket);
    glUniform4f(line_u_color_, bracket_rgba.r, bracket_rgba.g, bracket_rgba.b, bracket_rgba.a);

    glBindBuffer(GL_ARRAY_BUFFER, line_vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(glm::vec3)),
                 verts.data(), GL_STREAM_DRAW);

    glEnableVertexAttribArray(static_cast<GLuint>(line_a_position_));
    glVertexAttribPointer(static_cast<GLuint>(line_a_position_), 3, GL_FLOAT, GL_FALSE,
                          sizeof(glm::vec3), nullptr);

    glLineWidth(2.0f);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(verts.size()));

    glDisableVertexAttribArray(static_cast<GLuint>(line_a_position_));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glEnable(GL_DEPTH_TEST);
}

// ============================================================
// Selection Silhouette (3D shell pass, drawn under the lit geometry)
// ============================================================

static const char* SHELL_VERTEX_DECLS = R"(
    // Selection tag. Consumes the same packed vertex layout as the lit program,
    // so it draws straight from the layer VBOs with no extra buffer:
    //   a_position : vec3, raw quantized int16 (dequantization folded into u_mvp)
    // The normal is not read: this pass reproduces the object exactly where the
    // lit pass already drew it, and writes nothing but alpha.
    uniform mat4 u_mvp;
    attribute vec3 a_position;
)";

static const char* SHELL_VERTEX_MAIN = R"(
    void main() {
        gl_Position = u_mvp * vec4(a_position, 1.0);
    }
)";

/// Alpha written by the tag pass, matching the software rasterizer's tag so both
/// renderers hand stroke_selection_rim() the same thing.
static constexpr float SHELL_TAG_ALPHA = static_cast<float>(kSelectedAlpha) / 255.0f;

bool GCodeGLESRenderer::init_shell_program() {
    if (shell_program_)
        return true;

    const std::string vertex_src =
        std::string(SHELL_VERTEX_DECLS) + OCT_DECODE_GLSL + SHELL_VERTEX_MAIN;
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_src.c_str());
    if (!vs)
        return false;
    // Same flat-color fragment shader the bracket program uses — one uniform color,
    // no lighting. No reason for a second copy.
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, LINE_FRAGMENT_SHADER);
    if (!fs) {
        glDeleteShader(vs);
        return false;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLchar log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        spdlog::error("[GCode GLES] Shell program link failed: {}", log);
        glDeleteProgram(prog);
        return false;
    }

    shell_u_mvp_ = glGetUniformLocation(prog, "u_mvp");
    shell_u_color_ = glGetUniformLocation(prog, "u_color");
    shell_a_position_ = glGetAttribLocation(prog, "a_position");

    if (shell_a_position_ < 0) {
        spdlog::error("[GCode GLES] Tag program missing a_position");
        glDeleteProgram(prog);
        return false;
    }

    shell_program_ = prog;
    spdlog::debug("[GCode GLES] Selection tag program ready");
    return true;
}

void GCodeGLESRenderer::render_selection_tag(const ParsedGCodeFile& gcode,
                                             const glm::mat4& mvp_dequant, int layer_start,
                                             int layer_end) {
    // Gate before anything else, including the program link: an unselected plate
    // must cost nothing at all.
    if (!selection_.any_highlighted())
        return;
    if (!active_geometry_ || active_geometry_->object_runs.empty())
        return; // no exclude-object metadata, or the run-count guard tripped
    if (layer_end < layer_start)
        return;

    // Resolve the selected names to interned indices, once, into a flat lookup.
    // This is the same table the geometry builder resolved
    // segment.object_name_index against — the viewer builds the geometry from,
    // and renders, one ParsedGCodeFile — so the indices in object_runs are
    // directly comparable.
    //
    // A vector<int16_t> plus std::find was the first shape here, copied from the
    // code it replaced. That is a linear scan per RUN per LAYER, inside a
    // per-frame pass; an indexed array is the same information with the search
    // removed, and it is what SelectionState::classify() already does on the 2D
    // side.
    std::vector<bool> wanted(gcode.object_name_table.size(), false);
    bool any_wanted = false;
    for (const auto& name : selection_.highlighted()) {
        for (size_t i = 0; i < gcode.object_name_table.size(); ++i) {
            if (gcode.object_name_table[i] == name) {
                wanted[i] = true;
                any_wanted = true;
                break;
            }
        }
    }
    if (!any_wanted)
        return;

    if (!shell_program_ && !init_shell_program())
        return;

    // Save every piece of state this pass touches. render_brackets_3d brackets its
    // own glDisable(GL_DEPTH_TEST) the same way; anything left unrestored here
    // corrupts the frame.
    GLint prev_program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    GLint prev_buffer = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_buffer);
    GLboolean prev_depth_mask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prev_depth_mask);
    GLint prev_depth_func = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &prev_depth_func);

    // Alpha only. The color channels already hold the lit image and must survive
    // untouched; the rest of the frame runs with alpha writes masked off (see
    // setup_frame) so nothing but this pass can put kSelectedAlpha anywhere.
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);

    // Depth test stays enabled, but the function MUST be relaxed to LEQUAL. The
    // renderer never calls glDepthFunc, so it runs on the GL default of LESS, and
    // re-drawing a triangle at exactly the depth it already wrote fails LESS on
    // every single fragment — the pass drew its 125 runs and tagged zero pixels.
    // With LEQUAL it passes exactly where this object is the frontmost thing and
    // fails where something else occludes it, which is the definition of
    // "visible" and so the contour we want. Depth WRITES go off: the buffer is
    // already correct and re-writing it would be a no-op at best.
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);

    glUseProgram(shell_program_);
    glUniformMatrix4fv(shell_u_mvp_, 1, GL_FALSE, glm::value_ptr(mvp_dequant));
    glUniform4f(shell_u_color_, 0.0f, 0.0f, 0.0f, SHELL_TAG_ALPHA);

    glEnableVertexAttribArray(static_cast<GLuint>(shell_a_position_));

    constexpr size_t STRIDE = PackedVertex::stride();
    size_t runs_drawn = 0;

    for (int layer = layer_start; layer <= layer_end; ++layer) {
        if (layer < 0 || layer >= static_cast<int>(layer_vbos_.size()))
            continue;
        const auto& lv = layer_vbos_[static_cast<size_t>(layer)];
        if (!lv.vbo || lv.vertex_count == 0)
            continue;

        const auto runs = active_geometry_->layer_object_runs(static_cast<size_t>(layer));
        if (runs.empty())
            continue;

        bool bound = false;
        for (const auto& run : runs) {
            if (run.object_index < 0 || static_cast<size_t>(run.object_index) >= wanted.size() ||
                !wanted[static_cast<size_t>(run.object_index)]) {
                continue;
            }
            // Runs are validated at build time, but the VBO can lag the geometry
            // during incremental upload — never let a stale run read past the buffer.
            if (static_cast<size_t>(run.vertex_offset) + run.vertex_count > lv.vertex_count)
                continue;

            if (!bound) {
                glBindBuffer(GL_ARRAY_BUFFER, lv.vbo);
                glVertexAttribPointer(static_cast<GLuint>(shell_a_position_), 3, GL_SHORT, GL_FALSE,
                                      static_cast<GLsizei>(STRIDE),
                                      reinterpret_cast<void*>(PackedVertex::position_offset()));
                bound = true;
            }

            glDrawArrays(GL_TRIANGLES, static_cast<GLint>(run.vertex_offset),
                         static_cast<GLsizei>(run.vertex_count));
            ++runs_drawn;
        }
    }

    glDisableVertexAttribArray(static_cast<GLuint>(shell_a_position_));

    // Restore, in the reverse order of the saves. Alpha writes go back to masked
    // off, which is how the rest of the frame runs.
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prev_buffer));
    glDepthMask(prev_depth_mask);
    glDepthFunc(static_cast<GLenum>(prev_depth_func));
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glUseProgram(static_cast<GLuint>(prev_program));

    // One error check for the whole pass, matching draw_layers. A fault here is
    // the same class of driver failure and must fall back the same way.
    GLenum tag_err = glGetError();
    if (gl_draw_error_is_fatal(tag_err)) {
        spdlog::error("[GCode GLES] Fatal GL error in selection tag pass: 0x{:04X} — disabling "
                      "GPU rendering, falling back to 2D",
                      tag_err);
        gl_render_failed_ = true;
        cancel_job();
        return;
    }

    spdlog::trace("[GCode GLES] Selection tag: {} runs over layers {}..{}", runs_drawn, layer_start,
                  layer_end);
}

// ============================================================
// Object Picking (CPU-side, no GL needed)
// ============================================================

std::optional<std::string> GCodeGLESRenderer::pick_object(const glm::vec2& screen_pos,
                                                          const ParsedGCodeFile& gcode,
                                                          const GCodeCamera& camera) const {
    glm::mat4 transform = build_mvp(camera);
    float closest_distance = std::numeric_limits<float>::max();
    std::optional<std::string> picked_object;

    constexpr float PICK_THRESHOLD = selection::kPickThresholdPx;

    // Stage 1: which objects could the tap possibly be on?
    //
    // Stage 2 costs two mat4 multiplies and a point-to-segment distance for
    // EVERY segment of every visible layer, and the test plate parses to 135,197
    // of them. Projecting eight corners per object first, and skipping segments
    // whose object's projected box is nowhere near the tap, turns most of that
    // into one array lookup. The 2D picker got this treatment in 1ee6ba494; this
    // is the same idea against the same data.
    //
    // Objects are keyed by interned index, so the candidate set is a flat array
    // rather than a set of strings.
    std::vector<bool> candidate(gcode.object_name_table.size(), false);
    bool any_candidate = false;
    for (const auto& [name, obj] : gcode.objects) {
        const AABB& box = obj.bounding_box;
        // Defined but never extruded: the corners are +/-inf and projecting them
        // yields garbage that would match every tap.
        if (box.is_empty()) {
            continue;
        }
        int16_t idx = -1;
        for (size_t i = 0; i < gcode.object_name_table.size(); ++i) {
            if (gcode.object_name_table[i] == name) {
                idx = static_cast<int16_t>(i);
                break;
            }
        }
        if (idx < 0) {
            continue;
        }

        // This used to reject corners with std::abs(clip.w) < EPSILON, which let a
        // corner BEHIND the camera through and mirrored it onto the screen,
        // corrupting the candidate box. project_aabb_to_screen() rejects w <= EPSILON.
        const ScreenBounds sb =
            project_aabb_to_screen(transform, box, viewport_width_, viewport_height_);
        // A box that projects to nothing usable stays a candidate: better to pay
        // for stage 2 than to drop a pick outright.
        if (!sb.valid || sb.contains(screen_pos.x, screen_pos.y, PICK_THRESHOLD)) {
            candidate[static_cast<size_t>(idx)] = true;
            any_candidate = true;
        }
    }
    // No object metadata at all (or nothing near the tap): fall through with
    // everything eligible rather than silently refusing to pick.
    if (!any_candidate && gcode.objects.empty()) {
        candidate.assign(candidate.size(), true);
    }

    int ls = layer_start_;
    int le = (layer_end_ < 0 || layer_end_ >= static_cast<int>(gcode.layers.size()))
                 ? static_cast<int>(gcode.layers.size()) - 1
                 : layer_end_;

    for (int layer_idx = ls; layer_idx <= le; ++layer_idx) {
        if (layer_idx < 0 || layer_idx >= static_cast<int>(gcode.layers.size()))
            continue;
        const auto& layer = gcode.layers[static_cast<size_t>(layer_idx)];

        for (const auto& segment : layer.segments) {
            // The shared draw decision: auxiliary geometry (purge, prime
            // tower) is dropped by the builder, so it must not be pickable
            // here either - a tap on the blank tower area must select
            // nothing, not an invisible object.
            if (!segment_drawable(segment, /*is_support=*/false, show_extrusions_, show_extrusions_,
                                  /*show_travel=*/false)) {
                continue;
            }
            if (segment.object_name_index < 0)
                continue;
            // Stage 1 result: one array lookup instead of two mat4 multiplies.
            if (static_cast<size_t>(segment.object_name_index) < candidate.size() &&
                !candidate[static_cast<size_t>(segment.object_name_index)]) {
                continue;
            }

            // Was std::abs(w) < EPSILON here too, with the same behind-camera
            // defect as the candidate pass above.
            const auto start_screen =
                project_clip_to_screen(transform, segment.start, viewport_width_, viewport_height_);
            const auto end_screen =
                project_clip_to_screen(transform, segment.end, viewport_width_, viewport_height_);
            if (!start_screen || !end_screen)
                continue;

            // Both endpoints must be on screen. This was spelled as an NDC range
            // check on [-1, 1]; in screen pixels that is exactly the viewport rect.
            const auto on_screen = [&](const glm::vec2& p) {
                return p.x >= 0.0f && p.x <= static_cast<float>(viewport_width_) && p.y >= 0.0f &&
                       p.y <= static_cast<float>(viewport_height_);
            };
            if (!on_screen(*start_screen) || !on_screen(*end_screen)) {
                continue;
            }

            const float dist = point_segment_distance(screen_pos, *start_screen, *end_screen);

            if (dist < PICK_THRESHOLD && dist < closest_distance) {
                closest_distance = dist;
                picked_object = gcode.get_object_name(segment.object_name_index);
            }
        }
    }

    // Fallback: after 3D geometry build, ParsedGCodeFile::clear_segments() frees
    // per-layer toolpath data to save memory — segment iteration above then
    // returns nothing. Project each defined object's AABB to screen space and
    // hit-test the resulting 2D bounding rect so taps still work post-build.
    if (!picked_object) {
        float closest_inside_area = std::numeric_limits<float>::max();
        for (const auto& [name, obj] : gcode.objects) {
            const auto& bbox = obj.bounding_box;
            if (bbox.is_empty())
                continue;

            const ScreenBounds sb =
                project_aabb_to_screen(transform, bbox, viewport_width_, viewport_height_);
            if (!sb.valid)
                continue;

            if (!sb.contains(screen_pos.x, screen_pos.y))
                continue;

            // Prefer the smallest (innermost) hit when objects overlap on screen.
            float area = sb.area();
            if (area < closest_inside_area) {
                closest_inside_area = area;
                picked_object = name;
            }
        }
    }

    return picked_object;
}

} // namespace gcode
} // namespace helix

#endif // ENABLE_GLES_3D
