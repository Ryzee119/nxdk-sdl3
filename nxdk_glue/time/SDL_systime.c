// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2025 Ryan Wendland

#include <SDL3/SDL.h>
#include <windows.h>

#include "../SDL_build_config.h"
#include <../src/time/SDL_time_c.h>

#ifdef SDL_TIME_NXDK

void SDL_GetSystemTimeLocalePreferences(SDL_DateFormat *df, SDL_TimeFormat *tf)
{
    (void)df;
    (void)tf;
    // SDL defaults to ISO 8061 date format already. This will do.
    return;
}

bool SDL_GetCurrentTime(SDL_Time *ticks)
{
    if (!ticks) {
        return SDL_InvalidParamError("ticks");
    }

    FILETIME file_time;
    GetSystemTimePreciseAsFileTime(&file_time);

    *ticks = SDL_TimeFromWindows(file_time.dwLowDateTime, file_time.dwHighDateTime);
    return true;
}

bool SDL_TimeToDateTime(SDL_Time ticks, SDL_DateTime *dt, bool localTime)
{
    if (!dt) {
        return SDL_InvalidParamError("dt");
    }

    if (localTime) {
        TIME_ZONE_INFORMATION timezone;
        DWORD result = GetTimeZoneInformation(&timezone);

        dt->utc_offset = -timezone.Bias;
        if (result == TIME_ZONE_ID_STANDARD) {
            dt->utc_offset -= timezone.StandardBias;
        } else if (result == TIME_ZONE_ID_DAYLIGHT) {
            dt->utc_offset -= timezone.DaylightBias;
        }
        dt->utc_offset *= 60;

        ticks += SDL_NS_PER_SECOND * dt->utc_offset;
    } else {
        dt->utc_offset = 0;
    }

    FILETIME file_time;
    SDL_TimeToWindows(ticks, (Uint32 *)&file_time.dwLowDateTime, (Uint32 *)&file_time.dwHighDateTime);

    SYSTEMTIME system_time;
    FileTimeToSystemTime(&file_time, &system_time);

    dt->year = system_time.wYear;
    dt->month = system_time.wMonth;
    dt->day = system_time.wDay;
    dt->hour = system_time.wHour;
    dt->minute = system_time.wMinute;
    dt->second = system_time.wSecond;
    dt->nanosecond = ticks % SDL_NS_PER_SECOND;
    dt->day_of_week = system_time.wDayOfWeek;
    return true;
}

#endif
