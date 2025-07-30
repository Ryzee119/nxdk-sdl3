// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2025 Ryan Wendland

#include <SDL3/SDL.h>
#include <windows.h>

#include "../SDL_build_config.h"
#include <../src/thread/SDL_thread_c.h>

static DWORD WINAPI sdl_thread(LPVOID lpParameter)
{
    SDL_Thread *thread = (SDL_Thread *)lpParameter;
    SDL_RunThread(thread);

    return 0;
}

SDL_ThreadID SDL_GetCurrentThreadID(void)
{
    return (SDL_ThreadID)GetCurrentThreadId();
}

void SDL_SYS_SetupThread(const char *name)
{
    (void)name;
    return;
}

bool SDL_SYS_CreateThread(SDL_Thread *thread,
                          SDL_FunctionPointer vpfnBeginThread,
                          SDL_FunctionPointer vpfnEndThread)
{
    // Most backends don't use these, so we can ignore them.
    (void)vpfnBeginThread;
    (void)vpfnEndThread;

    const SIZE_T stack_size = (thread->stacksize > PAGE_SIZE) ? ROUND_TO_PAGES(thread->stacksize) : PAGE_SIZE;

    thread->handle = CreateThread(NULL, stack_size, sdl_thread, thread, 0, NULL);
    if (thread->handle == NULL) {
        return WIN_SetError("Not enough resources to create thread");
    }
    return true;
}

bool SDL_SYS_SetThreadPriority(SDL_ThreadPriority priority)
{
    int value;

    switch (priority) {
    case SDL_THREAD_PRIORITY_LOW:
        value = THREAD_PRIORITY_LOWEST;
        break;
    case SDL_THREAD_PRIORITY_HIGH:
        value = THREAD_PRIORITY_HIGHEST;
        break;
    case SDL_THREAD_PRIORITY_TIME_CRITICAL:
        value = THREAD_PRIORITY_TIME_CRITICAL;
        break;
    default:
        value = THREAD_PRIORITY_NORMAL;
        break;
    }

    if (SetThreadPriority(GetCurrentThread(), value) == FALSE) {
        return WIN_SetError("SetThreadPriority()");
    }

    return true;
}

void SDL_SYS_WaitThread(SDL_Thread *thread)
{
    WaitForSingleObjectEx(thread->handle, INFINITE, FALSE);
    CloseHandle(thread->handle);
}

void SDL_SYS_DetachThread(SDL_Thread *thread)
{
    CloseHandle(thread->handle);
}
