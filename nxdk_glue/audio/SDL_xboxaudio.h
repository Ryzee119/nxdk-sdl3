// SPDX-License-Identifier: Zlib
// SPDX-FileCopyrightText: 1997-2019 Sam Lantinga <slouken@libsdl.org>
// SPDX-FileCopyrightText: 2020 Jannik Vogel
// SPDX-FileCopyrightText: 2025 Ryan Wendland

#ifndef SDL_xboxaudio_h_
#define SDL_xboxaudio_h_

#include <../src/SDL_internal.h>
#include <../src/audio/SDL_sysaudio.h>

// We use the dummy driver to hook our own audio driver without needing to
// modify the SDL3 source code.
#define XBOXAUDIO_bootstrap DUMMYAUDIO_bootstrap

#endif /* SDL_xboxaudio_h_ */
