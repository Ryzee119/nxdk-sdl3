// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2025 Ryan Wendland

#include <SDL3/SDL.h>
#include <windows.h>

#include "../SDL_build_config.h"

#ifdef SDL_TIMER_NXDK

Uint64 SDL_GetPerformanceFrequency(void)
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return (Uint64)frequency.QuadPart;
}

Uint64 SDL_GetPerformanceCounter(void)
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (Uint64)counter.QuadPart;
}

void SDL_SYS_DelayNS(Uint64 ns)
{
    LARGE_INTEGER interval;
    interval.QuadPart = -(int64_t)(ns / 100);
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

#endif // SDL_TIMER_NXDK