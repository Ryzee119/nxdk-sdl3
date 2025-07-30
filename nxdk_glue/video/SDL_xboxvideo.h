// SPDX-License-Identifier: Zlib
// SPDX-FileCopyrightText: 2019-2021 Matt Borgerson
// SPDX-FileCopyrightText: 2020 Jannik Vogel
// SPDX-FileCopyrightText: 2025 Ryan Wendland

#ifndef SDL_xboxvideo_h_
#define SDL_xboxvideo_h_

#include <../src/SDL_internal.h>

#include <../src/events/SDL_events_c.h>
#include <../src/events/SDL_mouse_c.h>
#include <../src/events/SDL_keyboard_c.h>

#include <../src/video/SDL_sysvideo.h>
#include <../src/video/SDL_pixels_c.h>
#include <SDL_properties_c.h>

#include <hal/video.h>
#include <assert.h>
#include <stdbool.h>

// We use the dummy driver to hook our own video driver without needing to
// modify the SDL3 source code.
#define XBOX_bootstrap DUMMY_bootstrap

#endif /* SDL_xboxvideo_h_ */
