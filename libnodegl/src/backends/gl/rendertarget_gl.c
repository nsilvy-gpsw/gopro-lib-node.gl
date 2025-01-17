/*
 * Copyright 2018-2022 GoPro Inc.
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
#include "rendertarget_gl.h"
#include "format.h"
#include "gpu_ctx_gl.h"
#include "glcontext.h"
#include "glincludes.h"
#include "log.h"
#include "memory.h"
#include "internal.h"
#include "texture_gl.h"
#include "utils.h"

static GLenum get_gl_attachment_index(GLenum format)
{
    switch (format) {
    case GL_DEPTH_COMPONENT:
    case GL_DEPTH_COMPONENT16:
    case GL_DEPTH_COMPONENT24:
    case GL_DEPTH_COMPONENT32F:
        return GL_DEPTH_ATTACHMENT;
    case GL_DEPTH_STENCIL:
    case GL_DEPTH24_STENCIL8:
    case GL_DEPTH32F_STENCIL8:
        return GL_DEPTH_STENCIL_ATTACHMENT;
    case GL_STENCIL_INDEX:
    case GL_STENCIL_INDEX8:
        return GL_STENCIL_ATTACHMENT;
    default:
        return GL_COLOR_ATTACHMENT0;
    }
}

static void resolve_no_draw_buffers(struct rendertarget *s)
{
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;

    const int flags = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
    ngli_glBlitFramebuffer(gl, 0, 0, s->width, s->height, 0, 0, s->width, s->height, flags, GL_NEAREST);
}

static void resolve_draw_buffers(struct rendertarget *s)
{
    struct rendertarget_gl *s_priv = (struct rendertarget_gl *)s;
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    struct rendertarget_params *params = &s->params;

    for (int i = 0; i < params->nb_colors; i++) {
        const struct attachment *attachment = &params->colors[i];
        if (!attachment->resolve_target)
            continue;
        GLbitfield flags = GL_COLOR_BUFFER_BIT;
        if (i == 0)
            flags |= GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
        ngli_glReadBuffer(gl, GL_COLOR_ATTACHMENT0 + i);
#if defined(TARGET_DARWIN)
        ngli_glDrawBuffer(gl, GL_COLOR_ATTACHMENT0 + i);
#else
        GLenum draw_buffers[NGLI_MAX_COLOR_ATTACHMENTS] = {0};
        draw_buffers[i] = GL_COLOR_ATTACHMENT0 + i;
        ngli_glDrawBuffers(gl, i + 1, draw_buffers);
#endif
        ngli_glBlitFramebuffer(gl, 0, 0, s->width, s->height, 0, 0, s->width, s->height, flags, GL_NEAREST);
    }
    ngli_glReadBuffer(gl, GL_COLOR_ATTACHMENT0);
    ngli_glDrawBuffers(gl, params->nb_colors, s_priv->draw_buffers);
}

static int create_fbo(struct rendertarget *s, int resolve, GLuint *idp)
{
    int ret = -1;
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    const struct gpu_limits *limits = &gl->limits;
    const struct rendertarget_params *params = &s->params;

    GLuint id = 0;
    int nb_color_attachments = 0;

    ngli_glGenFramebuffers(gl, 1, &id);
    ngli_glBindFramebuffer(gl, GL_FRAMEBUFFER, id);

    for (int i = 0; i < params->nb_colors; i++) {
        const struct attachment *attachment = &params->colors[i];
        const struct texture *texture = resolve ? attachment->resolve_target : attachment->attachment;
        const int layer = resolve ? attachment->resolve_target_layer : attachment->attachment_layer;

        if (!texture)
            continue;

        const struct texture_gl *texture_gl = (const struct texture_gl *)texture;
        GLenum attachment_index = get_gl_attachment_index(texture_gl->format);
        ngli_assert(attachment_index == GL_COLOR_ATTACHMENT0);

        if (nb_color_attachments >= limits->max_color_attachments) {
            LOG(ERROR, "could not attach color buffer %d (maximum %d)",
                nb_color_attachments, limits->max_color_attachments);
            goto fail;
        }
        attachment_index = attachment_index + nb_color_attachments++;

        switch (texture_gl->target) {
        case GL_RENDERBUFFER:
            ngli_glFramebufferRenderbuffer(gl, GL_FRAMEBUFFER, attachment_index, GL_RENDERBUFFER, texture_gl->id);
            break;
        case GL_TEXTURE_RECTANGLE:
            ngli_glFramebufferTexture2D(gl, GL_FRAMEBUFFER, attachment_index, GL_TEXTURE_RECTANGLE, texture_gl->id, 0);
            break;
        case GL_TEXTURE_2D:
            ngli_glFramebufferTexture2D(gl, GL_FRAMEBUFFER, attachment_index, GL_TEXTURE_2D, texture_gl->id, 0);
            break;
        case GL_TEXTURE_CUBE_MAP:
            ngli_glFramebufferTexture2D(gl, GL_FRAMEBUFFER, attachment_index++, GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer, texture_gl->id, 0);
            break;
        default:
            ngli_assert(0);
        }
    }

    const struct attachment *attachment = &params->depth_stencil;
    struct texture *texture = resolve ? attachment->resolve_target : attachment->attachment;
    if (texture) {
        const struct texture_gl *texture_gl = (const struct texture_gl *)texture;
        const GLenum attachment_index = get_gl_attachment_index(texture_gl->format);
        ngli_assert(attachment_index != GL_COLOR_ATTACHMENT0);

        switch (texture_gl->target) {
        case GL_RENDERBUFFER:
            if (gl->backend == NGL_BACKEND_OPENGLES && gl->version < 300 && attachment_index == GL_DEPTH_STENCIL_ATTACHMENT) {
                ngli_glFramebufferRenderbuffer(gl, GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, texture_gl->id);
                ngli_glFramebufferRenderbuffer(gl, GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, texture_gl->id);
            } else {
                ngli_glFramebufferRenderbuffer(gl, GL_FRAMEBUFFER, attachment_index, GL_RENDERBUFFER, texture_gl->id);
            }
            break;
        case GL_TEXTURE_2D:
            if (gl->backend == NGL_BACKEND_OPENGLES && gl->version < 300 && attachment_index == GL_DEPTH_STENCIL_ATTACHMENT) {
                ngli_glFramebufferTexture2D(gl, GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, texture_gl->id, 0);
                ngli_glFramebufferTexture2D(gl, GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, texture_gl->id, 0);
            } else {
                ngli_glFramebufferTexture2D(gl, GL_FRAMEBUFFER, attachment_index, GL_TEXTURE_2D, texture_gl->id, 0);
            }
            break;
        default:
            ngli_assert(0);
        }
    }

    if (ngli_glCheckFramebufferStatus(gl, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOG(ERROR, "framebuffer %u is not complete", id);
        goto fail;
    }

    *idp = id;

    return 0;

fail:
    ngli_glDeleteFramebuffers(gl, 1, &id);
    return ret;
}

static int require_resolve_fbo(struct rendertarget *s)
{
    const struct rendertarget_params *params = &s->params;
    for (int i = 0; i < params->nb_colors; i++) {
        const struct attachment *attachment = &params->colors[i];
        if (attachment->resolve_target)
            return 1;
    }
    return 0;
}

static void clear_buffer(struct rendertarget *s)
{
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    struct rendertarget_gl *s_priv = (struct rendertarget_gl *)s;
    const struct rendertarget_params *params = &s->params;

    if (params->nb_colors >= 1) {
        const struct attachment *color = &params->colors[0];
        const float *clear_value = color->clear_value;
        ngli_glClearColor(gl, clear_value[0], clear_value[1], clear_value[2], clear_value[3]);
        ngli_glClear(gl, s_priv->clear_flags);
    }
}

static void clear_buffers(struct rendertarget *s)
{
    struct rendertarget_gl *s_priv = (struct rendertarget_gl *)s;
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    const struct rendertarget_params *params = &s->params;

    for (int i = 0; i < params->nb_colors; i++) {
        const struct attachment *color = &params->colors[i];
        if (color->load_op != NGLI_LOAD_OP_LOAD) {
            ngli_glClearBufferfv(gl, GL_COLOR, i, color->clear_value);
        }
    }

    if (params->depth_stencil.attachment || s_priv->wrapped) {
        const struct attachment *depth_stencil = &params->depth_stencil;
        if (depth_stencil->load_op != NGLI_LOAD_OP_LOAD) {
            ngli_glClearBufferfi(gl, GL_DEPTH_STENCIL, 0, 1.0f, 0);
        }
    }
}

static void invalidate_noop(struct rendertarget *s)
{
}

static void invalidate(struct rendertarget *s)
{
    struct rendertarget_gl *s_priv = (struct rendertarget_gl *)s;
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    ngli_glInvalidateFramebuffer(gl, GL_FRAMEBUFFER, s_priv->nb_invalidate_attachments, s_priv->invalidate_attachments);
}

struct rendertarget *ngli_rendertarget_gl_create(struct gpu_ctx *gpu_ctx)
{
    struct rendertarget_gl *s = ngli_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->parent.gpu_ctx = gpu_ctx;
    return (struct rendertarget *)s;
}

int ngli_rendertarget_gl_init(struct rendertarget *s, const struct rendertarget_params *params)
{
    struct rendertarget_gl *s_priv = (struct rendertarget_gl *)s;
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    const struct gpu_limits *limits = &gl->limits;

    s->params = *params;
    s->width = params->width;
    s->height = params->height;

    s_priv->wrapped = 0;

    int ret;
    if (require_resolve_fbo(s)) {
        if (!(gl->features & NGLI_FEATURE_GL_FRAMEBUFFER_OBJECT)) {
            LOG(ERROR, "context does not support the framebuffer object feature, "
                       "resolving MSAA attachments is not supported");
            return NGL_ERROR_GRAPHICS_UNSUPPORTED;
        }
        ret = create_fbo(s, 1, &s_priv->resolve_id);
        if (ret < 0)
            goto done;
    }

    ret = create_fbo(s, 0, &s_priv->id);
    if (ret < 0)
        goto done;

    if (gl->features & NGLI_FEATURE_GL_INVALIDATE_SUBDATA) {
        s_priv->invalidate = invalidate;
    } else {
        s_priv->invalidate = invalidate_noop;
    }

    if (gl->features & NGLI_FEATURE_GL_CLEAR_BUFFER) {
        s_priv->clear = clear_buffers;
    } else {
        s_priv->clear = clear_buffer;
    }

    s_priv->resolve = resolve_no_draw_buffers;
    if (gl->features & NGLI_FEATURE_GL_DRAW_BUFFERS) {
        if (params->nb_colors > limits->max_draw_buffers) {
            LOG(ERROR, "draw buffer count (%d) exceeds driver limit (%d)",
                params->nb_colors, limits->max_draw_buffers);
            ret = NGL_ERROR_GRAPHICS_UNSUPPORTED;
            goto done;
        }
        if (params->nb_colors > 1) {
            for (int i = 0; i < params->nb_colors; i++)
                s_priv->draw_buffers[i] = GL_COLOR_ATTACHMENT0 + i;
            ngli_glDrawBuffers(gl, params->nb_colors, s_priv->draw_buffers);
            s_priv->resolve = resolve_draw_buffers;
        }
    }

    for (int i = 0; i < params->nb_colors; i++) {
        const struct attachment *color = &params->colors[i];
        if (color->load_op == NGLI_LOAD_OP_DONT_CARE ||
            color->load_op == NGLI_LOAD_OP_CLEAR) {
            s_priv->clear_flags |= GL_COLOR_BUFFER_BIT;
        }
        if (color->store_op == NGLI_STORE_OP_DONT_CARE) {
            s_priv->invalidate_attachments[s_priv->nb_invalidate_attachments++] = GL_COLOR_ATTACHMENT0 + i;
        }
    }

    const struct attachment *depth_stencil = &params->depth_stencil;
    if (depth_stencil->attachment) {
        if (depth_stencil->load_op == NGLI_LOAD_OP_DONT_CARE ||
            depth_stencil->load_op == NGLI_LOAD_OP_CLEAR) {
            s_priv->clear_flags |= (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        }
        if (depth_stencil->store_op == NGLI_STORE_OP_DONT_CARE) {
            s_priv->invalidate_attachments[s_priv->nb_invalidate_attachments++] = GL_DEPTH_ATTACHMENT;
            s_priv->invalidate_attachments[s_priv->nb_invalidate_attachments++] = GL_STENCIL_ATTACHMENT;
        }
    }

done:;
    struct rendertarget *rt = gpu_ctx_gl->current_rt;
    struct rendertarget_gl *rt_gl = (struct rendertarget_gl *)rt;
    const GLuint fbo_id = rt_gl ? rt_gl->id : ngli_glcontext_get_default_framebuffer(gl);
    ngli_glBindFramebuffer(gl, GL_FRAMEBUFFER, fbo_id);

    return ret;
}

void ngli_rendertarget_gl_begin_pass(struct rendertarget *s)
{
    const struct rendertarget_gl *s_priv = (struct rendertarget_gl *)s;
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    struct glstate *glstate = &gpu_ctx_gl->glstate;

    static const GLboolean default_color_write_mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    if (memcmp(glstate->color_write_mask, default_color_write_mask, sizeof(default_color_write_mask))) {
        ngli_glColorMask(gl, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        memcpy(glstate->color_write_mask, &default_color_write_mask, sizeof(default_color_write_mask));
    }

    if (glstate->depth_write_mask != GL_TRUE) {
        ngli_glDepthMask(gl, GL_TRUE);
        glstate->depth_write_mask = GL_TRUE;
    }

    if (glstate->stencil_write_mask != 0xff) {
        ngli_glStencilMask(gl, 0xff);
        glstate->stencil_write_mask = 0xff;
    }

    if (glstate->scissor_test) {
        ngli_glDisable(gl, GL_SCISSOR_TEST);
        glstate->scissor_test = 0;
    }

    ngli_glBindFramebuffer(gl, GL_FRAMEBUFFER, s_priv->id);

    s_priv->clear(s);
}

void ngli_rendertarget_gl_end_pass(struct rendertarget *s)
{
    const struct rendertarget_gl *s_priv = (const struct rendertarget_gl *)s;
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    struct glstate *glstate = &gpu_ctx_gl->glstate;

    if (glstate->scissor_test) {
        ngli_glDisable(gl, GL_SCISSOR_TEST);
        glstate->scissor_test = 0;
    }

    if (s_priv->resolve_id) {
        ngli_glBindFramebuffer(gl, GL_READ_FRAMEBUFFER, s_priv->id);
        ngli_glBindFramebuffer(gl, GL_DRAW_FRAMEBUFFER, s_priv->resolve_id);

        s_priv->resolve(s);

        struct rendertarget *rt = gpu_ctx_gl->current_rt;
        struct rendertarget_gl *rt_gl = (struct rendertarget_gl *)rt;
        const GLuint fbo_id = rt_gl ? rt_gl->id : ngli_glcontext_get_default_framebuffer(gl);
        ngli_glBindFramebuffer(gl, GL_FRAMEBUFFER, fbo_id);
    }

    s_priv->invalidate(s);
}

void ngli_rendertarget_gl_freep(struct rendertarget **sp)
{
    if (!*sp)
        return;

    struct rendertarget *s = *sp;
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;
    struct rendertarget_gl *s_priv = (struct rendertarget_gl *)s;

    if (!s_priv->wrapped) {
        ngli_glDeleteFramebuffers(gl, 1, &s_priv->id);
        ngli_glDeleteFramebuffers(gl, 1, &s_priv->resolve_id);
    }

    ngli_freep(sp);
}

int ngli_rendertarget_gl_wrap(struct rendertarget *s, const struct rendertarget_params *params, GLuint id)
{
    struct rendertarget_gl *s_priv = (struct rendertarget_gl *)s;
    struct gpu_ctx_gl *gpu_ctx_gl = (struct gpu_ctx_gl *)s->gpu_ctx;
    struct glcontext *gl = gpu_ctx_gl->glcontext;

    ngli_assert(params->nb_colors == 1);
    ngli_assert(!params->colors[0].attachment);
    ngli_assert(!params->colors[0].resolve_target);
    ngli_assert(!params->depth_stencil.attachment);
    ngli_assert(!params->depth_stencil.resolve_target);

    s->params = *params;
    s->width = params->width;
    s->height = params->height;

    s_priv->wrapped = 1;
    s_priv->id = id;

    if (gl->features & NGLI_FEATURE_GL_INVALIDATE_SUBDATA) {
        s_priv->invalidate = invalidate;
    } else {
        s_priv->invalidate = invalidate_noop;
    }

    if (gl->features & NGLI_FEATURE_GL_CLEAR_BUFFER) {
        s_priv->clear = clear_buffers;
    } else {
        s_priv->clear = clear_buffer;
    }

    s_priv->resolve = resolve_no_draw_buffers;

    const struct attachment *color = &params->colors[0];
    if (color->load_op == NGLI_LOAD_OP_DONT_CARE ||
        color->load_op == NGLI_LOAD_OP_CLEAR) {
        s_priv->clear_flags |= GL_COLOR_BUFFER_BIT;
    }
    if (color->store_op == NGLI_STORE_OP_DONT_CARE) {
        s_priv->invalidate_attachments[s_priv->nb_invalidate_attachments++] = s_priv->id ? GL_COLOR_ATTACHMENT0 : GL_COLOR;
    }

    const struct attachment *depth_stencil = &params->depth_stencil;
    if (depth_stencil->load_op == NGLI_LOAD_OP_DONT_CARE ||
        depth_stencil->load_op == NGLI_LOAD_OP_CLEAR) {
        s_priv->clear_flags |= (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    if (depth_stencil->store_op == NGLI_STORE_OP_DONT_CARE) {
        s_priv->invalidate_attachments[s_priv->nb_invalidate_attachments++] = s_priv->id ? GL_DEPTH_ATTACHMENT : GL_DEPTH;
        s_priv->invalidate_attachments[s_priv->nb_invalidate_attachments++] = s_priv->id ? GL_STENCIL_ATTACHMENT : GL_STENCIL;
    }

    return 0;
}
