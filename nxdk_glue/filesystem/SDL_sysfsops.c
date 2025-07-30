// SPDX-License-Identifier: Zlib
// SPDX-FileCopyrightText: 1997-2025 Sam Lantinga <slouken@libsdl.org>
// SPDX-FileCopyrightText: 2025 Ryan Wendland

#include "SDL_internal.h"

#include <windows.h>

#if defined(SDL_FSOPS_NXDK)
#include <../src/filesystem/SDL_sysfilesystem.h>

bool SDL_SYS_EnumerateDirectory(const char *path, SDL_EnumerateDirectoryCallback cb, void *userdata)
{
    WIN32_FIND_DATAA ent;

    SDL_EnumerationResult result = SDL_ENUM_CONTINUE;
    // If the path is empty, enumerate drive letters.
    if (*path == '\0') { 
        const DWORD drives = GetLogicalDrives();
        char name[] = { 0, ':', '\\', '\0' };
        for (int i = 'A'; (result == SDL_ENUM_CONTINUE) && (i <= 'Z'); i++) {
            if (drives & (1 << (i - 'A'))) {
                name[0] = (char)i;
                result = cb(userdata, "", name);
            }
        }
    // Otherwise, enumerate the specified directory.
    } else {
        char *pattern = NULL;
        int patternlen = SDL_asprintf(&pattern, "%s\\\\", path); // we'll replace that second '\\' in the trimdown.
        if ((patternlen == -1) || (pattern == NULL)) {
            return false;
        }

        // Remove all trailing path separators
        patternlen--;
        while ((patternlen >= 0) && ((pattern[patternlen] == '\\') || (pattern[patternlen] == '/'))) {
            pattern[patternlen--] = '\0';
        }

        // Add a wildcard pattern to the end of the path.
        pattern[++patternlen] = '\\';
        pattern[++patternlen] = '*';
        pattern[++patternlen] = '\0';

        HANDLE dir = FindFirstFileA(pattern, &ent);
        if (dir == INVALID_HANDLE_VALUE) {
            SDL_free(pattern);
            return WIN_SetError("Failed to enumerate directory");
        }

        // Remove the wildcard from the pattern for the callback.
        pattern[--patternlen] = '\0';
        do {
            const char *fn = ent.cFileName;

            if (fn[0] == '.') { // ignore "." and ".."
                if ((fn[1] == '\0') || ((fn[1] == '.') && (fn[2] == '\0'))) {
                    continue;
                }
            }

            result = cb(userdata, pattern, fn);
        } while ((result == SDL_ENUM_CONTINUE) && (FindNextFileA(dir, &ent) != 0));

        FindClose(dir);
        SDL_free(pattern);
    }

    return (result != SDL_ENUM_FAILURE);
}

bool SDL_SYS_RemovePath(const char *path)
{
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) {
            return true;
        }
        WIN_SetError("Couldn't get path's attributes");
        return false;
    }

    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        return RemoveDirectoryA(path) != 0;
    } else {
        return DeleteFileA(path) != 0;
    }
}

bool SDL_SYS_RenamePath(const char *oldpath, const char *newpath)
{
    return MoveFileA(oldpath, newpath) != 0;
}

bool SDL_SYS_CopyFile(const char *oldpath, const char *newpath)
{
    return CopyFileA(oldpath, newpath, FALSE) != 0;
}

bool SDL_SYS_CreateDirectory(const char *path)
{
    DWORD rc = CreateDirectoryA(path, NULL);
    if (!rc && (GetLastError() == ERROR_ALREADY_EXISTS)) {
        WIN32_FILE_ATTRIBUTE_DATA file_attributes;
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &file_attributes)) {
            if (file_attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                rc = 1; // exists and is already a directory: cool.
            }
        }
    }

    if (!rc) {
        WIN_SetError("Couldn't create directory");
        return false;
    }
    return true;
}

bool SDL_SYS_GetPathInfo(const char *path, SDL_PathInfo *info)
{
    WIN32_FILE_ATTRIBUTE_DATA file_attributes;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &file_attributes)) {
        return false;
    }

    if (file_attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        info->type = SDL_PATHTYPE_DIRECTORY;
        info->size = 0;
    } else if (file_attributes.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) {
        info->type = SDL_PATHTYPE_OTHER;
        info->size = ((((Uint64)file_attributes.nFileSizeHigh) << 32) | file_attributes.nFileSizeLow);
    } else {
        info->type = SDL_PATHTYPE_FILE;
        info->size = ((((Uint64)file_attributes.nFileSizeHigh) << 32) | file_attributes.nFileSizeLow);
    }

    info->create_time = SDL_TimeFromWindows(file_attributes.ftCreationTime.dwLowDateTime, file_attributes.ftCreationTime.dwHighDateTime);
    info->modify_time = SDL_TimeFromWindows(file_attributes.ftLastWriteTime.dwLowDateTime, file_attributes.ftLastWriteTime.dwHighDateTime);
    info->access_time = SDL_TimeFromWindows(file_attributes.ftLastAccessTime.dwLowDateTime, file_attributes.ftLastAccessTime.dwHighDateTime);

    return true;
}

#endif // SDL_FSOPS_NXDK
