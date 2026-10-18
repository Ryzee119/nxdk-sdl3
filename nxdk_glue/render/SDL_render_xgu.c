// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2025 Ryan Wendland

#include <../src/SDL_internal.h>

#ifdef SDL_VIDEO_RENDER_XGU

#include "swizzle.h"
#include "xgu/xgux.h"
#include <../src/render/SDL_sysrender.h>
#include <SDL3/SDL_pixels.h>
#include <hal/video.h>
#include <stddef.h>
#include <stdint.h>

#define nxdk_RenderDriver GPU_RenderDriver

// pb_reset() drains the GPU every frame, so the entire arena is available each frame.
#ifndef SDL_XGU_VERTEX_BUFFER_SIZE
#define SDL_XGU_VERTEX_BUFFER_SIZE (512 * 1024)
#endif

#ifndef SDL_XGU_SHOW_FPS
#define SDL_XGU_SHOW_FPS 0
#endif

// RunCommandQueue batches commands into one pushbuffer block and kicks the GPU
// (pb_end) once this many dwords have accumulated. pbkit's pushbuffer threshold
// reserves 128 dwords before wrap-around, so single blocks must never exceed 128
// dwords. Keep headroom for state setup + draw packet before kicking.
#ifndef SDL_XGU_KICK_DWORDS
#ifdef DBG
#define SDL_XGU_KICK_DWORDS 0
#else
#define SDL_XGU_KICK_DWORDS 80
#endif
#endif

// NV2A places pixel sample centers at integer coordinates (0.0, 0.0), whereas
// SDL3 places pixel (0, 0)'s center at (0.5, 0.5). We apply a -0.5f hardware
// viewport offset so SDL geometry aligns with NV2A pixel/texel centers, and bias
// integer point/line coordinates by +0.5f to land on pixel centers.
#define SDL_XGU_PIXEL_BIAS (0.5f)

// CRTC scanout start address register (see nxdk lib/pbkit/outer.h)
#define SDL_XGU_PCRTC_START 0x00600800

// Non-increasing PFIFO method flag for batching multiple NV097_DRAW_ARRAYS dwords
#define SDL_XGU_METHOD_NON_INC 0x40000000u

#ifndef NV097_INVALIDATE_VERTEX_CACHE_FILE
#define NV097_INVALIDATE_VERTEX_CACHE_FILE 0x00001710
#endif

typedef struct xgu_texture
{
    int data_width;
    int data_height;
    int tex_width;
    int tex_height;
    int bytes_per_pixel;
    int pitch;
    int swizzled;
    float u_scale;
    float v_scale;
    XguTexFormatColor format;
    XguTexFilter filter;
    XguTextureAddress mode_u;
    XguTextureAddress mode_v;
    uint8_t *data;
    uint8_t *data_physical_address;
    uint32_t last_used_epoch;
} xgu_texture_t;

typedef struct xgu_point
{
    float pos[2];
} xgu_point_t;

typedef struct xgu_vertex
{
    float pos[2];
    uint32_t color32;
} xgu_vertex_t;

typedef struct xgu_vertex_texture
{
    float pos[2];
    uint32_t color32;
    float tex[2];
} xgu_vertex_textured_t;

typedef enum xgu_vertex_layout
{
    XGU_VERT_LAYOUT_UNKNOWN = 0,
    XGU_VERT_LAYOUT_POINT,
    XGU_VERT_LAYOUT_UNLIT,
    XGU_VERT_LAYOUT_TEXTURED,
} xgu_vertex_layout_t;

typedef struct xgu_render_data
{
    int texture_shader_active;
    xgu_vertex_layout_t active_vertex_layout;
    uint32_t active_vertex_base;
    const xgu_texture_t *active_texture;
    xgu_texture_t *active_render_target;
    uint32_t active_surface_format;
    uint32_t gpu_epoch;
    SDL_Rect viewport;
    SDL_Rect clip_rect;
    bool viewport_dirty;
    bool clip_rect_enabled;
    bool scissor_empty;
    SDL_BlendMode active_blend_mode;
    void *vertex_data;
    uint32_t vertex_data_physical;
    int vertex_arena_offset;
    struct s_CtxDma render_target_dma_ctx;
} xgu_render_data_t;

enum fps_stage
{
    FPS_STAGE_RESET,
    FPS_STAGE_CALCULATE,
    FPS_STAGE_DISPLAY,
};

// Pushbuffer pointer
static uint32_t *p = NULL;

static inline void drain_gpu(xgu_render_data_t *render_data)
{
    while (pb_busy()) {
        Sleep(0);
    }
    render_data->gpu_epoch++;
}

static inline void wait_texture_idle(xgu_render_data_t *render_data, xgu_texture_t *xgu_texture)
{
    if (xgu_texture->last_used_epoch == render_data->gpu_epoch) {
        drain_gpu(render_data);
    }
    // Force xgu_set_texture_offset to re-push on next draw to invalidate NV2A texture cache
    if (render_data->active_texture == xgu_texture) {
        render_data->active_texture = NULL;
    }
}

static inline void get_active_target_size(const xgu_render_data_t *render_data, int *w, int *h)
{
    if (render_data->active_render_target) {
        *w = render_data->active_render_target->tex_width;
        *h = render_data->active_render_target->tex_height;
    } else {
        *w = (int)pb_back_buffer_width();
        *h = (int)pb_back_buffer_height();
    }
}

// NV2A window clip max is inclusive (xgu_set_scissor_rect treats it as exclusive). w/h must be > 0.
static inline uint32_t *push_scissor(uint32_t *pb, int x, int y, int w, int h)
{
    pb = push_command_boolean(pb, NV097_SET_WINDOW_CLIP_TYPE, false);
    pb = push_command_parameter(pb, NV097_SET_WINDOW_CLIP_HORIZONTAL, ((uint32_t)(x + w - 1) << 16) | (uint32_t)x);
    pb = push_command_parameter(pb, NV097_SET_WINDOW_CLIP_VERTICAL, ((uint32_t)(y + h - 1) << 16) | (uint32_t)y);
    return pb;
}

static inline uint32_t *push_current_scissor(SDL_Renderer *renderer, uint32_t *pb)
{
    xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    SDL_Rect rect = render_data->viewport;

    if (render_data->clip_rect_enabled) {
        SDL_Rect abs_clip = render_data->clip_rect;
        abs_clip.x += render_data->viewport.x;
        abs_clip.y += render_data->viewport.y;
        if (!SDL_GetRectIntersection(&abs_clip, &render_data->viewport, &rect)) {
            rect = (SDL_Rect){ 0, 0, 0, 0 };
        }
    }

    int target_w, target_h;
    get_active_target_size(render_data, &target_w, &target_h);
    rect.x = SDL_clamp(rect.x, 0, target_w);
    rect.y = SDL_clamp(rect.y, 0, target_h);
    rect.w = SDL_clamp(rect.w, 0, target_w - rect.x);
    rect.h = SDL_clamp(rect.h, 0, target_h - rect.y);

    // The hardware cannot express an empty clip region; draws are skipped instead.
    render_data->scissor_empty = (rect.w <= 0 || rect.h <= 0);
    if (render_data->scissor_empty) {
        return pb;
    }
    return push_scissor(pb, rect.x, rect.y, rect.w, rect.h);
}

#if SDL_XGU_SHOW_FPS
static uint64_t frame_start;
static uint64_t frame_time = 0;
static int frame_time_index = 0;
static float fps = 0.0f;

static void calculate_fps(enum fps_stage stage)
{
    if (stage == FPS_STAGE_RESET) {
        frame_start = SDL_GetTicksNS();
    } else if (stage == FPS_STAGE_CALCULATE) {
        const int average_frame_count = 60;
        frame_time += (SDL_GetTicksNS() - frame_start);
        if (frame_time_index++ == average_frame_count - 1) {
            fps = 1e9f * (float)average_frame_count / (float)frame_time;
            frame_time_index = 0;
            frame_time = 0;
        }
    } else if (stage == FPS_STAGE_DISPLAY) {
        char pb_text[16];
        p = pb_begin();
        p = push_scissor(p, 0, 0, (int)pb_back_buffer_width(), (int)pb_back_buffer_height());
        pb_end(p);
        pb_erase_text_screen();
        int len = SDL_snprintf(pb_text, sizeof(pb_text), "FPS: %.02f", fps);
        pb_fill(20, 25, len * 10, 20, 0xFF000000);
        pb_print(pb_text);
        pb_draw_text_screen();
    }
}
#else
static inline void calculate_fps(enum fps_stage stage)
{
    (void)stage;
}
#endif

static bool sdl_to_xgu_surface_format(SDL_PixelFormat sdl_format, int *xgu_surface_format)
{
    switch (sdl_format) {
    case SDL_PIXELFORMAT_RGB565:
        *xgu_surface_format = NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5;
        return true;
    case SDL_PIXELFORMAT_XRGB8888:
    case SDL_PIXELFORMAT_ARGB8888:
        *xgu_surface_format = NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8;
        return true;
    default:
        return false;
    }
}

static bool sdl_to_xgu_texture_format(SDL_PixelFormat fmt, XguTexFormatColor *xgu_format, int *bytes_per_pixel, bool swizzled)
{
    *bytes_per_pixel = SDL_BYTESPERPIXEL(fmt);
    switch (fmt) {
    case SDL_PIXELFORMAT_ARGB1555:
        *xgu_format = swizzled ? XGU_TEXTURE_FORMAT_A1R5G5B5_SWIZZLED : XGU_TEXTURE_FORMAT_A1R5G5B5;
        return true;
    case SDL_PIXELFORMAT_RGB565:
        *xgu_format = swizzled ? XGU_TEXTURE_FORMAT_R5G6B5_SWIZZLED : XGU_TEXTURE_FORMAT_R5G6B5;
        return true;
    case SDL_PIXELFORMAT_ARGB8888:
        *xgu_format = swizzled ? XGU_TEXTURE_FORMAT_A8R8G8B8_SWIZZLED : XGU_TEXTURE_FORMAT_A8R8G8B8;
        return true;
    case SDL_PIXELFORMAT_XRGB8888:
        *xgu_format = swizzled ? XGU_TEXTURE_FORMAT_X8R8G8B8_SWIZZLED : XGU_TEXTURE_FORMAT_X8R8G8B8;
        return true;
    case SDL_PIXELFORMAT_RGBA8888:
        *xgu_format = swizzled ? XGU_TEXTURE_FORMAT_R8G8B8A8_SWIZZLED : XGU_TEXTURE_FORMAT_R8G8B8A8;
        return true;
    case SDL_PIXELFORMAT_ABGR8888:
        *xgu_format = swizzled ? XGU_TEXTURE_FORMAT_A8B8G8R8_SWIZZLED : XGU_TEXTURE_FORMAT_A8B8G8R8;
        return true;
    case SDL_PIXELFORMAT_ARGB4444:
        *xgu_format = swizzled ? XGU_TEXTURE_FORMAT_A4R4G4B4_SWIZZLED : XGU_TEXTURE_FORMAT_A4R4G4B4;
        return true;
    case SDL_PIXELFORMAT_XRGB1555:
        *xgu_format = swizzled ? XGU_TEXTURE_FORMAT_X1R5G5B5_SWIZZLED : XGU_TEXTURE_FORMAT_X1R5G5B5;
        return true;
    default:
        return false;
    }
}

static void *arena_allocate(SDL_Renderer *renderer, size_t count, size_t elem_size, size_t *vertex_data_offset)
{
    xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    const size_t size = count * elem_size;

    // Align to elem_size so byte_offset / elem_size is an exact vertex index for NV097_DRAW_ARRAYS.
    // Consecutive allocations of the same elem_size already have rem == 0, preserving coalescing.
    const size_t cur = (size_t)render_data->vertex_arena_offset;
    const size_t rem = cur % elem_size;
    const size_t aligned_offset = cur + (rem ? (elem_size - rem) : 0);

    if (aligned_offset + size > (size_t)SDL_XGU_VERTEX_BUFFER_SIZE) {
        SDL_Log("Vertex buffer overflow (%zu bytes requested). Increase SDL_XGU_VERTEX_BUFFER_SIZE", size);
        return NULL;
    }

    *vertex_data_offset = aligned_offset;
    render_data->vertex_arena_offset = (int)(aligned_offset + size);
    return (uint8_t *)render_data->vertex_data + aligned_offset;
}

static void set_blend_mode(SDL_Renderer *renderer, SDL_BlendMode blendMode)
{
    xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    if (blendMode == render_data->active_blend_mode) {
        return;
    }

    if (blendMode == SDL_BLENDMODE_NONE) {
        p = xgu_set_blend_enable(p, false);
    } else {
        XguBlendFactor sfactor = XGU_FACTOR_SRC_ALPHA;
        XguBlendFactor dfactor = XGU_FACTOR_ONE_MINUS_SRC_ALPHA;

        switch (blendMode) {
        case SDL_BLENDMODE_BLEND:
            break;
        case SDL_BLENDMODE_BLEND_PREMULTIPLIED:
            sfactor = XGU_FACTOR_ONE;
            break;
        case SDL_BLENDMODE_ADD:
            dfactor = XGU_FACTOR_ONE;
            break;
        case SDL_BLENDMODE_ADD_PREMULTIPLIED:
            sfactor = XGU_FACTOR_ONE;
            dfactor = XGU_FACTOR_ONE;
            break;
        case SDL_BLENDMODE_MUL:
            sfactor = XGU_FACTOR_DST_COLOR;
            break;
        case SDL_BLENDMODE_MOD:
            sfactor = XGU_FACTOR_ZERO;
            dfactor = XGU_FACTOR_SRC_COLOR;
            break;
        default:
            SDL_Log("Unsupported blend mode %d, defaulting to SDL_BLENDMODE_BLEND", blendMode);
            break;
        }

        p = xgu_set_blend_enable(p, true);
        p = xgu_set_blend_func_sfactor(p, sfactor);
        p = xgu_set_blend_func_dfactor(p, dfactor);
        p = push_command_parameter(p, NV097_SET_BLEND_EQUATION, NV097_SET_BLEND_EQUATION_V_FUNC_ADD);
    }

    render_data->active_blend_mode = blendMode;
}

// clang-format off
static inline void combiner_init(void)
{
    p = pb_push1(p, NV097_SET_SHADER_OTHER_STAGE_INPUT, 0);

    p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW + 0 * 4,
        XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DST, 0x4)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_CD_DST, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_SUM_DST, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_MUX_ENABLE, 0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_AB_DOT_ENABLE, 0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_CD_DOT_ENABLE, 0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_OCW_OP, NV097_SET_COMBINER_COLOR_OCW_OP_NOSHIFT));

    p = pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW + 0 * 4,
        XGU_MASK(NV097_SET_COMBINER_ALPHA_OCW_AB_DST, 0x4)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_OCW_CD_DST, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_OCW_SUM_DST, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_OCW_MUX_ENABLE, 0)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_OCW_OP, NV097_SET_COMBINER_ALPHA_OCW_OP_NOSHIFT));

    p = pb_push1(p, NV097_SET_COMBINER_CONTROL,
        XGU_MASK(NV097_SET_COMBINER_CONTROL_FACTOR0, NV097_SET_COMBINER_CONTROL_FACTOR0_SAME_FACTOR_ALL)
        | XGU_MASK(NV097_SET_COMBINER_CONTROL_FACTOR1, NV097_SET_COMBINER_CONTROL_FACTOR1_SAME_FACTOR_ALL)
        | XGU_MASK(NV097_SET_COMBINER_CONTROL_ITERATION_COUNT, 1));

    p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0,
        XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_A_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_B_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_INVERSE, 0));

    p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1,
        XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_E_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_F_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_SOURCE, 0x4) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_G_INVERSE, 0)
        | XGU_MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW1_SPECULAR_CLAMP, 0));
}

static inline void set_texture_shader(xgu_render_data_t *render_data, int textured)
{
    if (render_data->texture_shader_active == textured) {
        return;
    }
    render_data->texture_shader_active = textured;

    const uint32_t a_src = textured ? 0x8 : 0x4;
    const uint32_t b_src = textured ? 0x4 : 0x0;
    const uint32_t b_map = textured ? 0x6 : 0x1;

    p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM,
        textured ? XGU_MASK(NV097_SET_SHADER_STAGE_PROGRAM_STAGE0, NV097_SET_SHADER_STAGE_PROGRAM_STAGE0_2D_PROJECTIVE) : 0);

    p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW + 0 * 4,
        XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_SOURCE, a_src) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_A_MAP, 0x6)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_SOURCE, b_src) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_B_MAP, b_map)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_C_MAP, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_ALPHA, 0) | XGU_MASK(NV097_SET_COMBINER_COLOR_ICW_D_MAP, 0x0));

    p = pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW + 0 * 4,
        XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_SOURCE, a_src) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_A_MAP, 0x6)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_SOURCE, b_src) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_B_MAP, b_map)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_C_MAP, 0x0)
        | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_SOURCE, 0x0) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_ALPHA, 1) | XGU_MASK(NV097_SET_COMBINER_ALPHA_ICW_D_MAP, 0x0));
}
// clang-format on

static inline uint32_t pack_color_rgba8888(const SDL_FColor *c, float c_scale)
{
    uint32_t r = (uint32_t)(int)(SDL_clamp(c->r * c_scale, 0.0f, 255.0f) + 0.5f);
    uint32_t g = (uint32_t)(int)(SDL_clamp(c->g * c_scale, 0.0f, 255.0f) + 0.5f);
    uint32_t b = (uint32_t)(int)(SDL_clamp(c->b * c_scale, 0.0f, 255.0f) + 0.5f);
    uint32_t a = (uint32_t)(int)(SDL_clamp(c->a * 255.0f, 0.0f, 255.0f) + 0.5f);

    return r | (g << 8) | (b << 16) | (a << 24);
}

static inline bool fcolor_bits_equal(const SDL_FColor *a, const SDL_FColor *b)
{
    const uint32_t *ua = (const uint32_t *)a;
    const uint32_t *ub = (const uint32_t *)b;
    return ((ua[0] ^ ub[0]) | (ua[1] ^ ub[1]) | (ua[2] ^ ub[2]) | (ua[3] ^ ub[3])) == 0;
}

static inline int get_vertex_index(const void *indices, int size_indices, int i)
{
    if (size_indices == 2)
        return ((const uint16_t *)indices)[i];
    if (size_indices == 4)
        return (int)((const uint32_t *)indices)[i];
    if (size_indices == 1)
        return ((const uint8_t *)indices)[i];
    return i;
}

static inline bool is_quad_at(const void *indices, int size_indices, int i, int v)
{
    return get_vertex_index(indices, size_indices, i + 0) == v + 0 &&
           get_vertex_index(indices, size_indices, i + 1) == v + 1 &&
           get_vertex_index(indices, size_indices, i + 2) == v + 2 &&
           get_vertex_index(indices, size_indices, i + 3) == v + 0 &&
           get_vertex_index(indices, size_indices, i + 4) == v + 2 &&
           get_vertex_index(indices, size_indices, i + 5) == v + 3;
}

static inline void generate_swizzle_masks(int width, int height, uint32_t *mask_x, uint32_t *mask_y)
{
    uint32_t x = 0, y = 0, mask_bit = 1;
    for (int bit = 1; bit < width || bit < height; bit <<= 1) {
        if (bit < width) {
            x |= mask_bit;
            mask_bit <<= 1;
        }
        if (bit < height) {
            y |= mask_bit;
            mask_bit <<= 1;
        }
    }
    *mask_x = x;
    *mask_y = y;
}

static inline uint32_t swizzle_coord(int val, uint32_t mask)
{
    uint32_t result = 0;
    for (int bit = 1; val && mask; bit <<= 1) {
        uint32_t lsb = mask & (~mask + 1u);
        if (val & bit) {
            result |= lsb;
        }
        mask &= ~lsb;
    }
    return result;
}

static void swizzle_subrect(const uint8_t *src, int src_pitch,
                            uint8_t *dst_base, int tex_w, int tex_h, int bpp,
                            const SDL_Rect *rect)
{
    uint32_t mask_x, mask_y;
    generate_swizzle_masks(tex_w, tex_h, &mask_x, &mask_y);
    const uint32_t start_x = swizzle_coord(rect->x, mask_x);
    uint32_t y_ofs = swizzle_coord(rect->y, mask_y);

    if (bpp == 4) {
        uint32_t *dst32 = (uint32_t *)dst_base;
        for (int y = 0; y < rect->h; y++) {
            const uint32_t *src_row = (const uint32_t *)(src + y * src_pitch);
            uint32_t x_ofs = start_x;
            for (int x = 0; x < rect->w; x++) {
                dst32[x_ofs | y_ofs] = src_row[x];
                x_ofs = (x_ofs - mask_x) & mask_x;
            }
            y_ofs = (y_ofs - mask_y) & mask_y;
        }
    } else if (bpp == 2) {
        uint16_t *dst16 = (uint16_t *)dst_base;
        for (int y = 0; y < rect->h; y++) {
            const uint16_t *src_row = (const uint16_t *)(src + y * src_pitch);
            uint32_t x_ofs = start_x;
            for (int x = 0; x < rect->w; x++) {
                dst16[x_ofs | y_ofs] = src_row[x];
                x_ofs = (x_ofs - mask_x) & mask_x;
            }
            y_ofs = (y_ofs - mask_y) & mask_y;
        }
    }
}

static inline int bind_vertex_stream(xgu_render_data_t *render_data, xgu_vertex_layout_t layout,
                                     uint32_t byte_offset, int count, uint32_t stride)
{
    uint32_t start_index = byte_offset / stride;
    uint32_t base_phys = render_data->vertex_data_physical;
    if (start_index + (uint32_t)count > 65535u) {
        base_phys += byte_offset;
        start_index = 0;
    }

    if (render_data->active_vertex_layout != layout) {
        p = xgu_set_vertex_data_array_format(p, XGU_VERTEX_ARRAY, XGU_FLOAT, 2, stride);
        if (layout == XGU_VERT_LAYOUT_POINT) {
            p = xgu_set_vertex_data_array_format(p, XGU_COLOR_ARRAY, XGU_FLOAT, 0, 0);
        } else {
            p = xgu_set_vertex_data_array_format(p, XGU_COLOR_ARRAY, XGU_UNSIGNED_BYTE_OGL, 4, stride);
        }
        p = xgu_set_vertex_data_array_format(p, XGU_TEXCOORD0_ARRAY, XGU_FLOAT,
                                             (layout == XGU_VERT_LAYOUT_TEXTURED) ? 2 : 0,
                                             (layout == XGU_VERT_LAYOUT_TEXTURED) ? stride : 0);
        render_data->active_vertex_layout = layout;
        render_data->active_vertex_base = 0;
    }

    if (render_data->active_vertex_base != base_phys) {
        p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + XGU_VERTEX_ARRAY * 4, base_phys);
        if (layout != XGU_VERT_LAYOUT_POINT) {
            p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + XGU_COLOR_ARRAY * 4,
                         base_phys + offsetof(xgu_vertex_t, color32));
        }
        if (layout == XGU_VERT_LAYOUT_TEXTURED) {
            p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + XGU_TEXCOORD0_ARRAY * 4,
                         base_phys + offsetof(xgu_vertex_textured_t, tex));
        }
        p = pb_push1(p, NV097_INVALIDATE_VERTEX_CACHE_FILE, 0);
        render_data->active_vertex_base = base_phys;
    }

    return (int)start_index;
}

static inline uint32_t *xgu_emit_draw_arrays(uint32_t *pb, uint32_t **block_start,
                                             XguPrimitiveType mode, int start, int count)
{
    const int max_batch = (mode == XGU_TRIANGLES) ? 255 : 256;

    pb = xgu_begin(pb, mode);

    while (count > 0) {
        if (pb - *block_start > SDL_XGU_KICK_DWORDS) {
            pb_end(pb);
            pb = pb_begin();
            *block_start = pb;
        }

        const int num_batches = SDL_min((count + max_batch - 1) / max_batch, 32);
        *pb++ = SDL_XGU_METHOD_NON_INC | ((uint32_t)num_batches << 18) | NV097_DRAW_ARRAYS;
        for (int i = 0; i < num_batches; i++) {
            int batch = SDL_min(count, max_batch);
            *pb++ = XGU_MASK(NV097_DRAW_ARRAYS_COUNT, batch - 1) |
                    XGU_MASK(NV097_DRAW_ARRAYS_START_INDEX, start);
            start += batch;
            count -= batch;
        }
    }

    return xgu_end(pb);
}

static void XBOX_WindowEvent(SDL_Renderer *renderer, const SDL_WindowEvent *event)
{
    (void)renderer;
    (void)event;
}

static bool XBOX_CreateTexture(SDL_Renderer *renderer, SDL_Texture *texture, SDL_PropertiesID create_props)
{
    (void)renderer;
    xgu_texture_t *xgu_texture = (xgu_texture_t *)SDL_calloc(1, sizeof(xgu_texture_t));
    if (xgu_texture == NULL) {
        return SDL_OutOfMemory();
    }

    const Sint64 access = SDL_GetNumberProperty(create_props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, 0);
    const bool is_render_target = (access == SDL_TEXTUREACCESS_TARGET);

    int surface_format;
    if (is_render_target && !sdl_to_xgu_surface_format(texture->format, &surface_format)) {
        SDL_free(xgu_texture);
        return SDL_SetError("[nxdk renderer] Unsupported render target format (%s)", SDL_GetPixelFormatName(texture->format));
    }

    // Swizzle static POT textures (at least 4x4) for faster sampling and hardware wrapping support
    const bool is_pot = (texture->w >= 4) && ((texture->w & (texture->w - 1)) == 0) &&
                        (texture->h >= 4) && ((texture->h & (texture->h - 1)) == 0);
    if (access == SDL_TEXTUREACCESS_STATIC && is_pot) {
        xgu_texture->swizzled = 1;
    }

    if (!sdl_to_xgu_texture_format(texture->format, &xgu_texture->format, &xgu_texture->bytes_per_pixel, xgu_texture->swizzled)) {
        SDL_free(xgu_texture);
        return SDL_SetError("[nxdk renderer] Unsupported texture format (%s)", SDL_GetPixelFormatName(texture->format));
    }

    xgu_texture->tex_width = texture->w;
    xgu_texture->tex_height = texture->h;

    if (xgu_texture->swizzled) {
        xgu_texture->data_width = texture->w;
        xgu_texture->data_height = texture->h;
        xgu_texture->u_scale = 1.0f;
        xgu_texture->v_scale = 1.0f;
    } else {
        // NV2A pitch alignment is 64 bytes for render targets and 8 bytes for linear textures
        const int min_dim = 8 / xgu_texture->bytes_per_pixel;
        const int align = (is_render_target ? 64 : 8) / xgu_texture->bytes_per_pixel;
        xgu_texture->data_width = (texture->w + align - 1) & ~(align - 1);
        xgu_texture->data_height = SDL_max(texture->h, min_dim);
        xgu_texture->u_scale = (float)texture->w;
        xgu_texture->v_scale = (float)texture->h;
    }

    xgu_texture->pitch = xgu_texture->data_width * xgu_texture->bytes_per_pixel;

    const SIZE_T allocation_size = (SIZE_T)xgu_texture->data_height * xgu_texture->pitch;
    xgu_texture->data = MmAllocateContiguousMemoryEx(allocation_size, 0, SDL_MAX_UINT32, 0, PAGE_WRITECOMBINE | PAGE_READWRITE);
    if (xgu_texture->data == NULL) {
        SDL_free(xgu_texture);
        return SDL_OutOfMemory();
    }
    xgu_texture->data_physical_address = (uint8_t *)MmGetPhysicalAddress(xgu_texture->data);
    SDL_memset(xgu_texture->data, 0, allocation_size);

    texture->internal = xgu_texture;
    return true;
}

static void XBOX_DestroyTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    xgu_texture_t *xgu_texture = (xgu_texture_t *)texture->internal;
    if (xgu_texture == NULL) {
        return;
    }

    if (render_data->active_render_target == xgu_texture) {
        render_data->active_render_target = NULL;
    }

    wait_texture_idle(render_data, xgu_texture);

    MmFreeContiguousMemory(xgu_texture->data);
    SDL_free(xgu_texture);
    texture->internal = NULL;
}

static bool XBOX_LockTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect, void **pixels, int *pitch)
{
    xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    xgu_texture_t *xgu_texture = (xgu_texture_t *)texture->internal;

    wait_texture_idle(render_data, xgu_texture);

    *pixels = &xgu_texture->data[rect->y * xgu_texture->pitch + rect->x * xgu_texture->bytes_per_pixel];
    *pitch = xgu_texture->pitch;
    return true;
}

static void XBOX_UnlockTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    (void)renderer;
    (void)texture;
}

static bool XBOX_UpdateTexture(SDL_Renderer *renderer, SDL_Texture *texture,
                               const SDL_Rect *rect, const void *pixels, int pitch)
{
    xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    xgu_texture_t *xgu_texture = (xgu_texture_t *)texture->internal;
    const uint8_t *src = (const uint8_t *)pixels;

    wait_texture_idle(render_data, xgu_texture);

    if (xgu_texture->swizzled) {
        if (rect->x == 0 && rect->y == 0 &&
            rect->w == xgu_texture->tex_width && rect->h == xgu_texture->tex_height) {
            swizzle_rect(src, xgu_texture->tex_width, xgu_texture->tex_height, xgu_texture->data, pitch, xgu_texture->bytes_per_pixel);
        } else {
            swizzle_subrect(src, pitch, xgu_texture->data,
                            xgu_texture->tex_width, xgu_texture->tex_height,
                            xgu_texture->bytes_per_pixel, rect);
        }
    } else {
        uint8_t *dst = &xgu_texture->data[rect->y * xgu_texture->pitch + rect->x * xgu_texture->bytes_per_pixel];
        const size_t row_bytes = (size_t)rect->w * xgu_texture->bytes_per_pixel;
        if (pitch == xgu_texture->pitch && row_bytes == (size_t)xgu_texture->pitch) {
            SDL_memcpy(dst, src, row_bytes * (size_t)rect->h);
        } else {
            for (int y = 0; y < rect->h; y++) {
                SDL_memcpy(dst, src, row_bytes);
                src += pitch;
                dst += xgu_texture->pitch;
            }
        }
    }

    return true;
}

static bool XBOX_SetRenderTarget(SDL_Renderer *renderer, SDL_Texture *texture)
{
    xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    xgu_texture_t *xgu_texture = texture ? (xgu_texture_t *)texture->internal : NULL;
    extern unsigned int pb_ColorFmt; // From pbkit.c

    if (render_data->active_render_target == xgu_texture) {
        return true;
    }

    uint32_t pitch, clip_width, clip_height, dma_channel, color_offset;

    if (xgu_texture == NULL) {
        dma_channel = DMA_CHANNEL_PIXEL_RENDERER;
        color_offset = 0;
        pitch = pb_back_buffer_pitch();
        clip_width = pb_back_buffer_width();
        clip_height = pb_back_buffer_height();
        render_data->active_surface_format = pb_ColorFmt;
    } else {
        int surface_format;
        bool status = sdl_to_xgu_surface_format(texture->format, &surface_format);
        assert(status);

        dma_channel = render_data->render_target_dma_ctx.ChannelID;
        color_offset = (uint32_t)xgu_texture->data_physical_address;
        pitch = xgu_texture->pitch;
        clip_width = xgu_texture->tex_width;
        clip_height = xgu_texture->tex_height;
        render_data->active_surface_format = (uint32_t)surface_format;
        xgu_texture->last_used_epoch = render_data->gpu_epoch;
    }

    const uint32_t format = XGU_MASK(NV097_SET_SURFACE_FORMAT_COLOR, render_data->active_surface_format) |
                            XGU_MASK(NV097_SET_SURFACE_FORMAT_ZETA, NV097_SET_SURFACE_FORMAT_ZETA_Z24S8) |
                            XGU_MASK(NV097_SET_SURFACE_FORMAT_TYPE, NV097_SET_SURFACE_FORMAT_TYPE_PITCH);
    const uint32_t zpitch = pb_back_buffer_width() * 4;

    render_data->active_render_target = xgu_texture;
    render_data->active_texture = NULL; // Force texture cache flush when target is later bound as texture
    render_data->viewport = (SDL_Rect){ 0, 0, (int)clip_width, (int)clip_height };
    render_data->clip_rect = render_data->viewport;
    render_data->viewport_dirty = false;
    render_data->clip_rect_enabled = false;
    render_data->scissor_empty = false;

    p = pb_begin();
    p = pb_push1(p, NV097_WAIT_FOR_IDLE, 0);
    p = pb_push1(p, NV097_SET_CONTEXT_DMA_COLOR, dma_channel);
    p = pb_push1(p, NV097_SET_SURFACE_PITCH,
                 XGU_MASK(NV097_SET_SURFACE_PITCH_COLOR, pitch) |
                     XGU_MASK(NV097_SET_SURFACE_PITCH_ZETA, zpitch));
    p = pb_push1(p, NV097_SET_SURFACE_COLOR_OFFSET, color_offset);
    p = pb_push1(p, NV097_SET_SURFACE_CLIP_HORIZONTAL,
                 XGU_MASK(NV097_SET_SURFACE_CLIP_HORIZONTAL_WIDTH, clip_width) |
                     XGU_MASK(NV097_SET_SURFACE_CLIP_HORIZONTAL_X, 0));
    p = pb_push1(p, NV097_SET_SURFACE_CLIP_VERTICAL,
                 XGU_MASK(NV097_SET_SURFACE_CLIP_VERTICAL_HEIGHT, clip_height) |
                     XGU_MASK(NV097_SET_SURFACE_CLIP_VERTICAL_Y, 0));
    p = pb_push1(p, NV097_SET_SURFACE_FORMAT, format);
    p = xgu_set_viewport_offset(p, 0.0f, 0.0f, 0.0f, 0.0f);
    p = push_scissor(p, 0, 0, (int)clip_width, (int)clip_height);
    pb_end(p);

    return true;
}

static bool XBOX_QueueDrawPoints(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count)
{
    xgu_point_t *verts = (xgu_point_t *)arena_allocate(renderer, (size_t)count, sizeof(xgu_point_t), &cmd->data.draw.first);
    if (!verts) {
        return SDL_OutOfMemory();
    }

    for (int i = 0; i < count; i++) {
        verts[i].pos[0] = points[i].x + SDL_XGU_PIXEL_BIAS;
        verts[i].pos[1] = points[i].y + SDL_XGU_PIXEL_BIAS;
    }
    cmd->data.draw.count = count;

    return true;
}

static bool XBOX_QueueGeometry(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
                               const float *xy, int xy_stride, const SDL_FColor *color, int color_stride,
                               const float *uv, int uv_stride,
                               int num_vertices, const void *indices, int num_indices, int size_indices,
                               float scale_x, float scale_y)
{
    size_indices = indices ? size_indices : 0;

    // Detect SDL quad batches (0,1,2, 0,2,3 per 4 vertices) so we can emit 4-vertex
    // hardware XGU_QUADS instead of expanding to 6-vertex triangle pairs.
    bool is_quad_batch = (size_indices == 2 || size_indices == 4) &&
                         (num_vertices > 0) && ((num_vertices & 3) == 0) &&
                         (num_indices == (num_vertices / 4) * 6);

    if (is_quad_batch) {
        for (int v = 0, i = 0; v < num_vertices; v += 4, i += 6) {
            if (!is_quad_at(indices, size_indices, i, v)) {
                is_quad_batch = false;
                break;
            }
        }
    }

    const int count = is_quad_batch ? num_vertices : (indices ? num_indices : num_vertices);
    const size_t sz = texture ? sizeof(xgu_vertex_textured_t) : sizeof(xgu_vertex_t);

    uint8_t *vertices = (uint8_t *)arena_allocate(renderer, (size_t)count, sz, &cmd->data.draw.first);
    if (!vertices) {
        return SDL_OutOfMemory();
    }

    cmd->data.draw.count = count;
    if (is_quad_batch) {
        // Tag bit 0 of byte offset (always 4-byte aligned) to signal XGU_QUADS
        cmd->data.draw.first |= 1u;
        size_indices = 0;
    }

    const float c_scale = cmd->data.draw.color_scale * 255.0f;
    const SDL_FColor *last_color_ptr = color;
    uint32_t cached_color32 = pack_color_rgba8888(color, c_scale);

    if (texture) {
        xgu_vertex_textured_t *out = (xgu_vertex_textured_t *)vertices;
        const xgu_texture_t *xgu_tex = (const xgu_texture_t *)texture->internal;
        const float u_scale = xgu_tex->u_scale;
        const float v_scale = xgu_tex->v_scale;

        for (int i = 0; i < count; i++) {
            const int j = size_indices ? get_vertex_index(indices, size_indices, i) : i;
            const float *vp = (const float *)((const char *)xy + j * xy_stride);
            const float *vt = (const float *)((const char *)uv + j * uv_stride);

            if (color_stride != 0) {
                const SDL_FColor *fc = (const SDL_FColor *)((const char *)color + j * color_stride);
                if (!fcolor_bits_equal(fc, last_color_ptr)) {
                    last_color_ptr = fc;
                    cached_color32 = pack_color_rgba8888(fc, c_scale);
                }
            }

            out[i].pos[0] = vp[0] * scale_x;
            out[i].pos[1] = vp[1] * scale_y;
            out[i].color32 = cached_color32;
            out[i].tex[0] = vt[0] * u_scale;
            out[i].tex[1] = vt[1] * v_scale;
        }
    } else {
        xgu_vertex_t *out = (xgu_vertex_t *)vertices;

        for (int i = 0; i < count; i++) {
            const int j = size_indices ? get_vertex_index(indices, size_indices, i) : i;
            const float *vp = (const float *)((const char *)xy + j * xy_stride);

            if (color_stride != 0) {
                const SDL_FColor *fc = (const SDL_FColor *)((const char *)color + j * color_stride);
                if (!fcolor_bits_equal(fc, last_color_ptr)) {
                    last_color_ptr = fc;
                    cached_color32 = pack_color_rgba8888(fc, c_scale);
                }
            }

            out[i].pos[0] = vp[0] * scale_x;
            out[i].pos[1] = vp[1] * scale_y;
            out[i].color32 = cached_color32;
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
    xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    const SDL_Rect *viewport = &cmd->data.viewport.rect;

    if (!render_data->viewport_dirty &&
        SDL_memcmp(viewport, &render_data->viewport, sizeof(SDL_Rect)) == 0) {
        return true;
    }

    render_data->viewport = *viewport;
    render_data->viewport_dirty = false;

    p = xgu_set_viewport_offset(p, (float)viewport->x, (float)viewport->y, 0.0f, 0.0f);
    p = push_current_scissor(renderer, p);

    return true;
}

static bool XBOX_RenderSetClipRect(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    const bool enabled = cmd->data.cliprect.enabled;
    const SDL_Rect *clip_rect = &cmd->data.cliprect.rect;

    if (render_data->clip_rect_enabled == enabled &&
        (!enabled || SDL_memcmp(clip_rect, &render_data->clip_rect, sizeof(SDL_Rect)) == 0)) {
        return true;
    }

    render_data->clip_rect_enabled = enabled;
    if (enabled) {
        render_data->clip_rect = *clip_rect;
    }

    p = push_current_scissor(renderer, p);

    return true;
}

static bool XBOX_RenderClear(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    const SDL_FColor color = cmd->data.color.color;

    const uint32_t r = (uint32_t)(int)(SDL_clamp(color.r * 255.0f, 0.0f, 255.0f) + 0.5f);
    const uint32_t g = (uint32_t)(int)(SDL_clamp(color.g * 255.0f, 0.0f, 255.0f) + 0.5f);
    const uint32_t b = (uint32_t)(int)(SDL_clamp(color.b * 255.0f, 0.0f, 255.0f) + 0.5f);
    const uint32_t a = (uint32_t)(int)(SDL_clamp(color.a * 255.0f, 0.0f, 255.0f) + 0.5f);

    uint32_t clear_color;
    if (render_data->active_surface_format == NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5) {
        clear_color = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    } else if (render_data->active_surface_format == NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5) {
        clear_color = ((a >> 7) << 15) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    } else {
        clear_color = (a << 24) | (r << 16) | (g << 8) | b;
    }

    int target_w, target_h;
    get_active_target_size(render_data, &target_w, &target_h);

    p = push_scissor(p, 0, 0, target_w, target_h);
    p = pb_push1(p, NV097_SET_CLEAR_RECT_HORIZONTAL, ((target_w - 1) << 16));
    p = pb_push1(p, NV097_SET_CLEAR_RECT_VERTICAL, ((target_h - 1) << 16));
    p = pb_push1(p, NV097_SET_COLOR_CLEAR_VALUE, clear_color);
    p = pb_push1(p, NV097_CLEAR_SURFACE, NV097_CLEAR_SURFACE_COLOR);
    p = push_current_scissor(renderer, p);

    return true;
}

static bool XBOX_RenderGeometry(SDL_Renderer *renderer, SDL_RenderCommand *cmd, uint32_t **block_start)
{
    xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    const int count = (int)cmd->data.draw.count;
    if (count <= 0 || render_data->scissor_empty) {
        return true;
    }

    set_blend_mode(renderer, cmd->data.draw.blend);

    const XguPrimitiveType prim = (cmd->data.draw.first & 1u) ? XGU_QUADS : XGU_TRIANGLES;
    const uint32_t byte_offset = (uint32_t)(cmd->data.draw.first & ~1u);

    if (cmd->data.draw.texture) {
        xgu_texture_t *xgu_texture = (xgu_texture_t *)cmd->data.draw.texture->internal;
        xgu_texture->last_used_epoch = render_data->gpu_epoch;

        set_texture_shader(render_data, 1);

        const XguTexFilter texture_filter =
            (cmd->data.draw.texture_scale_mode == SDL_SCALEMODE_LINEAR) ? XGU_TEXTURE_FILTER_LINEAR : XGU_TEXTURE_FILTER_NEAREST;

        // NV2A only supports hardware wrapping on swizzled POT textures
        const XguTextureAddress mode_u =
            (!xgu_texture->swizzled || cmd->data.draw.texture_address_mode_u == SDL_TEXTURE_ADDRESS_CLAMP)
                ? XGU_CLAMP_TO_EDGE
                : XGU_WRAP;
        const XguTextureAddress mode_v =
            (!xgu_texture->swizzled || cmd->data.draw.texture_address_mode_v == SDL_TEXTURE_ADDRESS_CLAMP)
                ? XGU_CLAMP_TO_EDGE
                : XGU_WRAP;

        if (render_data->active_texture != xgu_texture) {
            const int size_u = xgu_texture->swizzled ? __builtin_ctz(xgu_texture->data_width) : 0;
            const int size_v = xgu_texture->swizzled ? __builtin_ctz(xgu_texture->data_height) : 0;

            p = xgu_set_texture_offset(p, 0, xgu_texture->data_physical_address);
            p = xgu_set_texture_format(p, 0, 2, false, XGU_SOURCE_COLOR, 2, xgu_texture->format, 1, size_u, size_v, 0);
            p = xgu_set_texture_control0(p, 0, true, 0, 0);
            p = xgu_set_texture_control1(p, 0, xgu_texture->pitch);
            p = xgu_set_texture_image_rect(p, 0, xgu_texture->tex_width, xgu_texture->tex_height);
            render_data->active_texture = xgu_texture;
            xgu_texture->filter = -1;
            xgu_texture->mode_u = -1;
            xgu_texture->mode_v = -1;
        }

        if (xgu_texture->filter != texture_filter) {
            p = xgu_set_texture_filter(p, 0, 0, XGU_TEXTURE_CONVOLUTION_GAUSSIAN,
                                       texture_filter, texture_filter, false, false, false, false);
            xgu_texture->filter = texture_filter;
        }

        if (xgu_texture->mode_u != mode_u || xgu_texture->mode_v != mode_v) {
            p = xgu_set_texture_address(p, 0, mode_u, false, mode_v, false, XGU_CLAMP_TO_EDGE, false, false);
            xgu_texture->mode_u = mode_u;
            xgu_texture->mode_v = mode_v;
        }

        int start = bind_vertex_stream(render_data, XGU_VERT_LAYOUT_TEXTURED, byte_offset, count, sizeof(xgu_vertex_textured_t));
        p = xgu_emit_draw_arrays(p, block_start, prim, start, count);
    } else {
        set_texture_shader(render_data, 0);
        int start = bind_vertex_stream(render_data, XGU_VERT_LAYOUT_UNLIT, byte_offset, count, sizeof(xgu_vertex_t));
        p = xgu_emit_draw_arrays(p, block_start, prim, start, count);
    }

    return true;
}

static bool XBOX_RenderPrimitives(SDL_Renderer *renderer, SDL_RenderCommand *cmd, XguPrimitiveType mode, uint32_t **block_start)
{
    xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    const int count = (int)cmd->data.draw.count;
    if (count < ((mode == XGU_LINE_STRIP) ? 2 : 1) || render_data->scissor_empty) {
        return true;
    }

    set_blend_mode(renderer, cmd->data.draw.blend);
    set_texture_shader(render_data, 0);

    const uint32_t color32 = pack_color_rgba8888(&cmd->data.draw.color, cmd->data.draw.color_scale * 255.0f);
    const int start = bind_vertex_stream(render_data, XGU_VERT_LAYOUT_POINT, (uint32_t)cmd->data.draw.first, count, sizeof(xgu_point_t));

    p = pb_push1(p, NV097_SET_VERTEX_DATA4UB + XGU_COLOR_ARRAY * 4, color32);
    p = xgu_emit_draw_arrays(p, block_start, mode, start, count);

    return true;
}

static void XBOX_InvalidateCachedState(SDL_Renderer *renderer)
{
    xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    render_data->active_texture = NULL;
    render_data->active_vertex_layout = XGU_VERT_LAYOUT_UNKNOWN;
    render_data->active_vertex_base = 0;
    render_data->active_blend_mode = SDL_BLENDMODE_INVALID;
    render_data->texture_shader_active = -1;
    render_data->viewport_dirty = true;
}

static bool XBOX_RunCommandQueue(SDL_Renderer *renderer, SDL_RenderCommand *cmd, void *vertices, size_t vertsize)
{
    (void)vertices;
    (void)vertsize;

    xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    if (render_data->active_render_target) {
        render_data->active_render_target->last_used_epoch = render_data->gpu_epoch;
    }

    p = pb_begin();
    uint32_t *block_start = p;

    while (cmd) {
        if (p - block_start > SDL_XGU_KICK_DWORDS) {
            pb_end(p);
            p = pb_begin();
            block_start = p;
        }

        switch (cmd->command) {
        case SDL_RENDERCMD_SETVIEWPORT:
            XBOX_RenderSetViewPort(renderer, cmd);
            break;
        case SDL_RENDERCMD_SETCLIPRECT:
            XBOX_RenderSetClipRect(renderer, cmd);
            break;
        case SDL_RENDERCMD_CLEAR:
            XBOX_RenderClear(renderer, cmd);
            break;
        case SDL_RENDERCMD_DRAW_POINTS:
        {
            SDL_RenderCommand *first_cmd = cmd;
            while (cmd->next &&
                   cmd->next->command == SDL_RENDERCMD_DRAW_POINTS &&
                   cmd->next->data.draw.blend == first_cmd->data.draw.blend &&
                   cmd->next->data.draw.color_scale == first_cmd->data.draw.color_scale &&
                   fcolor_bits_equal(&cmd->next->data.draw.color, &first_cmd->data.draw.color) &&
                   cmd->next->data.draw.first == first_cmd->data.draw.first + first_cmd->data.draw.count * sizeof(xgu_point_t)) {
                first_cmd->data.draw.count += cmd->next->data.draw.count;
                cmd = cmd->next;
            }
            XBOX_RenderPrimitives(renderer, first_cmd, XGU_POINTS, &block_start);
            break;
        }
        case SDL_RENDERCMD_DRAW_LINES:
            XBOX_RenderPrimitives(renderer, cmd, XGU_LINE_STRIP, &block_start);
            break;
        case SDL_RENDERCMD_GEOMETRY:
        {
            SDL_RenderCommand *first_cmd = cmd;
            const size_t v_size = first_cmd->data.draw.texture ? sizeof(xgu_vertex_textured_t) : sizeof(xgu_vertex_t);
            const size_t first_flag = first_cmd->data.draw.first & 1u;
            const size_t first_offset = first_cmd->data.draw.first & ~1u;
            while (cmd->next &&
                   cmd->next->command == SDL_RENDERCMD_GEOMETRY &&
                   (cmd->next->data.draw.first & 1u) == first_flag &&
                   cmd->next->data.draw.texture == first_cmd->data.draw.texture &&
                   cmd->next->data.draw.blend == first_cmd->data.draw.blend &&
                   cmd->next->data.draw.texture_scale_mode == first_cmd->data.draw.texture_scale_mode &&
                   cmd->next->data.draw.texture_address_mode_u == first_cmd->data.draw.texture_address_mode_u &&
                   cmd->next->data.draw.texture_address_mode_v == first_cmd->data.draw.texture_address_mode_v &&
                   (cmd->next->data.draw.first & ~1u) == first_offset + first_cmd->data.draw.count * v_size) {
                first_cmd->data.draw.count += cmd->next->data.draw.count;
                cmd = cmd->next;
            }
            XBOX_RenderGeometry(renderer, first_cmd, &block_start);
            break;
        }
        default:
            break;
        }
        cmd = cmd->next;
    }

    pb_end(p);
    return true;
}

static SDL_Surface *XBOX_RenderReadPixels(SDL_Renderer *renderer, const SDL_Rect *rect)
{
    xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    const xgu_texture_t *target = render_data->active_render_target;

    SDL_PixelFormat src_format;
    int src_pitch;
    int src_bpp;
    const uint8_t *src8;

    p = pb_begin();
    p = pb_push1(p, NV097_NO_OPERATION, 0);
    p = pb_push1(p, NV097_WAIT_FOR_IDLE, 0);
    pb_end(p);

    drain_gpu(render_data);
    XVideoFlushFB();

    if (target) {
        src_format = renderer->target->format;
        src_pitch = target->pitch;
        src_bpp = target->bytes_per_pixel;
        src8 = target->data;
    } else {
        VIDEO_MODE vm = XVideoGetMode();
        if (vm.bpp == 15) {
            src_format = SDL_PIXELFORMAT_XRGB1555;
        } else if (vm.bpp == 16) {
            src_format = SDL_PIXELFORMAT_RGB565;
        } else {
            src_format = SDL_PIXELFORMAT_ARGB8888;
        }
        src_pitch = pb_back_buffer_pitch();
        src_bpp = (vm.bpp + 7) / 8;
        src8 = (const uint8_t *)pb_back_buffer();
    }

    SDL_Surface *surface = SDL_CreateSurface(rect->w, rect->h, src_format);
    if (surface == NULL) {
        return NULL;
    }

    SDL_ConvertPixels(rect->w, rect->h,
                      src_format, &src8[rect->y * src_pitch + rect->x * src_bpp], src_pitch,
                      surface->format, surface->pixels, surface->pitch);

    return surface;
}

static bool XBOX_RenderPresent(SDL_Renderer *renderer)
{
    xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    extern unsigned int pb_ColorFmt;

#if SDL_XGU_SHOW_FPS
    while (pb_busy()) {
    }
    calculate_fps(FPS_STAGE_CALCULATE);
    calculate_fps(FPS_STAGE_DISPLAY);
#endif

    // Wait while all 3 pbkit back buffers are queued, and ensure scanout of the new back buffer has finished
    while (pb_finished()) {
        pb_wait_for_vbl();
    }
    const uint32_t back_buffer = (uint32_t)pb_back_buffer() & 0x03FFFFFF;
    while ((VIDEOREG(SDL_XGU_PCRTC_START) & 0x03FFFFFF) == back_buffer) {
        pb_wait_for_vbl();
    }

    pb_reset();

    // pb_reset() drains PGRAPH and resets surface registers to the back buffer.
    // Reset active_vertex_base so the next frame re-pushes vertex array offsets and
    // invalidates the NV2A post-T&L vertex cache when reusing vertex indices from 0.
    render_data->gpu_epoch++;
    render_data->vertex_arena_offset = 0;
    render_data->active_vertex_base = 0;
    render_data->active_render_target = NULL;
    render_data->active_surface_format = pb_ColorFmt;

#if SDL_XGU_SHOW_FPS
    p = pb_begin();
    p = push_current_scissor(renderer, p);
    pb_end(p);
#endif

    calculate_fps(FPS_STAGE_RESET);
    return true;
}

static void XBOX_DestroyRenderer(SDL_Renderer *renderer)
{
    xgu_render_data_t *render_data = (xgu_render_data_t *)renderer->internal;
    if (render_data) {
        while (pb_busy()) {
            Sleep(0);
        }
        pb_kill();
        if (render_data->vertex_data) {
            MmFreeContiguousMemory(render_data->vertex_data);
        }
        SDL_free(render_data);
    }
    renderer->internal = NULL;
    renderer->vertex_data = NULL;
}

static bool XBOX_SetVSync(SDL_Renderer *renderer, const int vsync)
{
    (void)renderer;
    if (vsync == 0 || vsync == 1) {
        return true;
    }
    return SDL_Unsupported();
}

static bool XBOX_CreateRenderer(SDL_Renderer *renderer, SDL_Window *window, SDL_PropertiesID create_props)
{
    (void)create_props;
    extern unsigned int pb_ColorFmt;

    xgu_render_data_t *render_data = (xgu_render_data_t *)SDL_calloc(1, sizeof(xgu_render_data_t));
    if (render_data == NULL) {
        return SDL_OutOfMemory();
    }

    render_data->vertex_data = MmAllocateContiguousMemoryEx(SDL_XGU_VERTEX_BUFFER_SIZE, 0, 0xFFFFFFFF, 0,
                                                            PAGE_WRITECOMBINE | PAGE_READWRITE);
    if (render_data->vertex_data == NULL) {
        SDL_free(render_data);
        return SDL_SetError("Failed to allocate XGU arena");
    }

    render_data->vertex_data_physical = (uint32_t)MmGetPhysicalAddress(render_data->vertex_data);
    renderer->vertex_data = render_data->vertex_data;
    renderer->internal = render_data;

    const float m_identity[4 * 4] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    const VIDEO_MODE vm = XVideoGetMode();
    if (vm.bpp == 16) {
        pb_set_color_format(NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5, false);
    } else if (vm.bpp == 15) {
        pb_set_color_format(NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5, false);
    } else {
        pb_set_color_format(NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8, false);
    }

    while (pb_init() < 0) {
        DbgPrint("[nxdk renderer] pbkit initialization failed, retrying...\n");
        Sleep(10);
    }

    XVideoSetVideoEnable(true);
    pb_show_front_screen();
    pb_target_back_buffer();

    render_data->texture_shader_active = -1;

    p = pb_begin();
    combiner_init();
    set_texture_shader(render_data, 0);
    p = xgu_set_blend_enable(p, false);
    p = xgu_set_depth_test_enable(p, false);
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
    for (int i = 0; i < XGU_ATTRIBUTE_COUNT; i++) {
        p = xgu_set_vertex_data_array_format(p, i, XGU_FLOAT, 0, 0);
        p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + i * 4, 0);
    }
    p = xgu_set_transform_execution_mode(p, XGU_FIXED, XGU_RANGE_MODE_PRIVATE);
    p = xgu_set_projection_matrix(p, m_identity);
    p = xgu_set_composite_matrix(p, m_identity);
    p = xgu_set_viewport_offset(p, 0.0f, 0.0f, 0.0f, 0.0f);
    p = xgu_set_viewport_scale(p, 1.0f, 1.0f, 1.0f, 1.0f);
    p = push_scissor(p, 0, 0, (int)pb_back_buffer_width(), (int)pb_back_buffer_height());
    pb_end(p);

    const int SDL_XGU_RENDER_TARGET_DMA_CHANNEL = 3;
    pb_create_dma_ctx(SDL_XGU_RENDER_TARGET_DMA_CHANNEL, DMA_CLASS_3D, 0, MAXRAM, &render_data->render_target_dma_ctx);
    pb_bind_channel(&render_data->render_target_dma_ctx);

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
    renderer->window = window;
    renderer->name = "nxdk_xgu";
    renderer->npot_texture_wrap_unsupported = true;

    render_data->viewport = (SDL_Rect){ 0, 0, (int)pb_back_buffer_width(), (int)pb_back_buffer_height() };
    render_data->clip_rect = render_data->viewport;
    render_data->active_blend_mode = SDL_BLENDMODE_NONE;
    render_data->active_surface_format = pb_ColorFmt;
    render_data->gpu_epoch = 1;

    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_RGB565);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_ARGB8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_XRGB8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_RGBA8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_ABGR8888);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_ARGB4444);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_ARGB1555);
    SDL_AddSupportedTextureFormat(renderer, SDL_PIXELFORMAT_XRGB1555);
    SDL_SetNumberProperty(SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 4096);

    SDL_SetHint(SDL_HINT_RENDER_LINE_METHOD, "2");

    while (pb_busy()) {
        Sleep(0);
    }
    pb_reset();
    calculate_fps(FPS_STAGE_RESET);

    return true;
}

SDL_RenderDriver nxdk_RenderDriver = {
    XBOX_CreateRenderer, "nxdk_xgu"
};

#endif // SDL_VIDEO_RENDER_XGU
