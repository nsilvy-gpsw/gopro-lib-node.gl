/*
 * Copyright 2016-2022 GoPro Inc.
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

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "config.h"

#if defined(TARGET_ANDROID)
#include <jni.h>

#include "jni_utils.h"
#endif

#include "darray.h"
#include "gpu_ctx.h"
#include "graphicstate.h"
#include "log.h"
#include "math_utils.h"
#include "memory.h"
#include "nodegl.h"
#include "internal.h"
#include "pgcache.h"
#include "rnode.h"
#include "pthread_compat.h"

#if defined(HAVE_VAAPI)
#include "vaapi_ctx.h"
#endif

#if defined(TARGET_IPHONE) || defined(TARGET_ANDROID)
# define DEFAULT_BACKEND NGL_BACKEND_OPENGLES
#else
# define DEFAULT_BACKEND NGL_BACKEND_OPENGL
#endif

extern const struct api_impl api_gl;
extern const struct api_impl api_vk;

static const struct {
    const struct api_impl *api_impl;
} api_map[NGL_NB_BACKEND] = {
#ifdef BACKEND_GL
    [NGL_BACKEND_OPENGL] = {
        .api_impl = &api_gl,
    },
#endif
#ifdef BACKEND_GLES
    [NGL_BACKEND_OPENGLES] = {
        .api_impl = &api_gl,
    },
#endif
#ifdef BACKEND_VK
    [NGL_BACKEND_VULKAN] = {
        .api_impl = &api_vk,
    },
#endif
};

const char* ngli_get_backend_string_id(int backend)
{
    switch (backend) {
    case NGL_BACKEND_OPENGL:    return "opengl";
    case NGL_BACKEND_OPENGLES:  return "opengles";
    case NGL_BACKEND_VULKAN:    return "vulkan";
    }
    ngli_assert(0);
}

void ngl_log_set_callback(void *arg, ngl_log_callback_type callback)
{
    ngli_log_set_callback(arg, callback);
}

void ngl_log_set_min_level(int level)
{
    ngli_log_set_min_level(level);
}

static int get_default_platform(void)
{
#if defined(TARGET_LINUX)
    return NGL_PLATFORM_XLIB;
#elif defined(TARGET_IPHONE)
    return NGL_PLATFORM_IOS;
#elif defined(TARGET_DARWIN)
    return NGL_PLATFORM_MACOS;
#elif defined(TARGET_ANDROID)
    return NGL_PLATFORM_ANDROID;
#elif defined(TARGET_WINDOWS)
    return NGL_PLATFORM_WINDOWS;
#else
    return NGL_ERROR_UNSUPPORTED;
#endif
}

static int cmd_stop(struct ngl_ctx *s, void *arg)
{
    return 0;
}

static void reset_scene(struct ngl_ctx *s, int action)
{
    ngli_hud_freep(&s->hud);
    if (s->scene) {
        ngli_node_detach_ctx(s->scene, s);
        if (action == NGLI_ACTION_UNREF_SCENE)
            ngl_node_unrefp(&s->scene);
    }
    ngli_rnode_reset(&s->rnode);
}

int ngli_ctx_set_scene(struct ngl_ctx *s, struct ngl_node *scene)
{
    int ret = 0;

    ngli_gpu_ctx_wait_idle(s->gpu_ctx);
    reset_scene(s, NGLI_ACTION_UNREF_SCENE);

    ngli_rnode_init(&s->rnode);
    s->rnode_pos = &s->rnode;
    s->rnode_pos->graphicstate = NGLI_GRAPHICSTATE_DEFAULTS;
    s->rnode_pos->rendertarget_desc = *ngli_gpu_ctx_get_default_rendertarget_desc(s->gpu_ctx);

    if (scene) {
        int ret = ngli_node_attach_ctx(scene, s);
        if (ret < 0) {
            ngli_node_detach_ctx(scene, s);
            return ret;
        }
        s->scene = ngl_node_ref(scene);
    }

    const struct ngl_config *config = &s->config;
    if (config->hud) {
        s->hud = ngli_hud_create(s);
        if (!s->hud) {
            ret = NGL_ERROR_MEMORY;
            goto fail;
        }

        int ret = ngli_hud_init(s->hud);
        if (ret < 0)
            goto fail;
    }

    return 0;

fail:
    reset_scene(s, NGLI_ACTION_UNREF_SCENE);
    return ret;
}

void ngli_ctx_reset(struct ngl_ctx *s, int action)
{
    if (s->gpu_ctx)
        ngli_gpu_ctx_wait_idle(s->gpu_ctx);
    reset_scene(s, action);
#if defined(HAVE_VAAPI)
    ngli_vaapi_ctx_reset(&s->vaapi_ctx);
#endif
#if defined(TARGET_ANDROID)
    ngli_android_ctx_reset(&s->android_ctx);
#endif
    ngli_texture_freep(&s->font_atlas); // allocated by the first node text
    ngli_pgcache_reset(&s->pgcache);
    ngli_gpu_ctx_freep(&s->gpu_ctx);
    ngli_config_reset(&s->config);
}

int ngli_ctx_configure(struct ngl_ctx *s, const struct ngl_config *config)
{
    int reset_param = NGLI_ACTION_KEEP_SCENE;

    int ret = ngli_config_copy(&s->config, config);
    if (ret < 0)
        return ret;

    s->gpu_ctx = ngli_gpu_ctx_create(config);
    if (!s->gpu_ctx) {
        ngli_config_reset(&s->config);
        return NGL_ERROR_MEMORY;
    }

    ret = ngli_gpu_ctx_init(s->gpu_ctx);
    if (ret < 0) {
        LOG(ERROR, "could not initialize gpu context: %s", NGLI_RET_STR(ret));
        ngli_gpu_ctx_freep(&s->gpu_ctx);
        ngli_config_reset(&s->config);
        return ret;
    }

    ret = ngli_pgcache_init(&s->pgcache, s->gpu_ctx);
    if (ret < 0)
        goto fail;

#if defined(HAVE_VAAPI)
    ret = ngli_vaapi_ctx_init(s->gpu_ctx, &s->vaapi_ctx);
    if (ret < 0)
        LOG(WARNING, "could not initialize vaapi context");
#endif

#if defined(TARGET_ANDROID)
    struct android_ctx *android_ctx = &s->android_ctx;
    ret = ngli_android_ctx_init(s->gpu_ctx, android_ctx);
    if (ret < 0)
        LOG(WARNING, "could not initialize Android context");
#endif

    NGLI_ALIGNED_MAT(matrix) = NGLI_MAT4_IDENTITY;
    ngli_gpu_ctx_transform_projection_matrix(s->gpu_ctx, matrix);
    ngli_darray_clear(&s->projection_matrix_stack);
    if (!ngli_darray_push(&s->projection_matrix_stack, matrix)) {
        ret = NGL_ERROR_MEMORY;
        goto fail;
    }

    struct ngl_node *old_scene = s->scene; // note: the old scene is detached
    s->scene = NULL; // make sure the old scene is not unreferenced by set_scene()
    ret = ngli_ctx_set_scene(s, old_scene);
    if (ret < 0) {
        s->scene = old_scene; // restore detached scene on error
        goto fail;
    }
    ngl_node_unrefp(&old_scene); // set_scene() incremented the reference, so we drop the old one

    return 0;

fail:
    ngli_ctx_reset(s, reset_param);
    return ret;
}

int ngli_ctx_resize(struct ngl_ctx *s, int width, int height, const int *viewport)
{
    return ngli_gpu_ctx_resize(s->gpu_ctx, width, height, viewport);
}

int ngli_ctx_set_capture_buffer(struct ngl_ctx *s, void *capture_buffer)
{
    struct ngl_config *config = &s->config;

    int ret = ngli_gpu_ctx_set_capture_buffer(s->gpu_ctx, capture_buffer);
    if (ret < 0) {
        ngli_ctx_reset(s, NGLI_ACTION_KEEP_SCENE);
        return ret;
    }

    config->capture_buffer = capture_buffer;

    return 0;
}

int ngli_ctx_prepare_draw(struct ngl_ctx *s, double t)
{
    const int64_t start_time = s->hud ? ngli_gettime_relative() : 0;

    int ret = ngli_gpu_ctx_begin_update(s->gpu_ctx, t);
    if (ret < 0)
        return ret;

    struct ngl_node *scene = s->scene;
    if (!scene) {
        return ngli_gpu_ctx_end_update(s->gpu_ctx, t);
    }

    LOG(DEBUG, "prepare scene %s @ t=%f", scene->label, t);

    ret = ngli_node_honor_release_prefetch(scene, t);
    if (ret < 0)
        return ret;

    ret = ngli_node_update(scene, t);
    if (ret < 0)
        return ret;

    ret = ngli_gpu_ctx_end_update(s->gpu_ctx, t);
    if (ret < 0)
        return ret;

    s->cpu_update_time = s->hud ? ngli_gettime_relative() - start_time : 0;

    return 0;
}

int ngli_ctx_draw(struct ngl_ctx *s, double t)
{
    int ret = ngli_ctx_prepare_draw(s, t);
    if (ret < 0)
        return ret;

    ret = ngli_gpu_ctx_begin_draw(s->gpu_ctx, t);
    if (ret < 0)
        return ret;

    const int64_t cpu_start_time = s->hud ? ngli_gettime_relative() : 0;

    struct rendertarget *rt = ngli_gpu_ctx_get_default_rendertarget(s->gpu_ctx, NGLI_LOAD_OP_CLEAR);
    struct rendertarget *rt_resume = ngli_gpu_ctx_get_default_rendertarget(s->gpu_ctx, NGLI_LOAD_OP_LOAD);
    s->available_rendertargets[0] = rt;
    s->available_rendertargets[1] = rt_resume;
    s->current_rendertarget = rt;
    s->render_pass_started = 0;

    struct ngl_node *scene = s->scene;
    if (scene) {
        LOG(DEBUG, "draw scene %s @ t=%f", scene->label, t);
        ngli_node_draw(scene);
    }

    if (!s->render_pass_started) {
        ngli_gpu_ctx_begin_render_pass(s->gpu_ctx, s->current_rendertarget);
        s->render_pass_started = 1;
    }

    if (s->hud) {
        s->cpu_draw_time = ngli_gettime_relative() - cpu_start_time;

        if (s->render_pass_started) {
            ngli_gpu_ctx_end_render_pass(s->gpu_ctx);
            s->current_rendertarget = s->available_rendertargets[1];
            s->render_pass_started = 0;
        }
        ngli_gpu_ctx_query_draw_time(s->gpu_ctx, &s->gpu_draw_time);

        ngli_hud_draw(s->hud);
    }

    if (s->render_pass_started) {
        ngli_gpu_ctx_end_render_pass(s->gpu_ctx);
        s->render_pass_started = 0;
    }

    return ngli_gpu_ctx_end_draw(s->gpu_ctx, t);
}

int ngli_ctx_dispatch_cmd(struct ngl_ctx *s, cmd_func_type cmd_func, void *arg)
{
    pthread_mutex_lock(&s->lock);
    s->cmd_func = cmd_func;
    s->cmd_arg = arg;
    pthread_cond_signal(&s->cond_wkr);
    while (s->cmd_func)
        pthread_cond_wait(&s->cond_ctl, &s->lock);
    pthread_mutex_unlock(&s->lock);

    return s->cmd_ret;
}

static void *worker_thread(void *arg)
{
    struct ngl_ctx *s = arg;

    ngli_thread_set_name("ngl-thread");

    pthread_mutex_lock(&s->lock);
    for (;;) {
        while (!s->cmd_func)
            pthread_cond_wait(&s->cond_wkr, &s->lock);
        s->cmd_ret = s->cmd_func(s, s->cmd_arg);
        int need_stop = s->cmd_func == cmd_stop;
        s->cmd_func = s->cmd_arg = NULL;
        pthread_cond_signal(&s->cond_ctl);

        if (need_stop)
            break;
    }
    pthread_mutex_unlock(&s->lock);

    return NULL;
}

static const char *get_cap_string_id(unsigned cap_id)
{
    switch (cap_id) {
    case NGL_CAP_BLOCK:                         return "block";
    case NGL_CAP_COMPUTE:                       return "compute";
    case NGL_CAP_DEPTH_STENCIL_RESOLVE:         return "depth_stencil_resolve";
    case NGL_CAP_INSTANCED_DRAW:                return "instanced_draw";
    case NGL_CAP_MAX_COLOR_ATTACHMENTS:         return "max_color_attachments";
    case NGL_CAP_MAX_COMPUTE_GROUP_COUNT_X:     return "max_compute_group_count_x";
    case NGL_CAP_MAX_COMPUTE_GROUP_COUNT_Y:     return "max_compute_group_count_y";
    case NGL_CAP_MAX_COMPUTE_GROUP_COUNT_Z:     return "max_compute_group_count_z";
    case NGL_CAP_MAX_COMPUTE_GROUP_INVOCATIONS: return "max_compute_group_invocations";
    case NGL_CAP_MAX_COMPUTE_GROUP_SIZE_X:      return "max_compute_group_size_x";
    case NGL_CAP_MAX_COMPUTE_GROUP_SIZE_Y:      return "max_compute_group_size_y";
    case NGL_CAP_MAX_COMPUTE_GROUP_SIZE_Z:      return "max_compute_group_size_z";
    case NGL_CAP_MAX_COMPUTE_SHARED_MEMORY_SIZE:return "max_compute_shared_memory_size";
    case NGL_CAP_MAX_SAMPLES:                   return "max_samples";
    case NGL_CAP_MAX_TEXTURE_DIMENSION_1D:      return "max_texture_dimensions_1d";
    case NGL_CAP_MAX_TEXTURE_DIMENSION_2D:      return "max_texture_dimensions_2d";
    case NGL_CAP_MAX_TEXTURE_DIMENSION_3D:      return "max_texture_dimensions_3d";
    case NGL_CAP_MAX_TEXTURE_DIMENSION_CUBE:    return "max_texture_dimensions_cube";
    case NGL_CAP_NPOT_TEXTURE:                  return "npot_texture";
    case NGL_CAP_SHADER_TEXTURE_LOD:            return "shader_texture_lod";
    case NGL_CAP_TEXTURE_3D:                    return "texture_3d";
    case NGL_CAP_TEXTURE_CUBE:                  return "texture_cube";
    case NGL_CAP_UINT_UNIFORMS:                 return "uint_uniforms";
    }
    ngli_assert(0);
}

#define CAP(cap_id, value) {cap_id, get_cap_string_id(cap_id), value}
#define ALL_FEATURES(features, mask) ((features & (mask)) == mask)
#define ANY_FEATURES(features, mask) ((features & (mask)) != 0)

static int load_caps(struct ngl_backend *backend, const struct gpu_ctx *gpu_ctx)
{
    const int has_block          = ANY_FEATURES(gpu_ctx->features, NGLI_FEATURE_UNIFORM_BUFFER | NGLI_FEATURE_STORAGE_BUFFER);
    const int has_compute        = ALL_FEATURES(gpu_ctx->features, NGLI_FEATURE_COMPUTE);
    const int has_ds_resolve     = ALL_FEATURES(gpu_ctx->features, NGLI_FEATURE_DEPTH_STENCIL_RESOLVE);
    const int has_instanced_draw = ALL_FEATURES(gpu_ctx->features, NGLI_FEATURE_INSTANCED_DRAW);
    const int has_npot_texture   = ALL_FEATURES(gpu_ctx->features, NGLI_FEATURE_TEXTURE_NPOT);
    const int has_shader_texture_lod = ALL_FEATURES(gpu_ctx->features, NGLI_FEATURE_SHADER_TEXTURE_LOD);
    const int has_texture_3d     = ALL_FEATURES(gpu_ctx->features, NGLI_FEATURE_TEXTURE_3D);
    const int has_texture_cube   = ALL_FEATURES(gpu_ctx->features, NGLI_FEATURE_TEXTURE_CUBE_MAP);
    const int has_uint_uniforms  = ALL_FEATURES(gpu_ctx->features, NGLI_FEATURE_UINT_UNIFORMS);

    const struct gpu_limits *limits = &gpu_ctx->limits;
    const struct ngl_cap caps[] = {
        CAP(NGL_CAP_BLOCK,                         has_block),
        CAP(NGL_CAP_COMPUTE,                       has_compute),
        CAP(NGL_CAP_DEPTH_STENCIL_RESOLVE,         has_ds_resolve),
        CAP(NGL_CAP_INSTANCED_DRAW,                has_instanced_draw),
        CAP(NGL_CAP_MAX_COLOR_ATTACHMENTS,         limits->max_color_attachments),
        CAP(NGL_CAP_MAX_COMPUTE_GROUP_COUNT_X,     limits->max_compute_work_group_count[0]),
        CAP(NGL_CAP_MAX_COMPUTE_GROUP_COUNT_Y,     limits->max_compute_work_group_count[1]),
        CAP(NGL_CAP_MAX_COMPUTE_GROUP_COUNT_Z,     limits->max_compute_work_group_count[2]),
        CAP(NGL_CAP_MAX_COMPUTE_GROUP_INVOCATIONS, limits->max_compute_work_group_invocations),
        CAP(NGL_CAP_MAX_COMPUTE_GROUP_SIZE_X,      limits->max_compute_work_group_size[0]),
        CAP(NGL_CAP_MAX_COMPUTE_GROUP_SIZE_Y,      limits->max_compute_work_group_size[1]),
        CAP(NGL_CAP_MAX_COMPUTE_GROUP_SIZE_Z,      limits->max_compute_work_group_size[2]),
        CAP(NGL_CAP_MAX_COMPUTE_SHARED_MEMORY_SIZE,limits->max_compute_shared_memory_size),
        CAP(NGL_CAP_MAX_SAMPLES,                   limits->max_samples),
        CAP(NGL_CAP_MAX_TEXTURE_DIMENSION_1D,      limits->max_texture_dimension_1d),
        CAP(NGL_CAP_MAX_TEXTURE_DIMENSION_2D,      limits->max_texture_dimension_2d),
        CAP(NGL_CAP_MAX_TEXTURE_DIMENSION_3D,      limits->max_texture_dimension_3d),
        CAP(NGL_CAP_MAX_TEXTURE_DIMENSION_CUBE,    limits->max_texture_dimension_cube),
        CAP(NGL_CAP_NPOT_TEXTURE,                  has_npot_texture),
        CAP(NGL_CAP_SHADER_TEXTURE_LOD,            has_shader_texture_lod),
        CAP(NGL_CAP_TEXTURE_3D,                    has_texture_3d),
        CAP(NGL_CAP_TEXTURE_CUBE,                  has_texture_cube),
        CAP(NGL_CAP_UINT_UNIFORMS,                 has_uint_uniforms),
    };

    backend->nb_caps = NGLI_ARRAY_NB(caps);
    backend->caps = ngli_memdup(caps, sizeof(caps));
    if (!backend->caps)
        return NGL_ERROR_MEMORY;

    return 0;
}

enum {
    PROBE_MODE_FULL,
    PROBE_MODE_NO_GRAPHICS,
};

static int backend_probe(struct ngl_backend *backend, const struct ngl_config *config, int mode)
{
    int ret = 0;

    struct gpu_ctx *gpu_ctx = ngli_gpu_ctx_create(config);
    if (!gpu_ctx)
        return NGL_ERROR_MEMORY;

    ngli_assert(gpu_ctx->backend_str);
    ngli_assert(gpu_ctx->cls);

    backend->id         = config->backend;
    backend->string_id  = gpu_ctx->backend_str;
    backend->name       = gpu_ctx->cls->name;

    if (mode == PROBE_MODE_NO_GRAPHICS)
        goto end;

    ret = ngli_gpu_ctx_init(gpu_ctx);
    if (ret < 0)
        goto end;

    ret = load_caps(backend, gpu_ctx);
    if (ret < 0)
        goto end;

end:
    ngli_gpu_ctx_freep(&gpu_ctx);
    return ret;
}

static const int backend_ids[] = {
#if defined(BACKEND_GL)
    NGL_BACKEND_OPENGL,
#endif
#if defined(BACKEND_GLES)
    NGL_BACKEND_OPENGLES,
#endif
#ifdef BACKEND_VK
    NGL_BACKEND_VULKAN,
#endif
};

static int backends_probe(const struct ngl_config *user_config, int *nb_backendsp, struct ngl_backend **backendsp, int mode)
{
    static const struct ngl_config default_config = {
        .width     = 1,
        .height    = 1,
        .offscreen = 1,
    };

    if (!user_config)
        user_config = &default_config;

    const int platform = user_config->platform == NGL_PLATFORM_AUTO ? get_default_platform() : user_config->platform;

    struct ngl_backend *backends = ngli_calloc(NGLI_ARRAY_NB(backend_ids), sizeof(*backends));
    if (!backends)
        return NGL_ERROR_MEMORY;
    int nb_backends = 0;

    for (int i = 0; i < NGLI_ARRAY_NB(backend_ids); i++) {
        if (user_config->backend != NGL_BACKEND_AUTO && user_config->backend != backend_ids[i])
            continue;
        struct ngl_config config = *user_config;
        config.backend = backend_ids[i];
        config.platform = platform;

        int ret = backend_probe(&backends[nb_backends], &config, mode);
        if (ret < 0)
            continue;
        backends[nb_backends].is_default = backend_ids[i] == DEFAULT_BACKEND;

        nb_backends++;
    }

    if (!nb_backends)
        ngl_backends_freep(&backends);

    *backendsp = backends;
    *nb_backendsp = nb_backends;
    return 0;
}

int ngl_backends_probe(const struct ngl_config *user_config, int *nb_backendsp, struct ngl_backend **backendsp)
{
    return backends_probe(user_config, nb_backendsp, backendsp, PROBE_MODE_FULL);
}

int ngl_backends_get(const struct ngl_config *user_config, int *nb_backendsp, struct ngl_backend **backendsp)
{
    return backends_probe(user_config, nb_backendsp, backendsp, PROBE_MODE_NO_GRAPHICS);
}

void ngl_backends_freep(struct ngl_backend **backendsp)
{
    struct ngl_backend *backends = *backendsp;
    if (!backends)
        return;
    for (int i = 0; i < NGLI_ARRAY_NB(backend_ids); i++)
        ngli_free(backends[i].caps);
    ngli_freep(backendsp);
}

struct ngl_ctx *ngl_create(void)
{
    struct ngl_ctx *s = ngli_calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    if (pthread_mutex_init(&s->lock, NULL) ||
        pthread_cond_init(&s->cond_ctl, NULL) ||
        pthread_cond_init(&s->cond_wkr, NULL) ||
        pthread_create(&s->worker_tid, NULL, worker_thread, s)) {
        pthread_cond_destroy(&s->cond_ctl);
        pthread_cond_destroy(&s->cond_wkr);
        pthread_mutex_destroy(&s->lock);
        ngli_free(s);
        return NULL;
    }

    ngli_darray_init(&s->modelview_matrix_stack, 4 * 4 * sizeof(float), 1);
    ngli_darray_init(&s->projection_matrix_stack, 4 * 4 * sizeof(float), 1);
    ngli_darray_init(&s->activitycheck_nodes, sizeof(struct ngl_node *), 0);

    static const NGLI_ALIGNED_MAT(id_matrix) = NGLI_MAT4_IDENTITY;
    if (!ngli_darray_push(&s->modelview_matrix_stack, id_matrix) ||
        !ngli_darray_push(&s->projection_matrix_stack, id_matrix))
        goto fail;

    LOG(INFO, "context create in node.gl v%d.%d.%d",
        NGL_VERSION_MAJOR, NGL_VERSION_MINOR, NGL_VERSION_MICRO);

    return s;

fail:
    ngl_freep(&s);
    return NULL;
}

int ngl_configure(struct ngl_ctx *s, struct ngl_config *config)
{
    if (s->configured) {
        s->api_impl->reset(s, NGLI_ACTION_KEEP_SCENE);
        s->configured = 0;
    }

    if (!config) {
        LOG(ERROR, "context configuration cannot be NULL");
        return NGL_ERROR_INVALID_ARG;
    }

    if (config->backend == NGL_BACKEND_AUTO && config->backend_config) {
        LOG(ERROR, "backend specific configuration is not allowed "
                   "while automatic backend selection is used");
        return NGL_ERROR_INVALID_USAGE;
    }

    if (config->backend == NGL_BACKEND_AUTO)
        config->backend = DEFAULT_BACKEND;
    if (config->platform == NGL_PLATFORM_AUTO)
        config->platform = get_default_platform();
    if (config->platform < 0) {
        LOG(ERROR, "can not determine which platform to use");
        return config->platform;
    }

    if (config->backend < 0 ||
        config->backend >= NGL_NB_BACKEND) {
        LOG(ERROR, "unknown backend %d", config->backend);
        return NGL_ERROR_INVALID_ARG;
    }

    s->api_impl = api_map[config->backend].api_impl;
    if (!s->api_impl) {
        LOG(ERROR, "backend \"%s\" not available with this build",
            ngli_get_backend_string_id(config->backend));
        return NGL_ERROR_UNSUPPORTED;
    }

    int ret = s->api_impl->configure(s, config);
    if (ret < 0)
        return ret;

    s->configured = 1;
    return 0;
}

int ngl_resize(struct ngl_ctx *s, int width, int height, const int *viewport)
{
    if (!s->configured) {
        LOG(ERROR, "context must be configured before resizing rendering buffers");
        return NGL_ERROR_INVALID_USAGE;
    }

    return s->api_impl->resize(s, width, height, viewport);
}

int ngl_set_capture_buffer(struct ngl_ctx *s, void *capture_buffer)
{
    if (!s->configured) {
        LOG(ERROR, "context must be configured before setting a capture buffer");
        return NGL_ERROR_INVALID_USAGE;
    }

    int ret = s->api_impl->set_capture_buffer(s, capture_buffer);
    if (ret < 0) {
        s->configured = 0;
        return ret;
    }
    return ret;
}

int ngl_set_scene(struct ngl_ctx *s, struct ngl_node *scene)
{
    if (!s->configured) {
        LOG(ERROR, "context must be configured before setting a scene");
        return NGL_ERROR_INVALID_USAGE;
    }

    return s->api_impl->set_scene(s, scene);
}

int ngli_prepare_draw(struct ngl_ctx *s, double t)
{
    if (!s->configured) {
        LOG(ERROR, "context must be configured before updating");
        return NGL_ERROR_INVALID_USAGE;
    }

    return s->api_impl->prepare_draw(s, t);
}

int ngl_draw(struct ngl_ctx *s, double t)
{
    if (!s->configured) {
        LOG(ERROR, "context must be configured before drawing");
        return NGL_ERROR_INVALID_USAGE;
    }

    return s->api_impl->draw(s, t);
}

int ngl_gl_wrap_framebuffer(struct ngl_ctx *s, uint32_t framebuffer)
{
    if (!s->configured) {
        LOG(ERROR, "context must be configured before wrapping a new external OpenGL framebuffer");
        return NGL_ERROR_INVALID_USAGE;
    }

    if (!s->api_impl->gl_wrap_framebuffer) {
        LOG(ERROR, "wrapping external OpenGL framebuffer is not supported by context");
        return NGL_ERROR_UNSUPPORTED;
    }

    int ret = s->api_impl->gl_wrap_framebuffer(s, framebuffer);
    if (ret < 0) {
        s->configured = 0;
        return ret;
    }
    return 0;
 }

int ngl_livectls_get(struct ngl_node *scene, int *nb_livectlsp, struct ngl_livectl **livectlsp)
{
    return ngli_node_livectls_get(scene, nb_livectlsp, livectlsp);
}

void ngl_livectls_freep(struct ngl_livectl **livectlsp)
{
    ngli_node_livectls_freep(livectlsp);
}

void ngl_freep(struct ngl_ctx **ss)
{
    struct ngl_ctx *s = *ss;

    if (!s)
        return;

    if (s->configured) {
        s->api_impl->reset(s, NGLI_ACTION_UNREF_SCENE);
        s->configured = 0;
    }
    ngli_ctx_dispatch_cmd(s, cmd_stop, NULL);
    pthread_join(s->worker_tid, NULL);
    pthread_cond_destroy(&s->cond_ctl);
    pthread_cond_destroy(&s->cond_wkr);
    pthread_mutex_destroy(&s->lock);

    ngli_darray_reset(&s->modelview_matrix_stack);
    ngli_darray_reset(&s->projection_matrix_stack);
    ngli_darray_reset(&s->activitycheck_nodes);
    ngli_freep(ss);
}

#if defined(TARGET_ANDROID)
static void *java_vm;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

int ngl_jni_set_java_vm(void *vm)
{
    int ret = 0;

    pthread_mutex_lock(&lock);
    if (java_vm == NULL) {
        java_vm = vm;
    } else if (java_vm != vm) {
        ret = -1;
        LOG(ERROR, "a Java virtual machine has already been set");
    }
    pthread_mutex_unlock(&lock);

    return ret;
}

void *ngl_jni_get_java_vm(void)
{
    void *vm;

    pthread_mutex_lock(&lock);
    vm = java_vm;
    pthread_mutex_unlock(&lock);

    return vm;
}

static void *android_application_context;

int ngl_android_set_application_context(void *application_context)
{
    JNIEnv *env;

    env = ngli_jni_get_env();
    if (!env)
        return NGL_ERROR_EXTERNAL;

    pthread_mutex_lock(&lock);

    if (android_application_context) {
        (*env)->DeleteGlobalRef(env, android_application_context);
        android_application_context = NULL;
    }

    if (application_context)
        android_application_context = (*env)->NewGlobalRef(env, application_context);

    pthread_mutex_unlock(&lock);

    return 0;
}

void *ngl_android_get_application_context(void)
{
    void *context;

    pthread_mutex_lock(&lock);
    context = android_application_context;
    pthread_mutex_unlock(&lock);

    return context;
}

#else
int ngl_jni_set_java_vm(void *vm)
{
    return NGL_ERROR_UNSUPPORTED;
}

void *ngl_jni_get_java_vm(void)
{
    return NULL;
}

int ngl_android_set_application_context(void *application_context)
{
    return NGL_ERROR_UNSUPPORTED;
}

void *ngl_android_get_application_context(void)
{
    return NULL;
}
#endif
