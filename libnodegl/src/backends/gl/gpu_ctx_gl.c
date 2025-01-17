/*
 * Copyright 2019-2022 GoPro Inc.
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <string.h>

#include "config.h"

#if defined(TARGET_IPHONE) || defined(TARGET_DARWIN)
#include <CoreVideo/CoreVideo.h>
#endif

#include "buffer_gl.h"
#include "gpu_ctx.h"
#include "gpu_ctx_gl.h"
#include "glcontext.h"
#include "log.h"
#include "math_utils.h"
#include "memory.h"
#include "internal.h"
#include "pipeline_gl.h"
#include "program_gl.h"
#include "rendertarget_gl.h"
#include "texture_gl.h"
#if DEBUG_GPU_CAPTURE
#include "gpu_capture.h"
#endif

static void capture_cpu(struct gpu_ctx *s)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    struct glcontext *gl = s_priv->glcontext;
    struct ngl_config *config = &s->config;
    struct rendertarget *rt = s_priv->default_rt;
    struct rendertarget_gl *rt_gl = (struct rendertarget_gl *)rt;

    const GLuint fbo_id = rt_gl->resolve_id ? rt_gl->resolve_id : rt_gl->id;
    ngli_glBindFramebuffer(gl, GL_FRAMEBUFFER, fbo_id);
    ngli_glReadPixels(gl, 0, 0, rt->width, rt->height, GL_RGBA, GL_UNSIGNED_BYTE, config->capture_buffer);
}

static void capture_corevideo(struct gpu_ctx *s)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    struct glcontext *gl = s_priv->glcontext;

    ngli_glFinish(gl);
}

#if defined(TARGET_IPHONE) || defined(TARGET_DARWIN)
#if defined(TARGET_IPHONE)
static int wrap_capture_cvpixelbuffer(struct gpu_ctx *s,
                                      CVPixelBufferRef buffer,
                                      struct texture **texturep,
                                      CVOpenGLESTextureRef *cv_texturep)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    struct glcontext *gl = s_priv->glcontext;

    CVOpenGLESTextureRef cv_texture = NULL;
    CVOpenGLESTextureCacheRef *cache = ngli_glcontext_get_texture_cache(gl);
    const size_t width = CVPixelBufferGetWidth(buffer);
    const size_t height = CVPixelBufferGetHeight(buffer);
    CVReturn cv_ret = CVOpenGLESTextureCacheCreateTextureFromImage(kCFAllocatorDefault,
                                                                   *cache,
                                                                   buffer,
                                                                   NULL,
                                                                   GL_TEXTURE_2D,
                                                                   GL_RGBA,
                                                                   width,
                                                                   height,
                                                                   GL_BGRA,
                                                                   GL_UNSIGNED_BYTE,
                                                                   0,
                                                                   &cv_texture);
    if (cv_ret != kCVReturnSuccess) {
        LOG(ERROR, "could not create CoreVideo texture from CVPixelBuffer: %d", cv_ret);
        return NGL_ERROR_EXTERNAL;
    }

    GLuint id = CVOpenGLESTextureGetName(cv_texture);
    ngli_glBindTexture(gl, GL_TEXTURE_2D, id);
    ngli_glTexParameteri(gl, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    ngli_glTexParameteri(gl, GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    ngli_glBindTexture(gl, GL_TEXTURE_2D, 0);

    struct texture *texture = ngli_texture_create(s);
    if (!texture) {
        NGLI_CFRELEASE(cv_texture);
        return NGL_ERROR_MEMORY;
    }

    const struct texture_params attachment_params = {
        .type   = NGLI_TEXTURE_TYPE_2D,
        .format = NGLI_FORMAT_B8G8R8A8_UNORM,
        .width  = width,
        .height = height,
        .usage  = NGLI_TEXTURE_USAGE_COLOR_ATTACHMENT_BIT,
    };

    const struct texture_gl_wrap_params wrap_params = {
        .params  = &attachment_params,
        .texture = id,
    };

    int ret = ngli_texture_gl_wrap(texture, &wrap_params);
    if (ret < 0) {
        NGLI_CFRELEASE(cv_texture);
        ngli_texture_freep(&texture);
        return ret;
    }

    *texturep = texture;
    *cv_texturep = cv_texture;

    return 0;
}
#else
static int wrap_capture_cvpixelbuffer(struct gpu_ctx *s,
                                      CVPixelBufferRef buffer,
                                      struct texture **texturep,
                                      CVOpenGLTextureRef *cv_texturep)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    struct glcontext *gl = s_priv->glcontext;
    CVOpenGLTextureRef cv_texture = NULL;

    const size_t width = CVPixelBufferGetWidth(buffer);
    const size_t height = CVPixelBufferGetHeight(buffer);

    CVOpenGLTextureCacheRef cache = ngli_glcontext_get_texture_cache(gl);
    CVReturn cv_ret = CVOpenGLTextureCacheCreateTextureFromImage(kCFAllocatorDefault,
                                                                 cache,
                                                                 buffer,
                                                                 NULL,
                                                                 &cv_texture);
    if (cv_ret != kCVReturnSuccess) {
        LOG(ERROR, "could not create CoreVideo texture from CVPixelBuffer: %d", cv_ret);
        return NGL_ERROR_EXTERNAL;
    }

    GLuint fbName = CVOpenGLTextureGetName(cv_texture);
    ngli_glBindTexture(gl, GL_TEXTURE_RECTANGLE, fbName);
    ngli_glTexParameteri(gl, GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    ngli_glTexParameteri(gl, GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    ngli_glBindTexture(gl, GL_TEXTURE_RECTANGLE, 0);

    struct texture *texture = ngli_texture_create(s);
    if (!texture) {
        NGLI_CFRELEASE(cv_texture);
        return NGL_ERROR_MEMORY;
    }

    const struct texture_params attachment_params = {
        .type   = NGLI_TEXTURE_TYPE_RECTANGLE,
        .format = NGLI_FORMAT_B8G8R8A8_UNORM,
        .width  = width,
        .height = height,
        .usage  = NGLI_TEXTURE_USAGE_COLOR_ATTACHMENT_BIT,
    };

    const struct texture_gl_wrap_params wrap_params = {
        .params  = &attachment_params,
        .texture = fbName,
    };

    int ret = ngli_texture_gl_wrap(texture, &wrap_params);
    if (ret < 0) {
        NGLI_CFRELEASE(cv_texture);
        ngli_texture_freep(&texture);

        return ret;
    }

    *texturep = texture;
    *cv_texturep = cv_texture;

    return 0;
}
#endif 

static void reset_capture_cvpixelbuffer(struct gpu_ctx *s)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;

    NGLI_CFRELEASE(s_priv->capture_cvbuffer);
    NGLI_CFRELEASE(s_priv->capture_cvtexture);
}
#endif

static void gl_set_viewport(struct gpu_ctx *s, const int *viewport)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    memcpy(&s_priv->viewport, viewport, sizeof(s_priv->viewport));
}

static void gl_get_viewport(struct gpu_ctx *s, int *viewport)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    memcpy(viewport, &s_priv->viewport, sizeof(s_priv->viewport));
}

static void gl_set_scissor(struct gpu_ctx *s, const int *scissor)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    memcpy(&s_priv->scissor, scissor, sizeof(s_priv->scissor));
}

static void gl_get_scissor(struct gpu_ctx *s, int *scissor)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    memcpy(scissor, &s_priv->scissor, sizeof(s_priv->scissor));
}

static int create_texture(struct gpu_ctx *s, int format, int samples, struct texture **texturep)
{
    const struct ngl_config *config = &s->config;

    struct texture *texture = ngli_texture_create(s);
    if (!texture)
        return NGL_ERROR_MEMORY;

    const struct texture_params params = {
        .type    = NGLI_TEXTURE_TYPE_2D,
        .format  = format,
        .width   = config->width,
        .height  = config->height,
        .samples = samples,
        .usage   = NGLI_TEXTURE_USAGE_COLOR_ATTACHMENT_BIT,
    };

    int ret = ngli_texture_init(texture, &params);
    if (ret < 0) {
        ngli_texture_freep(&texture);
        return ret;
    }

    *texturep = texture;
    return 0;
}

static int create_rendertarget(struct gpu_ctx *s,
                               struct texture *color,
                               struct texture *resolve_color,
                               struct texture *depth_stencil,
                               int load_op,
                               struct rendertarget **rendertargetp)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    struct glcontext *gl = s_priv->glcontext;
    const struct ngl_config *config = &s->config;
    const struct ngl_config_gl *config_gl = config->backend_config;

    struct rendertarget *rendertarget = ngli_rendertarget_create(s);
    if (!rendertarget)
        return NGL_ERROR_MEMORY;

    const struct rendertarget_params params = {
        .width = config->width,
        .height = config->height,
        .nb_colors = 1,
        .colors[0] = {
            .attachment     = color,
            .resolve_target = resolve_color,
            .load_op        = load_op,
            .clear_value[0] = config->clear_color[0],
            .clear_value[1] = config->clear_color[1],
            .clear_value[2] = config->clear_color[2],
            .clear_value[3] = config->clear_color[3],
            .store_op       = NGLI_STORE_OP_STORE,
        },
        .depth_stencil = {
            .attachment = depth_stencil,
            .load_op    = load_op,
            .store_op   = NGLI_STORE_OP_STORE,
        },
    };

    int ret;
    if (color) {
        ret = ngli_rendertarget_init(rendertarget, &params);
    } else {
        const int external = config_gl ? config_gl->external : 0;
        const GLuint default_fbo_id = ngli_glcontext_get_default_framebuffer(gl);
        const GLuint fbo_id = external ? config_gl->external_framebuffer : default_fbo_id;
        ret = ngli_rendertarget_gl_wrap(rendertarget, &params, fbo_id);
    }
    if (ret < 0) {
        ngli_rendertarget_freep(&rendertarget);
        return ret;
    }

    *rendertargetp = rendertarget;
    return 0;
}

static int offscreen_rendertarget_init(struct gpu_ctx *s)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    struct glcontext *gl = s_priv->glcontext;
    struct ngl_config *config = &s->config;

    if (!(gl->features & NGLI_FEATURE_GL_FRAMEBUFFER_OBJECT) && config->samples > 0) {
        LOG(WARNING, "context does not support the framebuffer object feature, "
            "multisample anti-aliasing will be disabled");
        config->samples = 0;
    }

    if (config->capture_buffer_type == NGL_CAPTURE_BUFFER_TYPE_COREVIDEO) {
#if defined(TARGET_IPHONE) || defined(TARGET_DARWIN)
        if (config->capture_buffer) {
            s_priv->capture_cvbuffer = (CVPixelBufferRef)CFRetain(config->capture_buffer);
            int ret = wrap_capture_cvpixelbuffer(s, s_priv->capture_cvbuffer,
                                                 &s_priv->color, &s_priv->capture_cvtexture);
            if (ret < 0)
                return ret;
        } else {
            int ret = create_texture(s, NGLI_FORMAT_R8G8B8A8_UNORM, 0, &s_priv->color);
            if (ret < 0)
                return ret;
        }
#else
        LOG(ERROR, "CoreVideo capture is only supported on iOS and macOS");
        return NGL_ERROR_UNSUPPORTED;
#endif
    } else if (config->capture_buffer_type == NGL_CAPTURE_BUFFER_TYPE_CPU) {
        int ret = create_texture(s, NGLI_FORMAT_R8G8B8A8_UNORM, 0, &s_priv->color);
        if (ret < 0)
            return ret;
    } else {
        LOG(ERROR, "unsupported capture buffer type: %d", config->capture_buffer_type);
        return NGL_ERROR_UNSUPPORTED;
    }

    if (config->samples) {
        int ret = create_texture(s, NGLI_FORMAT_R8G8B8A8_UNORM, config->samples, &s_priv->ms_color);
        if (ret < 0)
            return ret;
    }

    int ret = create_texture(s, NGLI_FORMAT_D24_UNORM_S8_UINT, config->samples, &s_priv->depth_stencil);
    if (ret < 0)
        return ret;

    struct texture *color         = s_priv->ms_color ? s_priv->ms_color : s_priv->color;
    struct texture *resolve_color = s_priv->ms_color ? s_priv->color    : NULL;
    struct texture *depth_stencil = s_priv->depth_stencil;

    if ((ret = create_rendertarget(s, color, resolve_color, depth_stencil,
                                   NGLI_LOAD_OP_CLEAR, &s_priv->default_rt)) < 0 ||
        (ret = create_rendertarget(s, color, resolve_color, depth_stencil,
                                   NGLI_LOAD_OP_LOAD, &s_priv->default_rt_load)) < 0)
        return ret;

    static const capture_func_type capture_func_map[] = {
        [NGL_CAPTURE_BUFFER_TYPE_CPU]       = capture_cpu,
        [NGL_CAPTURE_BUFFER_TYPE_COREVIDEO] = capture_corevideo,
    };
    s_priv->capture_func = capture_func_map[config->capture_buffer_type];

    return 0;
}

static int onscreen_rendertarget_init(struct gpu_ctx *s)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;

    int ret;
    if ((ret = create_rendertarget(s, NULL, NULL, NULL, NGLI_LOAD_OP_CLEAR,
                                   &s_priv->default_rt)) < 0 ||
        (ret = create_rendertarget(s, NULL, NULL, NULL, NGLI_LOAD_OP_LOAD,
                                   &s_priv->default_rt_load)) < 0)
        return ret;

    return 0;
}

static void rendertarget_reset(struct gpu_ctx *s)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    ngli_rendertarget_freep(&s_priv->default_rt);
    ngli_rendertarget_freep(&s_priv->default_rt_load);
    ngli_texture_freep(&s_priv->color);
    ngli_texture_freep(&s_priv->ms_color);
    ngli_texture_freep(&s_priv->depth_stencil);
#if defined(TARGET_IPHONE) || defined(TARGET_DARWIN)
    reset_capture_cvpixelbuffer(s);
#endif
    s_priv->capture_func = NULL;
}

static void noop(const struct glcontext *gl, ...)
{
}

static int timer_init(struct gpu_ctx *s)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    struct glcontext *gl = s_priv->glcontext;

    if (gl->features & NGLI_FEATURE_GL_TIMER_QUERY) {
        s_priv->glGenQueries          = ngli_glGenQueries;
        s_priv->glDeleteQueries       = ngli_glDeleteQueries;
        s_priv->glBeginQuery          = ngli_glBeginQuery;
        s_priv->glEndQuery            = ngli_glEndQuery;
        s_priv->glQueryCounter        = ngli_glQueryCounter;
        s_priv->glGetQueryObjectui64v = ngli_glGetQueryObjectui64v;
    } else if (gl->features & NGLI_FEATURE_GL_EXT_DISJOINT_TIMER_QUERY) {
        s_priv->glGenQueries          = ngli_glGenQueriesEXT;
        s_priv->glDeleteQueries       = ngli_glDeleteQueriesEXT;
        s_priv->glBeginQuery          = ngli_glBeginQueryEXT;
        s_priv->glEndQuery            = ngli_glEndQueryEXT;
        s_priv->glQueryCounter        = ngli_glQueryCounterEXT;
        s_priv->glGetQueryObjectui64v = ngli_glGetQueryObjectui64vEXT;
    } else {
        s_priv->glGenQueries          = (void *)noop;
        s_priv->glDeleteQueries       = (void *)noop;
        s_priv->glBeginQuery          = (void *)noop;
        s_priv->glEndQuery            = (void *)noop;
        s_priv->glQueryCounter        = (void *)noop;
        s_priv->glGetQueryObjectui64v = (void *)noop;
    }
    s_priv->glGenQueries(gl, 2, s_priv->queries);

    return 0;
}

static void timer_reset(struct gpu_ctx *s)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    struct glcontext *gl = s_priv->glcontext;

    if (s_priv->glDeleteQueries)
        s_priv->glDeleteQueries(gl, 2, s_priv->queries);
}

static struct gpu_ctx *gl_create(const struct ngl_config *config)
{
    struct gpu_ctx_gl *s = ngli_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    return (struct gpu_ctx *)s;
}

#if DEBUG_GL
#define GL_DEBUG_LOG(log_level, ...) ngli_log_print(log_level, __FILE__, __LINE__, __func__, __VA_ARGS__)

static void NGLI_GL_APIENTRY gl_debug_message_callback(GLenum source,
                                                       GLenum type,
                                                       GLuint id,
                                                       GLenum severity,
                                                       GLsizei length,
                                                       const GLchar *message,
                                                       const void *user_param)
{
    const int log_level = type == GL_DEBUG_TYPE_ERROR ? NGL_LOG_ERROR : NGL_LOG_DEBUG;
    const char *msg_type = type == GL_DEBUG_TYPE_ERROR ? "ERROR" : "GENERAL";
    GL_DEBUG_LOG(log_level, "%s: %s", msg_type, message);
}
#endif

static const struct {
    uint64_t feature;
    uint64_t feature_gl;
} feature_map[] = {
    {NGLI_FEATURE_COMPUTE,                      NGLI_FEATURE_GL_COMPUTE_SHADER_ALL},
    {NGLI_FEATURE_INSTANCED_DRAW,               NGLI_FEATURE_GL_DRAW_INSTANCED | NGLI_FEATURE_GL_INSTANCED_ARRAY},
    {NGLI_FEATURE_COLOR_RESOLVE,                NGLI_FEATURE_GL_FRAMEBUFFER_OBJECT},
    {NGLI_FEATURE_SHADER_TEXTURE_LOD,           NGLI_FEATURE_GL_SHADER_TEXTURE_LOD},
    {NGLI_FEATURE_SOFTWARE,                     NGLI_FEATURE_GL_SOFTWARE},
    {NGLI_FEATURE_TEXTURE_3D,                   NGLI_FEATURE_GL_TEXTURE_3D},
    {NGLI_FEATURE_TEXTURE_CUBE_MAP,             NGLI_FEATURE_GL_TEXTURE_CUBE_MAP},
    {NGLI_FEATURE_TEXTURE_NPOT,                 NGLI_FEATURE_GL_TEXTURE_NPOT},
    {NGLI_FEATURE_UINT_UNIFORMS,                NGLI_FEATURE_GL_UINT_UNIFORMS},
    {NGLI_FEATURE_UNIFORM_BUFFER,               NGLI_FEATURE_GL_UNIFORM_BUFFER_OBJECT},
    {NGLI_FEATURE_STORAGE_BUFFER,               NGLI_FEATURE_GL_SHADER_STORAGE_BUFFER_OBJECT},
    {NGLI_FEATURE_DEPTH_STENCIL_RESOLVE,        NGLI_FEATURE_GL_FRAMEBUFFER_OBJECT},
    {NGLI_FEATURE_TEXTURE_FLOAT_RENDERABLE,     NGLI_FEATURE_GL_COLOR_BUFFER_FLOAT},
    {NGLI_FEATURE_TEXTURE_HALF_FLOAT_RENDERABLE,NGLI_FEATURE_GL_COLOR_BUFFER_HALF_FLOAT},
};

static void gpu_ctx_info_init(struct gpu_ctx *s)
{
    const struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    const struct glcontext *gl = s_priv->glcontext;

    s->version = gl->version;
    s->language_version = gl->glsl_version;
    for (int i = 0; i < NGLI_ARRAY_NB(feature_map); i++) {
        const uint64_t feature = feature_map[i].feature;
        const uint64_t feature_gl = feature_map[i].feature_gl;
        if ((gl->features & feature_gl) == feature_gl)
            s->features |= feature;
    }
    s->limits = gl->limits;
}

static int gl_init(struct gpu_ctx *s)
{
    int ret;
    struct ngl_config *config = &s->config;
    const struct ngl_config_gl *config_gl = config->backend_config;
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;

    const int external = config_gl ? config_gl->external : 0;
    if (external) {
        if (config->width <= 0 || config->height <= 0) {
            LOG(ERROR, "could not create external context with invalid dimensions (%dx%d)",
                config->width, config->height);
            return NGL_ERROR_INVALID_ARG;
        }
        if (config->capture_buffer) {
            LOG(ERROR, "capture_buffer is not supported by external context");
            return NGL_ERROR_INVALID_ARG;
        }
    } else if (config->offscreen) {
        if (config->width <= 0 || config->height <= 0) {
            LOG(ERROR, "could not create offscreen context with invalid dimensions (%dx%d)",
                config->width, config->height);
            return NGL_ERROR_INVALID_ARG;
        }
    } else {
        if (config->capture_buffer) {
            LOG(ERROR, "capture_buffer is not supported by onscreen context");
            return NGL_ERROR_INVALID_ARG;
        }
    }

#if DEBUG_GPU_CAPTURE
    const char *var = getenv("NGL_GPU_CAPTURE");
    s->gpu_capture = var && !strcmp(var, "yes");
    if (s->gpu_capture) {
        s->gpu_capture_ctx = ngli_gpu_capture_ctx_create(s);
        if (!s->gpu_capture_ctx) {
            LOG(ERROR, "could not create GPU capture context");
            return NGL_ERROR_MEMORY;
        }
        ret = ngli_gpu_capture_init(s->gpu_capture_ctx);
        if (ret < 0) {
            LOG(ERROR, "could not initialize GPU capture");
            s->gpu_capture = 0;
            return ret;
        }
    }
#endif

    const struct glcontext_params params = {
        .platform      = config->platform,
        .backend       = config->backend,
        .external      = external,
        .display       = config->display,
        .window        = config->window,
        .swap_interval = config->swap_interval,
        .offscreen     = config->offscreen,
        .width         = config->width,
        .height        = config->height,
        .samples       = config->samples,
    };

    s_priv->glcontext = ngli_glcontext_new(&params);
    if (!s_priv->glcontext)
        return NGL_ERROR_MEMORY;

    struct glcontext *gl = s_priv->glcontext;

#if DEBUG_GL
    if ((gl->features & NGLI_FEATURE_GL_KHR_DEBUG)) {
        ngli_glEnable(gl, GL_DEBUG_OUTPUT_SYNCHRONOUS);
        ngli_glDebugMessageCallback(gl, gl_debug_message_callback, NULL);
    }
#endif

#if DEBUG_GPU_CAPTURE
    if (s->gpu_capture)
        ngli_gpu_capture_begin(s->gpu_capture_ctx);
#endif

    if (external) {
        ret = ngli_gpu_ctx_gl_wrap_framebuffer(s, config_gl->external_framebuffer);
    } else if (gl->offscreen) {
        ret = offscreen_rendertarget_init(s);
    } else {
        /* Sync context config dimensions with glcontext (swapchain) dimensions */
        config->width = gl->width;
        config->height = gl->height;
        ret = onscreen_rendertarget_init(s);
    }
    if (ret < 0)
        return ret;

    ret = timer_init(s);
    if (ret < 0)
        return ret;

    gpu_ctx_info_init(s);

    s_priv->default_rt_desc.samples = gl->samples;
    s_priv->default_rt_desc.nb_colors = 1;
    s_priv->default_rt_desc.colors[0].format = NGLI_FORMAT_R8G8B8A8_UNORM;
    s_priv->default_rt_desc.colors[0].resolve = gl->samples > 1;
    s_priv->default_rt_desc.depth_stencil.format = NGLI_FORMAT_D24_UNORM_S8_UINT;
    s_priv->default_rt_desc.depth_stencil.resolve = gl->samples > 1;

    ngli_glstate_reset(gl, &s_priv->glstate);

    const int *viewport = config->viewport;
    if (viewport[2] > 0 && viewport[3] > 0) {
        gl_set_viewport(s, viewport);
    } else {
        const int default_viewport[] = {0, 0, config->width, config->height};
        gl_set_viewport(s, default_viewport);
    }

    const GLint scissor[] = {0, 0, config->width, config->height};
    gl_set_scissor(s, scissor);

    return 0;
}

static int gl_resize(struct gpu_ctx *s, int width, int height, const int *viewport)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    struct glcontext *gl = s_priv->glcontext;
    struct ngl_config *config = &s->config;
    struct ngl_config_gl *config_gl = config->backend_config;
    const int external = config_gl ? config_gl->external : 0;

    if (external) {
        config->width = width;
        config->height = height;
    } else if (!config->offscreen) {
        int ret = ngli_glcontext_resize(gl, width, height);
        if (ret < 0)
            return ret;
        config->width = gl->width;
        config->height = gl->height;
    } else {
        LOG(ERROR, "resize operation is not supported by offscreen context");
        return NGL_ERROR_UNSUPPORTED;
    }

    s_priv->default_rt->width = config->width;
    s_priv->default_rt->height = config->height;
    s_priv->default_rt_load->width = config->width;
    s_priv->default_rt_load->height = config->height;

    if (!external) {
        /*
        * The default framebuffer id can change after a resize operation on EAGL,
        * thus we need to update the rendertargets wrapping the default framebuffer
        */
        struct rendertarget_gl *rt_gl = (struct rendertarget_gl *)s_priv->default_rt;
        struct rendertarget_gl *rt_load_gl = (struct rendertarget_gl *)s_priv->default_rt_load;
        rt_gl->id = rt_load_gl->id = ngli_glcontext_get_default_framebuffer(gl);
    }

    if (viewport && viewport[2] > 0 && viewport[3] > 0) {
        gl_set_viewport(s, viewport);
    } else {
        const int default_viewport[] = {0, 0, config->width, config->height};
        gl_set_viewport(s, default_viewport);
    }

    const int scissor[] = {0, 0, config->width, config->height};
    gl_set_scissor(s, scissor);

    return 0;
}

#if defined(TARGET_IPHONE) || defined(TARGET_DARWIN)
static int update_capture_cvpixelbuffer(struct gpu_ctx *s, CVPixelBufferRef capture_buffer)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;

    ngli_rendertarget_freep(&s_priv->default_rt);
    ngli_rendertarget_freep(&s_priv->default_rt_load);
    ngli_texture_freep(&s_priv->color);
    reset_capture_cvpixelbuffer(s);

    if (capture_buffer) {
        s_priv->capture_cvbuffer = (CVPixelBufferRef)CFRetain(capture_buffer);
        int ret = wrap_capture_cvpixelbuffer(s, s_priv->capture_cvbuffer,
                                             &s_priv->color, &s_priv->capture_cvtexture);
        if (ret < 0)
            return ret;
    } else {
        int ret = create_texture(s, NGLI_FORMAT_R8G8B8A8_UNORM, 0, &s_priv->color);
        if (ret < 0)
            return ret;
    }

    struct texture *color         = s_priv->ms_color ? s_priv->ms_color : s_priv->color;
    struct texture *resolve_color = s_priv->ms_color ? s_priv->color : NULL;
    struct texture *depth_stencil = s_priv->depth_stencil;

    int ret;
    if ((ret = create_rendertarget(s, color, resolve_color, depth_stencil,
                                   NGLI_LOAD_OP_CLEAR, &s_priv->default_rt)) < 0 ||
        (ret = create_rendertarget(s, color, resolve_color, depth_stencil,
                                   NGLI_LOAD_OP_LOAD, &s_priv->default_rt_load)) < 0)
        return ret;

    ngli_texture_freep(&s_priv->color);
    return 0;
}
#endif

static int gl_set_capture_buffer(struct gpu_ctx *s, void *capture_buffer)
{
    struct ngl_config *config = &s->config;
    const struct ngl_config_gl *config_gl = config->backend_config;
    const int external = config_gl ? config_gl->external : 0;

    if (external) {
        LOG(ERROR, "capture_buffer is not supported by external context");
        return NGL_ERROR_UNSUPPORTED;
    }

    if (!config->offscreen) {
        LOG(ERROR, "capture_buffer is not supported by onscreen context");
        return NGL_ERROR_UNSUPPORTED;
    }

    if (config->capture_buffer_type == NGL_CAPTURE_BUFFER_TYPE_COREVIDEO) {
#if defined(TARGET_IPHONE) || defined(TARGET_DARWIN)
        int ret = update_capture_cvpixelbuffer(s, capture_buffer);
        if (ret < 0)
            return ret;
#else
        return NGL_ERROR_UNSUPPORTED;
#endif
    }

    config->capture_buffer = capture_buffer;

    return 0;
}

int ngli_gpu_ctx_gl_make_current(struct gpu_ctx *s)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    struct glcontext *gl = s_priv->glcontext;
    return ngli_glcontext_make_current(gl, 1);
}

int ngli_gpu_ctx_gl_release_current(struct gpu_ctx *s)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    struct glcontext *gl = s_priv->glcontext;
    return ngli_glcontext_make_current(gl, 0);
}

void ngli_gpu_ctx_gl_reset_state(struct gpu_ctx *s)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    struct glcontext *gl = s_priv->glcontext;
    ngli_glstate_reset(gl, &s_priv->glstate);
}

int ngli_gpu_ctx_gl_wrap_framebuffer(struct gpu_ctx *s, GLuint fbo)
{
    struct ngl_config *config = &s->config;
    struct ngl_config_gl *config_gl = config->backend_config;
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    struct glcontext *gl = s_priv->glcontext;

    const int external = config_gl ? config_gl->external : 0;
    if (!external) {
        LOG(ERROR, "wrapping external OpenGL framebuffers is not supported by context");
        return NGL_ERROR_UNSUPPORTED;
    }

    /*
     * NOTE: OpenGLES 2.0 cannot query the default framebuffer using
     * glGetFramebufferAttachmentParameteriv() and thus would require a
     * specific code path to perform the relevant sanity checks. For now, we
     * simply disable those checks on OpenGLES 2.0 (through the
     * NGLI_FEATURE_GL_FRAMEBUFFER_OBJECT requirement).
     */
    if (gl->features & NGLI_FEATURE_GL_FRAMEBUFFER_OBJECT) {
        GLuint prev_fbo = 0;
        ngli_glGetIntegerv(gl, GL_DRAW_FRAMEBUFFER_BINDING, (GLint *)&prev_fbo);

        const GLenum target = GL_DRAW_FRAMEBUFFER;
        ngli_glBindFramebuffer(gl, target, fbo);

        const int es = config->backend == NGL_BACKEND_OPENGLES;
        const GLenum default_color_attachment = es ? GL_BACK : GL_FRONT_LEFT;
        const GLenum color_attachment   = fbo ? GL_COLOR_ATTACHMENT0  : default_color_attachment;
        const GLenum depth_attachment   = fbo ? GL_DEPTH_ATTACHMENT   : GL_DEPTH;
        const GLenum stencil_attachment = fbo ? GL_STENCIL_ATTACHMENT : GL_STENCIL;
        const struct {
            const char *buffer_name;
            const char *component_name;
            GLenum attachment;
            const GLenum property;
        } components[] = {
            {"color",   "red",     color_attachment,   GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE},
            {"color",   "green",   color_attachment,   GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE},
            {"color",   "blue",    color_attachment,   GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE},
            {"color",   "alpha",   color_attachment,   GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE},
            {"depth",   "depth",   depth_attachment,   GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE},
            {"stencil", "stencil", stencil_attachment, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE},
        };
        for (int i = 0; i < NGLI_ARRAY_NB(components); i++) {
            GLint type = 0;
            ngli_glGetFramebufferAttachmentParameteriv(gl, target,
                components[i].attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
            if (!type) {
                LOG(ERROR, "external framebuffer have no %s buffer attached to it", components[i].buffer_name);
                ngli_glBindFramebuffer(gl, target, prev_fbo);
                return NGL_ERROR_GRAPHICS_UNSUPPORTED;
            }

            GLint size = 0;
            ngli_glGetFramebufferAttachmentParameteriv(gl, target,
                components[i].attachment, components[i].property, &size);
            if (!size) {
                LOG(ERROR, "external framebuffer have no %s component", components[i].component_name);
                ngli_glBindFramebuffer(gl, target, prev_fbo);
                return NGL_ERROR_GRAPHICS_UNSUPPORTED;
            }
        }

        ngli_glBindFramebuffer(gl, target, prev_fbo);
    }

    ngli_rendertarget_freep(&s_priv->default_rt);
    ngli_rendertarget_freep(&s_priv->default_rt_load);

    int ret;
    if ((ret = create_rendertarget(s, NULL, NULL, NULL,
                                   NGLI_LOAD_OP_CLEAR, &s_priv->default_rt)) < 0 ||
        (ret = create_rendertarget(s, NULL, NULL, NULL,
                                   NGLI_LOAD_OP_LOAD, &s_priv->default_rt_load)) < 0)
        return ret;

    config_gl->external_framebuffer = fbo;

    return 0;
}

static int gl_begin_update(struct gpu_ctx *s, double t)
{
    return 0;
}

static int gl_end_update(struct gpu_ctx *s, double t)
{
    return 0;
}

static int gl_begin_draw(struct gpu_ctx *s, double t)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    struct glcontext *gl = s_priv->glcontext;
    const struct ngl_config *config = &s->config;

    if (config->hud)
#if defined(TARGET_DARWIN)
        s_priv->glBeginQuery(gl, GL_TIME_ELAPSED, s_priv->queries[0]);
#else
        s_priv->glQueryCounter(gl, s_priv->queries[0], GL_TIMESTAMP);
#endif

    return 0;
}

static int gl_end_draw(struct gpu_ctx *s, double t)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    struct glcontext *gl = s_priv->glcontext;
    const struct ngl_config *config = &s->config;
    const struct ngl_config_gl *config_gl = config->backend_config;

    if (s_priv->capture_func && config->capture_buffer)
        s_priv->capture_func(s);

    int ret = ngli_glcontext_check_gl_error(gl, __func__);

    const int external = config_gl ? config_gl->external : 0;
    if (!external && !config->offscreen) {
        if (config->set_surface_pts)
            ngli_glcontext_set_surface_pts(gl, t);

        ngli_glcontext_swap_buffers(gl);
    }

    return ret;
}

static int gl_query_draw_time(struct gpu_ctx *s, int64_t *time)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    struct glcontext *gl = s_priv->glcontext;

    const struct ngl_config *config = &s->config;
    if (!config->hud)
        return NGL_ERROR_INVALID_USAGE;

#if defined(TARGET_DARWIN)
    GLuint64 time_elapsed = 0;
    s_priv->glEndQuery(gl, GL_TIME_ELAPSED);
    s_priv->glGetQueryObjectui64v(gl, s_priv->queries[0], GL_QUERY_RESULT, &time_elapsed);
    *time = time_elapsed;
#else
    s_priv->glQueryCounter(gl, s_priv->queries[1], GL_TIMESTAMP);

    GLuint64 start_time = 0;
    s_priv->glGetQueryObjectui64v(gl, s_priv->queries[0], GL_QUERY_RESULT, &start_time);

    GLuint64 end_time = 0;
    s_priv->glGetQueryObjectui64v(gl, s_priv->queries[1], GL_QUERY_RESULT, &end_time);

    *time = end_time - start_time;
#endif
    return 0;
}

static void gl_wait_idle(struct gpu_ctx *s)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    struct glcontext *gl = s_priv->glcontext;
    ngli_glFinish(gl);
}

static void gl_destroy(struct gpu_ctx *s)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    timer_reset(s);
    rendertarget_reset(s);
#if DEBUG_GPU_CAPTURE
    if (s->gpu_capture)
        ngli_gpu_capture_end(s->gpu_capture_ctx);
    ngli_gpu_capture_freep(&s->gpu_capture_ctx);
#endif
    ngli_glcontext_freep(&s_priv->glcontext);
}

static int gl_transform_cull_mode(struct gpu_ctx *s, int cull_mode)
{
    const struct ngl_config *config = &s->config;
    if (!config->offscreen)
        return cull_mode;
    static const int cull_mode_map[NGLI_CULL_MODE_NB] = {
        [NGLI_CULL_MODE_NONE]      = NGLI_CULL_MODE_NONE,
        [NGLI_CULL_MODE_FRONT_BIT] = NGLI_CULL_MODE_BACK_BIT,
        [NGLI_CULL_MODE_BACK_BIT]  = NGLI_CULL_MODE_FRONT_BIT,
    };
    return cull_mode_map[cull_mode];
}

static void gl_transform_projection_matrix(struct gpu_ctx *s, float *dst)
{
    const struct ngl_config *config = &s->config;
    if (!config->offscreen)
        return;
    static const NGLI_ALIGNED_MAT(matrix) = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f,-1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    ngli_mat4_mul(dst, matrix, dst);
}

static void gl_get_rendertarget_uvcoord_matrix(struct gpu_ctx *s, float *dst)
{
    const struct ngl_config *config = &s->config;
    if (config->offscreen)
        return;
    static const NGLI_ALIGNED_MAT(matrix) = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f,-1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 1.0f,
    };
    memcpy(dst, matrix, 4 * 4 * sizeof(float));
}

static struct rendertarget *gl_get_default_rendertarget(struct gpu_ctx *s, int load_op)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    switch (load_op) {
    case NGLI_LOAD_OP_DONT_CARE:
    case NGLI_LOAD_OP_CLEAR:
        return s_priv->default_rt;
    case NGLI_LOAD_OP_LOAD:
        return s_priv->default_rt_load;
    default:
        ngli_assert(0);
    }
}

static const struct rendertarget_desc *gl_get_default_rendertarget_desc(struct gpu_ctx *s)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;
    return &s_priv->default_rt_desc;
}

static void gl_begin_render_pass(struct gpu_ctx *s, struct rendertarget *rt)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;

    ngli_assert(rt && !s_priv->current_rt);
    ngli_rendertarget_gl_begin_pass(rt);
    s_priv->current_rt = rt;
}

static void gl_end_render_pass(struct gpu_ctx *s)
{
    struct gpu_ctx_gl *s_priv = (struct gpu_ctx_gl *)s;

    ngli_assert(s_priv->current_rt);
    ngli_rendertarget_gl_end_pass(s_priv->current_rt);
    s_priv->current_rt = NULL;
}

static int gl_get_preferred_depth_format(struct gpu_ctx *s)
{
    return NGLI_FORMAT_D16_UNORM;
}

static int gl_get_preferred_depth_stencil_format(struct gpu_ctx *s)
{
    return NGLI_FORMAT_D24_UNORM_S8_UINT;
}

#define DECLARE_GPU_CTX_CLASS(cls_suffix, cls_name)                              \
const struct gpu_ctx_class ngli_gpu_ctx_##cls_suffix = {                         \
    .name                               = cls_name,                              \
    .create                             = gl_create,                             \
    .init                               = gl_init,                               \
    .resize                             = gl_resize,                             \
    .set_capture_buffer                 = gl_set_capture_buffer,                 \
    .begin_update                       = gl_begin_update,                       \
    .end_update                         = gl_end_update,                         \
    .begin_draw                         = gl_begin_draw,                         \
    .end_draw                           = gl_end_draw,                           \
    .query_draw_time                    = gl_query_draw_time,                    \
    .wait_idle                          = gl_wait_idle,                          \
    .destroy                            = gl_destroy,                            \
                                                                                 \
    .transform_cull_mode                = gl_transform_cull_mode,                \
    .transform_projection_matrix        = gl_transform_projection_matrix,        \
    .get_rendertarget_uvcoord_matrix    = gl_get_rendertarget_uvcoord_matrix,    \
                                                                                 \
    .get_default_rendertarget           = gl_get_default_rendertarget,           \
    .get_default_rendertarget_desc      = gl_get_default_rendertarget_desc,      \
                                                                                 \
    .begin_render_pass                  = gl_begin_render_pass,                  \
    .end_render_pass                    = gl_end_render_pass,                    \
                                                                                 \
    .set_viewport                       = gl_set_viewport,                       \
    .get_viewport                       = gl_get_viewport,                       \
    .set_scissor                        = gl_set_scissor,                        \
    .get_scissor                        = gl_get_scissor,                        \
    .get_preferred_depth_format         = gl_get_preferred_depth_format,         \
    .get_preferred_depth_stencil_format = gl_get_preferred_depth_stencil_format, \
                                                                                 \
    .buffer_create                      = ngli_buffer_gl_create,                 \
    .buffer_init                        = ngli_buffer_gl_init,                   \
    .buffer_upload                      = ngli_buffer_gl_upload,                 \
    .buffer_map                         = ngli_buffer_gl_map,                    \
    .buffer_unmap                       = ngli_buffer_gl_unmap,                  \
    .buffer_freep                       = ngli_buffer_gl_freep,                  \
                                                                                 \
    .pipeline_create                    = ngli_pipeline_gl_create,               \
    .pipeline_init                      = ngli_pipeline_gl_init,                 \
    .pipeline_set_resources             = ngli_pipeline_gl_set_resources,        \
    .pipeline_update_attribute          = ngli_pipeline_gl_update_attribute,     \
    .pipeline_update_uniform            = ngli_pipeline_gl_update_uniform,       \
    .pipeline_update_texture            = ngli_pipeline_gl_update_texture,       \
    .pipeline_update_buffer             = ngli_pipeline_gl_update_buffer,        \
    .pipeline_draw                      = ngli_pipeline_gl_draw,                 \
    .pipeline_draw_indexed              = ngli_pipeline_gl_draw_indexed,         \
    .pipeline_dispatch                  = ngli_pipeline_gl_dispatch,             \
    .pipeline_freep                     = ngli_pipeline_gl_freep,                \
                                                                                 \
    .program_create                     = ngli_program_gl_create,                \
    .program_init                       = ngli_program_gl_init,                  \
    .program_freep                      = ngli_program_gl_freep,                 \
                                                                                 \
    .rendertarget_create                = ngli_rendertarget_gl_create,           \
    .rendertarget_init                  = ngli_rendertarget_gl_init,             \
    .rendertarget_freep                 = ngli_rendertarget_gl_freep,            \
                                                                                 \
    .texture_create                     = ngli_texture_gl_create,                \
    .texture_init                       = ngli_texture_gl_init,                  \
    .texture_upload                     = ngli_texture_gl_upload,                \
    .texture_generate_mipmap            = ngli_texture_gl_generate_mipmap,       \
    .texture_freep                      = ngli_texture_gl_freep,                 \
}                                                                                \

#ifdef BACKEND_GL
DECLARE_GPU_CTX_CLASS(gl,   "OpenGL");
#endif
#ifdef BACKEND_GLES
DECLARE_GPU_CTX_CLASS(gles, "OpenGL ES");
#endif
