// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2019 Lucas Eriksson
// SPDX-FileCopyrightText: 2025 Ryan Wendland

#ifndef SDL_xboxjoystick_h_
#define SDL_xboxjoystick_h_

#include <../src/SDL_internal.h>
#include <../src/joystick/SDL_sysjoystick.h>

// We use the dummy driver to hook our own joystick driver without needing to
// modify the SDL3 source code.
#define SDL_XBOX_JoystickDriver SDL_DUMMY_JoystickDriver

#endif /* SDL_xboxjoystick_h_ */
