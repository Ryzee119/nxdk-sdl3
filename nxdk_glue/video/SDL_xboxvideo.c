// SPDX-License-Identifier: Zlib
// SPDX-FileCopyrightText: 2019 2021 Matt Borgerson
// SPDX-FileCopyrightText: 2020 Jannik Vogel
// SPDX-FileCopyrightText: 2025 Ryan Wendland

#include "SDL_xboxvideo.h"

#define XBOXVID_DRIVER_NAME "xbox"
#define XBOX_SURFACE        "_SDL_XboxSurface"

/* Currently only one window */
static SDL_Window *xbox_window = NULL;

static bool XBOX_CreateWindow(SDL_VideoDevice *_this, SDL_Window *window, SDL_PropertiesID create_props)
{
    (void)_this;
    (void)create_props;

    if (xbox_window) {
        SDL_SetError("Xbox only supports one window");
        return false;
    }

    SDL_PixelFormat format = SDL_GetWindowPixelFormat(window);
    int bpp = SDL_BYTESPERPIXEL(format) * 8;

    // Scale up the window to the next usuable size
    if (window->w < 640) {
        window->w = 640;
    }
    if (window->h < 480) {
        window->h = 480;
    }
    if (window->w > 1280) {
        window->w = 1280;
    }
    if (window->h > 720) {
        window->h = 720;
    }

    if (!XVideoSetMode(window->w, window->h, bpp, REFRESH_DEFAULT)) {

        return SDL_SetError("Failed to set video mode to %dx%dx%d", window->w, window->h, bpp);
    }

    VIDEO_MODE vm = XVideoGetMode();
    window->x = 0;
    window->y = 0;
    window->w = vm.width;
    window->h = vm.height;

    window->flags &= ~(SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    window->flags |= SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_ALWAYS_ON_TOP;

    /* One window, it always has focus */
    SDL_SetMouseFocus(window);
    SDL_SetKeyboardFocus(window);

    xbox_window = window;

    return true;
}

static void XBOX_DeleteDevice(SDL_VideoDevice *device)
{
    SDL_free(device);
}

static void XBOX_PumpEvents(SDL_VideoDevice *device)
{
    (void)device;
    // Do nothing
}

static inline SDL_PixelFormat pixelFormatSelector(int bpp)
{
    SDL_PixelFormat ret_val = SDL_PIXELFORMAT_UNKNOWN;
    switch (bpp) {
    case 15:
        ret_val = SDL_PIXELFORMAT_XRGB1555;
        break;
    case 16:
        ret_val = SDL_PIXELFORMAT_RGB565;
        break;
    case 32:
        ret_val = SDL_PIXELFORMAT_XRGB8888;
        break;
    default:
        assert(0);
        break;
    }
    return ret_val;
}

static bool SDL_XBOX_CreateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, SDL_PixelFormat *format, void **pixels, int *pitch)
{
    (void)_this;
    SDL_Surface *surface;
    const SDL_PixelFormat surface_format = pixelFormatSelector(XVideoGetMode().bpp);
    int w, h;

    // Create a new framebuffer
    SDL_GetWindowSizeInPixels(window, &w, &h);
    surface = SDL_CreateSurface(w, h, surface_format);
    if (!surface) {
        return false;
    }

    // Save the info and return!
    SDL_SetSurfaceProperty(SDL_GetWindowProperties(window), XBOX_SURFACE, surface);
    *format = surface_format;
    *pixels = surface->pixels;
    *pitch = surface->pitch;
    return true;
}

bool SDL_XBOX_UpdateWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    (void)_this;
    SDL_Surface *surface;

    // return true;  // No need to update framebuffer in this backend, we use the GPU framebuffer directly.

    surface = (SDL_Surface *)SDL_GetPointerProperty(SDL_GetWindowProperties(window), XBOX_SURFACE, NULL);
    if (!surface) {
        return SDL_SetError("Couldn't find Xbox surface for window");
    }

    // Get information about SDL window surface to blit from
    VIDEO_MODE vm = XVideoGetMode();
    const void *src = surface->pixels;
    SDL_PixelFormat src_format = surface->format;
    int src_bytes_per_pixel = SDL_BYTESPERPIXEL(src_format);
    int src_pitch = surface->pitch;

    // Get information about GPU framebuffer to blit to
    void *dst = XVideoGetFB();
    SDL_PixelFormat dst_format = pixelFormatSelector(vm.bpp);
    int dst_bytes_per_pixel = SDL_BYTESPERPIXEL(dst_format);
    int dst_pitch = vm.width * dst_bytes_per_pixel;

    // Check if the SDL window fits into GPU framebuffer
    int width = surface->w;
    int height = surface->h;
    assert(width <= vm.width);
    assert(height <= vm.height);

    for (int i = 0; i < numrects; i++) {
        const SDL_Rect *rect = &rects[i];
        Uint8 *src8 = (Uint8 *)src;
        Uint8 *dst8 = (Uint8 *)dst;

        SDL_ConvertPixels(rect->w, rect->h,
                          src_format, &src8[rect->y * src_pitch + rect->x * src_bytes_per_pixel], src_pitch,
                          dst_format, &dst8[rect->y * dst_pitch + rect->x * dst_bytes_per_pixel], dst_pitch);
    }

    // Writeback WC buffers
    XVideoFlushFB();

    return true;
}

static void SDL_XBOX_DestroyWindowFramebuffer(SDL_VideoDevice *_this, SDL_Window *window)
{
    (void)_this;
    SDL_ClearProperty(SDL_GetWindowProperties(window), XBOX_SURFACE);
}

static bool XBOX_VideoInit(SDL_VideoDevice *_this)
{
    (void)_this;
    SDL_DisplayMode mode;
    VIDEO_MODE xmode;
    void *p = NULL;
    while (XVideoListModes(&xmode, 0, 0, &p)) {
        // Due to 1.6 bugs, lets just limit ourself to 32bpp modes
        if (xmode.bpp != 32) {
            continue;
        }

        // pbkit doesnt like 720 widths. FIXME?
        if (xmode.width == 720) {
            continue;
        }

        SDL_zero(mode);
        mode.format = pixelFormatSelector(xmode.bpp);
        mode.w = xmode.width;
        mode.h = xmode.height;

        if (SDL_AddBasicVideoDisplay(&mode) < 0) {
            return false;
        }
    }

    return true;
}

static bool XBOX_SetDisplayMode(SDL_VideoDevice *_this, SDL_VideoDisplay *display, SDL_DisplayMode *mode)
{
    (void)_this;
    (void)display;
    (void)mode;
    DbgPrint("XBOX_SetDisplayMode: %dx%d@%dHz\n", mode->w, mode->h, mode->refresh_rate);
    return 0;
}

static void XBOX_VideoQuit(SDL_VideoDevice *_this)
{
    (void)_this;
}

static SDL_VideoDevice *XBOX_CreateDevice(void)
{
    SDL_VideoDevice *device;

    /* Initialize all variables that we clean on shutdown */
    device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        SDL_OutOfMemory();
        return (0);
    }

    /* Set the function pointers */
    device->CreateSDLWindow = XBOX_CreateWindow;
    device->VideoInit = XBOX_VideoInit;
    device->VideoQuit = XBOX_VideoQuit;
    device->SetDisplayMode = XBOX_SetDisplayMode;
    device->PumpEvents = XBOX_PumpEvents;
    device->CreateWindowFramebuffer = SDL_XBOX_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = SDL_XBOX_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = SDL_XBOX_DestroyWindowFramebuffer;

    device->free = XBOX_DeleteDevice;

    return device;
}

VideoBootStrap XBOX_bootstrap = {
    .name = "nxdk_video",
    .desc = "SDL nxdk video driver",
    .create = XBOX_CreateDevice,
    .ShowMessageBox = NULL,
    .is_preferred = true,
};
