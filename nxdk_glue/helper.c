// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2025 Ryan Wendland

#include <SDL3/SDL.h>
#include <windows.h>

int WIN_SetError(const char *prefix)
{
    DWORD error = GetLastError();
    char buffer[256];
    SDL_snprintf(buffer, sizeof(buffer), "Error: %s: Code: %lu", (prefix) ? prefix : "", error);
    SDL_SetError("%s", buffer);
    return -1;
}
