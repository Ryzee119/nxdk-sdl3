// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2025 Ryan Wendland

#include <../src/SDL_internal.h>

#ifdef SDL_VIDEO_RENDER_XGU

#include "swizzle.h"
#include "xgu/xgux.h"
#include <../src/render/SDL_sysrender.h>
#include <SDL3/SDL_pixels.h>
#include <hal/video.h>
#include <stdint.h>

#define nxdk_RenderDriver GPU_RenderDriver

// Note: To avoid stalls this vertex buffer must be sufficient to
// hold three frames (pbkit is tripple buffered) worth of vertices. This is to allow for
// both the back buffers being rendered, and the active front buffer being calculated.
#ifndef SDL_XGU_VERTEX_BUFFER_SIZE
#define SDL_XGU_VERTEX_BUFFER_SIZE (1024 * 1024)
#endif

#ifndef SDL_XGU_VERTEX_ALIGNMENT
#define SDL_XGU_VERTEX_ALIGNMENT 32
#endif

#ifndef SDL_XGU_SHOW_FPS
#define SDL_XGU_SHOW_FPS 0
#endif

#define XGU_MAYBE_UNUSED __attribute__((unused))

// pbkit does not provide a way to see how many buffers are available, however it is currently
// hardcoded to 3 buffers.
#define NXDK_PBKIT_BUFFER_COUNT 3

typedef struct xgu_texture
{
    int data_width;
    int data_height;
    int tex_width;
    int tex_height;
    int bytes_per_pixel;
    int pitch;
    int swizzled;
    XguTexFormatColor format;
    uint8_t *data;
    uint8_t *data_physical_address;
} xgu_texture_t;

typedef struct xgu_point
{
    float pos[2]; // xy
} xgu_point_t;

typedef struct xgu_vertex
{
    float pos[2];      // xy
    uint32_t color[1]; // rgba8888
} xgu_vertex_t;

typedef struct xgu_vertex_texture
{
    float pos[2];      // xy
    uint32_t color[1]; // rgba8888
    float tex[2];      // uv
} xgu_vertex_textured_t;

typedef struct xgu_render_data
{
    int texture_shader_active;
    const xgu_texture_t *active_texture;
    const xgu_texture_t *active_render_target;
    SDL_Rect viewport;
    SDL_Rect clip_rect;
    SDL_BlendMode active_blend_mode;
    int vertex_arena_offset;
    int vertex_allocations[NXDK_PBKIT_BUFFER_COUNT];
    int frame_index;
} xgu_render_data_t;

// Forward declarations
static inline void combiner_init(void);
static inline void texture_combiner_apply(void);
static inline void unlit_combiner_apply(void);
static inline void clear_attribute_pointers(void);
static void set_blend_mode(SDL_Renderer *renderer, SDL_BlendMode blendMode);
static SDL_Rect sanitize_scissor_rect(SDL_Renderer *renderer, const SDL_Rect *rect);
static void *arena_allocate(SDL_Renderer *renderer, size_t size, size_t *offset);
static bool arena_init(SDL_Renderer *renderer);
static bool sdl_to_xgu_texture_format(SDL_PixelFormat sdl_format, int *xgu_texture_format, int *bytes_per_pixel, bool swizzled);
static bool sdl_to_xgu_surface_format(SDL_PixelFormat sdl_format, int *xgu_surface_format, int *bytes_per_pixel);
static inline uint32_t npot2pot(uint32_t num);
static void calculate_fps(int stage);

// pushbuffer pointer
static uint32_t *p = NULL;

static void XBOX_WindowEvent(SDL_Renderer *renderer, const SDL_WindowEvent *event)
{
    (void)renderer;
    (void)event;
}

static bool XBOX_CreateTexture(SDL_Renderer *renderer, SDL_Texture *texture, SDL_PropertiesID create_props)
{
    XGU_MAYBE_UNUSED xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    xgu_texture_t *xgu_texture = (xgu_texture_t *)SDL_calloc(1, sizeof(xgu_texture_t));
    if (xgu_texture == NULL) {
        return SDL_OutOfMemory();
    }

    // If this is a render target, we need to check if the render target format is supported
    if (SDL_GetNumberProperty(create_props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, 0) == SDL_TEXTUREACCESS_TARGET) {
        int surface_format;
        int bytes_per_pixel;
        if (sdl_to_xgu_surface_format(texture->format, &surface_format, &bytes_per_pixel) == false) {
            SDL_free(xgu_texture);
            return SDL_SetError("[nxdk renderer] Unsupported render target format (%s)", SDL_GetPixelFormatName(texture->format));
        }
    }

    // If static target we swizzle it because it has better performance is should not need updating often
    if (SDL_GetNumberProperty(create_props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, 0) == SDL_TEXTUREACCESS_STATIC) {
        xgu_texture->swizzled = 1;
    }

    // Ensure the texture format is supported
    if (sdl_to_xgu_texture_format(texture->format, &xgu_texture->format, &xgu_texture->bytes_per_pixel, xgu_texture->swizzled) == false) {
        SDL_free(xgu_texture);
        return SDL_SetError("[nxdk renderer] Unsupported texture format (%s)", SDL_GetPixelFormatName(texture->format));
    }

    xgu_texture->tex_width = texture->w;
    xgu_texture->tex_height = texture->h;

    if (xgu_texture->swizzled) {
        xgu_texture->data_width = npot2pot(texture->w);
        xgu_texture->data_height = npot2pot(texture->h);
    } else {
        xgu_texture->data_width = texture->w;
        xgu_texture->data_height = texture->h;
    }

    xgu_texture->pitch = xgu_texture->data_width * xgu_texture->bytes_per_pixel;

    const SIZE_T allocation_size = xgu_texture->data_height * xgu_texture->pitch;
    xgu_texture->data = MmAllocateContiguousMemoryEx(allocation_size, 0, 0xFFFFFFFF, 0, PAGE_WRITECOMBINE | PAGE_READWRITE);
    if (xgu_texture->data == NULL) {
        SDL_free(xgu_texture);
        return SDL_OutOfMemory();
    }
    xgu_texture->data_physical_address = (uint8_t *)MmGetPhysicalAddress(xgu_texture->data);

    texture->internal = xgu_texture;
    return true;
}

static void XBOX_DestroyTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    XGU_MAYBE_UNUSED xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    xgu_texture_t *xgu_texture = (xgu_texture_t *)texture->internal;
    if (xgu_texture == NULL) {
        return;
    }

    MmFreeContiguousMemory(xgu_texture->data);
    SDL_free(xgu_texture);
    texture->internal = NULL;
}

static bool XBOX_LockTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect, void **pixels, int *pitch)
{
    XGU_MAYBE_UNUSED xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    xgu_texture_t *xgu_texture = (xgu_texture_t *)texture->internal;
    uint8_t *pixels8 = (uint8_t *)xgu_texture->data;

    // We don't need to worry about unswizzling because you can only lock textures that are not swizzled
    *pixels = &pixels8[rect->y * xgu_texture->pitch +
                       rect->x * xgu_texture->bytes_per_pixel];

    *pitch = xgu_texture->pitch;
    return true;
}

static void XBOX_UnlockTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    (void)renderer;
    (void)texture;
    return;
}

static bool XBOX_UpdateTexture(SDL_Renderer *renderer, SDL_Texture *texture,
                               const SDL_Rect *rect, const void *pixels, int pitch)
{
    XGU_MAYBE_UNUSED xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    xgu_texture_t *xgu_texture = (xgu_texture_t *)texture->internal;
    const Uint8 *src = pixels;

    void *dst;
    int dpitch;
    XBOX_LockTexture(renderer, texture, rect, &dst, &dpitch);

    if (xgu_texture->swizzled) {
        const int length = rect->w * SDL_BYTESPERPIXEL(texture->format);
        swizzle_rect(src, rect->w, rect->h, dst, length, SDL_BYTESPERPIXEL(texture->format));
    } else {
        SDL_ConvertPixels(rect->w, rect->h,
                          texture->format, src, pitch,
                          texture->format, dst, dpitch);
    }

    XBOX_UnlockTexture(renderer, texture);

    return true;
}

static bool XBOX_SetRenderTarget(SDL_Renderer *renderer, SDL_Texture *texture)
{
    XGU_MAYBE_UNUSED xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    uint32_t address, pitch, zpitch, clip_width, clip_height, format, dma_channel;
    xgu_texture_t *xgu_texture = (texture) ? (xgu_texture_t *)texture->internal : NULL;
    extern unsigned int pb_ColorFmt; // From pbkit.c

    if (xgu_texture == NULL) {
        dma_channel = DMA_CHANNEL_PIXEL_RENDERER;
        address = 0;
        pitch = pb_back_buffer_pitch();
        clip_width = pb_back_buffer_width();
        clip_height = pb_back_buffer_height();
        format = XGU_MASK(NV097_SET_SURFACE_FORMAT_COLOR, pb_ColorFmt);
    } else {
        int surface_format, bytes_per_pixel;
        bool status = sdl_to_xgu_surface_format(texture->format, &surface_format, &bytes_per_pixel);

        // All the checks during texture creation should ensure this never fails
        assert(status);

        dma_channel = DMA_CHANNEL_3D_3;
        address = (uint32_t)xgu_texture->data_physical_address;
        pitch = xgu_texture->pitch;
        clip_width = xgu_texture->tex_width;
        clip_height = xgu_texture->tex_height;
        format = XGU_MASK(NV097_SET_SURFACE_FORMAT_COLOR, surface_format);
    }

    format |= XGU_MASK(NV097_SET_SURFACE_FORMAT_ZETA, NV097_SET_SURFACE_FORMAT_ZETA_Z24S8) |
              XGU_MASK(NV097_SET_SURFACE_FORMAT_TYPE, NV097_SET_SURFACE_FORMAT_TYPE_PITCH);

    // We are not using the depth buffer so it is unchanged. Stick to the back buffer width.
    // Z24S8 format has 4 bytes per pixel for the zeta buffer
    zpitch = pb_back_buffer_width() * 4;

    p = pb_begin();

    p = pb_push1(p, NV097_WAIT_FOR_IDLE, 0);
    p = pb_push1(p, NV097_SET_CONTEXT_DMA_COLOR, dma_channel);

    p = pb_push1(p, NV097_SET_SURFACE_PITCH,
                 XGU_MASK(NV097_SET_SURFACE_PITCH_COLOR, pitch) |
                     XGU_MASK(NV097_SET_SURFACE_PITCH_ZETA, zpitch));
    p = pb_push1(p, NV097_SET_SURFACE_COLOR_OFFSET, address);
    p = pb_push1(p, NV097_SET_SURFACE_CLIP_HORIZONTAL,
                 XGU_MASK(NV097_SET_SURFACE_CLIP_HORIZONTAL_WIDTH, clip_width) |
                     XGU_MASK(NV097_SET_SURFACE_CLIP_HORIZONTAL_X, 0));
    p = pb_push1(p, NV097_SET_SURFACE_CLIP_VERTICAL,
                 XGU_MASK(NV097_SET_SURFACE_CLIP_VERTICAL_HEIGHT, clip_height) |
                     XGU_MASK(NV097_SET_SURFACE_CLIP_VERTICAL_Y, 0));
    p = pb_push1(p, NV097_SET_SURFACE_FORMAT, format);

    pb_end(p);

    render_data->active_render_target = xgu_texture;
    return true;
}

static bool XBOX_QueueDrawPoints(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count)
{
    XGU_MAYBE_UNUSED xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;

    uint8_t *vertices = (uint8_t *)arena_allocate(renderer, count * 2 * sizeof(float), &cmd->data.draw.first);
    if (!vertices) {
        return SDL_OutOfMemory();
    }

    // Each point is a vertex with 2 floats (x, y)
    // This matches the xgu_vertex_t structure so we can copy it directly
    SDL_memcpy(vertices, points, count * sizeof(xgu_point_t));
    cmd->data.draw.count = count;

    return true;
}

static bool XBOX_QueueGeometry(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
                               const float *xy, int xy_stride, const SDL_FColor *color, int color_stride,
                               const float *uv, int uv_stride,
                               int num_vertices, const void *indices, int num_indices, int size_indices,
                               float scale_x, float scale_y)
{
    XGU_MAYBE_UNUSED xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    const int count = indices ? num_indices : num_vertices;
    const size_t sz = (texture) ? sizeof(xgu_vertex_textured_t) : sizeof(xgu_vertex_t);
    const float color_scale = cmd->data.draw.color_scale;

    uint8_t *vertices = (uint8_t *)arena_allocate(renderer, count * sz, &cmd->data.draw.first);
    if (vertices == NULL) {
        return SDL_OutOfMemory();
    }

    cmd->data.draw.count = count;
    size_indices = indices ? size_indices : 0;

    for (int i = 0; i < count; i++) {
        int j;
        if (size_indices == 4) {
            j = ((const uint32_t *)indices)[i];
        } else if (size_indices == 2) {
            j = ((const uint16_t *)indices)[i];
        } else if (size_indices == 1) {
            j = ((const uint8_t *)indices)[i];
        } else {
            j = i;
        }

        const float *vertex_pos = (float *)((char *)xy + j * xy_stride);
        const SDL_FColor *vertex_color = (SDL_FColor *)((intptr_t)color + j * color_stride);

        // We could keep the color as four floats but we can save alot of vertex buffer space making it a single uint32_t
        const uint8_t r = (uint8_t)SDL_min((uint32_t)(vertex_color->r * color_scale * 255.0f), 255);
        const uint8_t g = (uint8_t)SDL_min((uint32_t)(vertex_color->g * color_scale * 255.0f), 255);
        const uint8_t b = (uint8_t)SDL_min((uint32_t)(vertex_color->b * color_scale * 255.0f), 255);
        const uint8_t a = (uint8_t)SDL_min((uint32_t)(vertex_color->a * 255.0f), 255);
        const uint32_t color_value = r | (g << 8) | (b << 16) | (a << 24);

        if (texture) {
            xgu_vertex_textured_t *xgu_vertex = (xgu_vertex_textured_t *)vertices;
            xgu_vertex->pos[0] = vertex_pos[0] * scale_x;
            xgu_vertex->pos[1] = vertex_pos[1] * scale_y;
            xgu_vertex->color[0] = color_value;

            const xgu_texture_t *xgu_texture = (xgu_texture_t *)texture->internal;
            const float *vertex_uv = (float *)((char *)uv + j * uv_stride);

            // Swizzled texture coords must be normalised, otherwise we stick with unnormalised
            if (xgu_texture->swizzled) {
                xgu_vertex->tex[0] = vertex_uv[0] * (float)(xgu_texture->tex_width - 1) / (float)xgu_texture->data_width;
                xgu_vertex->tex[1] = vertex_uv[1] * (float)(xgu_texture->tex_height - 1) / (float)xgu_texture->data_height;
            } else {
                xgu_vertex->tex[0] = vertex_uv[0] * xgu_texture->tex_width - 1;
                xgu_vertex->tex[1] = vertex_uv[1] * xgu_texture->tex_height - 1;
            }

            vertices += sizeof(xgu_vertex_textured_t);
        } else {
            xgu_vertex_t *xgu_vertex = (xgu_vertex_t *)vertices;
            xgu_vertex->pos[0] = vertex_pos[0] * scale_x;
            xgu_vertex->pos[1] = vertex_pos[1] * scale_y;
            xgu_vertex->color[0] = color_value;
            vertices += sizeof(xgu_vertex_t);
        }
    }
    return true;
}

static bool XBOX_QueueNoOp(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    (void)renderer;
    (void)cmd;
    return true;
}

static bool XBOX_RenderSetViewPort(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    XGU_MAYBE_UNUSED xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    const SDL_Rect *viewport = &cmd->data.viewport.rect;

    // If the new viewport is the same as the current one, no need to update
    if (SDL_memcmp(viewport, &render_data->viewport, sizeof(SDL_Rect)) == 0) {
        return true;
    }

    // Create a new rect that is the intersection of the new viewport and the current clip rect
    // This is becaused SDL expects rendering to be clipped to the viewport and the clip rect
    // but we can only set one scissor at a time.
    SDL_Rect scissor_clipped_rect;
    SDL_GetRectIntersection(&render_data->clip_rect, viewport, &scissor_clipped_rect);

    scissor_clipped_rect = sanitize_scissor_rect(renderer, &scissor_clipped_rect);

    p = pb_begin();
    p = xgu_set_viewport_offset(p, viewport->x, viewport->y, 0.0f, 0.0f);
    p = xgu_set_scissor_rect(p, false, scissor_clipped_rect.x, scissor_clipped_rect.y,
                             scissor_clipped_rect.w, scissor_clipped_rect.h);
    pb_end(p);

    // Store the viewport in the render data
    render_data->viewport = *viewport;
    return true;
}

static bool XBOX_RenderSetClipRect(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    XGU_MAYBE_UNUSED xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    SDL_Rect *clip_rect = &cmd->data.cliprect.rect;

    // If clipping is disabled, reset the clip rect to the entire back buffer
    if (cmd->data.cliprect.enabled == false) {
        const SDL_Rect no_clip = { 0, 0, pb_back_buffer_width(), pb_back_buffer_height() };
        *clip_rect = no_clip;
    }

    // If the new clip rect is the same as the current one, no need to update
    if (SDL_memcmp(clip_rect, &render_data->clip_rect, sizeof(SDL_Rect)) == 0) {
        return true;
    }

    // Create a new rect that is the intersection of the new clip rect and the current viewport
    // This is because SDL expects rendering to be clipped to the viewport and the clip rect
    // but we can only set one scissor at a time.
    SDL_Rect scissor_clipped_rect;
    SDL_GetRectIntersection(&render_data->viewport, clip_rect, &scissor_clipped_rect);

    scissor_clipped_rect = sanitize_scissor_rect(renderer, &scissor_clipped_rect);

    p = pb_begin();
    p = xgu_set_scissor_rect(p, false, scissor_clipped_rect.x, scissor_clipped_rect.y,
                             scissor_clipped_rect.w, scissor_clipped_rect.h);
    pb_end(p);

    // Store the clip rect in the render data
    render_data->clip_rect = *clip_rect;
    return true;
}

static bool XBOX_RenderSetDrawColor(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    XGU_MAYBE_UNUSED xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    const SDL_FColor *color = &cmd->data.color.color;
    p = pb_begin();
    p = xgux_set_color4f(p, color->r, color->g, color->b, color->a);
    pb_end(p);

    return true;
}

static bool XBOX_RenderClear(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    XGU_MAYBE_UNUSED xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    const SDL_FColor color = cmd->data.color.color;

    const uint32_t color32 = ((uint32_t)(color.r * 255.0f) << 16) |
                             ((uint32_t)(color.g * 255.0f) << 8) |
                             ((uint32_t)(color.b * 255.0f) << 0) |
                             ((uint32_t)(color.a * 255.0f) << 24);

    pb_fill(0, 0, pb_back_buffer_width(), pb_back_buffer_height(), color32);
    return true;
}

static bool XBOX_RenderGeometry(SDL_Renderer *renderer, void *vertices, SDL_RenderCommand *cmd)
{
    XGU_MAYBE_UNUSED xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    const size_t count = cmd->data.draw.count;

    set_blend_mode(renderer, cmd->data.draw.blend);

    if (cmd->data.draw.texture) {
        xgu_texture_t *xgu_texture = (xgu_texture_t *)cmd->data.draw.texture->internal;

        p = pb_begin();

        if (render_data->texture_shader_active == 0) {
            texture_combiner_apply();
            render_data->texture_shader_active = 1;
        }

        // Neatest filtering is used for nearest and pixelart scale modes
        const XguTexFilter texture_filter =
            (cmd->data.draw.texture_scale_mode == SDL_SCALEMODE_LINEAR) ? XGU_TEXTURE_FILTER_LINEAR : XGU_TEXTURE_FILTER_NEAREST;

        if (render_data->active_texture != xgu_texture) {
            const int texture_index = 0;
            p = xgu_set_texture_offset(p, texture_index, xgu_texture->data_physical_address);
            p = xgu_set_texture_format(p, texture_index, 2, false, XGU_SOURCE_COLOR, 2, xgu_texture->format, 1,
                                       __builtin_ctz(xgu_texture->data_width), __builtin_ctz(xgu_texture->data_height), 0);
            p = xgu_set_texture_address(p, texture_index, XGU_CLAMP_TO_EDGE, false, XGU_CLAMP_TO_EDGE, false, XGU_CLAMP_TO_EDGE, false, false);
            p = xgu_set_texture_control0(p, texture_index, true, 0, 0);
            p = xgu_set_texture_control1(p, texture_index, xgu_texture->pitch);
            p = xgu_set_texture_image_rect(p, texture_index, xgu_texture->tex_width, xgu_texture->tex_height);
            p = xgu_set_texture_filter(p, texture_index, 0, XGU_TEXTURE_CONVOLUTION_GAUSSIAN, texture_filter, texture_filter, false, false, false, false);
            render_data->active_texture = xgu_texture;
        }

        pb_end(p);

        xgu_vertex_textured_t *xgu_verts = (xgu_vertex_textured_t *)vertices;
        clear_attribute_pointers();
        xgux_set_attrib_pointer(XGU_VERTEX_ARRAY, XGU_FLOAT,
                                SDL_arraysize(xgu_verts->pos), sizeof(xgu_vertex_textured_t), xgu_verts->pos);
        xgux_set_attrib_pointer(XGU_COLOR_ARRAY, XGU_UNSIGNED_BYTE_OGL,
                                sizeof(xgu_verts->color), sizeof(xgu_vertex_textured_t), xgu_verts->color);
        xgux_set_attrib_pointer(XGU_TEXCOORD0_ARRAY, XGU_FLOAT,
                                SDL_arraysize(xgu_verts->tex), sizeof(xgu_vertex_textured_t), xgu_verts->tex);
        xgux_draw_arrays(XGU_TRIANGLES, 0, count);

    } else {
        p = pb_begin();

        if (render_data->texture_shader_active == 1) {
            unlit_combiner_apply();
            render_data->texture_shader_active = 0;
        }

        pb_end(p);

        xgu_vertex_t *xgu_verts = (xgu_vertex_t *)vertices;
        clear_attribute_pointers();
        xgux_set_attrib_pointer(XGU_VERTEX_ARRAY, XGU_FLOAT,
                                SDL_arraysize(xgu_verts->pos), sizeof(xgu_vertex_t), xgu_verts->pos);
        xgux_set_attrib_pointer(XGU_COLOR_ARRAY, XGU_UNSIGNED_BYTE_OGL,
                                sizeof(xgu_verts->color), sizeof(xgu_vertex_t), xgu_verts->color);
        xgux_draw_arrays(XGU_TRIANGLES, 0, count);
    }

    return true;
}

static bool XBOX_RenderPoints(SDL_Renderer *renderer, void *vertices, SDL_RenderCommand *cmd)
{
    XGU_MAYBE_UNUSED xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    const size_t count = cmd->data.draw.count;

    set_blend_mode(renderer, cmd->data.draw.blend);

    xgu_point_t *xgu_verts = (xgu_point_t *)vertices;
    clear_attribute_pointers();
    xgux_set_attrib_pointer(XGU_VERTEX_ARRAY, XGU_FLOAT,
                            SDL_arraysize(xgu_verts->pos), sizeof(xgu_point_t), xgu_verts->pos);
    xgux_draw_arrays(XGU_POINTS, 0, count);

    return true;
}

static void XBOX_InvalidateCachedState(SDL_Renderer *renderer)
{
    (void)renderer;
}

static bool XBOX_RunCommandQueue(SDL_Renderer *renderer, SDL_RenderCommand *cmd, void *vertices, size_t vertsize)
{
    XGU_MAYBE_UNUSED xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    (void)vertsize;
    while (cmd) {
        switch (cmd->command) {
        case SDL_RENDERCMD_SETVIEWPORT:
        {
            XBOX_RenderSetViewPort(renderer, cmd);
            break;
        }
        case SDL_RENDERCMD_SETCLIPRECT:
        {
            XBOX_RenderSetClipRect(renderer, cmd);
            break;
        }
        case SDL_RENDERCMD_SETDRAWCOLOR:
        {
            XBOX_RenderSetDrawColor(renderer, cmd);
            break;
        }
        case SDL_RENDERCMD_CLEAR:
        {
            XBOX_RenderClear(renderer, cmd);
            break;
        }
        case SDL_RENDERCMD_DRAW_POINTS:
        {
            XBOX_RenderPoints(renderer, (uint8_t *)vertices + cmd->data.draw.first, cmd);
            break;
        }
        case SDL_RENDERCMD_GEOMETRY:
        {
            XBOX_RenderGeometry(renderer, (uint8_t *)vertices + cmd->data.draw.first, cmd);
            break;
        }
        // SDL should use XBOX_QueueGeometry instead of these commands.
        case SDL_RENDERCMD_DRAW_LINES:
        case SDL_RENDERCMD_FILL_RECTS:
        case SDL_RENDERCMD_COPY:
        case SDL_RENDERCMD_COPY_EX:
            break;
        case SDL_RENDERCMD_NO_OP:
            break;
        }
        cmd = cmd->next;
    }

    return true;
}

static SDL_Surface *XBOX_RenderReadPixels(SDL_Renderer *renderer, const SDL_Rect *rect)
{
    XGU_MAYBE_UNUSED xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    SDL_PixelFormat format = renderer->target ? renderer->target->format : SDL_PIXELFORMAT_ARGB8888;

    SDL_Surface *surface = SDL_CreateSurface(rect->w, rect->h, format);
    if (surface == NULL) {
        return NULL;
    }
    const SDL_PixelFormat dst_format = surface->format;
    const int dst_bytes_per_pixel = SDL_BYTESPERPIXEL(dst_format);
    const int dst_pitch = surface->pitch;
    uint8_t *dst8 = surface->pixels;

    // Ensure the back buffer is fully renderered before reading pixels
    p = pb_begin();
    p = pb_push1(p, NV097_NO_OPERATION, 0);
    p = pb_push1(p, NV097_WAIT_FOR_IDLE, 0);
    pb_end(p);

    while (pb_busy()) {
        Sleep(0);
    }

    XVideoFlushFB();

    // Get the back buffer as the source
    VIDEO_MODE vm = XVideoGetMode();
    SDL_PixelFormat src_format;
    if (vm.bpp == 15) {
        src_format = SDL_PIXELFORMAT_XRGB1555;
    } else if (vm.bpp == 16) {
        src_format = SDL_PIXELFORMAT_RGB565;
    } else {
        src_format = SDL_PIXELFORMAT_ARGB8888;
    }
    const int src_pitch = vm.width * ((vm.bpp + 7) / 8);
    const uint8_t *src8 = (const uint8_t *)pb_back_buffer();

    // Now copy the back buffer pixels to the surface
    SDL_ConvertPixels(rect->w, rect->h,
                      src_format, &src8[rect->y * src_pitch + rect->x * dst_bytes_per_pixel], src_pitch,
                      dst_format, &dst8[rect->y * dst_pitch + rect->x * dst_bytes_per_pixel], dst_pitch);

    return surface;
}

static bool XBOX_RenderPresent(SDL_Renderer *renderer)
{
    XGU_MAYBE_UNUSED xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;

    calculate_fps(2);

    while (pb_busy()) {
        Sleep(0);
    }

    while (pb_finished()) {
        Sleep(0);
    }

    calculate_fps(1);

    // It's probably better to wait on the vsync primitive here than it is to spin on pb_finished next loop
    pb_wait_for_vbl();

    // A back buffer frame is rendered so clear the vertex allocation tracking for that frame.
    render_data->frame_index = (render_data->frame_index + 1) % NXDK_PBKIT_BUFFER_COUNT;
    render_data->vertex_allocations[render_data->frame_index] = 0;

    // Reset for the next frame
    calculate_fps(0);
    pb_reset();
    pb_erase_depth_stencil_buffer(0, 0, pb_back_buffer_width(), pb_back_buffer_height());
    return true;
}

static void XBOX_DestroyRenderer(SDL_Renderer *renderer)
{
    XGU_MAYBE_UNUSED xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    pb_kill();

    SDL_free(render_data);
    MmFreeContiguousMemory(renderer->vertex_data);

    renderer->internal = NULL;
    renderer->vertex_data = NULL;
}

static bool XBOX_SetVSync(SDL_Renderer *renderer, const int vsync)
{
    (void)renderer;
    (void)vsync;

    // pbkit always flips on vblank and it can't easily be disabled.
    return SDL_Unsupported();
}

static bool XBOX_CreateRenderer(SDL_Renderer *renderer, SDL_Window *window, SDL_PropertiesID create_props)
{
    (void)create_props;
    xgu_render_data_t *render_data = (xgu_render_data_t *)SDL_calloc(1, sizeof(xgu_render_data_t));
    if (render_data == NULL) {
        return SDL_OutOfMemory();
    }

    const float m_identity[4 * 4] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    // Change the framebuffer surface format based on the video mode
    const VIDEO_MODE vm = XVideoGetMode();
    if (vm.bpp == 16) {
        pb_set_color_format(NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5, false);
    } else if (vm.bpp == 15) {
        pb_set_color_format(NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5, false);
    } else {
        pb_set_color_format(NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8, false);
    }

    pb_init();
    pb_show_front_screen();
    pb_target_back_buffer();

    p = pb_begin();
    combiner_init();
    unlit_combiner_apply();

    p = xgu_set_blend_enable(p, true);
    p = xgu_set_depth_test_enable(p, false);
    p = xgu_set_blend_func_sfactor(p, XGU_FACTOR_SRC_ALPHA);
    p = xgu_set_blend_func_dfactor(p, XGU_FACTOR_ONE_MINUS_SRC_ALPHA);
    p = xgu_set_depth_func(p, XGU_FUNC_LESS_OR_EQUAL);

    p = xgu_set_skin_mode(p, XGU_SKIN_MODE_OFF);
    p = xgu_set_normalization_enable(p, false);
    p = xgu_set_lighting_enable(p, false);
    p = xgu_set_cull_face_enable(p, false);
    p = xgu_set_clear_rect_vertical(p, 0, pb_back_buffer_height());
    p = xgu_set_clear_rect_horizontal(p, 0, pb_back_buffer_width());

    pb_end(p);

    for (int i = 0; i < XGU_TEXTURE_COUNT; i++) {
        p = pb_begin();
        p = xgu_set_texgen_s(p, i, XGU_TEXGEN_DISABLE);
        p = xgu_set_texgen_t(p, i, XGU_TEXGEN_DISABLE);
        p = xgu_set_texgen_r(p, i, XGU_TEXGEN_DISABLE);
        p = xgu_set_texgen_q(p, i, XGU_TEXGEN_DISABLE);
        p = xgu_set_texture_matrix_enable(p, i, false);
        p = xgu_set_texture_matrix(p, i, m_identity);
        pb_end(p);
    }

    for (int i = 0; i < XGU_WEIGHT_COUNT; i++) {
        p = pb_begin();
        p = xgu_set_model_view_matrix(p, i, m_identity);
        p = xgu_set_inverse_model_view_matrix(p, i, m_identity);
        pb_end(p);
    }

    p = pb_begin();
    p = xgu_set_transform_execution_mode(p, XGU_FIXED, XGU_RANGE_MODE_PRIVATE);
    p = xgu_set_projection_matrix(p, m_identity);
    p = xgu_set_composite_matrix(p, m_identity);
    p = xgu_set_viewport_offset(p, 0.0f, 0.0f, 0.0f, 0.0f);
    p = xgu_set_viewport_scale(p, 1.0f, 1.0f, 1.0f, 1.0f);
    p = xgu_set_scissor_rect(p, false, 0, 0, pb_back_buffer_width(), pb_back_buffer_height());
    pb_end(p);

    renderer->WindowEvent = XBOX_WindowEvent;
    renderer->CreateTexture = XBOX_CreateTexture;
    renderer->UpdateTexture = XBOX_UpdateTexture;
    renderer->LockTexture = XBOX_LockTexture;
    renderer->UnlockTexture = XBOX_UnlockTexture;
    renderer->SetRenderTarget = XBOX_SetRenderTarget;
    renderer->QueueSetViewport = XBOX_QueueNoOp;
    renderer->QueueSetDrawColor = XBOX_QueueNoOp;
    renderer->QueueDrawPoints = XBOX_QueueDrawPoints;
    renderer->QueueDrawLines = XBOX_QueueDrawPoints;
    renderer->QueueGeometry = XBOX_QueueGeometry;
    renderer->InvalidateCachedState = XBOX_InvalidateCachedState;
    renderer->RunCommandQueue = XBOX_RunCommandQueue;
    renderer->RenderPresent = XBOX_RenderPresent;
    renderer->DestroyTexture = XBOX_DestroyTexture;
    renderer->DestroyRenderer = XBOX_DestroyRenderer;
    renderer->RenderReadPixels = XBOX_RenderReadPixels;
    renderer->SetVSync = XBOX_SetVSync;
    renderer->internal = render_data;
    renderer->window = window;
    renderer->name = "nxdk_xgu";

    arena_init(renderer);

    // Initialize the default clip rect and viewport
    render_data->viewport = (SDL_Rect){ 0, 0, pb_back_buffer_width(), pb_back_buffer_height() };
    render_data->clip_rect = render_data->viewport;

    // Point the frame index to what would be the older frame which is the one just after the one we are rendering.
    render_data->frame_index = 1;

    // These are supported texture formats, however not all of them are supported as render targets.
    // There appears to be no way to differentiate this. CreateTexture will fail if the format is not supported as a render target.
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_RGB565);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_ARGB8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_XRGB8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_RGBA8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_ABGR8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_BGRA8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_ARGB4444);
    SDL_SetNumberProperty(SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 1024 * 1024);

    // This hint makes SDL use the geometry API to draw lines. This is more correct and allows for line thickness
    SDL_SetHint(SDL_HINT_RENDER_LINE_METHOD, "3");

    while (pb_busy()) {
        Sleep(0);
    }
    pb_reset();

    return true;
}

SDL_RenderDriver nxdk_RenderDriver = {
    XBOX_CreateRenderer, "nxdk_xgu"
};

static inline uint32_t npot2pot(uint32_t num)
{
    uint32_t msb;
    __asm__("bsr %1, %0" : "=r"(msb) : "r"(num));

    if ((1 << msb) == num) {
        return num;
    }

    return 1 << (msb + 1);
}

static bool sdl_to_xgu_surface_format(SDL_PixelFormat sdl_format, int *xgu_surface_format, int *bytes_per_pixel)
{
    switch (sdl_format) {
    case SDL_PIXELFORMAT_RGB565:
        *xgu_surface_format = NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5;
        *bytes_per_pixel = 2;
    case SDL_PIXELFORMAT_XRGB8888:
    case SDL_PIXELFORMAT_ARGB8888:
        *xgu_surface_format = NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8;
        *bytes_per_pixel = 4;
        return true;
    default:
        return false;
    }
}

static bool sdl_to_xgu_texture_format(SDL_PixelFormat fmt, int *xgu_format, int *bytes_per_pixel, bool swizzled)
{
    switch (fmt) {
    case SDL_PIXELFORMAT_ARGB1555:
        *xgu_format = (swizzled) ? XGU_TEXTURE_FORMAT_A1R5G5B5_SWIZZLED : XGU_TEXTURE_FORMAT_A1R5G5B5;
        *bytes_per_pixel = 2;
        return true;
    case SDL_PIXELFORMAT_RGB565:
        *xgu_format = (swizzled) ? XGU_TEXTURE_FORMAT_R5G6B5_SWIZZLED : XGU_TEXTURE_FORMAT_R5G6B5;
        *bytes_per_pixel = 2;
        return true;
    case SDL_PIXELFORMAT_ARGB8888:
        *xgu_format = (swizzled) ? XGU_TEXTURE_FORMAT_A8R8G8B8_SWIZZLED : XGU_TEXTURE_FORMAT_A8R8G8B8;
        *bytes_per_pixel = 4;
        return true;
    case SDL_PIXELFORMAT_XRGB8888:
        *xgu_format = (swizzled) ? XGU_TEXTURE_FORMAT_X8R8G8B8_SWIZZLED : XGU_TEXTURE_FORMAT_X8R8G8B8;
        *bytes_per_pixel = 4;
        return true;
    case SDL_PIXELFORMAT_RGBA8888:
        *xgu_format = (swizzled) ? XGU_TEXTURE_FORMAT_R8G8B8A8_SWIZZLED : XGU_TEXTURE_FORMAT_R8G8B8A8;
        *bytes_per_pixel = 4;
        return true;
    case SDL_PIXELFORMAT_ABGR8888:
        *xgu_format = (swizzled) ? XGU_TEXTURE_FORMAT_A8B8G8R8_SWIZZLED : XGU_TEXTURE_FORMAT_A8B8G8R8;
        *bytes_per_pixel = 4;
        return true;
    case SDL_PIXELFORMAT_ARGB4444:
        *xgu_format = (swizzled) ? XGU_TEXTURE_FORMAT_A4R4G4B4_SWIZZLED : XGU_TEXTURE_FORMAT_A4R4G4B4;
        *bytes_per_pixel = 2;
        return true;
    case SDL_PIXELFORMAT_XRGB1555:
        *xgu_format = (swizzled) ? XGU_TEXTURE_FORMAT_X1R5G5B5_SWIZZLED : XGU_TEXTURE_FORMAT_X1R5G5B5;
        *bytes_per_pixel = 2;
        return true;
    default:
        return false;
    }
}

// Although SDL provides SDL_AllocateRenderVertices, this allocates on the heap,
// which we don't want. Instead we provide our own that uses contiguous memory allocation
static bool arena_init(SDL_Renderer *renderer)
{
    renderer->vertex_data = MmAllocateContiguousMemoryEx(SDL_XGU_VERTEX_BUFFER_SIZE, 0, 0xFFFFFFFF, 0,
                                                         PAGE_WRITECOMBINE | PAGE_READWRITE);
    if (renderer->vertex_data == NULL) {
        SDL_SetError("Failed to allocate XGU arena");
        return false;
    }

    return true;
}

static void *arena_allocate(SDL_Renderer *renderer, size_t size, size_t *offset)
{
    xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    int total_allocated = 0;

    // Ensure alignment. If every allocation is aligned, we know every pointer will be aligned.
    size = (size + SDL_XGU_VERTEX_ALIGNMENT - 1) & ~(SDL_XGU_VERTEX_ALIGNMENT - 1);

    if (render_data->vertex_arena_offset + size > SDL_XGU_VERTEX_BUFFER_SIZE) {
        // We lost some space to end padding, so ensure we tag that on when we validate our allocation size
        total_allocated += render_data->vertex_arena_offset + size - SDL_XGU_VERTEX_BUFFER_SIZE;

        // Round robin back to the start of the arena
        render_data->vertex_arena_offset = 0;
    }

    // Area we going to overflow the vertex buffer?
    for (int i = 0; i < NXDK_PBKIT_BUFFER_COUNT; i++) {
        total_allocated += render_data->vertex_allocations[i];
    }
    if (total_allocated + size > SDL_XGU_VERTEX_BUFFER_SIZE) {
        SDL_Log("Vertex buffer overflow. Increase SDL_XGU_VERTEX_BUFFER_SIZE", size);
        return NULL;
    }

    void *ptr = (void *)((intptr_t)renderer->vertex_data + render_data->vertex_arena_offset);

    *offset = render_data->vertex_arena_offset;
    render_data->vertex_arena_offset += size;
    render_data->vertex_allocations[render_data->frame_index] += size;
    return ptr;
}

static inline void clear_attribute_pointers(void)
{
    for (int i = 0; i < XGU_ATTRIBUTE_COUNT; i++) {
        xgux_set_attrib_pointer(i, XGU_FLOAT, 0, 0, NULL);
    }
}

static void set_blend_mode(SDL_Renderer *renderer, SDL_BlendMode blendMode)
{
    XGU_MAYBE_UNUSED xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;

    if (blendMode == render_data->active_blend_mode) {
        return;
    }

    XguBlendFactor sfactor;
    XguBlendFactor dfactor;

    switch (blendMode) {
    case SDL_BLENDMODE_NONE:
        sfactor = XGU_FACTOR_ONE;
        dfactor = XGU_FACTOR_ZERO;
        break;
    case SDL_BLENDMODE_BLEND:
        sfactor = XGU_FACTOR_SRC_ALPHA;
        dfactor = XGU_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case SDL_BLENDMODE_BLEND_PREMULTIPLIED:
        sfactor = XGU_FACTOR_ONE;
        dfactor = XGU_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case SDL_BLENDMODE_ADD:
        sfactor = XGU_FACTOR_SRC_ALPHA;
        dfactor = XGU_FACTOR_ONE;
        break;
    case SDL_BLENDMODE_ADD_PREMULTIPLIED:
        sfactor = XGU_FACTOR_ONE;
        dfactor = XGU_FACTOR_ONE;
        break;
    case SDL_BLENDMODE_MUL:
        sfactor = XGU_FACTOR_DST_COLOR;
        dfactor = XGU_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case SDL_BLENDMODE_MOD:
        sfactor = XGU_FACTOR_ZERO;
        dfactor = XGU_FACTOR_SRC_COLOR;
        break;
    default:
        SDL_Log("Unsupported blend mode %d, defaulting to SDL_BLENDMODE_BLEND", blendMode);
        sfactor = XGU_FACTOR_SRC_ALPHA;
        dfactor = XGU_FACTOR_ONE_MINUS_SRC_ALPHA;
    }

    p = pb_begin();
    p = xgu_set_blend_func_sfactor(p, sfactor);
    p = xgu_set_blend_func_dfactor(p, dfactor);
    p = push_command_parameter(p, NV097_SET_BLEND_EQUATION, NV097_SET_BLEND_EQUATION_V_FUNC_ADD);
    pb_end(p);

    render_data->active_blend_mode = blendMode;
}

static SDL_Rect sanitize_scissor_rect(SDL_Renderer *renderer, const SDL_Rect *rect)
{
    XGU_MAYBE_UNUSED xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    SDL_Rect scissor_rect = {
        .x = rect->x,
        .y = rect->y,
        .w = rect->w,
        .h = rect->h
    };

    scissor_rect.w = SDL_max(scissor_rect.w, 0);
    scissor_rect.h = SDL_max(scissor_rect.h, 0);

    if (render_data->active_render_target) {
        scissor_rect.x = SDL_clamp(scissor_rect.x, 0, render_data->active_render_target->tex_width);
        scissor_rect.y = SDL_clamp(scissor_rect.y, 0, render_data->active_render_target->tex_height);
        scissor_rect.w = SDL_min(scissor_rect.w, render_data->active_render_target->tex_width - scissor_rect.x);
        scissor_rect.h = SDL_min(scissor_rect.h, render_data->active_render_target->tex_height - scissor_rect.y);
    } else {
        scissor_rect.x = SDL_clamp(scissor_rect.x, 0, (int)pb_back_buffer_width());
        scissor_rect.y = SDL_clamp(scissor_rect.y, 0, (int)pb_back_buffer_height());
        scissor_rect.w = SDL_min(scissor_rect.w, (int)pb_back_buffer_width() - scissor_rect.x);
        scissor_rect.h = SDL_min(scissor_rect.h, (int)pb_back_buffer_height() - scissor_rect.y);
    }

    return scissor_rect;
}

// clang-format off
static inline void combiner_init(void)
{
   pb_push1(p, NV097_SET_SHADER_OTHER_STAGE_INPUT,
    XGU_MASK(NV097_SET_SHADER_OTHER_STAGE_INPUT_STAGE1, 0)
    | XGU_MASK(NV097_SET_SHADER_OTHER_STAGE_INPUT_STAGE2, 0)
    | XGU_MASK(NV097_SET_SHADER_OTHER_STAGE_INPUT_STAGE3, 0));
    p += 2;
    pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM,
        XGU_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE0, NV097_SET_SHADER_STAGE_PROGRAM_STAGE0_PROGRAM_NONE)
        | XGU_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE1, NV097_SET_SHADER_STAGE_PROGRAM_STAGE1_PROGRAM_NONE)
        | XGU_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE2, NV097_SET_SHADER_STAGE_PROGRAM_STAGE2_PROGRAM_NONE)
        | XGU_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE3, NV097_SET_SHADER_STAGE_PROGRAM_STAGE3_PROGRAM_NONE));
    p += 2;

    pb_push1(p, NV097_SET_COMBINER_COLOR_ICW + 0 * 4,
    XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_MAP, 0x6)
    | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_MAP, 0x1)
    | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_MAP, 0x0)
    | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_MAP, 0x0));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_COLOR_OCW + 0 * 4,
        XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DST, 0x4)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_CD_DST, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_SUM_DST, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_MUX_ENABLE, 0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DOT_ENABLE, 0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_CD_DOT_ENABLE, 0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_OP, NV097_SET_COMBINER_COLOR_OCW_OP_NOSHIFT));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW + 0 * 4,
        XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_MAP, 0x6)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_MAP, 0x1)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_MAP, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_MAP, 0x0));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW + 0 * 4,
        XGU_MASK(NV097_SET_COMBINER_ALPHA_OCW_AB_DST, 0x4)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_OCW_CD_DST, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_OCW_SUM_DST, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_OCW_MUX_ENABLE, 0)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_OCW_OP, NV097_SET_COMBINER_ALPHA_OCW_OP_NOSHIFT));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_CONTROL,
        XGU_MASK(NV097_SET_COMBINER_CONTROL_FACTOR0, NV097_SET_COMBINER_CONTROL_FACTOR0_SAME_FACTOR_ALL)
        | XGU_MASK(NV097_SET_COMBINER_CONTROL_FACTOR1, NV097_SET_COMBINER_CONTROL_FACTOR1_SAME_FACTOR_ALL)
        | XGU_MASK(NV097_SET_COMBINER_CONTROL_ITERATION_COUNT, 1));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0,
        XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_INVERSE, 0));
    p += 2;
    pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1,
        XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_SPECULAR_CLAMP, 0));
    p += 2;
}

static inline void unlit_combiner_apply (void)
{
    p = pb_push1(p, NV097_SET_SHADER_OTHER_STAGE_INPUT, 0);
    p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM, 0);

    p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW + 0 * 4,
    XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_MAP, 0x6)
    | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_MAP, 0x1)
    | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_MAP, 0x0)
    | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_MAP, 0x0));

    p = pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW + 0 * 4,
        XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_MAP, 0x6)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_MAP, 0x1)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_MAP, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_MAP, 0x0));
}

static inline void texture_combiner_apply (void)
{
    p = pb_push1(p, NV097_SET_SHADER_OTHER_STAGE_INPUT, 0);
    p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM, XGU_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE0, NV097_SET_SHADER_STAGE_PROGRAM_STAGE0_2D_PROJECTIVE));

    p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW + 0 * 4,
    XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_SOURCE, 0x8) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_MAP, 0x6)
    | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_MAP, 0x6)
    | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_MAP, 0x0)
    | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_MAP, 0x0));
  
    p = pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW + 0 * 4,
        XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_SOURCE, 0x8) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_MAP, 0x6)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_MAP, 0x6)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_MAP, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_MAP, 0x0));
}
// clang-format on

#if SDL_XGU_SHOW_FPS
static uint64_t frame_start;
static uint64_t frame_time = 0;
static int frame_time_index = 0;
static float fps = 0.0f;

// This calculation is a bit more complex than usual but because pbkit forces a vblank flip
// we will often just get a capped FPS. This calculates the average frame rendering time without the
// vblank wait and then calculate the maximum 'theoretical FPS' based on that.
// This is not perfect but it gives a better idea of the actual rendering performance.
static void calculate_fps(int stage)
{
    // Stage 0 initializes the frame start time
    if (stage == 0) {
        frame_start = SDL_GetTicksNS();
        return;
    }
    // Stage 1 calculates the average frame time and FPS
    else if (stage == 1) {
        const int average_frame_count = 60;
        uint64_t frame_end = SDL_GetTicksNS();
        frame_time += (frame_end - frame_start);
        if (frame_time_index++ == average_frame_count - 1) {
            float frame_time_ms = (float)((double)frame_time / 1000000.0);
            fps = 1000.0f * (float)average_frame_count / frame_time_ms;
            if (fps < 5.0f) {
                fps = 5.0f; // Cap the FPS to a minimum of 5 to avoid too low values
            }
            frame_time_index = 0;
            frame_time = 0.0f;
        }
        return;
    }
    // Stage 2 displays the FPS on the screen
    else if (stage == 2) {
        char pb_text[16];
        pb_erase_text_screen();
        int len = SDL_snprintf(pb_text, sizeof(pb_text), "FPS: %.02f", fps);
        pb_fill(20, 25, len * 10, 20, 0xFF000000);
        pb_print(pb_text);
        pb_draw_text_screen();
        return;
    }
}
#else
static void calculate_fps(int stage)
{
    (void)stage;
}
#endif

#endif // SDL_VIDEO_RENDER_XGU
