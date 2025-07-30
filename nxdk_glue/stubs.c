// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2025 Ryan Wendland

#include <SDL3/SDL.h>
#include <windows.h>

HMODULE GetModuleHandle(LPCSTR lpModuleName) {
    return NULL;
}

void SDL_InitSteamVirtualGamepadInfo(void)
{

}

bool SDL_SteamVirtualGamepadEnabled(void)
{
    return false;
}

void SDL_QuitSteamVirtualGamepadInfo(void)
{

}

typedef void *SDL_SteamVirtualGamepadInfo;
const SDL_SteamVirtualGamepadInfo *SDL_GetSteamVirtualGamepadInfo(int slot)
{
    return NULL;
}

bool SDL_UpdateSteamVirtualGamepadInfo(void)
{
    return false;
}
