// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2025 Ryan Wendland

#include "SDL_internal.h"

#if defined(SDL_FILESYSTEM_NXDK) && !defined(SDL_FILESYSTEM_DISABLED)

// The letter is arbritrary but XDK generally uses 'D' for the base path of the running xbe
#ifndef SDL_NXDK_BASE_PATH_LETTER
#define SDL_NXDK_BASE_PATH_LETTER 'D'
#endif

#include <../src/filesystem/SDL_sysfilesystem.h>
#include <assert.h>
#include <nxdk/mount.h>
#include <nxdk/path.h>
#include <windows.h>

// As per SDL docs, the returned paths is guaranteed to end with a path separator ('\' on Windows, '/' on most other platforms).

char *SDL_SYS_GetBasePath(void)
{
    if (nxIsDriveMounted(SDL_NXDK_BASE_PATH_LETTER) == false) {
        char mount_path[MAX_PATH];
        nxGetCurrentXbeNtPath(mount_path);

        // The path includes the xbe name, so we need to remove it.
        char *end = strrchr(mount_path, '\\');
        if (end == NULL) {
            SDL_SetError("Failed to get base path");
            return NULL;
        }
        *(end + 1) = '\0';

        nxMountDrive(SDL_NXDK_BASE_PATH_LETTER, mount_path);
    }

    const char base_path[] = { SDL_NXDK_BASE_PATH_LETTER, ':', '\\', '\0' };
    return SDL_strdup(base_path);
}

char *SDL_SYS_GetPrefPath(const char *org, const char *app)
{
    (void)org;
    if (nxIsDriveMounted('E') == false) {
        nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    }

    char path[MAX_PATH];
    SDL_snprintf(path, sizeof(path), "E:\\UDATA\\%s\\", app);

    CreateDirectoryA("E:\\UDATA", NULL);
    CreateDirectoryA(path, NULL);

    return SDL_strdup(path);
}

char *SDL_SYS_GetUserFolder(SDL_Folder folder)
{
    (void)folder;
    if (nxIsDriveMounted('E') == false) {
        nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    }
    CreateDirectoryA("E:\\UDATA", NULL);

    return SDL_strdup("E:\\UDATA\\");
}

char *SDL_SYS_GetCurrentDirectory(void)
{
    return SDL_SYS_GetBasePath();
}

#endif // SDL_FILESYSTEM_NXDK
